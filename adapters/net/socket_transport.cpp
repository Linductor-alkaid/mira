#include "socket_transport.hpp"

#include <mira/model_contracts.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
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
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
using SockLen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
using SockLen = socklen_t;
#endif

namespace mira::adapters::net {
namespace {

constexpr int kPollSliceMs = 25;
constexpr std::size_t kMaxHeaderBlockBytes = 64 * 1024;

[[nodiscard]] Error transport_error(ModelDomainCode code, std::string message, bool retryable) {
    return make_model_error(code, std::move(message), retryable, std::nullopt);
}

[[nodiscard]] std::string lowercase_name(const std::string &name) {
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

[[nodiscard]] bool valid_header_name(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    });
}

#ifdef _WIN32
class WinsockGuard final {
  public:
    WinsockGuard() {
        WSADATA data{};
        const auto status = WSAStartup(MAKEWORD(2, 2), &data);
        initialized_ = status == 0;
    }
    ~WinsockGuard() {
        if (initialized_) {
            WSACleanup();
        }
    }
    WinsockGuard(const WinsockGuard &) = delete;
    WinsockGuard &operator=(const WinsockGuard &) = delete;
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    bool initialized_ = false;
};

WinsockGuard &winsock() {
    static WinsockGuard guard;
    return guard;
}
#endif

[[nodiscard]] SocketHandle native(std::intptr_t handle) {
    return static_cast<SocketHandle>(handle);
}

void close_native(std::intptr_t handle) noexcept {
    const auto socket = native(handle);
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void close_socket(SocketHandle socket) noexcept {
    close_native(static_cast<std::intptr_t>(socket));
}

[[nodiscard]] bool address_is_private(const sockaddr_storage &address) {
    if (address.ss_family == AF_INET) {
        const auto &v4 = reinterpret_cast<const sockaddr_in &>(address);
        const auto raw = static_cast<std::uint32_t>(ntohl(v4.sin_addr.s_addr));
        const std::uint8_t a = static_cast<std::uint8_t>(raw >> 24U);
        const std::uint8_t b = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
        if (a == 127 || a == 10 || a == 0) {
            return true; // Loopback, private, this-network.
        }
        if (a == 172 && b >= 16 && b <= 31) {
            return true; // Private.
        }
        if (a == 192 && b == 168) {
            return true; // Private.
        }
        if (a == 169 && b == 254) {
            return true; // Link-local, cloud metadata.
        }
        if (a == 100 && b >= 64 && b <= 127) {
            return true; // Carrier-grade NAT.
        }
        return false;
    }
    if (address.ss_family == AF_INET6) {
        const auto &v6 = reinterpret_cast<const sockaddr_in6 &>(address);
        const auto bytes = v6.sin6_addr.s6_addr;
        const bool zero_prefix = bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0 &&
                                 bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
                                 bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 &&
                                 bytes[11] == 0 && bytes[12] == 0 && bytes[13] == 0;
        if (zero_prefix && bytes[14] == 0 && bytes[15] == 1) {
            return true; // ::1
        }
        if (bytes[0] == 0xFE && (bytes[1] & 0xC0U) == 0x80U) {
            return true; // fe80::/10 link-local.
        }
        if ((bytes[0] & 0xFEU) == 0xFCU) {
            return true; // fc00::/7 unique-local.
        }
        if (bytes[10] == 0xFF && bytes[11] == 0xFF) {
            // IPv4-mapped; inspect the embedded address.
            sockaddr_storage mapped{};
            auto &mapped_v4 = reinterpret_cast<sockaddr_in &>(mapped);
            mapped_v4.sin_family = AF_INET;
            mapped_v4.sin_addr.s_addr = htonl((static_cast<std::uint32_t>(bytes[12]) << 24U) |
                                              (static_cast<std::uint32_t>(bytes[13]) << 16U) |
                                              (static_cast<std::uint32_t>(bytes[14]) << 8U) |
                                              static_cast<std::uint32_t>(bytes[15]));
            return address_is_private(mapped);
        }
        return false;
    }
    return true; // Unknown families are denied by default.
}

// Poll-sliced wait shared by connect, handshake and I/O stages. The slice
// bounds cancellation and deadline latency without cross-thread closes.
[[nodiscard]] Result<void> poll_wait(std::intptr_t handle, bool readable,
                                     std::chrono::steady_clock::time_point until,
                                     const std::function<bool()> &cancelled) {
    const auto socket = native(handle);
    for (;;) {
        if (cancelled != nullptr && cancelled()) {
            return transport_error(ModelDomainCode::ModelCancelled,
                                   "exchange was cancelled while waiting on the socket", false);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) {
            return transport_error(ModelDomainCode::ModelDeadlineExceeded,
                                   "exchange stage deadline exceeded", false);
        }
#ifdef _WIN32
        WSAPOLLFD descriptor{};
#else
        pollfd descriptor{};
#endif
        descriptor.fd = static_cast<decltype(descriptor.fd)>(socket);
        descriptor.events = readable ? static_cast<short>(POLLIN | POLLRDNORM)
                                     : static_cast<short>(POLLOUT | POLLWRNORM);
        descriptor.revents = 0;
#ifdef _WIN32
        const int ready = WSAPoll(&descriptor, 1, kPollSliceMs);
#else
        const int ready = ::poll(&descriptor, 1, kPollSliceMs);
#endif
        if (ready < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            return transport_error(ModelDomainCode::TransportFailed, "socket poll failed", true);
        }
        if (ready == 0) {
            continue; // Slice elapsed; re-check cancellation and deadline.
        }
        const short events = descriptor.revents;
        const short wanted = readable ? static_cast<short>(POLLIN | POLLRDNORM)
                                      : static_cast<short>(POLLOUT | POLLWRNORM);
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (events & wanted) == 0) {
            if (readable) {
                // Hangup still allows the final drained read; recv reports
                // the end of stream.
                return Result<void>{};
            }
            return transport_error(ModelDomainCode::TransportFailed,
                                   "socket reported an error state", false);
        }
        return Result<void>{};
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SocketHttpTransport::UrlParts
// ---------------------------------------------------------------------------

std::string SocketHttpTransport::UrlParts::origin() const {
    return scheme + "://" + host + ":" + std::to_string(port);
}

SocketHttpTransport::Exchange::Exchange(std::intptr_t handle, std::unique_ptr<ITlsChannel> channel)
    : socket(handle), tls(std::move(channel)) {}

SocketHttpTransport::Exchange::Exchange(Exchange &&other) noexcept
    : socket(other.socket), tls(std::move(other.tls)) {
    other.socket = -1;
}

SocketHttpTransport::Exchange::~Exchange() { close(); }

void SocketHttpTransport::Exchange::close() noexcept {
    if (tls != nullptr) {
        tls->close();
    }
    close_native(socket);
    socket = -1;
}

Result<void> SocketHttpTransport::Exchange::wait(bool readable,
                                                 std::chrono::steady_clock::time_point until,
                                                 const std::function<bool()> &cancelled) const {
    return poll_wait(socket, readable, until, cancelled);
}

Result<void>
SocketHttpTransport::Exchange::handshake(std::chrono::steady_clock::time_point until,
                                         const std::function<bool()> &cancelled) const {
    for (;;) {
        bool want_read = false;
        bool want_write = false;
        auto done = tls->handshake(want_read, want_write);
        if (!done) {
            return done.error();
        }
        if (done.value()) {
            return Result<void>{};
        }
        auto ready = wait(want_read, until, cancelled);
        if (!ready) {
            return ready;
        }
    }
}

Result<std::size_t>
SocketHttpTransport::Exchange::write_all(const std::string &data,
                                         std::chrono::steady_clock::time_point until,
                                         const std::function<bool()> &cancelled) const {
    std::size_t written = 0;
    const auto *bytes = reinterpret_cast<const std::byte *>(data.data());
    while (written < data.size()) {
        if (tls != nullptr) {
            bool want_read = false;
            bool want_write = false;
            auto sent = tls->write_some(std::span(bytes + written, data.size() - written),
                                        want_read, want_write);
            if (!sent) {
                return sent.error();
            }
            if (sent.value() == 0 && (want_read || want_write)) {
                auto ready = wait(want_read, until, cancelled);
                if (!ready) {
                    return ready.error();
                }
                continue;
            }
            written += sent.value();
            continue;
        }
        auto ready = wait(false, until, cancelled);
        if (!ready) {
            return ready.error();
        }
#ifdef _WIN32
        const int status = ::send(native(socket), data.data() + written,
                                  static_cast<int>(data.size() - written), 0);
        const bool would_block = status == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const ssize_t status =
            ::send(native(socket), bytes + written, data.size() - written, MSG_NOSIGNAL);
        const bool would_block = status < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
        if (status <= 0) {
            if (would_block) {
                continue;
            }
            return transport_error(ModelDomainCode::TransportFailed, "socket write failed", true);
        }
        written += static_cast<std::size_t>(status);
    }
    return written;
}

Result<std::size_t>
SocketHttpTransport::Exchange::read_some(std::byte *data, std::size_t capacity,
                                         std::chrono::steady_clock::time_point until,
                                         const std::function<bool()> &cancelled) const {
    for (;;) {
        if (tls != nullptr) {
            bool want_read = false;
            bool want_write = false;
            auto received = tls->read_some(std::span(data, capacity), want_read, want_write);
            if (!received) {
                return received.error();
            }
            if (received.value() == 0 && (want_read || want_write)) {
                auto ready = wait(want_read, until, cancelled);
                if (!ready) {
                    return ready.error();
                }
                continue;
            }
            return received;
        }
        auto ready = wait(true, until, cancelled);
        if (!ready) {
            return ready.error();
        }
#ifdef _WIN32
        const int status =
            ::recv(native(socket), reinterpret_cast<char *>(data), static_cast<int>(capacity), 0);
        if (status == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            return transport_error(ModelDomainCode::TransportFailed, "socket read failed", true);
        }
#else
        const ssize_t status = ::recv(native(socket), data, capacity, 0);
        if (status < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return transport_error(ModelDomainCode::TransportFailed, "socket read failed", true);
        }
#endif
        if (status == 0) {
            return static_cast<std::size_t>(0);
        }
        return static_cast<std::size_t>(status);
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

class SocketHttpTransport::Worker final : public executor::IBlockingIoWorker {
  public:
    explicit Worker(SocketHttpTransport &owner) : owner_(owner) {}

    void run(executor::StopToken stop_token) override {
        while (!stop_token.stop_requested()) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(owner_.mutex_);
                owner_.work_available_.wait(lock, [&] {
                    return stop_token.stop_requested() || !owner_.queue_.empty() ||
                           owner_.stopping_;
                });
                if (stop_token.stop_requested() || owner_.stopping_) {
                    break;
                }
                if (owner_.queue_.empty()) {
                    continue;
                }
                job = owner_.queue_.front();
                owner_.queue_.pop_front();
            }
            owner_.run_job(*job);
        }
        // Settle everything left so no caller waits past its guard timeout.
        std::deque<std::shared_ptr<Job>> remaining;
        {
            std::lock_guard lock(owner_.mutex_);
            remaining.swap(owner_.queue_);
        }
        for (auto &job : remaining) {
            job->settlement.set_value(
                transport_error(ModelDomainCode::ModelCancelled,
                                "transport shut down before the exchange started", false));
        }
    }

    void wakeup() noexcept override { owner_.work_available_.notify_all(); }

  private:
    SocketHttpTransport &owner_;
};

void SocketHttpTransport::run_job(Job &job) {
    auto result = perform_exchange(job, *job.trace);
    try {
        job.settlement.set_value(std::move(result));
    } catch (...) {
        // An abandoned promise only lengthens the caller's bounded guard
        // wait; the worker status still reports the exchange outcome.
    }
}

// ---------------------------------------------------------------------------
// SocketHttpTransport
// ---------------------------------------------------------------------------

SocketHttpTransport::SocketHttpTransport(executor::Executor &executor,
                                         std::shared_ptr<ISecretResolver> secrets,
                                         std::shared_ptr<ITlsChannelFactory> tls,
                                         SocketTransportConfig config)
    : executor_(executor), secrets_(std::move(secrets)), tls_(std::move(tls)),
      config_(std::move(config)) {
    if (secrets_ == nullptr) {
        secrets_ = std::make_shared<NullSecretResolver>();
    }
}

SocketHttpTransport::~SocketHttpTransport() { shutdown(); }

bool SocketHttpTransport::start() {
#ifdef _WIN32
    if (!winsock().initialized()) {
        return false;
    }
#endif
    if (config_.worker_count == 0 || config_.max_queued_exchanges == 0) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (started_) {
        return true;
    }
    for (std::size_t index = 0; index < config_.worker_count; ++index) {
        executor::BlockingIoConfig worker_config;
        worker_config.thread_name = config_.worker_count == 1
                                        ? config_.worker_name
                                        : config_.worker_name + "-" + std::to_string(index);
        auto handle = executor_.start_worker(executor::BlockingWorkerSpec{
            worker_config.thread_name, worker_config, std::make_unique<Worker>(*this)});
        if (!handle.started()) {
            return false;
        }
        handles_.push_back(std::move(handle));
        ++active_workers_;
    }
    started_ = true;
    return true;
}

void SocketHttpTransport::shutdown() {
    std::unique_lock lock(mutex_);
    if (stopping_) {
        return;
    }
    stopping_ = true;
    work_available_.notify_all();
    auto handles = std::move(handles_);
    const bool was_started = started_;
    started_ = false;
    lock.unlock();
    if (was_started) {
        // Join workers from a non-worker thread; each in-flight exchange
        // settles first within its bounded deadlines.
        for (auto &handle : handles) {
            handle.stop();
        }
    }
    lock.lock();
    active_workers_ = 0;
}

bool SocketHttpTransport::running() const noexcept {
    std::lock_guard lock(mutex_);
    return started_;
}

std::size_t SocketHttpTransport::queued_exchanges() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

Result<HttpResponseInfo> SocketHttpTransport::execute(const HttpRequest &request,
                                                      const TransportLimits &limits,
                                                      const OperationContext &context,
                                                      const HttpChunkCallback &on_chunk,
                                                      TransportTrace &trace) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) {
            return transport_error(ModelDomainCode::ModelResourceExhausted,
                                   "transport is not running", false);
        }
        if (queue_.size() >= config_.max_queued_exchanges) {
            return transport_error(ModelDomainCode::ModelResourceExhausted,
                                   "transport queue is full", true);
        }
    }
    if (context.cancelled()) {
        return transport_error(ModelDomainCode::ModelCancelled,
                               "exchange was cancelled before admission", false);
    }
    auto job = std::make_shared<Job>();
    job->request = request;
    job->limits = limits;
    job->cancelled = context.cancellation_requested;
    job->deadline = context.deadline;
    job->on_chunk = on_chunk;
    const auto job_trace = job->trace;
    auto future = job->settlement.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(job));
    }
    work_available_.notify_all();

    // Workers enforce staged deadlines; this outer bound only guards against
    // a lost worker so no caller can wait forever.
    const auto guard =
        std::chrono::steady_clock::now() + limits.deadlines.total + std::chrono::seconds(5);
    if (future.wait_until(guard) != std::future_status::ready) {
        return transport_error(ModelDomainCode::ModelDeadlineExceeded,
                               "transport settlement timed out", false);
    }
    auto result = future.get();
    trace.write_started = trace.write_started || job_trace->write_started;
    trace.write_completed = trace.write_completed || job_trace->write_completed;
    trace.headers_received = trace.headers_received || job_trace->headers_received;
    return result;
}

