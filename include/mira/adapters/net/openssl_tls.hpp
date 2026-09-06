#pragma once

// OpenSSL-backed TLS channel factory for the socket transport. This is a
// platform adapter built only where OpenSSL development libraries are
// available; profiles targeting other platforms must inject their own
// ITlsChannelFactory or fail closed on https endpoints.

#include <mira/model_transport.hpp>

#include <memory>
#include <string>

namespace mira::adapters::net {

// Creates client TLS channels with peer and hostname verification enabled;
// verification cannot be disabled through TlsOptions.
class OpenSslTlsChannelFactory final : public ITlsChannelFactory {
  public:
    // `ca_file` seeds an additional trust anchor (for example a test CA) on
    // top of the system default store.
    explicit OpenSslTlsChannelFactory(std::string ca_file = std::string());
    ~OpenSslTlsChannelFactory() override;

    OpenSslTlsChannelFactory(const OpenSslTlsChannelFactory &) = delete;
    OpenSslTlsChannelFactory &operator=(const OpenSslTlsChannelFactory &) = delete;

    // Loads the trust store; must succeed once before create().
    [[nodiscard]] bool initialize();

    [[nodiscard]] Result<std::unique_ptr<ITlsChannel>>
    create(std::intptr_t native_handle, const std::string &host,
           const TlsOptions &options) override;

  private:
    struct Ctx;
    std::unique_ptr<Ctx> ctx_;
    std::string ca_file_;
};

} // namespace mira::adapters::net
