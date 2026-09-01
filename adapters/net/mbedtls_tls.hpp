#pragma once

// Cross-platform Mbed TLS channel for POSIX, Winsock and Android NDK sockets.
// The adapter uses caller-owned nonblocking sockets and creates no threads;
// all waits remain in SocketHttpTransport's Executor blocking worker.

#include <mira/model_transport.hpp>

#include <memory>
#include <string>

namespace mira::adapters::net {

class MbedTlsChannelFactory final : public ITlsChannelFactory {
  public:
    // A PEM CA bundle is mandatory because Mbed TLS intentionally does not
    // depend on platform-specific system trust-store APIs.
    explicit MbedTlsChannelFactory(std::string ca_file);
    ~MbedTlsChannelFactory() override;

    MbedTlsChannelFactory(const MbedTlsChannelFactory &) = delete;
    MbedTlsChannelFactory &operator=(const MbedTlsChannelFactory &) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] Result<std::unique_ptr<ITlsChannel>> create(std::intptr_t native_handle,
                                                              const std::string &host,
                                                              const TlsOptions &options) override;

  private:
    std::string ca_file_;
    bool initialized_ = false;
};

} // namespace mira::adapters::net
