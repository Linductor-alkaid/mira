#pragma once

// Private deterministic ASCII primitives shared by Core policy consumers.
#include <string>
#include <string_view>
#include <vector>

namespace mira::text_support {

[[nodiscard]] inline std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char &character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// Callers validate policy before scanning. Empty markers are invalid policy;
// the primitive itself does not treat an empty string as a content match.
[[nodiscard]] inline bool carries_marker(std::string_view text,
                                         const std::vector<std::string> &markers) {
    const std::string lowered = to_lower_ascii(text);
    for (const auto &marker : markers) {
        if (!marker.empty() && lowered.find(to_lower_ascii(marker)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace mira::text_support
