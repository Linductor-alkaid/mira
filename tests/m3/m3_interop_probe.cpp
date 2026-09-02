#include "mbedtls_tls.hpp"
#include "socket_transport.hpp"

#include <mira/model_digest.hpp>
#include <mira/model_provider.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_upload.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::adapters::net;

[[nodiscard]] std::optional<std::string> environment(const char *name) {
#ifdef _WIN32
    char *raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&std::free)> value(raw, &std::free);
    return *value == '\0' ? std::nullopt : std::optional<std::string>(value.get());
#else
    const char *value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>(value);
#endif
}

class ProbeSecrets final : public ISecretResolver {
  public:
    void set(std::string name, std::string value) { values_[std::move(name)] = std::move(value); }

    Result<std::string> resolve(const SecretRef &reference) override {
        const auto found = values_.find(reference.name);
        if (found == values_.end()) {
            return make_model_error(ModelDomainCode::AuthenticationFailed,
                                    "interop secret reference is unavailable");
        }
        return found->second;
    }

  private:
    std::map<std::string, std::string> values_;
};

class ProbeArtifacts final : public IArtifactSource {
  public:
    ProbeArtifacts()
        : text_payload_("Mira controlled interop fixture. No user data.\n"),
          image_payload_{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
                         0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
                         0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00, 0x00, 0xb5,
                         0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
                         0x54, 0x78, 0xda, 0x63, 0xfc, 0xff, 0x1f, 0x00, 0x03, 0x03,
                         0x02, 0x00, 0xef, 0xa3, 0x07, 0x5d, 0x00, 0x00, 0x00, 0x00,
                         0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82} {
        text_reference_.id = ArtifactId::generate();
        text_reference_.digest = digest_string(text_payload_);
        text_reference_.byte_size = text_payload_.size();
        text_reference_.media_type = "text/plain";
        text_reference_.sensitivity = Sensitivity::Public;
        image_reference_.id = ArtifactId::generate();
        image_reference_.digest =
            digest_bytes(std::as_bytes(std::span(image_payload_.data(), image_payload_.size())));
        image_reference_.byte_size = image_payload_.size();
        image_reference_.media_type = "image/png";
        image_reference_.sensitivity = Sensitivity::Public;
    }

    Result<std::vector<std::byte>> fetch(const ArtifactRef &reference) override {
        if (reference.id == text_reference_.id) {
            std::vector<std::byte> bytes;
            bytes.reserve(text_payload_.size());
            for (const char value : text_payload_) {
                bytes.push_back(static_cast<std::byte>(value));
            }
            return bytes;
        }
        if (reference.id == image_reference_.id) {
            std::vector<std::byte> bytes;
            bytes.reserve(image_payload_.size());
            for (const auto value : image_payload_) {
                bytes.push_back(static_cast<std::byte>(value));
            }
            return bytes;
        }
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "interop artifact reference is unknown");
    }

    [[nodiscard]] const ArtifactRef &text_reference() const noexcept { return text_reference_; }
    [[nodiscard]] const ArtifactRef &image_reference() const noexcept { return image_reference_; }

  private:
    std::string text_payload_;
    std::vector<std::uint8_t> image_payload_;
    ArtifactRef text_reference_;
    ArtifactRef image_reference_;
};

