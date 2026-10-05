#pragma once

// Internal to the model layer: small helpers shared by the device agent loop
// and the conversational loop.

#include <mira/model_contracts.hpp>

#include <string>

namespace mira::loop_support {

// Rate limits, overload and transport failures are recoverable within a
// bounded recovery budget; permission, policy and request-shape failures are
// not. Kept in one place so both loops classify identically.
[[nodiscard]] inline bool recoverable_model_failure(const Error &error) {
    if (!error.retryable) {
        return false;
    }
    if (error.domain != "mira.model") {
        return true;
    }
    return error.domain_code == static_cast<std::int32_t>(ModelDomainCode::RateLimited) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::TransportFailed);
}

} // namespace mira::loop_support
