#pragma once

// Private net Adapter policy. Backend TLS setup and I/O stay in each backend.
#include <mira/model_transport.hpp>

#include <memory>
#include <string>
#include <utility>

namespace mira::adapters::net::tls_support {

[[nodiscard]] inline Error tls_error(std::string message, bool retryable = false) {
    Error error;
    error.code = retryable ? ErrorCode::Unavailable : ErrorCode::PermissionDenied;
    error.domain = "mira.model.tls";
    error.safe_message = std::move(message);
    error.retryable = retryable;
    return error;
}

template <typename MakeChannel>
[[nodiscard]] Result<std::unique_ptr<ITlsChannel>>
create_verified_channel(bool initialized, const TlsOptions &options, MakeChannel make_channel) {
    if (!initialized) {
        return tls_error("tls factory was not initialized");
    }
    if (!options.verify_peer) {
        return tls_error("tls peer verification cannot be disabled");
    }
    auto channel = make_channel();
    if (!channel->setup()) {
        return tls_error("tls channel setup failed");
    }
    return std::unique_ptr<ITlsChannel>(std::move(channel));
}

} // namespace mira::adapters::net::tls_support
