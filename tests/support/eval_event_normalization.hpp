#pragma once

// Test-only canonicalization for the recorded eval G4 gate (DEC-034).
#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mira::testing {

[[nodiscard]] inline bool is_hex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

// G4 normalization (profile §9.1): Id128 identifiers are generated through
// std::random_device and content digests embed those ids (profile digests,
// for one), so distinct tokens of 32/64 hex digits or the 8-4-4-4-12 UUID
// shape map to first-occurrence ordinals. Equal inputs still compare equal
// and unequal ones still diverge; sequence numbers, epochs, enum names and
// bounded counters compare verbatim.
[[nodiscard]] inline std::string normalize_identifiers(const std::string &payload,
                                                       std::map<std::string, int> &ids) {
    std::string output;
    output.reserve(payload.size());
    std::size_t index = 0;
    while (index < payload.size()) {
        const char character = payload[index];
        if (!is_hex(character)) {
            output.push_back(character);
            ++index;
            continue;
        }
        std::size_t end = index;
        while (end < payload.size() && is_hex(payload[end])) {
            ++end;
        }
        const std::size_t length = end - index;
        const bool token_shape =
            length == 32 || length == 64 ||
            (length == 36 && payload[index + 8] == '-' && payload[index + 13] == '-' &&
             payload[index + 18] == '-' && payload[index + 23] == '-');
        if (token_shape) {
            const std::string token = payload.substr(index, length);
            const auto inserted = ids.emplace(token, static_cast<int>(ids.size()));
            output += "<id" + std::to_string(inserted.first->second) + ">";
        } else {
            output += payload.substr(index, length);
        }
        index = end;
    }
    return output;
}

using EvalEventPayload = std::pair<std::string, std::string>;

// Workflow audit and drive events may interleave. Assign identity ordinals
// only after stabilizing their traversal by event type, preserving order
// within each producer's event type. Sorting after ID assignment is too late:
// a legal interleaving can already have changed the identity map.
[[nodiscard]] inline std::vector<std::string>
normalize_eval_events(std::vector<EvalEventPayload> events, bool concurrent) {
    if (concurrent) {
        std::stable_sort(events.begin(), events.end(), [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
    }
    std::map<std::string, int> ids;
    std::vector<std::string> normalized;
    normalized.reserve(events.size());
    for (const auto &[type, payload] : events) {
        normalized.push_back(type + "|" + normalize_identifiers(payload, ids));
    }
    if (concurrent) {
        std::sort(normalized.begin(), normalized.end());
    }
    return normalized;
}

[[nodiscard]] inline std::string
first_eval_event_difference(const std::vector<std::string> &baseline,
                            const std::vector<std::string> &repeat) {
    std::size_t index = 0;
    while (index < baseline.size() && index < repeat.size() && baseline[index] == repeat[index]) {
        ++index;
    }
    if (index == baseline.size() && index == repeat.size())
        return {};
    const auto clip = [index](const std::vector<std::string> &events) {
        return index < events.size() ? events[index].substr(0, 300) : std::string("<end>");
    };
    return "event " + std::to_string(index) + " baseline [" + clip(baseline) + "] repeat [" +
           clip(repeat) + "]";
}

} // namespace mira::testing
