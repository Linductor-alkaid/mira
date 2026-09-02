// Cross-platform Mbed TLS contract. The server uses the pinned Mbed TLS
// dependency too, so Windows can execute the client contract without an
// OpenSSL development package.

#include "mbedtls_tls.hpp"
#include "socket_transport.hpp"
#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <executor/executor.hpp>
#include <mira/model_contracts.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <test/test_certs.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace mira;
using namespace mira::adapters::net;
using namespace mira::testing;

constexpr char kServerCertificate[] = TEST_SRV_CRT_RSA_SHA256_PEM;
constexpr char kServerKey[] = TEST_SRV_KEY_RSA_PEM;
constexpr char kTrustedCa[] = TEST_CA_CRT_RSA_SHA256_PEM;
constexpr char kOtherCa[] = TEST_CA_CRT_EC_PEM;

class TemporaryCaFile final {
  public:
    TemporaryCaFile(const std::string &name, const std::string_view contents)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        valid_ = output.good();
    }
    ~TemporaryCaFile() { std::filesystem::remove(path_); }
    TemporaryCaFile(const TemporaryCaFile &) = delete;
    TemporaryCaFile &operator=(const TemporaryCaFile &) = delete;
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
    bool valid_ = false;
};

int socket_send(void *context, const unsigned char *buffer, std::size_t size) {
    const auto socket = *static_cast<TestSocket *>(context);
    const auto bounded = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef _WIN32
    const int status =
        ::send(socket, reinterpret_cast<const char *>(buffer), static_cast<int>(bounded), 0);
    if (status == SOCKET_ERROR) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
#else
    const auto status = ::send(socket, buffer, bounded, MSG_NOSIGNAL);
    if (status < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
#endif
    return static_cast<int>(status);
}

int socket_receive(void *context, unsigned char *buffer, std::size_t size) {
    const auto socket = *static_cast<TestSocket *>(context);
    const auto bounded = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef _WIN32
    const int status =
        ::recv(socket, reinterpret_cast<char *>(buffer), static_cast<int>(bounded), 0);
    if (status == SOCKET_ERROR) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
#else
    const auto status = ::recv(socket, buffer, bounded, 0);
    if (status < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
#endif
    return static_cast<int>(status);
}

class MbedTlsServer final {
  public:
    MbedTlsServer() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_x509_crt_init(&certificate_);
        mbedtls_pk_init(&key_);
        mbedtls_ssl_config_init(&configuration_);
        mbedtls_ssl_init(&ssl_);
    }

    ~MbedTlsServer() {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&configuration_);
        mbedtls_pk_free(&key_);
        mbedtls_x509_crt_free(&certificate_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
    }

    MbedTlsServer(const MbedTlsServer &) = delete;
    MbedTlsServer &operator=(const MbedTlsServer &) = delete;

    [[nodiscard]] bool initialize() {
        constexpr unsigned char personalization[] = "mira-m3-portable-tls-server";
        if (mbedtls_ctr_drbg_seed(&random_, mbedtls_entropy_func, &entropy_, personalization,
                                  sizeof(personalization) - 1) != 0 ||
            mbedtls_x509_crt_parse(&certificate_,
                                   reinterpret_cast<const unsigned char *>(kServerCertificate),
                                   sizeof(kServerCertificate)) != 0 ||
            mbedtls_pk_parse_key(&key_, reinterpret_cast<const unsigned char *>(kServerKey),
                                 sizeof(kServerKey), nullptr, 0, mbedtls_ctr_drbg_random,
                                 &random_) != 0 ||
            mbedtls_ssl_config_defaults(&configuration_, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        mbedtls_ssl_conf_min_tls_version(&configuration_, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_rng(&configuration_, mbedtls_ctr_drbg_random, &random_);
        mbedtls_ssl_conf_authmode(&configuration_, MBEDTLS_SSL_VERIFY_NONE);
        if (mbedtls_ssl_conf_own_cert(&configuration_, &certificate_, &key_) != 0 ||
            mbedtls_ssl_setup(&ssl_, &configuration_) != 0) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool serve(ScriptedHttpServer &server, std::string *connect_request = nullptr) {
        if (!server.accept_client(std::chrono::seconds{5})) {
            return false;
        }
        if (connect_request != nullptr) {
            *connect_request = server.read_request(std::chrono::seconds{2}, false);
            server.write_all("HTTP/1.1 200 Connection Established\r\nContent-Length: 0\r\n\r\n");
        }
        socket_ = server.native_client();
        mbedtls_ssl_set_bio(&ssl_, &socket_, socket_send, socket_receive, nullptr);
        int status = 0;
        do {
            status = mbedtls_ssl_handshake(&ssl_);
        } while (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (status != 0) {
            return false;
        }

        std::string request;
        unsigned char buffer[4096];
        do {
            status = mbedtls_ssl_read(&ssl_, buffer, sizeof(buffer));
            if (status > 0) {
                request.append(reinterpret_cast<const char *>(buffer),
                               static_cast<std::size_t>(status));
            }
        } while (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (request.find("POST /v1/responses") != 0) {
            return false;
        }
        const std::string body = R"({"status":"completed","id":"tls"})";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::size_t written = 0;
        while (written < response.size()) {
            status = mbedtls_ssl_write(
                &ssl_, reinterpret_cast<const unsigned char *>(response.data() + written),
                response.size() - written);
            if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (status <= 0) {
                return false;
            }
            written += static_cast<std::size_t>(status);
        }
        static_cast<void>(mbedtls_ssl_close_notify(&ssl_));
        return true;
    }

  private:
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context random_{};
    mbedtls_x509_crt certificate_{};
    mbedtls_pk_context key_{};
    mbedtls_ssl_config configuration_{};
    mbedtls_ssl_context ssl_{};
    TestSocket socket_ = MIRA_TEST_INVALID_SOCKET;
};

int run_verified_request(bool through_proxy) {
    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    executor.initialize(config);

    ScriptedHttpServer socket_server;
    MIRA_CHECK(socket_server.valid());
    MbedTlsServer tls_server;
    MIRA_CHECK(tls_server.initialize());
    std::string connect_request;
    auto server_task = executor.submit_auto([&] {
        return tls_server.serve(socket_server, through_proxy ? &connect_request : nullptr);
    });

    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("proxy-auth", "Basic dGVzdDp0ZXN0");
    TemporaryCaFile ca_file("mira-m3-mbedtls-ca.pem", kTrustedCa);
    MIRA_CHECK(ca_file.valid());
    auto factory = std::make_shared<MbedTlsChannelFactory>(ca_file.path().string());
    MIRA_CHECK(factory->initialize());
    SocketHttpTransport transport(executor, secrets, factory);
    MIRA_CHECK(transport.start());

    HttpRequest request;
    request.method = "POST";
    request.url = "https://localhost:" + std::to_string(socket_server.port()) + "/v1/responses";
    request.body = R"({"model":"m"})";
    TransportLimits limits;
    limits.allow_private_endpoints = true;
    limits.deadlines.connect = std::chrono::seconds{3};
    limits.deadlines.total = std::chrono::seconds{8};
    if (through_proxy) {
        limits.proxy = ModelProxyConfig{"http://127.0.0.1:" + std::to_string(socket_server.port()),
                                        SecretRef{"proxy-auth"},
                                        true,
                                        {"127.0.0.1"}};
    }
    OperationContext operation;
    operation.operation = OperationId::generate();
    operation.started_at = Timestamp::now();
    std::string body;
    TransportTrace trace;
    auto result = transport.execute(
        request, limits, operation,
        [&](std::string_view chunk) { body.append(chunk.data(), chunk.size()); }, trace);
    MIRA_CHECK(server_task.get());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().status == 200);
    MIRA_CHECK(body == R"({"status":"completed","id":"tls"})");
    MIRA_CHECK(trace.write_completed && trace.headers_received);
    if (through_proxy) {
        MIRA_CHECK(connect_request.find("CONNECT localhost:") == 0);
        MIRA_CHECK(connect_request.find("Proxy-Authorization: Basic dGVzdDp0ZXN0") !=
                   std::string::npos);
    }
    transport.shutdown();
    static_cast<void>(executor.shutdown(true));
    return 0;
}

int wrong_ca_fails_closed() {
    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    executor.initialize(config);

    ScriptedHttpServer socket_server;
    MbedTlsServer tls_server;
    MIRA_CHECK(socket_server.valid() && tls_server.initialize());
    auto server_task = executor.submit_auto([&] { return tls_server.serve(socket_server); });
    TemporaryCaFile ca_file("mira-m3-mbedtls-other-ca.pem", kOtherCa);
    MIRA_CHECK(ca_file.valid());
    auto factory = std::make_shared<MbedTlsChannelFactory>(ca_file.path().string());
    MIRA_CHECK(factory->initialize());
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(executor, secrets, factory);
    MIRA_CHECK(transport.start());

    HttpRequest request;
    request.url = "https://localhost:" + std::to_string(socket_server.port()) + "/x";
    TransportLimits limits;
    limits.allow_private_endpoints = true;
    limits.deadlines.connect = std::chrono::seconds{3};
    limits.deadlines.total = std::chrono::seconds{8};
    OperationContext operation;
    operation.operation = OperationId::generate();
    operation.started_at = Timestamp::now();
    TransportTrace trace;
    auto result = transport.execute(request, limits, operation, nullptr, trace);
    MIRA_CHECK(!server_task.get());
    MIRA_CHECK(!result.has_value());
    MIRA_CHECK(result.error().domain == "mira.model.tls");
    MIRA_CHECK(!trace.headers_received);
    transport.shutdown();
    static_cast<void>(executor.shutdown(true));
    return 0;
}

} // namespace

int main() {
    if (const int status = run_verified_request(false); status != 0) {
        return status;
    }
    if (const int status = run_verified_request(true); status != 0) {
        return status;
    }
    return wrong_ca_fails_closed();
}
