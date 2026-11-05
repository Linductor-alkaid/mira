#pragma once

#include <mira/core_contracts.hpp>

#include <string>
#include <utility>

namespace mira::tool_module_support {

[[nodiscard]] inline Error make_module_error(ErrorCode code, std::string safe_message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_module";
    error.safe_message = std::move(safe_message);
    return error;
}

} // namespace mira::tool_module_support
