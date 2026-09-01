// TLS channel verification against an in-process OpenSSL server with a
// self-signed certificate. Built only when the OpenSSL adapter target
// exists; skipped (not faked) elsewhere.

#include "socket_transport.hpp"
#include "support/m3_support.hpp"
#include "support/test.hpp"
#ifdef MIRA_TEST_MBEDTLS
#include "mbedtls_tls.hpp"
#else
#include "openssl_tls.hpp"
#endif

#include <executor/executor.hpp>
#include <mira/model_contracts.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace mira;
using namespace mira::adapters::net;
using namespace mira::testing;

#ifdef MIRA_TEST_MBEDTLS
using TestTlsChannelFactory = MbedTlsChannelFactory;
#else
using TestTlsChannelFactory = OpenSslTlsChannelFactory;
#endif

// Minimal self-signed certificate generation for the test CA.
class TestCertificate final {
  public:
    TestCertificate() {
        pkey_.reset(EVP_RSA_gen(2048));
        x509_.reset(X509_new());
        if (pkey_ == nullptr || x509_ == nullptr) {
            return;
        }
        ASN1_INTEGER_set(X509_get_serialNumber(x509_.get()), 1);
        X509_gmtime_adj(X509_get_notBefore(x509_.get()), 0);
        X509_gmtime_adj(X509_get_notAfter(x509_.get()), 3600);
        X509_set_pubkey(x509_.get(), pkey_.get());
        X509_NAME *name = X509_get_subject_name(x509_.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0);
        X509_set_issuer_name(x509_.get(), name);
        X509_sign(x509_.get(), pkey_.get(), EVP_sha256());
    }

    [[nodiscard]] bool valid() const noexcept { return pkey_ != nullptr && x509_ != nullptr; }
    [[nodiscard]] EVP_PKEY *key() const noexcept { return pkey_.get(); }
    [[nodiscard]] X509 *cert() const noexcept { return x509_.get(); }

  private:
    struct PkeyDelete {
        void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
    };
    struct X509Delete {
        void operator()(X509 *p) const { X509_free(p); }
    };
    std::unique_ptr<EVP_PKEY, PkeyDelete> pkey_;
    std::unique_ptr<X509, X509Delete> x509_;
};

class TlsServer final {
  public:
    TlsServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            return;
        }
        sockaddr_in bound{};
        socklen_t size = sizeof(bound);
        if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&bound), &size) == 0) {
            port_ = ntohs(bound.sin_port);
        }
    }
    ~TlsServer() {
        if (client_ != -1) {
            ::close(client_);
        }
        if (listener_ != -1) {
            ::close(listener_);
        }
    }
    TlsServer(const TlsServer &) = delete;
    TlsServer &operator=(const TlsServer &) = delete;

    [[nodiscard]] bool valid() const noexcept { return listener_ != -1 && port_ != 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Accepts one client, handshakes, reads the request, answers with a
    // fixed body over TLS.
    [[nodiscard]] bool serve_one(const TestCertificate &certificate, std::string &seen_request,
                                 std::string *seen_connect = nullptr) {
        client_ = ::accept(listener_, nullptr, nullptr);
        if (client_ < 0) {
            return false;
        }
        if (seen_connect != nullptr) {
            char connect_buffer[4096];
            const auto connect_bytes = ::recv(client_, connect_buffer, sizeof(connect_buffer), 0);
            if (connect_bytes <= 0) {
                return false;
            }
            seen_connect->assign(connect_buffer, static_cast<std::size_t>(connect_bytes));
            const std::string established =
                "HTTP/1.1 200 Connection Established\r\nContent-Length: 0\r\n\r\n";
            if (::send(client_, established.data(), established.size(), MSG_NOSIGNAL) <= 0) {
                return false;
            }
        }
        ctx_.reset(SSL_CTX_new(TLS_server_method()));
        if (ctx_ == nullptr) {
            return false;
        }
        SSL_CTX_use_certificate(ctx_.get(), certificate.cert());
        SSL_CTX_use_PrivateKey(ctx_.get(), certificate.key());
        ssl_.reset(SSL_new(ctx_.get()));
        SSL_set_fd(ssl_.get(), client_);
        if (SSL_accept(ssl_.get()) != 1) {
            return false;
        }
        char buffer[4096];
        const int received = SSL_read(ssl_.get(), buffer, sizeof(buffer));
        if (received <= 0) {
            return false;
        }
        seen_request.assign(buffer, static_cast<std::size_t>(received));
        const std::string body = R"({"status":"completed","id":"tls"})";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        SSL_write(ssl_.get(), response.data(), static_cast<int>(response.size()));
        SSL_shutdown(ssl_.get());
        return true;
    }

  private:
    int listener_ = -1;
    int client_ = -1;
    std::uint16_t port_ = 0;
    struct CtxDelete {
        void operator()(SSL_CTX *p) const { SSL_CTX_free(p); }
    };
    struct SslDelete {
        void operator()(SSL *p) const { SSL_free(p); }
    };
    std::unique_ptr<SSL_CTX, CtxDelete> ctx_;
    std::unique_ptr<SSL, SslDelete> ssl_;
};

