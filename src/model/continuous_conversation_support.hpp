#pragma once

// DEC-056: synchronous retained-state helpers; no executor/lifecycle owner.
#include <mira/conversation_loop.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace mira::continuous_support {

inline void add_counter(std::uint64_t &value, std::uint64_t amount = 1) {
    value = amount > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + amount;
}

inline std::string bounded_text(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xc0U) == 0x80U)
            --limit;
        text.resize(limit);
        text += "...";
    }
    return text;
}

inline std::int64_t event_counter(std::uint64_t value) {
    return static_cast<std::int64_t>(
        std::min(value, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
}

// ModelInputItem has no public standalone codec. Reuse the canonical request
// codec, measuring only its input array (artifact references, never pixels).
inline JsonValue input_json(const std::vector<ModelInputItem> &items) {
    ModelRequest request;
    request.input = items;
    const auto encoded = model_request_to_json(request);
    return *encoded.find("input");
}

inline bool complete_protocol(const std::vector<ModelInputItem> &items) {
    std::set<std::string> pending;
    for (const auto &item : items) {
        if ((item.role != ModelRole::User && item.role != ModelRole::Assistant) ||
            item.content.empty() || (item.role == ModelRole::Assistant && !pending.empty())) {
            return false;
        }
        for (const auto &part : item.content) {
            if (const auto *call = std::get_if<ToolCallPart>(&part)) {
                if (item.role != ModelRole::Assistant || call->provider_call_id.value.empty() ||
                    !pending.insert(call->provider_call_id.value).second) {
                    return false;
                }
            } else if (const auto *result = std::get_if<ToolResultPart>(&part)) {
                if (item.role != ModelRole::User ||
                    pending.erase(result->provider_call_id.value) != 1) {
                    return false;
                }
            } else if (!pending.empty() && item.role == ModelRole::User) {
                return false;
            }
        }
    }
    return pending.empty();
}

inline bool protected_tail_matches(const std::vector<ModelInputItem> &history,
                                   const std::vector<ModelInputItem> &transformed,
                                   std::size_t protected_items) {
    if (protected_items > history.size() || protected_items > transformed.size()) {
        return false;
    }
    const auto tail = [protected_items](const auto &items) {
        return input_json(std::vector<ModelInputItem>(
            items.end() - static_cast<std::ptrdiff_t>(protected_items), items.end()));
    };
    return tail(history) == tail(transformed);
}

} // namespace mira::continuous_support
