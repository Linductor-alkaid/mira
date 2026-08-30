#pragma once

// Shared fixtures for M3 tests: profiles, scripted transports, an in-process
// scripted HTTP server and scripted providers.

#include <mira/agent_loop.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_provider.hpp>
#include <mira/model_transport.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using TestSocket = SOCKET;
#define MIRA_TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
#define MIRA_TEST_INVALID_SOCKET (-1)
#endif

namespace mira::testing {

// ---------------------------------------------------------------------------
// Profiles and secrets
// ---------------------------------------------------------------------------

[[nodiscard]] inline ModelProfile make_profile(ProtocolDialect dialect,
                                               const std::string &origin) {
    ModelProfile profile;
    profile.id = ModelProfileId::generate();
    profile.display_name = "test-profile";
    profile.version = SemanticVersion{1, 0, 0};
    profile.dialect = dialect;
    profile.endpoint_origin = origin;
    profile.api_prefix = "/v1";
    profile.model_selector = "test-model";
    profile.credential = SecretRef{"test-credential"};
    auto verified = CapabilityEvidence::FixtureVerified;
    profile.capabilities.text = CapabilityFlag{true, verified, ""};
    profile.capabilities.image_input = CapabilityFlag{true, verified, ""};
    profile.capabilities.file_input = CapabilityFlag{true, verified, ""};
    profile.capabilities.strict_json_schema = CapabilityFlag{true, verified, ""};
    profile.capabilities.function_tools = CapabilityFlag{true, verified, ""};
    profile.capabilities.parallel_tool_calls = CapabilityFlag{true, verified, ""};
    profile.capabilities.sse = CapabilityFlag{true, verified, ""};
    profile.capabilities.exact_token_count = CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    profile.capabilities.continuation = CapabilityFlag{true, verified, ""};
    profile.capabilities.remote_retention = CapabilityFlag{true, verified, ""};
    profile.capabilities.upload = CapabilityFlag{true, verified, ""};
    profile.capabilities.generation.seed = ParamMapping::Unsupported;
    profile.deadlines.connect = std::chrono::milliseconds{2'000};
    profile.deadlines.total = std::chrono::milliseconds{10'000};
    profile.default_data_policy.store = false;
    return profile;
}

class MapSecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        const auto found = secrets_.find(reference.name);
        if (found == secrets_.end()) {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.domain = "mira.test";
            error.safe_message = "secret is not configured";
            return error;
        }
        return found->second;
    }
    void set(const std::string &name, std::string value) { secrets_[name] = std::move(value); }

  private:
    std::map<std::string, std::string> secrets_;
};

// ---------------------------------------------------------------------------
// Mock transport (records requests, serves scripted wire responses)
// ---------------------------------------------------------------------------

struct MockStep final {
    // HTTP response to deliver; body is written in `chunk_pieces` parts.
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::vector<std::string> chunk_pieces;
    // Optional transport-level failure instead of a response.
    std::optional<Error> failure;
    // Marks the chunk boundary after which the "connection" drops without a
    // terminal response.
    bool drop_after_chunks = false;
};

class MockHttpTransport final : public IHttpTransport {
  public:
    explicit MockHttpTransport(std::shared_ptr<ISecretResolver> secrets)
        : secrets_(std::move(secrets)) {}

    Result<HttpResponseInfo> execute(const HttpRequest &request,
                                     const TransportLimits & /*limits*/,
                                     const OperationContext &context,
                                     const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override {
        Recorded recorded;
        recorded.url = request.url;
        recorded.body = request.body;
        recorded.headers = request.headers;
        recorded.authorization =
            request.authorization.has_value()
                ? resolve_secret(*request.authorization)
                : std::string("<none>");
        recorded.cancelled_before_admission = context.cancelled();

        std::unique_lock lock(mutex_);
        recorded_requests_.push_back(std::move(recorded));
        if (steps_.empty()) {
            return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                    "mock transport script is exhausted");
        }
        MockStep step = std::move(steps_.front());
        steps_.pop_front();
        lock.unlock();

        trace.write_started = true;
        trace.write_completed = true;
        if (step.failure.has_value()) {
            // Pre-write failures leave nothing on the wire; post-write ones
            // are ambiguous. The script controls which via retryable hints.
            return step.failure.value();
        }
        std::string body = step.body;
        if (!step.chunk_pieces.empty()) {
            body.clear();
            for (const auto &piece : step.chunk_pieces) {
                body += piece;
            }
        }
        if (on_chunk != nullptr && !body.empty()) {
            if (!step.chunk_pieces.empty()) {
                for (const auto &piece : step.chunk_pieces) {
                    on_chunk(piece);
                }
            } else {
                on_chunk(body);
            }
        }
        if (step.drop_after_chunks) {
            return make_model_error(ModelDomainCode::AmbiguousCompletion,
                                    "connection closed before response headers arrived");
        }
        trace.headers_received = true;
        HttpResponseInfo info;
        info.status = step.status;
        info.headers = step.headers;
        info.body_bytes = body.size();
        return info;
    }

