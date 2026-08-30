#include "openssl_tls.hpp"

#include <mira/model_contracts.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace mira::adapters::net {
namespace {

[[nodiscard]] Error tls_error(std::string message, bool retryable = false) {
    Error error;
    error.code = retryable ? ErrorCode::Unavailable : ErrorCode::PermissionDenied;
    error.domain = "mira.model.tls";
    error.safe_message = std::move(message);
    error.retryable = retryable;
    return error;
}

class OpenSslChannel final : public ITlsChannel {
  public:
    OpenSslChannel(SSL_CTX *context, std::intptr_t handle, std::string host)
        : ssl_(SSL_new(context)), native_handle_(handle), host_(std::move(host)) {}

    ~OpenSslChannel() override {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
        }
    }

    OpenSslChannel(const OpenSslChannel &) = delete;
    OpenSslChannel &operator=(const OpenSslChannel &) = delete;

    [[nodiscard]] bool setup() {
        if (ssl_ == nullptr) {
            return false;
        }
        if (SSL_set_fd(ssl_, static_cast<int>(native_handle_)) != 1) {
            return false;
        }
        // Hostname verification is mandatory; a pinning policy can only be
        // stricter.
        auto *param = SSL_get0_param(ssl_);
        if (X509_VERIFY_PARAM_set1_host(param, host_.c_str(), host_.size()) != 1) {
            return false;
        }
        SSL_set_connect_state(ssl_);
        return true;
    }

    Result<bool> handshake(bool &want_read, bool &want_write) override {
        ERR_clear_error();
        const int status = SSL_connect(ssl_);
        if (status == 1) {
            return true;
        }
        const int reason = SSL_get_error(ssl_, status);
        want_read = reason == SSL_ERROR_WANT_READ;
        want_write = reason == SSL_ERROR_WANT_WRITE;
        if (want_read || want_write) {
            return false;
        }
        if (reason == SSL_ERROR_SSL) {
            const auto verify_result = SSL_get_verify_result(ssl_);
            if (verify_result != X509_V_OK) {
                return tls_error("tls certificate verification failed");
            }
            return tls_error("tls handshake failed");
        }
        if (reason == SSL_ERROR_SYSCALL) {
            return tls_error("tls handshake was interrupted by the transport", true);
        }
        return tls_error("tls handshake failed");
    }

    Result<std::size_t> write_some(std::span<const std::byte> bytes, bool &want_read,
                                   bool &want_write) override {
        ERR_clear_error();
        const int status = SSL_write(ssl_, bytes.data(), static_cast<int>(bytes.size()));
        if (status > 0) {
            return static_cast<std::size_t>(status);
        }
        const int reason = SSL_get_error(ssl_, status);
        want_read = reason == SSL_ERROR_WANT_READ;
        want_write = reason == SSL_ERROR_WANT_WRITE;
        if (want_read || want_write) {
            return static_cast<std::size_t>(0);
        }
        return tls_error("tls write failed", reason == SSL_ERROR_SYSCALL);
    }

    Result<std::size_t> read_some(std::span<std::byte> bytes, bool &want_read,
                                  bool &want_write) override {
        ERR_clear_error();
        const int status = SSL_read(ssl_, bytes.data(), static_cast<int>(bytes.size()));
        if (status > 0) {
            return static_cast<std::size_t>(status);
        }
        const int reason = SSL_get_error(ssl_, status);
        want_read = reason == SSL_ERROR_WANT_READ;
        want_write = reason == SSL_ERROR_WANT_WRITE;
        if (want_read || want_write) {
            return static_cast<std::size_t>(0);
        }
        if (reason == SSL_ERROR_ZERO_RETURN) {
            return static_cast<std::size_t>(0); // Clean EOF, no pending wait.
        }
        return tls_error("tls read failed", reason == SSL_ERROR_SYSCALL);
    }

    void close() override {
        if (ssl_ != nullptr) {
            // Best-effort shutdown notification; the socket close follows.
            SSL_shutdown(ssl_);
        }
    }

  private:
    SSL *ssl_ = nullptr;
    std::intptr_t native_handle_;
    std::string host_;
};

} // namespace

struct OpenSslTlsChannelFactory::Ctx final {
    SSL_CTX *context = nullptr;
};

OpenSslTlsChannelFactory::OpenSslTlsChannelFactory(std::string ca_file)
    : ctx_(std::make_unique<Ctx>()), ca_file_(std::move(ca_file)) {}

OpenSslTlsChannelFactory::~OpenSslTlsChannelFactory() {
    if (ctx_ != nullptr && ctx_->context != nullptr) {
        SSL_CTX_free(ctx_->context);
    }
}

bool OpenSslTlsChannelFactory::initialize() {
    if (ctx_->context != nullptr) {
        return true;
    }
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr) {
        return false;
    }
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    // Peer verification is structural, not an option.
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    if (!ca_file_.empty() &&
        SSL_CTX_load_verify_locations(context, ca_file_.c_str(), nullptr) != 1) {
        SSL_CTX_free(context);
        return false;
    }
    if (ca_file_.empty() && SSL_CTX_set_default_verify_paths(context) != 1) {
        // A missing system trust store is not fatal here; it fails closed at
        // verification time with a precise error.
        ERR_clear_error();
    }
    ctx_->context = context;
    return true;
}

Result<std::unique_ptr<ITlsChannel>>
OpenSslTlsChannelFactory::create(std::intptr_t native_handle, const std::string &host,
                                 const TlsOptions &options) {
    if (ctx_->context == nullptr) {
        return tls_error("tls factory was not initialized");
    }
    if (!options.verify_peer) {
        return tls_error("tls peer verification cannot be disabled");
    }
    auto channel = std::make_unique<OpenSslChannel>(ctx_->context, native_handle, host);
    if (!channel->setup()) {
        return tls_error("tls channel setup failed");
    }
    return std::unique_ptr<ITlsChannel>(std::move(channel));
}

} // namespace mira::adapters::net