namespace {

[[nodiscard]] Result<SocketHttpTransport::UrlParts> parse_url(const std::string &url) {
    SocketHttpTransport::UrlParts parts;
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "url must be absolute with a scheme", false);
    }
    parts.scheme = lowercase_name(url.substr(0, scheme_end));
    if (parts.scheme == "https") {
        parts.tls = true;
    } else if (parts.scheme != "http") {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "only http and https endpoints are accepted", false);
    }

    const auto authority_end = url.find('/', scheme_end + 3);
    std::string authority = url.substr(scheme_end + 3, authority_end == std::string::npos
                                                           ? std::string::npos
                                                           : authority_end - scheme_end - 3);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "url authority is empty or carries credentials", false);
    }
    std::string port_text;
    if (authority.front() == '[') {
        const auto bracket_end = authority.find(']');
        if (bracket_end == std::string::npos) {
            return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                   "ipv6 literal is malformed", false);
        }
        parts.host = authority.substr(1, bracket_end - 1);
        if (bracket_end + 1 < authority.size() && authority[bracket_end + 1] == ':') {
            port_text = authority.substr(bracket_end + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon == std::string::npos) {
            parts.host = authority;
        } else {
            parts.host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        }
    }
    if (parts.host.empty() || parts.host.find(' ') != std::string::npos) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied, "url host is malformed",
                               false);
    }
    parts.port = parts.tls ? 443 : 80;
    if (!port_text.empty()) {
        try {
            const auto port = std::stoul(port_text);
            if (port == 0 || port > 65535) {
                return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                       "url port is out of range", false);
            }
            parts.port = static_cast<std::uint16_t>(port);
        } catch (const std::exception &) {
            return transport_error(ModelDomainCode::EndpointPolicyDenied, "url port is malformed",
                                   false);
        }
    }
    parts.path_query = authority_end == std::string::npos ? "/" : url.substr(authority_end);
    if (parts.path_query.find('#') != std::string::npos) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "url fragments are not sent over the wire", false);
    }
    return parts;
}

} // namespace