    void enqueue(MockStep step) {
        std::lock_guard lock(mutex_);
        steps_.push_back(std::move(step));
    }
    void enqueue_json(int status, std::string body) {
        MockStep step;
        step.status = status;
        step.headers.emplace_back("content-type", "application/json");
        step.body = std::move(body);
        enqueue(std::move(step));
    }

    struct Recorded final {
        std::string url;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string authorization;
        bool cancelled_before_admission = false;
    };

    [[nodiscard]] std::vector<Recorded> recorded() const {
        std::lock_guard lock(mutex_);
        return recorded_requests_;
    }
    [[nodiscard]] std::size_t remaining() const {
        std::lock_guard lock(mutex_);
        return steps_.size();
    }

  private:
    [[nodiscard]] std::string resolve_secret(const SecretRef &reference) const {
        if (secrets_ == nullptr) {
            return "<no-resolver>";
        }
        auto value = const_cast<MockHttpTransport *>(this)->resolve_through(reference);
        return value;
    }
    [[nodiscard]] std::string resolve_through(const SecretRef &reference) {
        auto resolved = secrets_->resolve(reference);
        return resolved ? resolved.value() : std::string("<unresolved>");
    }

    mutable std::mutex mutex_;
    std::deque<MockStep> steps_;
    std::vector<Recorded> recorded_requests_;
    std::shared_ptr<ISecretResolver> secrets_;
};

// ---------------------------------------------------------------------------
// Scripted IModelProvider
// ---------------------------------------------------------------------------

class ScriptedProvider final : public IModelProvider {
  public:
    explicit ScriptedProvider(std::shared_ptr<const ModelProfile> profile,
                              std::vector<ModelResponse> script)
        : profile_(std::move(profile)), script_(std::move(script)) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &) override {
        if (context.cancelled()) {
            return make_model_error(ModelDomainCode::ModelCancelled, "scripted provider cancelled",
                                    false, request.operation_id);
        }
        std::lock_guard lock(mutex_);
        if (cursor_ >= script_.size()) {
            return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                    "scripted provider is exhausted", false,
                                    request.operation_id);
        }
        ModelResponse response = script_[cursor_++];
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        return response;
    }
    [[nodiscard]] std::size_t consumed() const {
        std::lock_guard lock(mutex_);
        return cursor_;
    }

  private:
    std::shared_ptr<const ModelProfile> profile_;
    mutable std::mutex mutex_;
    std::vector<ModelResponse> script_;
    std::size_t cursor_ = 0;
};

// ---------------------------------------------------------------------------
// Decision helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string decision_body(const std::string &action, double x, double y,
                                               const std::string &reason = "test") {
    std::ostringstream stream;
    stream << "{\"action\":\"" << action << "\",\"x\":" << x << ",\"y\":" << y << ",\"reason\":\""
           << reason << "\"}";
    return stream.str();
}

[[nodiscard]] inline ModelResponse text_response(const std::string &text) {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.status = ModelCompletionStatus::Completed;
    MessageOutput message;
    message.role = ModelRole::Assistant;
    OutputTextPart part;
    part.text = text;
    message.content.emplace_back(std::move(part));
    response.output.emplace_back(std::move(message));
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    response.usage.quality = UsageQuality::ProviderReported;
    response.requested_model = "test-model";
    return response;
}

[[nodiscard]] inline ModelResponse refused_response() {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.status = ModelCompletionStatus::Refused;
    RefusalOutput refusal;
    refusal.safe_summary = "provider refused";
    response.output.emplace_back(std::move(refusal));
    response.requested_model = "test-model";
    return response;
}

