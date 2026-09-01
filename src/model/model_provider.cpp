#include <mira/model_digest.hpp>
#include <mira/model_provider.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

class BoundArtifactSource final : public IArtifactSource {
  public:
    explicit BoundArtifactSource(std::shared_ptr<IArtifactSource> source)
        : source_(std::move(source)) {}

    Result<std::vector<std::byte>> fetch(const ArtifactRef &reference) override {
        return source_->fetch(reference);
    }

    std::optional<std::string> remote_file_id(const ArtifactRef &reference) const override {
        const auto found = bindings_.find(reference.id.to_string());
        return found == bindings_.end() ? std::nullopt : std::optional<std::string>(found->second);
    }

    void bind(const RemoteFileRef &file) {
        bindings_[file.source.id.to_string()] = file.provider_file_id;
    }

  private:
    std::shared_ptr<IArtifactSource> source_;
    std::map<std::string, std::string> bindings_;
};

class UploadRetirement final {
  public:
    UploadRetirement(std::shared_ptr<IRemoteFileStore> store, std::chrono::seconds retention)
        : store_(std::move(store)), retention_(retention) {}
    ~UploadRetirement() {
        if (store_ == nullptr) {
            return;
        }
        for (const auto &file : files_) {
            static_cast<void>(store_->retire(file, retention_));
        }
    }
    void add(RemoteFileRef file) { files_.push_back(std::move(file)); }

  private:
    std::shared_ptr<IRemoteFileStore> store_;
    std::chrono::seconds retention_;
    std::vector<RemoteFileRef> files_;
};

[[nodiscard]] TransportLimits limits_from_profile(const ModelProfile &profile,
                                                  const ModelRequest &request) {
    TransportLimits limits;
    limits.deadlines = profile.deadlines;
    limits.max_response_bytes = profile.capabilities.limits.max_response_bytes;
    limits.max_redirects = profile.max_redirects;
    limits.proxy = profile.proxy;
    const auto host = profile.endpoint_host();
    if (!host.empty()) {
        limits.allowed_hosts.push_back(host);
    }
    limits.allow_private_endpoints = false;
    if (profile.endpoint_origin.rfind("http://", 0) == 0) {
        // Plain-http origins are a test/dev configuration; production
        // profiles use https.
    }
    static_cast<void>(request);
    return limits;
}

} // namespace

const TransportTrace &IModelProvider::last_trace() const {
    static const TransportTrace kEmpty{};
    return kEmpty;
}

const SseStreamStats &IModelProvider::last_sse_stats() const {
    static const SseStreamStats kEmpty{};
    return kEmpty;
}

std::optional<std::chrono::milliseconds> IModelProvider::last_retry_after_hint() const {
    return std::nullopt;
}

RequestStage classify_provider_stage(const TransportTrace &trace, const Error &failure) {
    if (trace.headers_received) {
        return RequestStage::AwaitingResponse;
    }
    if (trace.write_completed || trace.write_started) {
        // Bytes left the process: the remote may have received the request.
        return RequestStage::AwaitingResponse;
    }
    if (failure.domain == "mira.model" &&
        failure.domain_code == static_cast<std::int32_t>(ModelDomainCode::AmbiguousCompletion)) {
        return RequestStage::AwaitingResponse;
    }
    return RequestStage::PreWriteFailure;
}

OpenAiCompatibleProvider::OpenAiCompatibleProvider(
    std::shared_ptr<const ModelProfile> profile, std::shared_ptr<IHttpTransport> transport,
    std::shared_ptr<IArtifactSource> artifacts, std::shared_ptr<IArtifactStore> protected_artifacts,
    std::shared_ptr<IRemoteFileStore> remote_files)
    : profile_(std::move(profile)), transport_(std::move(transport)),
      artifacts_(std::move(artifacts)), protected_artifacts_(std::move(protected_artifacts)),
      remote_files_(std::move(remote_files)) {}

OpenAiCompatibleProvider::~OpenAiCompatibleProvider() = default;

