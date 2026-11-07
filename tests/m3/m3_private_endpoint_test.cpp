#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <kairo/executor.hpp>
#include <mira/adapters/net/socket_transport.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

class Fixture final {
  public:
    Fixture() {
        kairo::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        executor.initialize(config);
        auto secrets = std::make_shared<MapSecretResolver>();
        secrets->set("test-credential", "fixture-key");
        transport = std::make_shared<adapters::net::SocketHttpTransport>(executor, secrets);
    }
    ~Fixture() {
        transport->shutdown();
        (void)executor.shutdown(true);
    }

    Result<ModelResponse> infer(const ModelProfile &profile) {
        OpenAiCompatibleProvider provider(std::make_shared<ModelProfile>(profile), transport,
                                          std::make_shared<NullArtifactSource>());
        ModelRequest request;
        request.contract_version = SchemaVersion{1, 0};
        request.request_id = ModelRequestId::generate();
        request.operation_id = OperationId::generate();
        request.task_id = TaskId::generate();
        request.profile_id = profile.id;
        ModelInputItem item;
        item.role = ModelRole::User;
        item.content.emplace_back(TextPart{"hello"});
        request.input.emplace_back(std::move(item));
        request.output_contract.mode = OutputMode::Text;
        request.data_policy.store = false;
        OperationContext context;
        context.operation = request.operation_id;
        context.started_at = Timestamp::now();
        auto result = provider.infer(request, context, ProviderInferOptions{});
        trace = provider.last_trace();
        return result;
    }

    kairo::Executor executor;
    std::shared_ptr<adapters::net::SocketHttpTransport> transport;
    TransportTrace trace;
};

std::string serve_response(ScriptedHttpServer &server) {
    if (!server.accept_client(std::chrono::milliseconds{2'000})) {
        return {};
    }
    auto request = server.read_request(std::chrono::milliseconds{2'000});
    const std::string body =
        R"({"id":"local-response","status":"completed","model":"test-model","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"local-ok"}]}]})";
    server.write_all("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
    server.close_client();
    return request;
}

bool has_local_reply(const ModelResponse &response) {
    if (response.output.size() != 1) {
        return false;
    }
    const auto *message = std::get_if<MessageOutput>(&response.output.front());
    if (message == nullptr || message->content.size() != 1) {
        return false;
    }
    const auto *text = std::get_if<OutputTextPart>(&message->content.front());
    return text != nullptr && text->text == "local-ok";
}

int default_denial_and_explicit_loopback_consent() {
    Fixture fixture;
    MIRA_CHECK(fixture.transport->start());
    // Numeric addresses exercise DNS-result policy without external services.
    for (const std::string host :
         {"127.0.0.1", "10.0.0.1", "100.64.0.1", "169.254.169.254", "[::1]"}) {
        auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "http://" + host + ":9");
        auto denied = fixture.infer(profile);
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
        MIRA_CHECK(!fixture.trace.write_started);
        MIRA_CHECK(classify_provider_stage(fixture.trace, denied.error()) ==
                   RequestStage::PreWriteFailure);
    }

    ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1,
                                "http://127.0.0.1:" + std::to_string(server.port()));
    profile.allow_private_endpoint = true;
    auto server_task = fixture.executor.submit_auto([&server] { return serve_response(server); });
    auto response = fixture.infer(profile);
    const auto received = server_task.get();
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(response.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(has_local_reply(response.value()));
    MIRA_CHECK(fixture.trace.write_completed);
    MIRA_CHECK(received.find("POST /v1/responses HTTP/1.1") == 0);
    MIRA_CHECK(received.find("Authorization: Bearer fixture-key") != std::string::npos);
    return 0;
}