Result<HttpResponseInfo> SocketHttpTransport::perform_exchange(Job &job, TransportTrace &trace) {
    const auto started_at = std::chrono::steady_clock::now();
    auto total_deadline = started_at + job.limits.deadlines.total;
    if (job.deadline.has_value() && *job.deadline < total_deadline) {
        total_deadline = *job.deadline;
    }

    if (job.request.method.empty() ||
        !std::all_of(job.request.method.begin(), job.request.method.end(),
                     [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; })) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "http method contains invalid characters", false);
    }
    for (const auto &header : job.request.headers) {
        const auto lowered = lowercase_name(header.first);
        if (!valid_header_name(header.first) || header.second.find('\r') != std::string::npos ||
            header.second.find('\n') != std::string::npos || lowered == "host" ||
            lowered == "content-length" || lowered == "connection" || lowered == "authorization" ||
            lowered == "proxy-authorization") {
            return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                   "http request header is invalid or reserved", false);
        }
    }

    std::optional<std::string> authorization;
    if (job.request.authorization.has_value()) {
        auto secret = secrets_->resolve(*job.request.authorization);
        if (!secret) {
            return secret.error();
        }
        authorization = std::move(secret).value();
    }

    std::optional<std::string> proxy_authorization;
    if (job.limits.proxy.has_value() && job.limits.proxy->authorization.has_value()) {
        auto secret = secrets_->resolve(*job.limits.proxy->authorization);
        if (!secret) {
            return secret.error();
        }
        proxy_authorization = std::move(secret).value();
        if (proxy_authorization->find('\r') != std::string::npos ||
            proxy_authorization->find('\n') != std::string::npos) {
            return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                   "proxy authorization contains invalid characters", false);
        }
    }

    auto parts = parse_url(job.request.url);
    if (!parts) {
        return parts.error();
    }
    if (job.original_origin.empty()) {
        job.original_origin = parts.value().origin();
    }

    if (!job.limits.allowed_hosts.empty() &&
        std::find(job.limits.allowed_hosts.begin(), job.limits.allowed_hosts.end(),
                  parts.value().host) == job.limits.allowed_hosts.end()) {
        return transport_error(ModelDomainCode::EndpointPolicyDenied,
                               "endpoint host is not in the allowlist", false);
    }

    const auto resolve_candidates =
        [&](const UrlParts &subject, bool allow_private,
            const char *label) -> Result<std::vector<sockaddr_storage>> {
        // getaddrinfo itself is not interruptible; enforce its budget by
        // measurement and keep it on the transport's Executor worker.
        const auto dns_began = std::chrono::steady_clock::now();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo *raw_addresses = nullptr;
        const std::string port_text = std::to_string(subject.port);
        const int dns_status =
            ::getaddrinfo(subject.host.c_str(), port_text.c_str(), &hints, &raw_addresses);
        if (dns_status != 0) {
            return transport_error(ModelDomainCode::TransportFailed,
                                   std::string(label) + " dns resolution failed", true);
        }
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw_addresses,
                                                                       &::freeaddrinfo);
        if (std::chrono::steady_clock::now() - dns_began > job.limits.deadlines.dns) {
            return transport_error(ModelDomainCode::ModelDeadlineExceeded,
                                   std::string(label) + " dns resolution exceeded its deadline",
                                   false);
        }
        std::vector<sockaddr_storage> resolved;
        for (auto *entry = addresses.get(); entry != nullptr; entry = entry->ai_next) {
            sockaddr_storage storage{};
            if (entry->ai_addrlen > sizeof(storage)) {
                continue;
            }
            std::memcpy(&storage, entry->ai_addr, entry->ai_addrlen);
            if (!allow_private && address_is_private(storage)) {
                continue;
            }
            resolved.push_back(storage);
        }
        if (resolved.empty()) {
            return transport_error(
                ModelDomainCode::EndpointPolicyDenied,
                std::string("no policy-compliant address was resolved for the ") + label, false);
        }
        return resolved;
    };

    // Always resolve and validate the destination, even when the proxy will
    // perform the actual connect, so a proxy cannot become an SSRF bypass.
    auto destination_candidates =
        resolve_candidates(parts.value(), job.limits.allow_private_endpoints, "endpoint");
    if (!destination_candidates) {
        return destination_candidates.error();
    }

    std::optional<UrlParts> proxy_parts;
    std::vector<sockaddr_storage> candidates = std::move(destination_candidates).value();
    if (job.limits.proxy.has_value()) {
        auto parsed_proxy = parse_url(job.limits.proxy->url);
        if (!parsed_proxy) {
            return parsed_proxy.error();
        }
        if (parsed_proxy.value().tls || parsed_proxy.value().path_query != "/") {
            return transport_error(ModelDomainCode::CapabilityMismatch,
                                   "proxy must be an http origin without a path", false);
        }
        if (!job.limits.proxy->allowed_hosts.empty() &&
            std::find(job.limits.proxy->allowed_hosts.begin(),
                      job.limits.proxy->allowed_hosts.end(),
                      parsed_proxy.value().host) == job.limits.proxy->allowed_hosts.end()) {
            return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                   "proxy host is not in the allowlist", false);
        }
        auto proxy_candidates = resolve_candidates(
            parsed_proxy.value(), job.limits.proxy->allow_private_endpoint, "proxy");
        if (!proxy_candidates) {
            return proxy_candidates.error();
        }
        candidates = std::move(proxy_candidates).value();
        proxy_parts = std::move(parsed_proxy).value();
    }

    SocketHandle socket = kInvalidSocket;
    for (const auto &candidate : candidates) {
        socket = ::socket(candidate.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (socket == kInvalidSocket) {
            continue;
        }
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(socket, FIONBIO, &mode);
#else
        const int flags = ::fcntl(socket, F_GETFL, 0);
        ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
        const int connected = ::connect(socket, reinterpret_cast<const sockaddr *>(&candidate),
                                        static_cast<SockLen>(sizeof(candidate)));
        if (connected != 0) {
#ifdef _WIN32
            const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool pending = errno == EINPROGRESS;
#endif
            if (!pending) {
                close_socket(socket);
                socket = kInvalidSocket;
                continue;
            }
            auto connect_deadline =
                std::min(started_at + job.limits.deadlines.connect, total_deadline);
            // A bare wait (no Exchange): the socket is only owned once the
            // connection attempt succeeds.
            auto writable = poll_wait(static_cast<std::intptr_t>(socket), false, connect_deadline,
                                      job.cancelled);
            if (!writable) {
                close_socket(socket);
                if (writable.error().code == ErrorCode::DeadlineExceeded) {
                    return transport_error(ModelDomainCode::ModelDeadlineExceeded,
                                           "connect exceeded its deadline", false);
                }
                return writable.error();
            }
            int error = 0;
            SockLen error_size = sizeof(error);
            if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                             &error_size) != 0 ||
                error != 0) {
                close_socket(socket);
                socket = kInvalidSocket;
                continue;
            }
        }
        break;
    }
    if (socket == kInvalidSocket) {
        return transport_error(ModelDomainCode::TransportFailed,
                               "no candidate address accepted the connection", true);
    }

    Exchange exchange(static_cast<std::intptr_t>(socket), nullptr);

    // HTTPS proxies are deliberately unsupported above. For an HTTPS target,
    // establish a plain CONNECT tunnel first; model request bytes have not
    // started at this point, so a rejected tunnel remains a pre-write failure.
    if (proxy_parts.has_value() && parts.value().tls) {
        const std::string authority = parts.value().host + ":" + std::to_string(parts.value().port);
        std::string connect_request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority +
                                      "\r\nProxy-Connection: keep-alive\r\n";
        if (proxy_authorization.has_value()) {
            connect_request += "Proxy-Authorization: " + *proxy_authorization + "\r\n";
        }
        connect_request += "\r\n";
        const auto connect_deadline =
            std::min(started_at + job.limits.deadlines.connect, total_deadline);
        if (auto sent = exchange.write_all(connect_request, connect_deadline, job.cancelled);
            !sent) {
            return sent.error();
        }
        std::string response_headers;
        while (response_headers.find("\r\n\r\n") == std::string::npos) {
            if (response_headers.size() > kMaxHeaderBlockBytes) {
                return transport_error(ModelDomainCode::ProtocolViolation,
                                       "proxy CONNECT response headers exceeded the limit", false);
            }
            std::array<std::byte, 1024> chunk{};
            auto received =
                exchange.read_some(chunk.data(), chunk.size(), connect_deadline, job.cancelled);
            if (!received) {
                return received.error();
            }
            if (received.value() == 0) {
                return transport_error(ModelDomainCode::TransportFailed,
                                       "proxy closed the CONNECT tunnel", true);
            }
            response_headers.append(reinterpret_cast<const char *>(chunk.data()), received.value());
        }
        std::istringstream status_stream(response_headers.substr(0, response_headers.find("\r\n")));
        std::string version;
        int proxy_status = 0;
        status_stream >> version >> proxy_status;
        if (proxy_status != 200) {
            return transport_error(proxy_status == 407 ? ModelDomainCode::AuthenticationFailed
                                                       : ModelDomainCode::TransportFailed,
                                   "proxy CONNECT request was rejected", proxy_status >= 500);
        }
    }

    std::unique_ptr<ITlsChannel> channel;
    if (parts.value().tls) {
        if (tls_ == nullptr) {
            return transport_error(ModelDomainCode::CapabilityMismatch,
                                   "https endpoint configured without a TLS channel factory",
                                   false);
        }
        auto created =
            tls_->create(static_cast<std::intptr_t>(socket), parts.value().host, TlsOptions{});
        if (!created) {
            return created.error();
        }
        channel = std::move(created).value();
        exchange.tls = std::move(channel);
    }

    if (parts.value().tls) {
        auto tls_deadline = std::min(started_at + job.limits.deadlines.tls, total_deadline);
        auto status = exchange.handshake(tls_deadline, job.cancelled);
        if (!status) {
            return status.error();
        }
    }
    const bool cross_origin = parts.value().origin() != job.original_origin;
    return finish_exchange(std::move(exchange), job, parts.value(), authorization, cross_origin,
                           proxy_parts.has_value() && !parts.value().tls, proxy_authorization,
                           total_deadline, started_at, trace);
}

