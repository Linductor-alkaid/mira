#pragma once

#include <cstdint>

namespace mira {

struct Version final {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

inline constexpr Version kVersion{0, 1, 0};

} // namespace mira
