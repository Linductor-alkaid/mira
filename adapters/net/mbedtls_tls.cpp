#include "mbedtls_tls.hpp"

#include <mira/model_contracts.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#else
#include <cerrno>
#include <sys/socket.h>
#endif

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

class MbedTlsChannel final : public ITlsChannel {
  public:
    MbedTlsChannel(std::intptr_t handle, std::string host, std::string ca_file)
        : native_handle_(handle), host_(std::move(host)), ca_file_(std::move(ca_file)) {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_entropy_init(&entropy_);
    }

    ~MbedTlsChannel() override {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    [[nodiscard]] bool setup() {
        static constexpr unsigned char kPersonalization[] = "mira-mbedtls-channel-v1";
        if (mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, kPersonalization,
                                  sizeof(kPersonalization) - 1) != 0) {
            return false;
        }
        if (mbedtls_x509_crt_parse_file(&ca_, ca_file_.c_str()) != 0) {
            return false;
        }
        if (mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        mbedtls_ssl_conf_min_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config_, &ca_, nullptr);
        mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &drbg_);
        if (mbedtls_ssl_setup(&ssl_, &config_) != 0 ||
            mbedtls_ssl_set_hostname(&ssl_, host_.c_str()) != 0) {
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, this, &MbedTlsChannel::send_callback,
                            &MbedTlsChannel::receive_callback, nullptr);
        return true;
    }

    Result<bool> handshake(bool &want_read, bool &want_write) override {
        const int status = mbedtls_ssl_handshake(&ssl_);
        if (status == 0) {
            if (mbedtls_ssl_get_verify_result(&ssl_) != 0) {
                return tls_error("tls certificate verification failed");
            }
            return true;
        }
        if (set_wants(status, want_read, want_write)) {
            return false;
        }
        if (status == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            return tls_error("tls certificate verification failed");
        }
        return tls_error("tls handshake failed", is_transport_failure(status));
    }

    Result<std::size_t> write_some(std::span<const std::byte> bytes, bool &want_read,
                                   bool &want_write) override {
        const auto count = std::min<std::size_t>(bytes.size(), MBEDTLS_SSL_OUT_CONTENT_LEN);
        const int status =
            mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char *>(bytes.data()), count);
        if (status > 0) {
            return static_cast<std::size_t>(status);
        }
        if (set_wants(status, want_read, want_write)) {
            return static_cast<std::size_t>(0);
        }
        return tls_error("tls write failed", is_transport_failure(status));
    }

    Result<std::size_t> read_some(std::span<std::byte> bytes, bool &want_read,
                                  bool &want_write) override {
        const int status =
            mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char *>(bytes.data()), bytes.size());
        if (status > 0) {
            return static_cast<std::size_t>(status);
        }
        if (status == 0 || status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return static_cast<std::size_t>(0);
        }
        if (set_wants(status, want_read, want_write)) {
            return static_cast<std::size_t>(0);
        }
        return tls_error("tls read failed", is_transport_failure(status));
    }

    void close() override { static_cast<void>(mbedtls_ssl_close_notify(&ssl_)); }

  private:
    [[nodiscard]] static bool set_wants(int status, bool &want_read, bool &want_write) {
        want_read = status == MBEDTLS_ERR_SSL_WANT_READ;
        want_write = status == MBEDTLS_ERR_SSL_WANT_WRITE;
        return want_read || want_write;
    }

    [[nodiscard]] static bool is_transport_failure(int status) {
        return status == MBEDTLS_ERR_NET_SEND_FAILED || status == MBEDTLS_ERR_NET_RECV_FAILED ||
               status == MBEDTLS_ERR_NET_CONN_RESET;
    }

    static int send_callback(void *context, const unsigned char *data, std::size_t size) {
        auto &channel = *static_cast<MbedTlsChannel *>(context);
#ifdef _WIN32
        const int status = ::send(static_cast<SOCKET>(channel.native_handle_),
                                  reinterpret_cast<const char *>(data),
                                  static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
        if (status == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            return error == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                           : MBEDTLS_ERR_NET_SEND_FAILED;
        }
#else
        const auto status =
            ::send(static_cast<int>(channel.native_handle_), data, size, MSG_NOSIGNAL);
        if (status < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                           : MBEDTLS_ERR_NET_SEND_FAILED;
        }
#endif
        return static_cast<int>(status);
    }

    static int receive_callback(void *context, unsigned char *data, std::size_t size) {
        auto &channel = *static_cast<MbedTlsChannel *>(context);
#ifdef _WIN32
        const int status =
            ::recv(static_cast<SOCKET>(channel.native_handle_), reinterpret_cast<char *>(data),
                   static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
        if (status == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            return error == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ
                                           : MBEDTLS_ERR_NET_RECV_FAILED;
        }
#else
        const auto status = ::recv(static_cast<int>(channel.native_handle_), data, size, 0);
        if (status < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ
                                                           : MBEDTLS_ERR_NET_RECV_FAILED;
        }
#endif
        return static_cast<int>(status);
    }

    std::intptr_t native_handle_;
    std::string host_;
    std::string ca_file_;
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config config_{};
    mbedtls_x509_crt ca_{};
    mbedtls_ctr_drbg_context drbg_{};
    mbedtls_entropy_context entropy_{};
};

} // namespace

MbedTlsChannelFactory::MbedTlsChannelFactory(std::string ca_file) : ca_file_(std::move(ca_file)) {}

MbedTlsChannelFactory::~MbedTlsChannelFactory() = default;

bool MbedTlsChannelFactory::initialize() {
    if (ca_file_.empty()) {
        return false;
    }
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    const bool valid = mbedtls_x509_crt_parse_file(&certificate, ca_file_.c_str()) == 0;
    mbedtls_x509_crt_free(&certificate);
    initialized_ = valid;
    return valid;
}

Result<std::unique_ptr<ITlsChannel>> MbedTlsChannelFactory::create(std::intptr_t native_handle,
                                                                   const std::string &host,
                                                                   const TlsOptions &options) {
    if (!initialized_) {
        return tls_error("tls factory was not initialized");
    }
    if (!options.verify_peer) {
        return tls_error("tls peer verification cannot be disabled");
    }
    auto channel = std::make_unique<MbedTlsChannel>(native_handle, host, ca_file_);
    if (!channel->setup()) {
        return tls_error("tls channel setup failed");
    }
    return std::unique_ptr<ITlsChannel>(std::move(channel));
}

} // namespace mira::adapters::net
