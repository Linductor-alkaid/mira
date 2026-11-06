#pragma once

#include <mira/json.hpp>
#include <mira/task_checkpoint.hpp>

namespace mira::checkpoint_wire {
[[nodiscard]] inline JsonValue event_ids_to_json(const std::vector<EventId> &ids) {
    JsonValue::Array array;
    for (const auto &id : ids) {
        array.emplace_back(id.to_string());
    }
    return JsonValue(std::move(array));
}

[[nodiscard]] inline std::vector<EventId> event_ids_from_json(const JsonValue &json) {
    std::vector<EventId> ids;
    if (json.is_array()) {
        for (const auto &entry : *json.as_array()) {
            if (auto parsed = EventId::parse(entry.as_string() ? *entry.as_string() : ""); parsed) {
                ids.emplace_back(*parsed);
            }
        }
    }
    return ids;
}

[[nodiscard]] inline JsonValue fact_to_json(const VerifiedFact &fact) {
    JsonValue::Object object;
    object.emplace_back("key", fact.key);
    object.emplace_back("value", fact.value);
    object.emplace_back("provenance", event_ids_to_json(fact.provenance));
    if (fact.valid_from)
        object.emplace_back("valid_from", wall_nanos(*fact.valid_from));
    if (fact.valid_until)
        object.emplace_back("valid_until", wall_nanos(*fact.valid_until));
    return JsonValue(std::move(object));
}

[[nodiscard]] inline VerifiedFact fact_from(const JsonValue &json) {
    VerifiedFact fact;
    if (const auto *key = json.find("key"); key != nullptr && key->is_string()) {
        fact.key = *key->as_string();
    }
    if (const auto *value = json.find("value"); value != nullptr && value->is_string()) {
        fact.value = *value->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        fact.provenance = event_ids_from_json(*provenance);
    }
    for (auto [key, target] :
         {std::pair{"valid_from", &fact.valid_from}, std::pair{"valid_until", &fact.valid_until}}) {
        if (const auto *value = json.find(key); value != nullptr) {
            // A malformed bound must fail validation, never become unbounded.
            *target = value->is_integer()
                          ? WallTimePoint{std::chrono::nanoseconds(*value->as_integer())}
                          : WallTimePoint{};
        }
    }
    return fact;
}

[[nodiscard]] inline bool fact_validity_valid(const VerifiedFact &fact, SchemaVersion schema) {
    return fact.valid_from.has_value() == fact.valid_until.has_value() &&
           (!fact.valid_from ||
            (schema.major >= 2 && fact.valid_from->time_since_epoch().count() > 0 &&
             *fact.valid_until > *fact.valid_from));
}

} // namespace mira::checkpoint_wire