int happy_path_over_tls() {
    executor::Executor ex;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    ex.initialize(config);

    TestCertificate certificate;
    MIRA_CHECK(certificate.valid());
    TlsServer server;
    MIRA_CHECK(server.valid());

    // Trust anchor written to a temp PEM file for the client factory.
    std::filesystem::path ca_path = std::filesystem::temp_directory_path() /
                                    ("mira-test-ca-" + std::to_string(::getpid()) + ".pem");
    {
        FILE *file = std::fopen(ca_path.string().c_str(), "wb");
        if (file != nullptr) {
            PEM_write_X509(file, certificate.cert());
            std::fclose(file);
        }
    }

    auto factory = std::make_shared<TestTlsChannelFactory>(ca_path.string());
    MIRA_CHECK(factory->initialize());
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(ex, secrets, factory);
    MIRA_CHECK(transport.start());

    std::string seen_request;
    auto server_task = ex.submit_auto([&] { (void)server.serve_one(certificate, seen_request); });

    HttpRequest request;
    request.method = "POST";
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/v1/responses";
    request.body = R"({"model":"m"})";
    TransportLimits limits;
    limits.deadlines.connect = std::chrono::milliseconds{3'000};
    limits.deadlines.total = std::chrono::milliseconds{8'000};
    limits.allow_private_endpoints = true;
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    std::string body;
    TransportTrace trace;
    auto response = transport.execute(
        request, limits, context,
        [&](std::string_view chunk) { body.append(chunk.data(), chunk.size()); }, trace);
    server_task.get();
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(response.value().status == 200);
    MIRA_CHECK(body == R"({"status":"completed","id":"tls"})");
    MIRA_CHECK(trace.write_completed);
    MIRA_CHECK(trace.headers_received);
    MIRA_CHECK(seen_request.find("POST /v1/responses") == 0);
    std::filesystem::remove(ca_path);
    transport.shutdown();
    (void)ex.shutdown(true);
    return 0;
}

int untrusted_certificate_fails_closed() {
    executor::Executor ex;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    ex.initialize(config);

    TestCertificate server_certificate;
    MIRA_CHECK(server_certificate.valid());
    TlsServer server;
    MIRA_CHECK(server.valid());

    // The client trusts a DIFFERENT certificate: verification must fail
    // closed before any application bytes are processed.
    TestCertificate other_ca;
    std::filesystem::path ca_path = std::filesystem::temp_directory_path() /
                                    ("mira-test-other-" + std::to_string(::getpid()) + ".pem");
    {
        FILE *file = std::fopen(ca_path.string().c_str(), "wb");
        if (file != nullptr) {
            PEM_write_X509(file, other_ca.cert());
            std::fclose(file);
        }
    }
    auto factory = std::make_shared<TestTlsChannelFactory>(ca_path.string());
    MIRA_CHECK(factory->initialize());
    auto secrets = std::make_shared<MapSecretResolver>();
    SocketHttpTransport transport(ex, secrets, factory);
    MIRA_CHECK(transport.start());

    std::string seen;
    auto server_task = ex.submit_auto([&] { (void)server.serve_one(server_certificate, seen); });

    HttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/x";
    TransportLimits limits;
    limits.deadlines.connect = std::chrono::milliseconds{3'000};
    limits.deadlines.total = std::chrono::milliseconds{8'000};
    limits.allow_private_endpoints = true;
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    TransportTrace trace;
    auto response = transport.execute(request, limits, context, nullptr, trace);
    server_task.get();
    MIRA_CHECK(!response.has_value());
    MIRA_CHECK(response.error().domain == "mira.model.tls");
    MIRA_CHECK(!trace.headers_received);
    std::filesystem::remove(ca_path);
    transport.shutdown();
    (void)ex.shutdown(true);
    return 0;
}

int https_connect_proxy_then_verified_tls() {
    executor::Executor ex;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    ex.initialize(config);

    TestCertificate certificate;
    MIRA_CHECK(certificate.valid());
    TlsServer proxy;
    MIRA_CHECK(proxy.valid());
    const auto ca_path = std::filesystem::temp_directory_path() /
                         ("mira-test-proxy-ca-" + std::to_string(::getpid()) + ".pem");
    {
        FILE *file = std::fopen(ca_path.string().c_str(), "wb");
        if (file != nullptr) {
            PEM_write_X509(file, certificate.cert());
            std::fclose(file);
        }
    }
    auto factory = std::make_shared<TestTlsChannelFactory>(ca_path.string());
    MIRA_CHECK(factory->initialize());
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("proxy-auth", "Basic dGVzdDp0ZXN0");
    SocketHttpTransport transport(ex, secrets, factory);
    MIRA_CHECK(transport.start());

    std::string seen_connect;
    std::string seen_request;
    auto server_task =
        ex.submit_auto([&] { (void)proxy.serve_one(certificate, seen_request, &seen_connect); });
    HttpRequest request;
    request.method = "POST";
    request.url = "https://127.0.0.1:" + std::to_string(proxy.port()) + "/v1/responses";
    request.body = R"({"model":"m"})";
    TransportLimits limits;
    limits.deadlines.connect = std::chrono::milliseconds{3'000};
    limits.deadlines.total = std::chrono::milliseconds{8'000};
    limits.allow_private_endpoints = true;
    limits.proxy = ModelProxyConfig{"http://127.0.0.1:" + std::to_string(proxy.port()),
                                    SecretRef{"proxy-auth"},
                                    true,
                                    {"127.0.0.1"}};
    OperationContext operation;
    operation.operation = OperationId::generate();
    operation.started_at = Timestamp::now();
    TransportTrace trace;
    std::string body;
    auto response = transport.execute(
        request, limits, operation,
        [&](std::string_view chunk) { body.append(chunk.data(), chunk.size()); }, trace);
    server_task.get();
    MIRA_CHECK(response.has_value());
    MIRA_CHECK(body == R"({"status":"completed","id":"tls"})");
    MIRA_CHECK(seen_connect.find("CONNECT 127.0.0.1:") == 0);
    MIRA_CHECK(seen_connect.find("Proxy-Authorization: Basic dGVzdDp0ZXN0") != std::string::npos);
    MIRA_CHECK(seen_request.find("POST /v1/responses") == 0);
    MIRA_CHECK(seen_request.find("Proxy-Authorization") == std::string::npos);
    std::filesystem::remove(ca_path);
    transport.shutdown();
    static_cast<void>(ex.shutdown(true));
    return 0;
}

} // namespace

int main() {
    if (const int status = happy_path_over_tls(); status != 0) {
        return status;
    }
    if (const int status = untrusted_certificate_fails_closed(); status != 0) {
        return status;
    }
    if (const int status = https_connect_proxy_then_verified_tls(); status != 0) {
        return status;
    }
    return 0;
}
