#include "mbedtls_tls.hpp"
#include "socket_transport.hpp"

#include <mira/model_digest.hpp>
#include <mira/model_provider.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_upload.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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
    ProbeArtifacts() : payload_("Mira controlled interop fixture. No user data.\n") {
        reference_.id = ArtifactId::generate();
        reference_.digest = digest_string(payload_);
        reference_.byte_size = payload_.size();
        reference_.media_type = "text/plain";
        reference_.sensitivity = Sensitivity::Public;
    }

    Result<std::vector<std::byte>> fetch(const ArtifactRef &reference) override {
        if (reference.id != reference_.id) {
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "interop artifact reference is unknown");
        }
        std::vector<std::byte> bytes;
        bytes.reserve(payload_.size());
        for (const char value : payload_) {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

    [[nodiscard]] const ArtifactRef &reference() const noexcept { return reference_; }

  private:
    std::string payload_;
    ArtifactRef reference_;
};

[[nodiscard]] ModelProfile make_profile(const std::string &origin, const std::string &prefix,
                                        const std::string &model,
                                        const std::optional<std::string> &proxy_url) {
    ModelProfile profile;
    profile.id = ModelProfileId::generate();
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
        R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"],"additionalProperties":false})");
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

} // namespace

int main() {
    const auto key = environment("MIRA_INTEROP_API_KEY");
    const auto model = environment("MIRA_INTEROP_MODEL");
    const auto ca_file = environment("MIRA_INTEROP_CA_FILE");
    const auto max_requests = environment("MIRA_INTEROP_MAX_REQUESTS");
    if (!key.has_value() || !model.has_value() || !ca_file.has_value() ||
        !max_requests.has_value() || (*max_requests != "2" && *max_requests != "3")) {
        std::cerr << "refusing network interop: set MIRA_INTEROP_API_KEY, MIRA_INTEROP_MODEL, "
                     "MIRA_INTEROP_CA_FILE and MIRA_INTEROP_MAX_REQUESTS=2 (or 3 with upload)\n";
        return 2;
    }
    const bool test_upload = *max_requests == "3";
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
        const auto upload_request = make_request(*profile, artifacts->reference());
        auto upload = provider.infer(upload_request, context(), ProviderInferOptions{});
        upload_ok = upload.has_value() && valid_decision(upload_request, upload.value());
        const auto audit = remote->audit();
        upload_ok = upload_ok && audit.size() >= 2 &&
                    audit[audit.size() - 2].kind == RemoteFileAuditKind::Uploaded &&
                    audit.back().kind == RemoteFileAuditKind::Deleted;
    }

    const auto profile_digest = profile->profile_digest().to_string();
    const auto resolved = sync.value().resolved_model.value_or("unreported");
    std::cout << "{\"status\":\"" << (upload_ok ? "passed" : "failed") << "\",\"profile_digest\":\""
              << profile_digest << "\",\"requested_model\":\"" << profile->model_selector
              << "\",\"resolved_model\":\"" << resolved << "\",\"sync_usage\":\""
              << usage_name(sync.value().usage.quality) << "\",\"stream_usage\":\""
              << usage_name(stream.value().usage.quality) << "\",\"upload_cleanup\":\""
              << (test_upload ? (upload_ok ? "passed" : "failed") : "not-run") << "\"}\n";

    remote->shutdown();
    transport->shutdown();
    static_cast<void>(executor.shutdown(true));
    return upload_ok ? 0 : 1;
}
