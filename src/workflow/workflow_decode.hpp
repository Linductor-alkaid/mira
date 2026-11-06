#pragma once

// Private to workflow. Schema-specific error codes and prefixes stay with
// the caller; only the fail-closed scan and byte bound are shared (DEC-079).
#include <mira/json.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mira::workflow_detail {

template <typename Code, typename ErrorFactory>
[[nodiscard]] Result<void>
check_keys(const JsonValue &object, std::span<const std::string_view> required,
           std::span<const std::string_view> optional, std::string_view where, Code invalid_shape,
           Code unknown_field, ErrorFactory make_error) {
    if (!object.is_object()) {
        return make_error(invalid_shape, std::string{where} + " must be an object");
    }
    for (const auto &member : *object.as_object()) {
        const bool known = std::any_of(required.begin(), required.end(),
                                       [&](std::string_view key) { return key == member.first; }) ||
                           std::any_of(optional.begin(), optional.end(),
                                       [&](std::string_view key) { return key == member.first; });
        if (!known) {
            return make_error(unknown_field,
                              std::string{where} + ": unknown field '" + member.first + "'");
        }
    }
    for (const auto key : required) {
        if (object.find(key) == nullptr) {
            return make_error(invalid_shape,
                              std::string{where} + ": missing field '" + std::string{key} + "'");
        }
    }
    return Result<void>{};
}

[[nodiscard]] inline std::optional<std::string> bounded_string(const JsonValue &value,
                                                               std::size_t max_bytes) {
    const auto *text = value.as_string();
    if (text == nullptr || text->empty() || text->size() > max_bytes) {
        return std::nullopt;
    }
    return *text;
}

} // namespace mira::workflow_detail
