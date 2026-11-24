#pragma once

#include <mira/core_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

using GoalTime = std::chrono::sys_time<std::chrono::milliseconds>;

struct GoalZoneTransition final {
    GoalTime at{};
    std::chrono::minutes offset{};
    friend bool operator==(const GoalZoneTransition &, const GoalZoneTransition &) = default;
};

// All offsets apply from the listed UTC instant, and the snapshot covers
// [valid_from, valid_until). Weekday bit zero represents Monday.
struct GoalCalendarRule final {
    std::string time_zone;
    std::string rules_version;
    GoalTime valid_from{};
    GoalTime valid_until{};
    std::chrono::minutes initial_offset{};
    std::vector<GoalZoneTransition> transitions;
    std::chrono::minutes local_time_of_day{};
    std::uint8_t weekdays = 0x7f;
    friend bool operator==(const GoalCalendarRule &, const GoalCalendarRule &) = default;
};

[[nodiscard]] Result<void> validate_goal_calendar(const GoalCalendarRule &rule);
// Inclusive UTC lower bound. A gap is skipped; a fold uses its earlier UTC
// interpretation. An empty successful result means rules coverage exhausted.
[[nodiscard]] Result<std::optional<GoalTime>>
next_goal_calendar_occurrence(const GoalCalendarRule &rule, GoalTime not_before);

} // namespace mira