std::optional<std::chrono::milliseconds> OpenAiCompatibleProvider::last_retry_after_hint() const {
    return parse_retry_after(last_headers_, std::chrono::milliseconds{30'000});
}

std::optional<UnvalidatedModelPreview> OpenAiCompatibleProvider::take_last_preview() {
    if (last_preview_.has_value()) {
        auto preview = std::move(last_preview_);
        last_preview_.reset();
        return preview;
    }
    return std::nullopt;
}

Result<ModelResponse> OpenAiCompatibleProvider::infer(const ModelRequest &request,
                                                      const OperationContext &context,
                                                      const ProviderInferOptions &options) {
    const IDialectMapper &mapper = profile_->dialect == ProtocolDialect::OpenAIResponsesV1
                                       ? static_cast<const IDialectMapper &>(responses_)
                                       : static_cast<const IDialectMapper &>(chat_);

    BoundArtifactSource bound_artifacts(artifacts_);
    UploadRetirement retirement(remote_files_, request.data_policy.remote_retention);
    if (remote_files_ != nullptr && request.data_policy.allow_uploads &&
        profile_->dialect == ProtocolDialect::OpenAIResponsesV1) {
        for (const auto &item : request.input) {
            for (const auto &part : item.content) {
                const ArtifactRef *source = nullptr;
                std::string display_name;
                if (const auto *image = std::get_if<ImagePart>(&part)) {
                    source = &image->source;
                    display_name = "image-" + image->source.digest.to_string().substr(0, 12);
                } else if (const auto *file = std::get_if<FilePart>(&part)) {
                    source = &file->source;
                    display_name = file->display_name;
                }
                if (source == nullptr || bound_artifacts.remote_file_id(*source).has_value()) {
                    continue;
                }
                auto uploaded =
                    remote_files_->upload(*source, display_name, request.data_policy, context);
                if (!uploaded) {
                    return uploaded.error();
                }
                bound_artifacts.bind(uploaded.value());
                retirement.add(std::move(uploaded).value());
            }
        }
    }

    auto wire = mapper.encode_request(request, *profile_, options.stream, bound_artifacts);
    if (!wire) {
        return wire.error();
    }

    HttpRequest http;
    http.method = "POST";
    http.url = profile_->endpoint_url();
    http.body = to_json_string(wire.value());
    http.headers.emplace_back("Content-Type", "application/json");
    http.headers.emplace_back("Accept", options.stream ? "text/event-stream" : "application/json");
    if (request.data_policy.organization.has_value()) {
        http.headers.emplace_back("OpenAI-Organization", *request.data_policy.organization);
    }
    if (request.data_policy.project.has_value()) {
        http.headers.emplace_back("OpenAI-Project", *request.data_policy.project);
    }
    http.authorization = profile_->credential;

    TransportTrace trace;
    last_trace_ = TransportTrace{};
    last_headers_.clear();

    const auto stash_headers =
        [this](const std::vector<std::pair<std::string, std::string>> &headers) {
            last_headers_ = headers;
        };
    static_cast<void>(stash_headers);

    if (options.stream) {
        if (profile_->dialect != ProtocolDialect::OpenAIResponsesV1) {
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "streaming is not enabled for this dialect");
        }
        ResponsesSseParser parser(request, *profile_);
        std::string raw;
        std::optional<Error> parse_failure;
        auto info = transport_->execute(
            http, limits_from_profile(*profile_, request), context,
            [&](std::string_view chunk) {
                raw.append(chunk.data(), chunk.size());
                if (parse_failure.has_value()) {
                    return; // Stop feeding after the first protocol failure.
                }
                auto status = parser.feed(chunk);
                if (!status) {
                    parse_failure = status.error();
                }
            },
            trace);
        last_trace_ = trace;
        if (info.has_value()) {
            last_headers_ = info.value().headers;
        }
        sse_stats_ = parser.stats();
        last_preview_ = parser.take_preview();
        if (!info) {
            return info.error();
        }
        if (parse_failure.has_value()) {
            return parse_failure.value();
        }
        if (info.value().status < 200 || info.value().status >= 300) {
            return map_http_error_status(
                WireHttpResponse{info.value().status, info.value().headers, raw});
        }
        auto finished = parser.finish();
        if (!finished) {
            return finished.error();
        }
        auto result = std::move(finished).value();
        result.rate_limit = parse_rate_limit_headers(last_headers_);
        if (options.capture_raw_response && protected_artifacts_ != nullptr) {
            auto reference = store_raw_response(raw);
            if (reference) {
                result.protected_raw_response = std::move(reference).value();
            }
        }
        return result;
    }

    std::string raw;
    auto info = transport_->execute(
        http, limits_from_profile(*profile_, request), context,
        [&](std::string_view chunk) { raw.append(chunk.data(), chunk.size()); }, trace);
    last_trace_ = trace;
    if (info) {
        last_headers_ = info.value().headers;
    }
    if (!info) {
        return info.error();
    }
    // The wire object owns the single body copy; decode and the protected
    // artifact writer both read from it.
    WireHttpResponse wire_response;
    wire_response.status = info.value().status;
    wire_response.headers = info.value().headers;
    wire_response.body = std::move(raw);
    auto decoded = mapper.decode_response(request, *profile_, wire_response);
    if (!decoded) {
        return decoded.error();
    }
    if (options.capture_raw_response && protected_artifacts_ != nullptr) {
        auto reference = store_raw_response(wire_response.body);
        if (reference) {
            decoded.value().protected_raw_response = std::move(reference).value();
        }
    }
    return decoded;
}

Result<ArtifactRef> OpenAiCompatibleProvider::store_raw_response(const std::string &raw) const {
    if (protected_artifacts_ == nullptr) {
        return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                "no protected artifact store is configured");
    }
    ArtifactWriteSpec spec;
    spec.media_type = "application/json";
    spec.encoding = ArtifactEncoding::Utf8;
    spec.sensitivity = Sensitivity::Sensitive;
    spec.content_schema = SchemaVersion{1, 0};
    spec.max_bytes = 16ULL * 1024ULL * 1024ULL;
    auto writer = protected_artifacts_->begin(spec);
    if (!writer) {
        return writer.error();
    }
    if (auto written = writer.value().write(raw.data(), raw.size()); !written) {
        return written.error();
    }
    auto descriptor = protected_artifacts_->commit(writer.value());
    if (!descriptor) {
        return descriptor.error();
    }
    ArtifactRef reference;
    reference.id = descriptor.value().id;
    reference.digest = descriptor.value().digest;
    reference.byte_size = descriptor.value().byte_size;
    reference.media_type = descriptor.value().media_type;
    reference.sensitivity = Sensitivity::Sensitive;
    return reference;
}

} // namespace mira
