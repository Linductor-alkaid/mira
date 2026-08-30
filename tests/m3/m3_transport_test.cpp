#include "support/m3_support.hpp"

#include <executor/executor.hpp>
#include "support/test.hpp"

#include "socket_transport.hpp"
#include <mira/model_contracts.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace mira;
using namespace mira::adapters::net;
using namespace mira::testing;

class ExecutorFixture final {
  public:
    ExecutorFixture() {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        executor_.initialize(config);
    }
    ~ExecutorFixture() { (void)executor_.shutdown(true); }
    executor::Executor &executor() { return executor_; }

  private:
    executor::Executor executor_;
};

[[nodiscard]] OperationContext make_context(std::function<bool()> cancelled = nullptr) {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.cancellation_requested = std::move(cancelled);
    return context;
}

[[nodiscard]] TransportLimits loopback_limits() {
    TransportLimits limits;
    limits.deadlines.connect = std::chrono::milliseconds{2'000};
    limits.deadlines.total = std::chrono::milliseconds{5'000};
    limits.allow_private_endpoints = true; // Tests talk to 127.0.0.1.
    return limits;
}

int happy_path_sync_and_sse() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-test-123");
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    testing::ScriptedHttpServer server;
    MIRA_CHECK(server.valid());

    // The scripted server is driven by an Executor-managed task; the client
    // exchange runs on the transport worker (M3-05 evidence).
    HttpRequest request;
    request.method = "POST";
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/responses";
    request.body = R"({"model":"m"})";
    request.headers.emplace_back("Content-Type", "application/json");
    request.authorization = SecretRef{"test-credential"};
    std::string received_body;
    auto server_task = fixture.executor().submit_auto([&server] {
        if (!server.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        (void)server.read_request(std::chrono::milliseconds{2'000});
        const std::string body = R"({"status":"completed","id":"x"})";
        server.write_all("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body);
        server.close_client();
    });
    TransportTrace trace;
    auto response = transport.execute(request, loopback_limits(), make_context(),
                                      [&](std::string_view chunk) {
                                          received_body.append(chunk.data(), chunk.size());
                                      },
                                      trace);
    server_task.get();
    const auto &body = received_body;
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(response.value().status == 200);
    MIRA_CHECK(body == R"({"status":"completed","id":"x"})");
    MIRA_CHECK(response.value().body_bytes == body.size());
    MIRA_CHECK(response.value().headers[0].first == "content-type");

    transport.shutdown();
    MIRA_CHECK(!transport.running());
    return 0;
}

int secrets_arrive_at_socket_only() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-secret-abcdef");
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    testing::ScriptedHttpServer server;
    MIRA_CHECK(server.valid());

    std::string seen_request;
    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
    request.authorization = SecretRef{"test-credential"};
    TransportTrace trace;
    auto server_task = fixture.executor().submit_auto([&server, &seen_request] {
        if (!server.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        seen_request = server.read_request(std::chrono::milliseconds{2'000});
        server.write_all("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
        server.close_client();
    });
    auto response =
        transport.execute(request, loopback_limits(), make_context(), nullptr, trace);
    server_task.get();
    MIRA_CHECK(response.has_value());
    // The credential reached the socket boundary...
    MIRA_CHECK(seen_request.find("Authorization: Bearer sk-secret-abcdef") != std::string::npos);
    // ...and never appears in any result structure or trace.
    MIRA_CHECK(response.value().headers.empty() ||
               response.value().headers[0].second.find("sk-secret") == std::string::npos);
    transport.shutdown();
    return 0;
}

int ssrf_and_allowlist_fail_closed() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    HttpRequest request;
    request.url = "http://127.0.0.1:9/x"; // Loopback without opt-in.
    TransportTrace trace;
    auto denied = transport.execute(request, TransportLimits{}, make_context(), nullptr, trace);
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(denied.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));

    // Private ranges beyond loopback are denied too.
    request.url = "http://10.0.0.1/x";
    auto private_denied = transport.execute(request, TransportLimits{}, make_context(), nullptr, trace);
    MIRA_CHECK(!private_denied.has_value());

    // Non-http schemes and credentials-in-URL are policy failures.
    request.url = "file:///etc/passwd";
    auto scheme = transport.execute(request, TransportLimits{}, make_context(), nullptr, trace);
    MIRA_CHECK(!scheme.has_value());
    request.url = "http://user:pass@127.0.0.1:1/x";
    auto userinfo = transport.execute(request, TransportLimits{}, make_context(), nullptr, trace);
    MIRA_CHECK(!userinfo.has_value());

    // Host allowlists are enforced even with private endpoints allowed.
    testing::ScriptedHttpServer server;
    TransportLimits limits = loopback_limits();
    limits.allowed_hosts = {"api.example.test"};
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
    auto not_allowed = transport.execute(request, limits, make_context(), nullptr, trace);
    MIRA_CHECK(!not_allowed.has_value());
    MIRA_CHECK(not_allowed.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));

    // Unresolvable hosts surface as transport failures.
    request.url = "http://this-host-does-not-exist.invalid./x";
    auto dns = transport.execute(request, TransportLimits{}, make_context(), nullptr, trace);
    MIRA_CHECK(!dns.has_value());
    MIRA_CHECK(dns.error().domain_code == static_cast<std::int32_t>(ModelDomainCode::TransportFailed));
    transport.shutdown();
    return 0;
}

int redirects_revalidate_policy_and_drop_credentials() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-redirect-secret");
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    testing::ScriptedHttpServer server;
    testing::ScriptedHttpServer other;
    MIRA_CHECK(server.valid() && other.valid());

    std::string first_request;
    std::string second_request;
    HttpRequest request;
    request.method = "POST";
    request.body = "data";
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/start";
    request.authorization = SecretRef{"test-credential"};
    auto first_task = fixture.executor().submit_auto([&server, &other, &first_request] {
        if (!server.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        first_request = server.read_request(std::chrono::milliseconds{2'000});
        // Redirect to a different loopback origin.
        server.write_all("HTTP/1.1 307 Temporary Redirect\r\nLocation: http://127.0.0.1:" +
                         std::to_string(other.port()) + "/final\r\nContent-Length: 0\r\n\r\n");
        server.close_client();
    });
    auto second_task = fixture.executor().submit_auto([&other, &second_request] {
        if (!other.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        second_request = other.read_request(std::chrono::milliseconds{2'000});
        other.write_all("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        other.close_client();
    });
    TransportTrace trace;
    auto response = transport.execute(request, loopback_limits(), make_context(), nullptr, trace);
    first_task.get();
    second_task.get();
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(response.value().status == 200);
    MIRA_CHECK(response.value().redirects_followed == 1);
    // Same-origin hop keeps credentials.
    MIRA_CHECK(first_request.find("Authorization: Bearer sk-redirect-secret") != std::string::npos);
    // Cross-origin redirect drops them.
    MIRA_CHECK(second_request.find("Authorization") == std::string::npos);
    MIRA_CHECK(second_request.find("POST /final") == 0);

    // Redirect chains beyond the limit fail closed.
    testing::ScriptedHttpServer loop;
    auto loop_task = fixture.executor().submit_auto([&loop] {
        for (int hop = 0; hop < 5; ++hop) {
            if (!loop.accept_client(std::chrono::milliseconds{2'000})) {
                return;
            }
            (void)loop.read_request(std::chrono::milliseconds{1'000});
            loop.write_all("HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n");
            loop.close_client();
        }
    });
    request.url = "http://127.0.0.1:" + std::to_string(loop.port()) + "/start";
    auto chain = transport.execute(request, loopback_limits(), make_context(), nullptr, trace);
    loop_task.get();
    MIRA_CHECK(!chain.has_value());
    MIRA_CHECK(chain.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
    transport.shutdown();
    return 0;
}

int cancellation_releases_socket_waits() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    testing::ScriptedHttpServer server;
    MIRA_CHECK(server.valid());

    std::atomic<bool> cancel{false};
    auto context = make_context([&cancel] { return cancel.load(); });

    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/stream";
    std::size_t events = 0;
    auto server_task = fixture.executor().submit_auto([&server, &cancel] {
        if (!server.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        (void)server.read_request(std::chrono::milliseconds{2'000});
        // Start an SSE response, deliver one event, then stall.
        server.write_all("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                         "event: response.created\ndata: {}\n\n");
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        cancel.store(true);
        // Keep the connection open and silent so the client's only exit is
        // the cancellation probe, not an EOF.
        std::this_thread::sleep_for(std::chrono::milliseconds{2'000});
        server.close_client();
    });
    TransportTrace trace;
    const auto started = std::chrono::steady_clock::now();
    auto response = transport.execute(
        request, loopback_limits(), context, [&](std::string_view) { ++events; }, trace);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server_task.get();
    MIRA_CHECK(!response.has_value());
    MIRA_CHECK(response.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ModelCancelled));
    MIRA_CHECK(elapsed < std::chrono::seconds{4}); // Bounded by poll slices, not deadlines.
    transport.shutdown();
    return 0;
}

int shutdown_settles_pending_exchanges() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    testing::ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    std::atomic<bool> stop{false};
    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/hang";
    TransportTrace trace;
    TransportLimits limits;
    limits.deadlines.total = std::chrono::milliseconds{10'000};
    limits.allow_private_endpoints = true;

    auto server_task = fixture.executor().submit_auto([&server, &stop] {
        if (!server.accept_client(std::chrono::milliseconds{5'000})) {
            return;
        }
        (void)server.read_request(std::chrono::milliseconds{2'000});
        // Never answer; the shutdown path must still settle the caller.
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        server.close_client();
    });
    // The exchange itself runs as an Executor-managed finite task.
    auto pending = fixture.executor().submit_auto(
        [&transport, &request, &limits, &trace] {
            return transport.execute(request, limits, make_context(), nullptr, trace);
        });
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    transport.shutdown();
    auto settled = pending.get();
    stop.store(true);
    server_task.get();
    // The exchange either completed or failed; it never hangs past shutdown.
    MIRA_CHECK(!settled.has_value() || settled.value().status > 0);
    MIRA_CHECK(!transport.running());

    // Post-shutdown admissions are rejected deterministically.
    auto rejected = transport.execute(request, limits, make_context(), nullptr, trace);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ModelResourceExhausted));
    return 0;
}

int chunked_and_oversize_bodies() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    {
        testing::ScriptedHttpServer server;
        MIRA_CHECK(server.valid());
        auto server_task = fixture.executor().submit_auto([&server] {
            if (!server.accept_client(std::chrono::milliseconds{5'000})) {
                return;
            }
            (void)server.read_request(std::chrono::milliseconds{2'000});
            server.write_all(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
            server.close_client();
        });
        HttpRequest request;
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
        std::string body;
        TransportTrace trace;
        auto response = transport.execute(
            request, loopback_limits(), make_context(),
            [&](std::string_view chunk) { body.append(chunk.data(), chunk.size()); }, trace);
        server_task.get();
        MIRA_CHECK(response.has_value());
        MIRA_CHECK(body == "hello world");
    }

    {
        testing::ScriptedHttpServer server;
        MIRA_CHECK(server.valid());
        auto server_task = fixture.executor().submit_auto([&server] {
            if (!server.accept_client(std::chrono::milliseconds{5'000})) {
                return;
            }
            (void)server.read_request(std::chrono::milliseconds{2'000});
            server.write_all("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n" +
                             std::string(4096, 'z'));
            server.close_client();
        });
        HttpRequest request;
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
        TransportLimits limits = loopback_limits();
        limits.max_response_bytes = 1024;
        TransportTrace trace;
        auto response = transport.execute(request, limits, make_context(), nullptr, trace);
        server_task.get();
        MIRA_CHECK(!response.has_value());
        MIRA_CHECK(response.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::ResponseTooLarge));
    }
    transport.shutdown();
    return 0;
}

int connect_deadline_enforced() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets);
    MIRA_CHECK(transport.start());

    HttpRequest request;
    // A firewalled address: connection attempts time out rather than refuse.
    request.url = "http://10.255.255.1/x";
    TransportLimits limits;
    limits.deadlines.connect = std::chrono::milliseconds{400};
    limits.deadlines.total = std::chrono::milliseconds{2'000};
    limits.allow_private_endpoints = true;
    TransportTrace trace;
    const auto started = std::chrono::steady_clock::now();
    auto response = transport.execute(request, limits, make_context(), nullptr, trace);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    MIRA_CHECK(!response.has_value());
    MIRA_CHECK(response.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ModelDeadlineExceeded));
    MIRA_CHECK(elapsed < std::chrono::seconds{2});
    MIRA_CHECK(!trace.write_started);
    transport.shutdown();
    return 0;
}

int https_without_tls_fails_closed() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(fixture.executor(), secrets, nullptr);
    MIRA_CHECK(transport.start());
    testing::ScriptedHttpServer server;
    MIRA_CHECK(server.valid());

    HttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/";
    TransportTrace trace;
    auto response = transport.execute(request, loopback_limits(), make_context(), nullptr, trace);
    MIRA_CHECK(!response.has_value());
    MIRA_CHECK(response.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));
    MIRA_CHECK(!trace.write_started);
    transport.shutdown();
    return 0;
}

} // namespace

int main() {
    if (const int status = happy_path_sync_and_sse(); status != 0) {
        return status;
    }
    if (const int status = secrets_arrive_at_socket_only(); status != 0) {
        return status;
    }
    if (const int status = ssrf_and_allowlist_fail_closed(); status != 0) {
        return status;
    }
    if (const int status = redirects_revalidate_policy_and_drop_credentials(); status != 0) {
        return status;
    }
    if (const int status = cancellation_releases_socket_waits(); status != 0) {
        return status;
    }
    if (const int status = shutdown_settles_pending_exchanges(); status != 0) {
        return status;
    }
    if (const int status = chunked_and_oversize_bodies(); status != 0) {
        return status;
    }
    if (const int status = connect_deadline_enforced(); status != 0) {
        return status;
    }
    if (const int status = https_without_tls_fails_closed(); status != 0) {
        return status;
    }
    return 0;
}
