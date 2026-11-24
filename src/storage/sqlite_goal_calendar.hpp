#pragma once

#include <mira/goal_calendar.hpp>

namespace mira::storage {

[[nodiscard]] std::string encode_goal_calendar(const GoalCalendarRule &rule);
[[nodiscard]] Result<GoalCalendarRule> decode_goal_calendar(std::string_view document);

} // namespace mira::storage
