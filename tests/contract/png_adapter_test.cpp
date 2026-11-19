#include "support/test.hpp"

#include <mira/adapters/image/png.hpp>

#include <png.h>

#include <array>

int main() {
    using mira::adapters::image::encode_rgba8_png;
    constexpr std::array<unsigned char, 12> rows{255, 0, 0, 128, 61, 62, 63, 64, 0, 255, 0, 255};
    const auto bytes = std::as_bytes(std::span(rows));
    const auto encoded = encode_rgba8_png(bytes, 1, 2, 8, 256);
    MIRA_CHECK(encoded);
    MIRA_CHECK(!encode_rgba8_png(bytes, 1, 2, 8, 8));
    MIRA_CHECK(!encode_rgba8_png(bytes.first(11), 1, 2, 8, 256));
    MIRA_CHECK(!encode_rgba8_png(bytes, 1, 2, 0, 256));
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    MIRA_CHECK(
        png_image_begin_read_from_memory(&image, encoded.value().data(), encoded.value().size()));
    image.format = PNG_FORMAT_RGBA;
    std::array<unsigned char, 8> decoded{};
    const bool ok = png_image_finish_read(&image, nullptr, decoded.data(), 0, nullptr) != 0;
    png_image_free(&image);
    MIRA_CHECK(ok);
    MIRA_CHECK((decoded == std::array<unsigned char, 8>{255, 0, 0, 128, 0, 255, 0, 255}));
    return 0;
}