Result<HttpResponseInfo> SocketHttpTransport::finish_exchange(
    Exchange exchange, Job &job, const UrlParts &parts,
    const std::optional<std::string> &authorization, bool cross_origin, bool forward_proxy,
    const std::optional<std::string> &proxy_authorization,
    std::chrono::steady_clock::time_point total_deadline,
    std::chrono::steady_clock::time_point started_at, TransportTrace &trace) {
    std::string request_text;
    request_text.reserve(256 + job.request.body.size());
    const std::string request_target = forward_proxy ? job.request.url : parts.path_query;
    request_text += job.request.method + " " + request_target + " HTTP/1.1\r\n";
    request_text += "Host: " + parts.host + "\r\n";
    for (const auto &header : job.request.headers) {
        request_text += header.first + ": " + header.second + "\r\n";
    }
    request_text += "Content-Length: " + std::to_string(job.request.body.size()) + "\r\n";
    request_text += "Connection: close\r\n";
    request_text += "Accept: application/json, text/event-stream\r\n";
    // Credentials follow redirects only within the original origin.
    if (authorization.has_value() && !cross_origin) {
        request_text += "Authorization: Bearer " + *authorization + "\r\n";
    }
    if (forward_proxy && proxy_authorization.has_value()) {
        request_text += "Proxy-Authorization: " + *proxy_authorization + "\r\n";
    }
    request_text += "\r\n";
    request_text += job.request.body;

    const auto write_deadline = std::min(started_at + job.limits.deadlines.write, total_deadline);
    trace.write_started = true;
    if (auto written = exchange.write_all(request_text, write_deadline, job.cancelled); !written) {
        return written.error();
    }
    trace.write_completed = true;

    std::string header_block;
    header_block.reserve(1024);
    bool first_byte = true;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= total_deadline) {
            return transport_error(ModelDomainCode::ModelDeadlineExceeded,
                                   "response header wait exceeded the total deadline", false);
        }
        std::array<std::byte, 2048> chunk{};
        const auto stage_deadline =
            first_byte ? std::min(started_at + job.limits.deadlines.first_byte, total_deadline)
                       : std::min(now + job.limits.deadlines.idle_read, total_deadline);
        auto received =
            exchange.read_some(chunk.data(), chunk.size(), stage_deadline, job.cancelled);
        if (!received) {
            return received.error();
        }
        if (received.value() == 0) {
            if (header_block.find("\r\n\r\n") == std::string::npos) {
                // Request bytes already left the process; the billable outcome
                // is unknown, so the completion is ambiguous.
                return transport_error(ModelDomainCode::AmbiguousCompletion,
                                       "connection closed before response headers arrived", false);
            }
            break;
        }
        first_byte = false;
        header_block.append(reinterpret_cast<const char *>(chunk.data()), received.value());
        if (header_block.size() > kMaxHeaderBlockBytes) {
            return transport_error(ModelDomainCode::ResponseTooLarge,
                                   "response header block exceeded the limit", false);
        }
        if (header_block.find("\r\n\r\n") != std::string::npos) {
            trace.headers_received = true;
            break;
        }
    }

    const auto header_end = header_block.find("\r\n\r\n");
    std::string buffered =
        header_end == std::string::npos ? std::string() : header_block.substr(header_end + 4);
    std::istringstream header_stream(
        header_end == std::string::npos ? header_block : header_block.substr(0, header_end));
    std::string status_line;
    std::getline(header_stream, status_line);
    if (!status_line.empty() && status_line.back() == '\r') {
        status_line.pop_back();
    }
    int status = 0;
    {
        std::istringstream status_stream(status_line);
        std::string version;
        status_stream >> version >> status;
        if (status == 0) {
            return transport_error(ModelDomainCode::ProtocolViolation, "malformed http status line",
                                   false);
        }
    }
    std::vector<std::pair<std::string, std::string>> headers;
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto name = lowercase_name(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        headers.emplace_back(std::move(name), std::move(value));
    }

    const auto find_header_value = [&headers](const char *name) -> const std::string * {
        for (const auto &header : headers) {
            if (header.first == name) {
                return &header.second;
            }
        }
        return nullptr;
    };

    // Redirects restart the whole pipeline: policy, DNS and TLS are
    // re-validated from scratch for the new hop.
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const auto *location = find_header_value("location");
        if (location == nullptr) {
            return transport_error(ModelDomainCode::ProtocolViolation,
                                   "redirect carried no location header", false);
        }
        if (job.redirects_followed >= job.limits.max_redirects) {
            return transport_error(ModelDomainCode::EndpointPolicyDenied,
                                   "redirect chain exceeded the configured limit", false);
        }
        std::string next_url = *location;
        if (next_url.rfind("http://", 0) != 0 && next_url.rfind("https://", 0) != 0) {
            next_url = parts.scheme + "://" + parts.host + ":" + std::to_string(parts.port) +
                       (next_url.empty() || next_url.front() != '/' ? "/" : "") + next_url;
        }
        if (status != 307 && status != 308) {
            // 301/302/303 normalize to GET without a body.
            job.request.method = "GET";
            job.request.body.clear();
        }
        job.request.url = next_url;
        ++job.redirects_followed;
        return perform_exchange(job, trace);
    }

    std::uint64_t body_bytes = 0;
    const auto deliver = [&](std::string_view data) -> Result<void> {
        body_bytes += data.size();
        if (body_bytes > job.limits.max_response_bytes) {
            return transport_error(ModelDomainCode::ResponseTooLarge,
                                   "response body exceeded the configured cap", false);
        }
        if (job.on_chunk != nullptr) {
            job.on_chunk(data);
        }
        return Result<void>{};
    };

    const auto read_more = [&](std::string &sink) -> Result<bool> {
        const auto now = std::chrono::steady_clock::now();
        std::array<std::byte, 8192> chunk{};
        auto received = exchange.read_some(
            chunk.data(), chunk.size(),
            std::min(now + job.limits.deadlines.idle_read, total_deadline), job.cancelled);
        if (!received) {
            return received.error();
        }
        if (received.value() == 0) {
            return false;
        }
        sink.append(reinterpret_cast<const char *>(chunk.data()), received.value());
        return true;
    };

    const auto *transfer_encoding = find_header_value("transfer-encoding");
    const auto *content_length = find_header_value("content-length");
    if (transfer_encoding != nullptr && transfer_encoding->find("chunked") != std::string::npos) {
        for (;;) {
            auto line_end = buffered.find("\r\n");
            while (line_end == std::string::npos) {
                auto more = read_more(buffered);
                if (!more) {
                    return more.error();
                }
                if (!more.value()) {
                    return transport_error(ModelDomainCode::ProtocolViolation,
                                           "chunked body ended mid-chunk", false);
                }
                line_end = buffered.find("\r\n");
            }
            const auto size_line = buffered.substr(0, line_end);
            const auto semicolon = size_line.find(';');
            const auto size_text =
                semicolon == std::string::npos ? size_line : size_line.substr(0, semicolon);
            std::size_t chunk_size = 0;
            try {
                chunk_size = static_cast<std::size_t>(std::stoull(size_text, nullptr, 16));
            } catch (const std::exception &) {
                return transport_error(ModelDomainCode::ProtocolViolation,
                                       "malformed chunk size in response", false);
            }
            buffered.erase(0, line_end + 2);
            if (chunk_size == 0) {
                break;
            }
            while (buffered.size() < chunk_size + 2) {
                auto more = read_more(buffered);
                if (!more) {
                    return more.error();
                }
                if (!more.value()) {
                    return transport_error(ModelDomainCode::ProtocolViolation,
                                           "chunked body ended mid-chunk", false);
                }
            }
            if (auto delivered = deliver(std::string_view(buffered).substr(0, chunk_size));
                !delivered) {
                return delivered.error();
            }
            buffered.erase(0, chunk_size + 2); // Chunk plus trailing CRLF.
        }
    } else if (content_length != nullptr) {
        std::uint64_t expected = 0;
        try {
            expected = std::stoull(*content_length);
        } catch (const std::exception &) {
            return transport_error(ModelDomainCode::ProtocolViolation,
                                   "malformed content-length in response", false);
        }
        if (expected > job.limits.max_response_bytes) {
            return transport_error(ModelDomainCode::ResponseTooLarge,
                                   "declared response body exceeds the configured cap", false);
        }
        if (!buffered.empty()) {
            const auto take = std::min<std::uint64_t>(buffered.size(), expected);
            if (auto delivered = deliver(std::string_view(buffered).substr(0, take)); !delivered) {
                return delivered.error();
            }
            buffered.erase(0, static_cast<std::size_t>(take));
        }
        while (body_bytes < expected) {
            auto more = read_more(buffered);
            if (!more) {
                return more.error();
            }
            if (!more.value()) {
                return transport_error(ModelDomainCode::ProtocolViolation,
                                       "connection closed before the full body arrived", false);
            }
            const auto remaining = static_cast<std::size_t>(expected - body_bytes);
            const auto take = std::min(remaining, buffered.size());
            if (auto delivered = deliver(std::string_view(buffered).substr(0, take)); !delivered) {
                return delivered.error();
            }
            buffered.erase(0, take);
        }
    } else {
        // Streams (SSE) read until the server closes the connection.
        if (!buffered.empty()) {
            if (auto delivered = deliver(buffered); !delivered) {
                return delivered.error();
            }
            buffered.clear();
        }
        for (;;) {
            auto more = read_more(buffered);
            if (!more) {
                return more.error();
            }
            if (!more.value()) {
                break;
            }
            if (auto delivered = deliver(buffered); !delivered) {
                return delivered.error();
            }
            buffered.clear();
        }
    }

    HttpResponseInfo info;
    info.status = status;
    info.headers = std::move(headers);
    info.body_bytes = body_bytes;
    info.redirects_followed = job.redirects_followed;
    return info;
}

} // namespace mira::adapters::net
