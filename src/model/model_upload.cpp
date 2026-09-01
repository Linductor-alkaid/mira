#include <mira/model_upload.hpp>

#include <mira/model_digest.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

constexpr std::size_t kMaxPendingDeletes = 256;

[[nodiscard]] bool safe_provider_id(const std::string &value) {
    return !value.empty() && value.size() <= 512 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
           });
}

[[nodiscard]] bool safe_filename(const std::string &value) {
    return !value.empty() && value.size() <= 255 && value.find('\r') == std::string::npos &&
           value.find('\n') == std::string::npos && value.find('"') == std::string::npos;
}

[[nodiscard]] bool safe_media_type(const std::string &value) {
    return !value.empty() && value.size() <= 127 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '/' || ch == '+' || ch == '-' || ch == '.';
           });
}

[[nodiscard]] TransportLimits upload_limits(const ModelProfile &profile) {
    TransportLimits limits;
    limits.deadlines = profile.deadlines;
    limits.max_response_bytes = std::min<std::uint64_t>(
        profile.capabilities.limits.max_response_bytes, 1ULL * 1024ULL * 1024ULL);
    limits.max_redirects = profile.max_redirects;
    limits.proxy = profile.proxy;
    const auto host = profile.endpoint_host();
    if (!host.empty()) {
        limits.allowed_hosts.push_back(host);
    }
    return limits;
}

[[nodiscard]] std::string files_url(const ModelProfile &profile) {
    std::string prefix = profile.api_prefix;
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return profile.endpoint_origin + prefix + "/files";
}

[[nodiscard]] RemoteFileAudit make_audit(RemoteFileAuditKind kind, const RemoteFileRef &file,
                                         std::string detail) {
    RemoteFileAudit audit;
    audit.kind = kind;
    audit.artifact_id = file.source.id;
    audit.provider_id_digest = digest_string(file.provider_file_id);
    audit.safe_detail = std::move(detail);
    return audit;
}

} // namespace

struct OpenAiRemoteFileStore::Impl final {
    struct PendingDelete final {
        RemoteFileRef file;
        executor::TimerHandle handle;
        std::future<Result<void>> future;
    };

    executor::Executor &executor;
    std::shared_ptr<const ModelProfile> profile;
    std::shared_ptr<IHttpTransport> transport;
    std::shared_ptr<IArtifactSource> artifacts;
    mutable std::mutex mutex;
    bool stopping = false;
    std::vector<PendingDelete> pending;
    std::vector<RemoteFileAudit> audits;

    Impl(executor::Executor &owner, std::shared_ptr<const ModelProfile> configured_profile,
         std::shared_ptr<IHttpTransport> configured_transport,
         std::shared_ptr<IArtifactSource> configured_artifacts)
        : executor(owner), profile(std::move(configured_profile)),
          transport(std::move(configured_transport)), artifacts(std::move(configured_artifacts)) {}

    void append(RemoteFileAudit audit) {
        std::lock_guard lock(mutex);
        audits.push_back(std::move(audit));
    }

