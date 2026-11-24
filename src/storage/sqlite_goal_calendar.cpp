#include "sqlite_goal_calendar.hpp"

#include <mira/json.hpp>

#include <utility>

namespace mira::storage {
namespace {

Error invalid_calendar_document() {
    Error error;
    error.code = ErrorCode::DataLoss;
    error.domain = "mira.goal_store";
    error.safe_message = "goal calendar document is invalid";
    return error;
}

std::optional<std::int64_t> integer_member(const JsonValue &document, const char *name) {
    const auto *value = document.find(name);
    return value ? value->as_integer() : std::nullopt;
}

GoalTime time_from(std::int64_t value) { return GoalTime{std::chrono::milliseconds{value}}; }

} // namespace

std::string encode_goal_calendar(const GoalCalendarRule &rule) {
    JsonValue::Array transitions;
    for (const auto &transition : rule.transitions) {
        transitions.emplace_back(
            JsonValue::Array{static_cast<std::int64_t>(transition.at.time_since_epoch().count()),
                             static_cast<std::int64_t>(transition.offset.count())});
    }
    return canonical_json_string(JsonValue::Object{
        {"zone", rule.time_zone},
        {"version", rule.rules_version},
        {"from", static_cast<std::int64_t>(rule.valid_from.time_since_epoch().count())},
        {"until", static_cast<std::int64_t>(rule.valid_until.time_since_epoch().count())},
        {"initial_offset", static_cast<std::int64_t>(rule.initial_offset.count())},
        {"transitions", std::move(transitions)},
        {"minute", static_cast<std::int64_t>(rule.local_time_of_day.count())},
        {"weekdays", static_cast<std::int64_t>(rule.weekdays)}});
}

Result<GoalCalendarRule> decode_goal_calendar(std::string_view document) {
    if (document.size() > 8192)
        return invalid_calendar_document();
    const auto parsed = parse_json(document);
    if (!parsed)
        return invalid_calendar_document();
    const auto &value = parsed.value();
    const auto *zone = value.find("zone");
    const auto *version = value.find("version");
    const auto *transitions = value.find("transitions");
    const auto from = integer_member(value, "from");
    const auto until = integer_member(value, "until");
    const auto initial_offset = integer_member(value, "initial_offset");
    const auto minute = integer_member(value, "minute");
    const auto weekdays = integer_member(value, "weekdays");
    if (!zone || !zone->as_string() || !version || !version->as_string() || !from || !until ||
        !initial_offset || !minute || !weekdays || *weekdays < 1 || *weekdays > 127 ||
        !transitions || !transitions->as_array() || transitions->as_array()->size() > 128)
        return invalid_calendar_document();
    GoalCalendarRule result{*zone->as_string(),
                            *version->as_string(),
                            time_from(*from),
                            time_from(*until),
                            std::chrono::minutes{*initial_offset},
                            {},
                            std::chrono::minutes{*minute},
                            static_cast<std::uint8_t>(*weekdays)};
    for (const auto &transition : *transitions->as_array()) {
        const auto *pair = transition.as_array();
        if (!pair || pair->size() != 2)
            return invalid_calendar_document();
        const auto at = pair->front().as_integer();
        const auto offset = pair->back().as_integer();
        if (!at || !offset)
            return invalid_calendar_document();
        result.transitions.push_back({time_from(*at), std::chrono::minutes{*offset}});
    }
    if (!validate_goal_calendar(result))
        return invalid_calendar_document();
    return result;
}

} // namespace mira::storage