int proxy_and_destination_consent_are_independent() {
    Fixture fixture;
    MIRA_CHECK(fixture.transport->start());
    ScriptedHttpServer proxy;
    MIRA_CHECK(proxy.valid());
    auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "http://127.0.0.1:9");
    profile.proxy = ModelProxyConfig{
        "http://127.0.0.1:" + std::to_string(proxy.port()), std::nullopt, true, {"127.0.0.1"}};
    // Proxy consent alone cannot authorize the private destination.
    auto denied = fixture.infer(profile);
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(denied.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
    MIRA_CHECK(!fixture.trace.write_started);

    // Destination consent alone cannot authorize the private proxy.
    profile.allow_private_endpoint = true;
    profile.proxy->allow_private_endpoint = false;
    denied = fixture.infer(profile);
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(denied.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
    MIRA_CHECK(!fixture.trace.write_started);

    profile.proxy->allow_private_endpoint = true;
    auto proxy_task = fixture.executor.submit_auto([&proxy] { return serve_response(proxy); });
    auto response = fixture.infer(profile);
    const auto received = proxy_task.get();
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(has_local_reply(response.value()));
    MIRA_CHECK(received.find("POST http://127.0.0.1:9/v1/responses HTTP/1.1") == 0);
    return 0;
}

int consent_does_not_bypass_redirect_allowlist() {
    Fixture fixture;
    MIRA_CHECK(fixture.transport->start());
    ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1,
                                "http://127.0.0.1:" + std::to_string(server.port()));
    profile.allow_private_endpoint = true;
    auto server_task = fixture.executor.submit_auto([&server] {
        if (!server.accept_client(std::chrono::milliseconds{2'000})) {
            return false;
        }
        (void)server.read_request(std::chrono::milliseconds{2'000});
        server.write_all("HTTP/1.1 307 Temporary Redirect\r\nLocation: http://localhost:9/blocked"
                         "\r\nContent-Length: 0\r\n\r\n");
        server.close_client();
        return true;
    });
    auto response = fixture.infer(profile);
    const bool served = server_task.get();
    MIRA_CHECK(served);
    MIRA_CHECK(!response.has_value());
    MIRA_CHECK(response.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
    return 0;
}

int file_operations_share_endpoint_consent() {
    Fixture fixture;
    MIRA_CHECK(fixture.transport->start());
    ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1,
                                "http://127.0.0.1:" + std::to_string(server.port()));
    class Source final : public IArtifactSource {
      public:
        Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
            return std::vector<std::byte>{std::byte{1}};
        }
    };
    auto source = std::make_shared<Source>();
    ArtifactRef reference;
    reference.id = ArtifactId::generate();
    reference.digest = digest_bytes(std::vector<std::byte>{std::byte{1}});
    reference.byte_size = 1;
    reference.media_type = "text/plain";
    ModelDataPolicy policy;
    policy.allow_uploads = true;
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();

    OpenAiRemoteFileStore denied_store(fixture.executor, std::make_shared<ModelProfile>(profile),
                                       fixture.transport, source);
    auto denied = denied_store.upload(reference, "local.txt", policy, context);
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(denied.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
    denied_store.shutdown();

    profile.allow_private_endpoint = true;
    OpenAiRemoteFileStore store(fixture.executor, std::make_shared<ModelProfile>(profile),
                                fixture.transport, source);
    auto server_task = fixture.executor.submit_auto([&server] {
        std::vector<std::string> received;
        for (int exchange = 0; exchange < 2; ++exchange) {
            if (!server.accept_client(std::chrono::milliseconds{2'000})) {
                break;
            }
            received.push_back(server.read_request(std::chrono::milliseconds{2'000}));
            const std::string body = R"({"id":"file_local","deleted":true})";
            server.write_all("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\n\r\n" + body);
            server.close_client();
        }
        return received;
    });
    auto uploaded = store.upload(reference, "local.txt", policy, context);
    Result<void> retired = uploaded ? store.retire(uploaded.value(), std::chrono::seconds{0})
                                    : Result<void>{uploaded.error()};
    const auto received = server_task.get();
    store.shutdown();
    MIRA_CHECK(uploaded.has_value());
    MIRA_CHECK(retired.has_value());
    MIRA_CHECK(received.size() == 2);
    MIRA_CHECK(received[0].find("POST /v1/files HTTP/1.1") == 0);
    MIRA_CHECK(received[1].find("DELETE /v1/files/file_local HTTP/1.1") == 0);
    return 0;
}

} // namespace

int main() {
    if (const int status = default_denial_and_explicit_loopback_consent(); status != 0) {
        return status;
    }
    if (const int status = proxy_and_destination_consent_are_independent(); status != 0) {
        return status;
    }
    if (const int status = consent_does_not_bypass_redirect_allowlist(); status != 0) {
        return status;
    }
    return file_operations_share_endpoint_consent();
}
