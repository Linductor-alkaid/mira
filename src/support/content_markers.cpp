#include <mira/content_markers.hpp>

#include "ascii_text.hpp"

#include <iterator>
#include <string_view>

namespace mira {

bool matches_content_marker(std::string_view text, const std::vector<std::string> &markers) {
    return text_support::carries_marker(text, markers);
}
namespace {
constexpr std::string_view kForbidden[] = {
    "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};
constexpr std::string_view kInjection[] = {
    "ignore previous", "disregard previous", "you are now",
    "system:",         "new instructions:",  "override policy"};
} // namespace

std::vector<std::string> default_forbidden_content_markers() {
    return {std::begin(kForbidden), std::end(kForbidden)};
}

std::vector<std::string> default_injection_content_markers() {
    return {std::begin(kInjection), std::end(kInjection)};
}

} // namespace mira
