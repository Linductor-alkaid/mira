#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/adapters/net/socket_transport.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <future>
#include <sstream>

namespace {
using namespace mira;
using namespace mira::adapters::net;
using namespace mira::testing;
using namespace std::chrono_literals;

class Fixture final {
  public:
    Fixture() : transport(executor, std::make_shared<NullSecretResolver>()) {
        kairo::ExecutorConfig config;
        config.min_threads = 3;
        config.max_threads = 3;
        config.queue_capacity = 32;
        ready = executor.initialize(config) && transport.start();
    }
    ~Fixture() {
        transport.shutdown();
        (void)executor.shutdown(true);
    }
    kairo::Executor executor;
    SocketHttpTransport transport;
    bool ready = false;
};

TransportLimits limits() {
    TransportLimits result;
    result.allow_private_endpoints = true;
    result.deadlines.first_byte = 2s;
    result.deadlines.idle_read = 2s;
    result.deadlines.total = 5s;
    return result;
}

HttpRequest stream_request(const ScriptedHttpServer &server) {
    HttpRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/_events";
    request.headers.emplace_back("Accept", "text/event-stream");
    return request;
}

std::vector<std::string> connection_values(const std::string &request) {
    std::vector<std::string> values;
    std::istringstream input(request);
    std::string line;
    (void)std::getline(input, line); // Request line.
    while (std::getline(input, line) && line != "\r" && !line.empty()) {
        if (line.back() == '\r')
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        auto name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name == "connection")
            values.push_back(line.substr(colon + 2));
    }
    return values;
}

// The peer stays open until the client releases its Exchange. A bounded
// socket wait tests ownership without sleeping or closing the peer first.
bool peer_closed(const ScriptedHttpServer &server) {
    const auto handle = server.native_client();
    if (handle == MIRA_TEST_INVALID_SOCKET)
        return false;
    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(handle, &readers);
    timeval timeout{};
    timeout.tv_sec = 3;
    if (::select(static_cast<int>(handle + 1), &readers, nullptr, nullptr, &timeout) <= 0)
        return false;
    char byte = 0;
    return ::recv(handle, &byte, 1, 0) <= 0;
}

std::string chunk(std::string_view payload) {
    std::ostringstream encoded;
    encoded << std::hex << payload.size() << "\r\n" << payload << "\r\n";
    return encoded.str();
}

constexpr auto kHeaders = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n";

struct ServerResult final {
    std::string request;
    bool first_event_delivered = false;
    bool client_closed = false;
};

int sensitive_stream_and_default_compatibility() {
    Fixture fixture;
    MIRA_CHECK(fixture.ready);
    for (bool enabled : {false, true}) {
        ScriptedHttpServer server;
        MIRA_CHECK(server.valid());
        auto request = stream_request(server);
        MIRA_CHECK(!request.keep_alive); // Existing request default is unchanged.
        request.keep_alive = enabled;
        std::promise<void> first_event;
        auto observed = first_event.get_future();
        auto serving = fixture.executor.submit_auto([&] {
            ServerResult result;
            if (!server.accept_client(2s))
                return result;
            result.request = server.read_request(2s, false);
            server.write_all(kHeaders);
            // Model the reported endpoint's policy; not a claim that every
            // Node HTTP server suppresses SSE when the request says close.
            if (connection_values(result.request) == std::vector<std::string>{"keep-alive"}) {
                server.write_all(chunk("id: 1\ndata: one\n") + chunk("\n"));
                result.first_event_delivered = observed.wait_for(2s) == std::future_status::ready;
                if (result.first_event_delivered)
                    server.write_all(chunk("id: 2\ndata: two\n\n") + "0\r\n\r\n");
            }
            result.client_closed = peer_closed(server);
            server.close_client();
            return result;
        });
        auto bound = limits();
        if (!enabled)
            bound.deadlines.idle_read = 150ms;
        SseFramingParser parser;
        std::vector<SseMessage> events;
        std::optional<Error> parse_error;
        TransportTrace trace;
        const auto response = fixture.transport.execute(
            request, bound, OperationContext{},
            [&](std::string_view bytes) {
                auto parsed = parser.feed(bytes);
                if (!parsed) {
                    parse_error = parsed.error();
                    return;
                }
                for (auto &event : parsed.value()) {
                    events.push_back(std::move(event));
                    if (events.size() == 1)
                        first_event.set_value();
                }
            },
            trace);
        const auto served = serving.get();
        MIRA_CHECK(served.client_closed);
        MIRA_CHECK(trace.headers_received);
        MIRA_CHECK(!parse_error);
        const auto values = connection_values(served.request);
        MIRA_CHECK(values == std::vector<std::string>{enabled ? "keep-alive" : "close"});
        if (enabled) {
            MIRA_CHECK(response);
            MIRA_CHECK(served.first_event_delivered); // Incremental delivery before EOF.
            MIRA_CHECK(events.size() == 2);
            MIRA_CHECK(events[0].data == "one" && events[1].data == "two");
            MIRA_CHECK(events[0].id == "1" && events[1].id == "2");
            MIRA_CHECK(parser.finish());
        } else {
            MIRA_CHECK(!response);
            MIRA_CHECK(response.error().domain_code ==
                       static_cast<std::int32_t>(ModelDomainCode::ModelDeadlineExceeded));
            MIRA_CHECK(events.empty());
        }
    }
    return 0;
}

