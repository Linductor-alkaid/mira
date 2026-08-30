#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/model_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// HTTP exchange contracts (transport-boundary)
// ---------------------------------------------------------------------------

// One outbound HTTP request. Headers here are never secret; credentials are
// referenced by name and resolved only inside the transport.
struct HttpRequest final {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::optional<SecretRef> authorization;
};

// Staged deadlines plus hard caps for one exchange. The final deadline is the
// minimum of these and any operation-level deadline.
struct TransportLimits final {
    TransportDeadlines deadlines;
    std::uint64_t max_response_bytes = 8ULL * 1024ULL * 1024ULL;
    std::uint32_t max_redirects = 2;
    // SSRF posture: loopback, private, link-local and metadata addresses are
    // denied unless the caller explicitly opts in (tests do).
    bool allow_private_endpoints = false;
    // When non-empty, every hop (including redirects) must match a host.
    std::vector<std::string> allowed_hosts;
};

struct HttpResponseInfo final {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // Lowercase names.
    std::uint64_t body_bytes = 0;
    // Redirect hops actually followed, for diagnostics.
    std::uint32_t redirects_followed = 0;
};

// How far one exchange progressed; filled on success and failure so retry
// decisions can distinguish "nothing left the process" from "the remote may
// have received and billed the request".
struct TransportTrace final {
    bool write_started = false;
    bool write_completed = false;
    bool headers_received = false;
};

// Receives raw body bytes as they arrive. Called on the transport worker
// thread; implementations must be thread-safe and must not block.
using HttpChunkCallback = std::function<void(std::string_view)>;

// Executor-managed HTTP transport. Implementations must:
//  - run blocking socket waits on an Executor blocking I/O worker;
//  - release waits on OperationContext cancellation within bounded latency;
//  - enforce staged deadlines, size caps and the endpoint policy on every hop;
//  - settle in-flight exchanges deterministically on shutdown.
class IHttpTransport {
  public:
    virtual ~IHttpTransport() = default;
    virtual Result<HttpResponseInfo> execute(const HttpRequest &request,
                                             const TransportLimits &limits,
                                             const OperationContext &context,
                                             const HttpChunkCallback &on_chunk,
                                             TransportTrace &trace) = 0;
};

// Resolves a named secret at the transport boundary; plaintext never leaves
// the transport, never enters events or digests.
class ISecretResolver {
  public:
    virtual ~ISecretResolver() = default;
    virtual Result<std::string> resolve(const SecretRef &reference) = 0;
};

// A resolver that fails closed; used when a profile needs no credentials.
class NullSecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &) override {
        Error error;
        error.code = ErrorCode::PermissionDenied;
        error.domain = "mira.model";
        error.safe_message = "no secret resolver is configured";
        return error;
    }
};

// ---------------------------------------------------------------------------
// TLS channel boundary
// ---------------------------------------------------------------------------

struct TlsOptions final {
    // PEM file with trusted CA certificates; empty uses the system default.
    std::string ca_file;
    // Server certificate hostname; defaults to the URL host.
    std::string expected_host;
    // Verification may never be disabled in production profiles.
    bool verify_peer = true;
};

// Poll-driven TLS channel over a native socket handle. Every operation
// reports whether it wants readable or writable socket state; the transport
// polls accordingly. The native handle remains owned by the transport.
class ITlsChannel {
  public:
    virtual ~ITlsChannel() = default;
    // Returns false with want flags set when the operation must wait.
    virtual Result<bool> handshake(bool &want_read, bool &want_write) = 0;
    virtual Result<std::size_t> write_some(std::span<const std::byte> bytes, bool &want_read,
                                           bool &want_write) = 0;
    virtual Result<std::size_t> read_some(std::span<std::byte> bytes, bool &want_read,
                                          bool &want_write) = 0;
    virtual void close() = 0;
};

// Creates TLS channels for https endpoints. When no factory is configured,
// https endpoints fail closed before any bytes are written.
class ITlsChannelFactory {
  public:
    virtual ~ITlsChannelFactory() = default;
    virtual Result<std::unique_ptr<ITlsChannel>>
    create(std::intptr_t native_handle, const std::string &host, const TlsOptions &options) = 0;
};

} // namespace mira
