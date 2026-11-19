#pragma once

#include <mira/core_contracts.hpp>

#include <span>
#include <vector>

namespace mira::adapters::image {

// Encode caller-authorized, top-down straight-alpha RGBA8 rows. Row stride is
// in bytes and may include padding. The adapter holds no worker or artifact
// store. Link Mira::png_adapter and publish the resulting bytes as image/png.
[[nodiscard]] Result<std::vector<std::byte>>
encode_rgba8_png(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height,
                 std::size_t stride_bytes, std::size_t max_output_bytes);

} // namespace mira::adapters::image