[[nodiscard]] ModelProfile make_profile(const std::string &origin, const std::string &prefix,
                                        const std::string &model,
                                        const std::optional<std::string> &proxy_url) {
    ModelProfile profile;
    profile.id = ModelProfileId{Id128(Id128::Bytes{
        0x8d, 0x8e, 0x36, 0x51, 0x90, 0xe8, 0x4a, 0x50,
        0x8f, 0x09, 0x08, 0xbe, 0x9b, 0x53, 0xf7, 0x0d,
    })};
    profile.display_name = "controlled-interop-profile";
    profile.version = SemanticVersion{1, 0, 0};
    profile.dialect = ProtocolDialect::OpenAIResponsesV1;
    profile.endpoint_origin = origin;
    profile.api_prefix = prefix;
    profile.model_selector = model;
    profile.credential = SecretRef{"interop-api-key"};
    const CapabilityFlag configured{true, CapabilityEvidence::Configured, "interop probe"};
    profile.capabilities.text = configured;
    profile.capabilities.image_input = configured;
    profile.capabilities.file_input = configured;
    profile.capabilities.strict_json_schema = configured;
    profile.capabilities.function_tools = configured;
    profile.capabilities.parallel_tool_calls = configured;
    profile.capabilities.sse = configured;
    profile.capabilities.upload = configured;
    profile.capabilities.limits.max_output_tokens = 128;
    profile.default_data_policy.store = false;
    if (proxy_url.has_value()) {
        ModelProxyConfig proxy;
        proxy.url = *proxy_url;
        proxy.allow_private_endpoint = environment("MIRA_INTEROP_PROXY_ALLOW_PRIVATE") == "1";
        ModelProfile proxy_origin;
        proxy_origin.endpoint_origin = *proxy_url;
        proxy.allowed_hosts = {proxy_origin.endpoint_host()};
        if (environment("MIRA_INTEROP_PROXY_AUTH").has_value()) {
            proxy.authorization = SecretRef{"interop-proxy-authorization"};
        }
        profile.proxy = std::move(proxy);
    }
    return profile;
}

[[nodiscard]] ModelRequest make_request(const ModelProfile &profile,
                                        const std::optional<ArtifactRef> &file = std::nullopt) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = profile.id;
    ModelInputItem system;
    system.role = ModelRole::System;
    system.content.emplace_back(
        TextPart{"Follow the exact controlled protocol-test contract.", Sensitivity::Public});
    request.input.push_back(std::move(system));
    ModelInputItem item;
    item.role = ModelRole::User;
    item.content.emplace_back(
        TextPart{"Return {\"ok\":true}. This is a controlled protocol test.", Sensitivity::Public});
    if (file.has_value()) {
        item.content.emplace_back(FilePart{*file, "text/plain", "mira-interop.txt"});
        request.data_policy.allow_uploads = true;
    }
    request.input.push_back(std::move(item));
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id = SchemaId::generate();
    request.output_contract.schema_version = SemanticVersion{1, 0, 0};
    auto schema = parse_json(
        R"({"type":"object","properties":{"ok":{"type":"boolean","const":true}},"required":["ok"],"additionalProperties":false})");
    request.output_contract.schema.root = std::move(schema).value();
    request.output_contract.canonical_schema_digest =
        canonical_json_digest(request.output_contract.schema.root);
    request.generation.max_output_tokens = 64;
    request.data_policy.store = false;
    request.data_policy.remote_retention = std::chrono::seconds{0};
    request.budget.max_output_tokens = 64;
    request.budget.max_requests = 3;
    return request;
}

[[nodiscard]] ModelRequest make_tool_request(const ModelProfile &profile) {
    auto request = make_request(profile);
    request.input.back().content.clear();
    request.input.back().content.emplace_back(TextPart{
        "Call mira_protocol_probe exactly once with {\"ok\":true}; do not answer with text.",
        Sensitivity::Public});
    request.output_contract.mode = OutputMode::StrictFunctionTool;
    ExposedToolSpec tool;
    tool.tool_id = ToolId::generate();
    tool.version = SemanticVersion{1, 0, 0};
    tool.wire_name = "mira_protocol_probe";
    tool.description = "Returns the controlled protocol-test marker.";
    tool.parameters_schema.root = parse_json(
        R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"],"additionalProperties":false})")
                                      .value();
    request.tools.push_back(std::move(tool));
    request.tools.front().spec_digest = tool_snapshot_digest(request.tools);
    request.tool_choice.mode = ToolChoiceMode::Named;
    request.tool_choice.required_tool = request.tools.front().tool_id;
    return request;
}

