// Explicit opt-in paid interop probe. Inputs/credentials arrive through private
// child environment; no credential or wire body is written to output.
#include <cstdlib>
#include <executor/executor.hpp>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mira/adapters/net/openssl_tls.hpp>
#include <mira/adapters/net/socket_transport.hpp>
#include <mira/model_digest.hpp>
#include <mira/model_provider.hpp>

using namespace mira;
namespace {
std::string environment(const char *name) {
#ifdef _WIN32
    char *raw = nullptr;
    std::size_t count = 0;
    if (_dupenv_s(&raw, &count, name) || !raw)
        return {};
    std::string result(raw);
    std::free(raw);
    return result;
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}
class Secrets final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &) override {
        return environment("MIRA_MESSAGES_KEY");
    }
};
class Image final : public IArtifactSource {
  public:
    ArtifactRef reference;
    std::vector<std::byte> bytes;
    Result<std::vector<std::byte>> fetch(const ArtifactRef &ref) override {
        if (ref.id != reference.id)
            return make_model_error(ModelDomainCode::InvalidModelRequest, "unknown fixture");
        return bytes;
    }
};
} // namespace
int main() {
    if (environment("MIRA_MESSAGES_PROBE") != "1" || environment("MIRA_MESSAGES_KEY").empty())
        return 2;
    auto artifacts = std::make_shared<Image>();
    std::ifstream input(environment("MIRA_MESSAGES_IMAGE"), std::ios::binary);
    if (input) {
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length <= 0 || length > 1024 * 1024)
            return 2;
        input.seekg(0);
        artifacts->bytes.resize(static_cast<std::size_t>(length));
        if (!input.read(reinterpret_cast<char *>(artifacts->bytes.data()), length))
            return 2;
        artifacts->reference = {ArtifactId::generate(), digest_bytes(artifacts->bytes),
                                artifacts->bytes.size(), "image/png", Sensitivity::Public};
    }
    auto profile = std::make_shared<ModelProfile>();
    profile->id = ModelProfileId::generate();
    profile->display_name = "Messages controlled interop";
    profile->dialect = ProtocolDialect::AnthropicMessagesV1;
    profile->endpoint_origin = environment("MIRA_MESSAGES_ORIGIN");
    profile->api_prefix = environment("MIRA_MESSAGES_PREFIX");
    profile->model_selector = environment("MIRA_MESSAGES_MODEL");
    profile->credential = SecretRef{"controlled-key"};
    profile->capabilities.text = {true, CapabilityEvidence::FixtureVerified, "M3-21"};
    profile->capabilities.image_input = {true, CapabilityEvidence::Configured, "interop test"};
    profile->capabilities.sse = {true, CapabilityEvidence::FixtureVerified, "M3-21"};
    profile->deadlines.total = std::chrono::seconds{90};
    if (!profile->validate())
        return 2;
    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    config.queue_capacity = 8;
    if (!executor.initialize(config))
        return 2;
    auto tls = std::make_shared<adapters::net::OpenSslTlsChannelFactory>();
    if (!tls->initialize()) {
        (void)executor.shutdown(true);
        return 2;
    }
    auto transport = std::make_shared<adapters::net::SocketHttpTransport>(
        executor, std::make_shared<Secrets>(), tls);
    if (!transport->start()) {
        (void)executor.shutdown(true);
        return 2;
    }
    OpenAiCompatibleProvider provider(profile, transport, artifacts);
    auto future = executor.submit_auto([&] {
        ModelRequest request;
        request.request_id = ModelRequestId::generate();
        request.operation_id = OperationId::generate();
        request.profile_id = profile->id;
        request.output_contract.mode = OutputMode::Text;
        request.generation.max_output_tokens = 2048;
        request.data_policy.store = false;
        ModelInputItem user;
        user.role = ModelRole::User;
        user.content.emplace_back(
            TextPart{environment("MIRA_MESSAGES_PROMPT"), Sensitivity::Public});
        if (!artifacts->bytes.empty())
            user.content.emplace_back(
                ImagePart{artifacts->reference, ImageDetail::Auto, "image/png"});
        request.input.push_back(std::move(user));
        OperationContext context;
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
        ProviderInferOptions options;
        options.stream = true;
        std::size_t previews = 0;
        options.preview_sink = [&](const auto &, const auto &preview) {
            if (!preview.text.empty())
                ++previews;
        };
        auto response = provider.infer(request, context, options);
        JsonValue result{
            JsonValue::Object{{"image_bytes", artifacts->bytes.size()},
                              {"previews", previews},
                              {"terminal_seen", provider.last_sse_stats().terminal_seen}}};
        if (!response) {
            result.set("ok", false);
            result.set("error_domain", response.error().domain);
            result.set("error_code", response.error().domain_code);
            result.set("safe_message", response.error().safe_message);
        } else {
            std::string text;
            for (const auto &out : response.value().output)
                if (const auto *message = std::get_if<MessageOutput>(&out))
                    for (const auto &part : message->content)
                        if (const auto *value = std::get_if<OutputTextPart>(&part))
                            text += value->text;
            result.set("ok", response.value().status == ModelCompletionStatus::Completed);
            result.set("answer", text);
            if (response.value().usage.input_tokens)
                result.set("input_tokens",
                           static_cast<std::int64_t>(*response.value().usage.input_tokens));
        }
        return result;
    });
    const auto result = future.get();
    transport->shutdown();
    (void)executor.shutdown(true);
    std::cout << to_json_string(result) << '\n';
    return result.find("ok")->as_boolean().value_or(false) ? 0 : 1;
}