    [[nodiscard]] Result<void> delete_remote(const RemoteFileRef &file) {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                audits.push_back(make_audit(RemoteFileAuditKind::DeleteCancelled, file,
                                            "remote delete cancelled during shutdown"));
                return make_model_error(ModelDomainCode::ModelCancelled,
                                        "remote file cleanup was cancelled during shutdown");
            }
        }
        if (!safe_provider_id(file.provider_file_id)) {
            auto error =
                make_model_error(ModelDomainCode::ProtocolViolation, "provider file id is invalid");
            append(make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
            return error;
        }
        HttpRequest request;
        request.method = "DELETE";
        request.url = files_url(*profile) + "/" + file.provider_file_id;
        request.headers.emplace_back("Accept", "application/json");
        request.authorization = profile->credential;
        TransportTrace trace;
        std::string body;
        OperationContext context;
        context.deadline = std::chrono::steady_clock::now() + profile->deadlines.total;
        auto response = transport->execute(
            request, upload_limits(*profile), context,
            [&](std::string_view chunk) { body.append(chunk.data(), chunk.size()); }, trace);
        if (!response) {
            append(
                make_audit(RemoteFileAuditKind::DeleteFailed, file, response.error().safe_message));
            return response.error();
        }
        if (response.value().status < 200 || response.value().status >= 300) {
            auto error = map_http_error_status(
                WireHttpResponse{response.value().status, response.value().headers, body});
            append(make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
            return error;
        }
        auto parsed = parse_json(body);
        const auto *deleted = parsed ? parsed.value().find("deleted") : nullptr;
        if (deleted == nullptr || !deleted->as_boolean().value_or(false)) {
            auto error = make_model_error(ModelDomainCode::ProtocolViolation,
                                          "remote file delete was not acknowledged");
            append(make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
            return error;
        }
        append(make_audit(RemoteFileAuditKind::Deleted, file, "remote file deleted"));
        return Result<void>{};
    }
};

OpenAiRemoteFileStore::OpenAiRemoteFileStore(executor::Executor &executor,
                                             std::shared_ptr<const ModelProfile> profile,
                                             std::shared_ptr<IHttpTransport> transport,
                                             std::shared_ptr<IArtifactSource> artifacts)
    : impl_(std::make_shared<Impl>(executor, std::move(profile), std::move(transport),
                                   std::move(artifacts))) {}

OpenAiRemoteFileStore::~OpenAiRemoteFileStore() { shutdown(); }

Result<RemoteFileRef> OpenAiRemoteFileStore::upload(const ArtifactRef &source,
                                                    const std::string &display_name,
                                                    const ModelDataPolicy &policy,
                                                    const OperationContext &context) {
    if (impl_->profile == nullptr || impl_->transport == nullptr || impl_->artifacts == nullptr) {
        return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                "remote file store dependencies are not configured");
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return make_model_error(ModelDomainCode::ModelCancelled,
                                    "remote file store is shutting down");
        }
    }
    if (!policy.allow_uploads || !impl_->profile->capabilities.upload.supported) {
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "remote uploads are not allowed by policy and profile");
    }
    if (source.sensitivity == Sensitivity::Sensitive || source.sensitivity == Sensitivity::Secret) {
        return make_model_error(ModelDomainCode::EndpointPolicyDenied,
                                "sensitive artifacts cannot be uploaded remotely");
    }
    if (!safe_filename(display_name) || !safe_media_type(source.media_type)) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "remote upload filename or media type is invalid");
    }
    if (source.byte_size > impl_->profile->capabilities.limits.max_request_bytes) {
        return make_model_error(ModelDomainCode::ResponseTooLarge,
                                "remote upload exceeds the profile request limit");
    }
    auto payload = impl_->artifacts->fetch(source);
    if (!payload) {
        return payload.error();
    }
    const std::string boundary = "mira-" + source.digest.to_string().substr(0, 24);
    const std::string marker = "\r\n--" + boundary;
    const auto *raw = reinterpret_cast<const char *>(payload.value().data());
    const std::string_view payload_view(raw, payload.value().size());
    if (payload_view.find(marker) != std::string_view::npos) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "artifact conflicts with the multipart boundary");
    }
    std::string body;
    body.reserve(payload.value().size() + 512);
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"purpose\"\r\n\r\nuser_data\r\n";
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" +
            display_name + "\"\r\nContent-Type: " + source.media_type + "\r\n\r\n";
    body.append(raw, payload.value().size());
    body += "\r\n--" + boundary + "--\r\n";
    if (body.size() > impl_->profile->capabilities.limits.max_request_bytes) {
        return make_model_error(ModelDomainCode::ResponseTooLarge,
                                "multipart upload exceeds the profile request limit");
    }

    HttpRequest request;
    request.method = "POST";
    request.url = files_url(*impl_->profile);
    request.headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
    request.headers.emplace_back("Accept", "application/json");
    if (policy.organization.has_value()) {
        request.headers.emplace_back("OpenAI-Organization", *policy.organization);
    }
    if (policy.project.has_value()) {
        request.headers.emplace_back("OpenAI-Project", *policy.project);
    }
    request.authorization = impl_->profile->credential;
    request.body = std::move(body);
    TransportTrace trace;
    std::string response_body;
    auto response = impl_->transport->execute(
        request, upload_limits(*impl_->profile), context,
        [&](std::string_view chunk) { response_body.append(chunk.data(), chunk.size()); }, trace);
    if (!response) {
        return response.error();
    }
    if (response.value().status < 200 || response.value().status >= 300) {
        return map_http_error_status(
            WireHttpResponse{response.value().status, response.value().headers, response_body});
    }
    auto parsed = parse_json(response_body);
    const auto *id = parsed ? parsed.value().find("id") : nullptr;
    if (id == nullptr || !id->is_string() || !safe_provider_id(*id->as_string())) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "remote upload returned an invalid file id");
    }
    RemoteFileRef remote{source, *id->as_string()};
    impl_->append(make_audit(RemoteFileAuditKind::Uploaded, remote, "remote file uploaded"));
    return remote;
}