[[nodiscard]] bool valid_tool_call(const ModelRequest &request, const ModelResponse &response) {
    if (response.status != ModelCompletionStatus::Completed) {
        return false;
    }
    const ToolCallOutput *matched = nullptr;
    for (const auto &item : response.output) {
        if (const auto *call = std::get_if<ToolCallOutput>(&item); call != nullptr) {
            if (matched != nullptr) {
                return false;
            }
            matched = call;
        }
    }
    if (matched == nullptr || matched->tool_id != request.tools.front().tool_id) {
        return false;
    }
    const auto *ok = matched->arguments.find("ok");
    return ok != nullptr && ok->as_boolean().value_or(false);
}

[[nodiscard]] bool has_domain_code(const Result<ModelResponse> &result, ModelDomainCode code) {
    return !result.has_value() &&
           result.error().domain_code == static_cast<std::int32_t>(code);
}

[[nodiscard]] OperationContext context() {
    OperationContext result;
    result.operation = OperationId::generate();
    result.started_at = Timestamp::now();
    result.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    return result;
}

[[nodiscard]] bool valid_decision(const ModelRequest &request, const ModelResponse &response) {
    const auto parsed = parse_decision(request, response);
    if (parsed.outcome != DecisionParseOutcome::Decision || !parsed.decision.has_value()) {
        return false;
    }
    const auto *ok = parsed.decision->value.find("ok");
    return ok != nullptr && ok->as_boolean().value_or(false);
}

[[nodiscard]] const char *usage_name(UsageQuality quality) {
    switch (quality) {
    case UsageQuality::Exact:
        return "Exact";
    case UsageQuality::ProviderReported:
        return "ProviderReported";
    case UsageQuality::Estimated:
        return "Estimated";
    case UsageQuality::Partial:
        return "Partial";
    case UsageQuality::Missing:
        return "Missing";
    }
    return "Unknown";
}

