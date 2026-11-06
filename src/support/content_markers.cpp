#include <mira/content_markers.hpp>

#include <iterator>
#include <string_view>

namespace mira {
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