Result<void> OpenAiRemoteFileStore::retire(const RemoteFileRef &file,
                                           std::chrono::seconds retention) {
    const auto retirement_error = [this, &file](ModelDomainCode code, const char *message,
                                                bool retryable = false) -> Error {
        auto error = make_model_error(code, message, retryable);
        impl_->append(make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
        return error;
    };
    if (retention <= std::chrono::seconds::zero()) {
        return impl_->delete_remote(file);
    }
    if (retention.count() > std::numeric_limits<std::int64_t>::max() / 1000) {
        return retirement_error(ModelDomainCode::InvalidModelRequest,
                                "remote retention interval is invalid");
    }
    const auto requested = std::chrono::duration_cast<std::chrono::milliseconds>(retention);
    if (requested <= std::chrono::milliseconds::zero()) {
        return retirement_error(ModelDomainCode::InvalidModelRequest,
                                "remote retention interval is invalid");
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
        auto error =
            make_model_error(ModelDomainCode::ModelCancelled, "remote file store is shutting down");
        impl_->audits.push_back(
            make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
        return error;
    }
    if (impl_->pending.size() >= kMaxPendingDeletes) {
        auto error = make_model_error(ModelDomainCode::ModelResourceExhausted,
                                      "remote cleanup queue is full", true);
        impl_->audits.push_back(
            make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
        return error;
    }
    try {
        auto state = impl_;
        auto submission = impl_->executor.submit_delayed_with_handle(
            requested.count(), [state, file] { return state->delete_remote(file); });
        impl_->pending.push_back(
            Impl::PendingDelete{file, std::move(submission.handle), std::move(submission.future)});
        impl_->audits.push_back(
            make_audit(RemoteFileAuditKind::DeleteScheduled, file, "remote delete scheduled"));
    } catch (const std::exception &) {
        auto error = make_model_error(ModelDomainCode::ModelResourceExhausted,
                                      "Executor rejected remote cleanup scheduling", true);
        impl_->audits.push_back(
            make_audit(RemoteFileAuditKind::DeleteFailed, file, error.safe_message));
        return error;
    }
    return Result<void>{};
}

std::vector<RemoteFileAudit> OpenAiRemoteFileStore::audit() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->audits;
}

void OpenAiRemoteFileStore::shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    std::vector<Impl::PendingDelete> pending;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return;
        }
        impl_->stopping = true;
        pending.swap(impl_->pending);
    }
    for (auto &item : pending) {
        static_cast<void>(item.handle.cancel());
    }
    for (auto &item : pending) {
        try {
            static_cast<void>(item.future.get());
        } catch (const std::exception &) {
            impl_->append(make_audit(RemoteFileAuditKind::DeleteCancelled, item.file,
                                     "remote delete timer cancelled during shutdown"));
        }
    }
}

} // namespace mira
