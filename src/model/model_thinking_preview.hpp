#pragma once

#include <cstddef>
#include <mira/model_sse.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace mira::detail {
// Keep the newest complete code points without allocating an oversized
// intermediate preview. The canonical response budget is independent.
inline std::size_t append_thinking_preview(std::string &buffer, std::string_view delta,
                                           std::size_t limit) {
    if (delta.empty())
        return 0;
    auto boundary = [](std::string_view text, std::size_t position) {
        while (position < text.size() &&
               (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U)
            ++position;
        return position;
    };
    if (delta.size() > limit) {
        const auto begin = boundary(delta, delta.size() - limit);
        buffer.assign(delta.substr(begin));
        return 1;
    }
    if (buffer.size() > limit - delta.size()) {
        const auto erase = boundary(buffer, buffer.size() - (limit - delta.size()));
        buffer.erase(0, erase);
        buffer.append(delta);
        return 1;
    }
    buffer.append(delta);
    return 0;
}
inline UnvalidatedModelThinkingPreview take_thinking_preview(std::string &buffer,
                                                             std::size_t &drops) {
    const auto loss = std::exchange(drops, 0);
    return {std::exchange(buffer, {}), loss, loss != 0};
}
} // namespace mira::detail