enum class Exit { Cancel, Idle, Shutdown, TooLarge, Truncated };

int stream_exits_release_exchange() {
    for (const auto exit :
         {Exit::Cancel, Exit::Idle, Exit::Shutdown, Exit::TooLarge, Exit::Truncated}) {
        Fixture fixture;
        MIRA_CHECK(fixture.ready);
        ScriptedHttpServer server;
        MIRA_CHECK(server.valid());
        auto request = stream_request(server);
        request.keep_alive = true;
        auto bound = limits();
        if (exit == Exit::Idle)
            bound.deadlines.idle_read = 150ms;
        if (exit == Exit::TooLarge)
            bound.max_response_bytes = 1;
        std::atomic<bool> cancel{false};
        OperationContext context;
        context.cancellation_requested = [&] { return cancel.load(); };
        std::promise<void> delivered;
        auto first = delivered.get_future();
        auto serving = fixture.executor.submit_auto([&] {
            ServerResult result;
            if (!server.accept_client(2s))
                return result;
            result.request = server.read_request(2s, false);
            if (exit == Exit::Truncated) {
                server.write_all(std::string(kHeaders) + "20\r\ndata: partial");
                server.close_client();
                return result;
            }
            const std::string body = exit == Exit::Idle ? "" : chunk("data: first\n\n");
            server.write_all(std::string(kHeaders) + body);
            result.client_closed = peer_closed(server);
            server.close_client();
            return result;
        });
        TransportTrace trace;
        bool received = false;
        auto exchange = fixture.executor.submit_auto([&] {
            return fixture.transport.execute(
                request, bound, context,
                [&](std::string_view) {
                    if (!received) {
                        received = true;
                        delivered.set_value();
                    }
                    if (exit == Exit::Cancel)
                        cancel.store(true);
                },
                trace);
        });
        bool shutdown_ready = true;
        if (exit == Exit::Shutdown) {
            shutdown_ready = first.wait_for(2s) == std::future_status::ready;
            // External owner stops producers, cancels active contexts, then
            // joins the transport workers; shutdown is not a new cancel source.
            cancel.store(true);
            fixture.transport.shutdown();
        }
        const auto response = exchange.get();
        const auto served = serving.get();
        MIRA_CHECK(shutdown_ready);
        MIRA_CHECK(!response);
        MIRA_CHECK(trace.headers_received);
        MIRA_CHECK(exit == Exit::Truncated || served.client_closed);
        ModelDomainCode expected = ModelDomainCode::ModelCancelled;
        if (exit == Exit::Idle)
            expected = ModelDomainCode::ModelDeadlineExceeded;
        else if (exit == Exit::TooLarge)
            expected = ModelDomainCode::ResponseTooLarge;
        else if (exit == Exit::Truncated)
            expected = ModelDomainCode::ProtocolViolation;
        if (response.error().domain_code != static_cast<std::int32_t>(expected))
            std::cerr << "stream exit " << static_cast<int>(exit) << ": expected "
                      << static_cast<int>(expected) << ", got " << response.error().domain_code
                      << " (" << response.error().safe_message << ")\n";
        MIRA_CHECK(response.error().domain_code == static_cast<std::int32_t>(expected));
        MIRA_CHECK(received == (exit == Exit::Cancel || exit == Exit::Shutdown));
        MIRA_CHECK(connection_values(served.request) == std::vector<std::string>{"keep-alive"});
        if (exit == Exit::Shutdown) {
            auto rejected = fixture.transport.execute(request, bound, context, nullptr, trace);
            MIRA_CHECK(!rejected);
            MIRA_CHECK(rejected.error().domain_code ==
                       static_cast<std::int32_t>(ModelDomainCode::ModelResourceExhausted));
        }
    }
    return 0;
}