// ---------------------------------------------------------------------------
// Scripted real-socket HTTP server (single-threaded, test-driven)
// ---------------------------------------------------------------------------

class ScriptedHttpServer final {
  public:
    ScriptedHttpServer() {
#ifdef _WIN32
        WSADATA data{};
        initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            shutdown();
            return;
        }
        sockaddr_in bound{};
        socklen_t size = sizeof(bound);
        if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&bound), &size) == 0) {
            port_ = ntohs(bound.sin_port);
        }
    }

    ~ScriptedHttpServer() { shutdown(); }

    ScriptedHttpServer(const ScriptedHttpServer &) = delete;
    ScriptedHttpServer &operator=(const ScriptedHttpServer &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return listener_ != MIRA_TEST_INVALID_SOCKET && port_ != 0
#ifdef _WIN32
               && initialized_
#endif
            ;
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Blocks until a client connects; returns false on timeout/invalid.
    [[nodiscard]] bool accept_client(std::chrono::milliseconds timeout) {
        if (!valid()) {
            return false;
        }
        set_blocking(listener_, true);
        // Portable timeout through select().
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener_, &set);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeout.count() / 1000);
        wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        const int ready =
            ::select(static_cast<int>(listener_ + 1), &set, nullptr, nullptr, &wait);
        if (ready <= 0) {
            return false;
        }
        client_ = ::accept(listener_, nullptr, nullptr);
        return client_ != MIRA_TEST_INVALID_SOCKET;
    }

    // Reads whatever the client sent within the timeout.
    [[nodiscard]] std::string read_request(std::chrono::milliseconds timeout) {
        std::string received;
        if (client_ == MIRA_TEST_INVALID_SOCKET) {
            return received;
        }
        set_blocking(client_, true);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char buffer[4096];
        for (;;) {
            if (received.find("\r\n\r\n") != std::string::npos) {
                // Wait briefly for any body bytes following the headers.
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0 || !wait_readable(remaining)) {
                    break;
                }
            } else if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            const int status =
                static_cast<int>(::recv(client_, buffer, static_cast<int>(sizeof(buffer)), 0));
            if (status <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(status));
            if (received.size() > 1024U * 1024U) {
                break;
            }
        }
        return received;
    }

    void write_all(const std::string &data) {
        if (client_ == MIRA_TEST_INVALID_SOCKET) {
            return;
        }
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto remaining = data.size() - sent;
#ifdef _WIN32
            const int status = static_cast<int>(
                ::send(client_, data.data() + sent, static_cast<int>(remaining), 0));
#else
            const auto status = ::send(client_, data.data() + sent, remaining, 0);
#endif
            if (status <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(status);
        }
    }

    void close_client() {
        if (client_ != MIRA_TEST_INVALID_SOCKET) {
#ifdef _WIN32
            closesocket(client_);
#else
            ::close(client_);
#endif
            client_ = MIRA_TEST_INVALID_SOCKET;
        }
    }

    void shutdown() {
        close_client();
        if (listener_ != MIRA_TEST_INVALID_SOCKET) {
#ifdef _WIN32
            closesocket(listener_);
            if (initialized_) {
                WSACleanup();
                initialized_ = false;
            }
#else
            ::close(listener_);
#endif
            listener_ = MIRA_TEST_INVALID_SOCKET;
        }
    }

  private:
    static void set_blocking(TestSocket handle, bool blocking) {
#ifdef _WIN32
        u_long mode = blocking ? 0 : 1;
        ioctlsocket(handle, FIONBIO, &mode);
#else
        const int flags = ::fcntl(handle, F_GETFL, 0);
        ::fcntl(handle, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
    }

    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(client_, &set);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeout.count() / 1000);
        wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        return ::select(static_cast<int>(client_ + 1), &set, nullptr, nullptr, &wait) > 0;
    }

    TestSocket listener_ = MIRA_TEST_INVALID_SOCKET;
    TestSocket client_ = MIRA_TEST_INVALID_SOCKET;
    std::uint16_t port_ = 0;
#ifdef _WIN32
    bool initialized_ = false;
#endif
};

} // namespace mira::testing