[[nodiscard]] const char *completion_name(ModelCompletionStatus status) {
    switch (status) {
    case ModelCompletionStatus::Completed:
        return "Completed";
    case ModelCompletionStatus::Incomplete:
        return "Incomplete";
    case ModelCompletionStatus::Refused:
        return "Refused";
    case ModelCompletionStatus::ContentFiltered:
        return "ContentFiltered";
    case ModelCompletionStatus::Failed:
        return "Failed";
    case ModelCompletionStatus::Cancelled:
        return "Cancelled";
    case ModelCompletionStatus::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace

int main() {
    const auto key = environment("MIRA_INTEROP_API_KEY");
    const auto model = environment("MIRA_INTEROP_MODEL");
    const auto ca_file = environment("MIRA_INTEROP_CA_FILE");
    const auto max_requests = environment("MIRA_INTEROP_MAX_REQUESTS");
    const auto selected_case = environment("MIRA_INTEROP_CASE");
    const bool image_only = max_requests == "1" && selected_case == "image";
    if (!key.has_value() || !model.has_value() || !ca_file.has_value() ||
        !max_requests.has_value() || (!image_only && *max_requests != "2" &&
                                     *max_requests != "3" && *max_requests != "6")) {
        std::cerr << "refusing network interop: set MIRA_INTEROP_API_KEY, MIRA_INTEROP_MODEL, "
                     "MIRA_INTEROP_CA_FILE and MIRA_INTEROP_MAX_REQUESTS=2, 3, or 6 "
                     "(or 1 with MIRA_INTEROP_CASE=image)\n";
        return 2;
    }
    const bool test_upload = *max_requests == "3";
    const bool test_full = *max_requests == "6";
    const auto origin =
        environment("MIRA_INTEROP_ENDPOINT_ORIGIN").value_or("https://api.openai.com");
    const auto prefix = environment("MIRA_INTEROP_API_PREFIX").value_or("/v1");

    executor::Executor executor;
    executor::ExecutorConfig executor_config;
    executor_config.min_threads = 2;
    executor_config.max_threads = 2;
    executor_config.queue_capacity = 16;
    if (!executor.initialize(executor_config)) {
        std::cerr << "interop Executor initialization failed\n";
        return 1;
    }
    auto secrets = std::make_shared<ProbeSecrets>();
    secrets->set("interop-api-key", *key);
    if (const auto proxy_auth = environment("MIRA_INTEROP_PROXY_AUTH"); proxy_auth.has_value()) {
        secrets->set("interop-proxy-authorization", *proxy_auth);
    }
    auto tls = std::make_shared<MbedTlsChannelFactory>(*ca_file);
    if (!tls->initialize()) {
        std::cerr << "interop CA bundle initialization failed\n";
        static_cast<void>(executor.shutdown(true));
        return 1;
    }
    auto transport = std::make_shared<SocketHttpTransport>(executor, secrets, tls);
    if (!transport->start()) {
        std::cerr << "interop transport start failed\n";
        static_cast<void>(executor.shutdown(true));
        return 1;
    }
    auto profile = std::make_shared<ModelProfile>(
        make_profile(origin, prefix, *model, environment("MIRA_INTEROP_PROXY_URL")));
    if (auto valid = profile->validate(); !valid) {
        std::cerr << "interop profile validation failed: " << valid.error().safe_message << '\n';
        transport->shutdown();
        static_cast<void>(executor.shutdown(true));
        return 1;
    }
    auto artifacts = std::make_shared<ProbeArtifacts>();
    auto remote = std::make_shared<OpenAiRemoteFileStore>(executor, profile, transport, artifacts);
    OpenAiCompatibleProvider provider(profile, transport, artifacts, nullptr, remote);

    if (image_only) {
        auto image_request = make_request(*profile);
        image_request.input.back().content.emplace_back(
            ImagePart{artifacts->image_reference(), ImageDetail::Low, "image/png"});
        auto image = provider.infer(image_request, context(), ProviderInferOptions{});
        if (image.has_value()) {
            std::cout << "{\"case\":\"image\",\"result\":\""
                      << (valid_decision(image_request, image.value()) ? "passed" : "not-passed")
                      << "\",\"completion\":\"" << completion_name(image.value().status)
                      << "\"}\n";
        } else {
            std::cout << "{\"case\":\"image\",\"result\":\"error\",\"domain_code\":\""
                      << model_domain_code_name(
                             static_cast<ModelDomainCode>(image.error().domain_code))
                      << "\"}\n";
        }
        remote->shutdown();
        transport->shutdown();
        static_cast<void>(executor.shutdown(true));
        return 0;
    }

    const auto sync_request = make_request(*profile);
    auto sync = provider.infer(sync_request, context(), ProviderInferOptions{});
    if (!sync || !valid_decision(sync_request, sync.value())) {
        std::cerr << "sync strict-schema interop failed: "
                  << (sync ? "local decision validation" : sync.error().safe_message) << '\n';
        remote->shutdown();
        transport->shutdown();
        static_cast<void>(executor.shutdown(true));
        return 1;
    }

    const auto stream_request = make_request(*profile);
    auto stream = provider.infer(stream_request, context(), ProviderInferOptions{true, false});
    if (!stream || !valid_decision(stream_request, stream.value())) {
        std::cerr << "SSE strict-schema interop failed: "
                  << (stream ? "local decision validation" : stream.error().safe_message) << '\n';
        remote->shutdown();
        transport->shutdown();
        static_cast<void>(executor.shutdown(true));
        return 1;
    }

    bool upload_ok = !test_upload;
    if (test_upload) {
        const auto upload_request = make_request(*profile, artifacts->text_reference());
        auto upload = provider.infer(upload_request, context(), ProviderInferOptions{});
        upload_ok = upload.has_value() && valid_decision(upload_request, upload.value());
        const auto audit = remote->audit();
        upload_ok = upload_ok && audit.size() >= 2 &&
                    audit[audit.size() - 2].kind == RemoteFileAuditKind::Uploaded &&
                    audit.back().kind == RemoteFileAuditKind::Deleted;
    }

    bool tool_ok = !test_full;
    std::string image_result = "not-run";
    std::string image_detail = "not-run";
    bool error_ok = !test_full;
    bool cancellation_ok = !test_full;
    if (test_full) {
        const auto tool_request = make_tool_request(*profile);
        auto tool = provider.infer(tool_request, context(), ProviderInferOptions{});
        tool_ok = tool.has_value() && valid_tool_call(tool_request, tool.value());

        auto image_request = make_request(*profile);
        image_request.input.back().content.emplace_back(
            ImagePart{artifacts->image_reference(), ImageDetail::Low, "image/png"});
        auto image = provider.infer(image_request, context(), ProviderInferOptions{});
        if (image.has_value() && valid_decision(image_request, image.value())) {
            image_result = "passed";
            image_detail = completion_name(image.value().status);
        } else if (has_domain_code(image, ModelDomainCode::InvalidModelRequest) ||
                   has_domain_code(image, ModelDomainCode::CapabilityMismatch)) {
            image_result = "unsupported";
            image_detail = model_domain_code_name(
                static_cast<ModelDomainCode>(image.error().domain_code));
        } else if (image.has_value()) {
            image_result = "failed";
            image_detail = completion_name(image.value().status);
        } else {
            image_result = "failed";
            image_detail = model_domain_code_name(
                static_cast<ModelDomainCode>(image.error().domain_code));
        }

        auto invalid_profile = std::make_shared<ModelProfile>(*profile);
        invalid_profile->id = ModelProfileId::generate();
        invalid_profile->model_selector = "mira-invalid-model-for-error-contract";
        OpenAiCompatibleProvider invalid_provider(invalid_profile, transport, artifacts);
        const auto invalid_request = make_request(*invalid_profile);
        auto invalid = invalid_provider.infer(invalid_request, context(), ProviderInferOptions{});
        error_ok = has_domain_code(invalid, ModelDomainCode::InvalidModelRequest) ||
                   has_domain_code(invalid, ModelDomainCode::ProviderPermissionDenied);

        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto cancellation_timer = executor.submit_delayed_with_handle(
            10, [cancelled] { cancelled->store(true, std::memory_order_release); });
        auto cancellation_context = context();
        cancellation_context.cancellation_requested = [cancelled] {
            return cancelled->load(std::memory_order_acquire);
        };
        auto cancellation_request = make_request(*profile);
        auto cancellation = provider.infer(cancellation_request, cancellation_context,
                                           ProviderInferOptions{true, false});
        if (cancellation_timer.future.valid()) {
            cancellation_timer.future.get();
        }
        cancellation_ok = has_domain_code(cancellation, ModelDomainCode::ModelCancelled);
    }

    const auto profile_digest = profile->profile_digest().to_string();
    const auto resolved = sync.value().resolved_model.value_or("unreported");
    const bool passed = upload_ok && tool_ok && image_result != "failed" && error_ok &&
                        cancellation_ok;
    const auto response_digest = canonical_json_digest(model_response_to_json(sync.value()));
    const auto provider_id_digest = sync.value().provider_response_id.has_value()
                                        ? digest_string(*sync.value().provider_response_id).to_string()
                                        : "unreported";
    std::cout << "{\"status\":\"" << (passed ? "passed" : "failed") << "\",\"profile_digest\":\""
              << profile_digest << "\",\"requested_model\":\"" << profile->model_selector
              << "\",\"resolved_model\":\"" << resolved << "\",\"sync_usage\":\""
              << usage_name(sync.value().usage.quality) << "\",\"stream_usage\":\""
              << usage_name(stream.value().usage.quality) << "\",\"upload_cleanup\":\""
              << (test_upload ? (upload_ok ? "passed" : "failed") : "not-run")
              << "\",\"tool\":\"" << (test_full ? (tool_ok ? "passed" : "failed") : "not-run")
              << "\",\"image\":\"" << image_result << "\",\"image_detail\":\""
              << image_detail << "\",\"error_mapping\":\""
              << (test_full ? (error_ok ? "passed" : "failed") : "not-run")
              << "\",\"cancellation\":\""
              << (test_full ? (cancellation_ok ? "passed" : "failed") : "not-run")
              << "\",\"schema_digest\":\""
              << sync_request.output_contract.canonical_schema_digest.to_string()
              << "\",\"response_digest\":\"" << response_digest.to_string()
              << "\",\"provider_id_digest\":\"" << provider_id_digest
              << "\",\"store\":false}\n";

    remote->shutdown();
    transport->shutdown();
    static_cast<void>(executor.shutdown(true));
    return passed ? 0 : 1;
}
