#pragma once

#include <mira/core_contracts.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace mira::context_detail {

// Private binding for the simple ErrorCode-only Context domains. Rich
// domain-code mappings remain in their existing public error factories.
struct ErrorFactory final {
    std::string_view domain;

    [[nodiscard]] Error operator()(ErrorCode code, std::string message) const {
        Error error;
        error.code = code;
        error.domain = domain;
        error.safe_message = std::move(message);
        return error;
    }
};

} // namespace mira::context_detail