int reserved_headers_redirects_and_proxy() {
    Fixture fixture;
    MIRA_CHECK(fixture.ready);
    ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    auto request = stream_request(server);
    for (bool enabled : {false, true}) {
        request.keep_alive = enabled;
        for (const auto *name : {"Connection", "connection", "cOnNeCtIoN"}) {
            request.headers = {{name, enabled ? "close" : "keep-alive"}};
            TransportTrace trace;
            const auto rejected = fixture.transport.execute(request, limits(), {}, nullptr, trace);
            MIRA_CHECK(!rejected);
            MIRA_CHECK(rejected.error().domain_code ==
                       static_cast<std::int32_t>(ModelDomainCode::EndpointPolicyDenied));
            MIRA_CHECK(!trace.write_started);
        }
    }
    auto unexpected = fixture.executor.submit_auto([&] { return server.accept_client(20ms); });
    MIRA_CHECK(!unexpected.get());
    request.headers.clear();
    request.keep_alive = true;
    auto redirecting = fixture.executor.submit_auto([&] {
        std::vector<std::string> requests;
        for (int hop = 0; hop < 2; ++hop) {
            if (!server.accept_client(2s))
                return requests;
            requests.push_back(server.read_request(2s, false));
            if (hop == 0)
                server.write_all("HTTP/1.1 307 Temporary Redirect\r\nLocation: /next\r\n"
                                 "Content-Length: 0\r\n\r\n");
            else
                server.write_all("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
            server.close_client();
        }
        return requests;
    });
    TransportTrace trace;
    auto redirected = fixture.transport.execute(request, limits(), {}, nullptr, trace);
    const auto hops = redirecting.get();
    MIRA_CHECK(redirected && redirected.value().redirects_followed == 1);
    MIRA_CHECK(hops.size() == 2);
    for (const auto &hop : hops)
        MIRA_CHECK(connection_values(hop) == std::vector<std::string>{"keep-alive"});
    MIRA_CHECK(hops[1].find("GET /next HTTP/1.1\r\n") == 0);

    // HTTP forward proxy serializes the same request policy in absolute form.
    request.url = "http://127.0.0.1:9/events";
    auto bound = limits();
    bound.proxy = ModelProxyConfig{
        "http://127.0.0.1:" + std::to_string(server.port()), std::nullopt, true, {"127.0.0.1"}};
    auto proxy = fixture.executor.submit_auto([&] {
        ServerResult result;
        if (!server.accept_client(2s))
            return result;
        result.request = server.read_request(2s, false);
        server.write_all(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
        result.client_closed = peer_closed(server);
        server.close_client();
        return result;
    });
    const auto proxied = fixture.transport.execute(request, bound, {}, nullptr, trace);
    const auto served = proxy.get();
    MIRA_CHECK(proxied && served.client_closed); // Content-Length ends without peer EOF.
    MIRA_CHECK(served.request.find("GET http://127.0.0.1:9/events HTTP/1.1\r\n") == 0);
    MIRA_CHECK(connection_values(served.request) == std::vector<std::string>{"keep-alive"});
    return 0;
}

int provider_stream_sets_connection_policy() {
    for (const auto dialect :
         {ProtocolDialect::OpenAIResponsesV1, ProtocolDialect::OpenAIChatCompletionsV1,
          ProtocolDialect::AnthropicMessagesV1}) {
        for (bool stream : {false, true}) {
            auto profile =
                std::make_shared<ModelProfile>(make_profile(dialect, "https://api.test"));
            auto transport = std::make_shared<MockHttpTransport>(nullptr);
            MockStep step;
            step.pre_write = true;
            step.failure = make_model_error(ModelDomainCode::TransportFailed, "capture only");
            transport->enqueue(std::move(step));
            OpenAiCompatibleProvider provider(profile, transport,
                                              std::make_shared<NullArtifactSource>());
            ModelRequest request;
            request.profile_id = profile->id;
            request.output_contract.mode = OutputMode::Text;
            request.generation.max_output_tokens = 128;
            ModelInputItem user;
            user.role = ModelRole::User;
            user.content.emplace_back(TextPart{"hello"});
            request.input.push_back(std::move(user));
            ProviderInferOptions options;
            options.stream = stream;
            const auto response = provider.infer(request, {}, options);
            MIRA_CHECK(!response);
            const auto captured = transport->recorded();
            MIRA_CHECK(captured.size() == 1);
            MIRA_CHECK(captured[0].keep_alive == stream);
            MIRA_CHECK(response.error().safe_message == "capture only");
        }
    }
    return 0;
}
} // namespace

int main() {
    try {
        if (const int status = sensitive_stream_and_default_compatibility(); status != 0)
            return status;
        if (const int status = stream_exits_release_exchange(); status != 0)
            return status;
        if (const int status = reserved_headers_redirects_and_proxy(); status != 0)
            return status;
        return provider_stream_sets_connection_policy();
    } catch (const std::exception &error) {
        std::cerr << "connection policy test exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "connection policy test non-standard exception\n";
        return 1;
    }
}
