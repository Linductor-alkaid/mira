#include <mira/adapters/image/png.hpp>

#include <png.h>

#include <limits>

namespace mira::adapters::image {
namespace {
Error png_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.image.png";
    error.safe_message = message;
    return error;
}

struct PngImage final {
    png_image value{};
    ~PngImage() { png_image_free(&value); }
};
} // namespace

Result<std::vector<std::byte>> encode_rgba8_png(std::span<const std::byte> rgba,
                                                std::uint32_t width, std::uint32_t height,
                                                std::size_t stride_bytes,
                                                std::size_t max_output_bytes) {
    constexpr std::uint64_t max_pixels = 32ULL * 1024ULL * 1024ULL;
    constexpr std::size_t max_bytes = 256ULL * 1024ULL * 1024ULL;
    if (width == 0 || height == 0 || max_output_bytes == 0)
        return png_error(ErrorCode::InvalidArgument,
                         "PNG dimensions and output budget must be positive");
    if (static_cast<std::uint64_t>(width) * height > max_pixels || rgba.size() > max_bytes ||
        max_output_bytes > max_bytes)
        return png_error(ErrorCode::ResourceExhausted, "PNG encoding exceeds configured bounds");
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    if (stride_bytes < row_bytes || stride_bytes > std::numeric_limits<png_int_32>::max() ||
        row_bytes > rgba.size() || height - 1 > (rgba.size() - row_bytes) / stride_bytes)
        return png_error(ErrorCode::InvalidArgument,
                         "RGBA rows exceed the input span or stride bounds");

    PngImage image;
    image.value.version = PNG_IMAGE_VERSION;
    image.value.width = width;
    image.value.height = height;
    image.value.format = PNG_FORMAT_RGBA;
    const auto stride = static_cast<png_int_32>(stride_bytes);
    png_alloc_size_t encoded_bytes = 0;
    if (!png_image_write_to_memory(&image.value, nullptr, &encoded_bytes, 0, rgba.data(), stride,
                                   nullptr))
        return png_error(ErrorCode::Internal, "PNG size pass failed");
    if (encoded_bytes > max_output_bytes)
        return png_error(ErrorCode::ResourceExhausted, "PNG exceeds the output budget");
    std::vector<std::byte> encoded(encoded_bytes);
    if (!png_image_write_to_memory(&image.value, encoded.data(), &encoded_bytes, 0, rgba.data(),
                                   stride, nullptr))
        return png_error(ErrorCode::Internal, "PNG encoding failed");
    encoded.resize(encoded_bytes);
    return encoded;
}

} // namespace mira::adapters::image
