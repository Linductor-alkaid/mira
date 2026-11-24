#include <mira/goal_calendar.hpp>

#include <algorithm>
#include <utility>

namespace mira {
namespace {
using namespace std::chrono;

constexpr auto kOffsetLimit = hours{24};
constexpr auto kCoverageLimit = days{5 * 366};
constexpr auto kLatestRuleTime = sys_days{year{2200} / 1 / 1};

Error calendar_error(std::string message) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.goal_calendar";
    error.safe_message = std::move(message);
    return error;
}

bool valid_label(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '-' || c == '_' || c == '+' || c == '.';
    });
}

bool valid_offset(minutes offset) { return offset >= -kOffsetLimit && offset <= kOffsetLimit; }

std::optional<GoalTime> earliest_utc(const GoalCalendarRule &rule, GoalTime local) {
    auto start = rule.valid_from;
    auto offset = rule.initial_offset;
    for (const auto &transition : rule.transitions) {
        const auto candidate = local - offset;
        if (candidate >= start && candidate < transition.at)
            return candidate;
        start = transition.at;
        offset = transition.offset;
    }
    const auto candidate = local - offset;
    if (candidate >= start && candidate < rule.valid_until)
        return candidate;
    return std::nullopt;
}
} // namespace

Result<void> validate_goal_calendar(const GoalCalendarRule &rule) {
    if (!valid_label(rule.time_zone) || !valid_label(rule.rules_version) ||
        rule.valid_from < GoalTime{} || rule.valid_until > kLatestRuleTime ||
        rule.valid_until <= rule.valid_from ||
        rule.valid_until - rule.valid_from > kCoverageLimit || rule.transitions.size() > 128 ||
        !valid_offset(rule.initial_offset) || rule.local_time_of_day < minutes{0} ||
        rule.local_time_of_day >= days{1} || rule.weekdays == 0 || rule.weekdays > 0x7f)
        return calendar_error("invalid goal calendar rule");
    auto previous = rule.valid_from;
    for (const auto &transition : rule.transitions) {
        if (transition.at <= previous || transition.at >= rule.valid_until ||
            !valid_offset(transition.offset))
            return calendar_error("invalid goal calendar offset transition");
        previous = transition.at;
    }
    return {};
}

Result<std::optional<GoalTime>> next_goal_calendar_occurrence(const GoalCalendarRule &rule,
                                                              GoalTime not_before) {
    if (auto valid = validate_goal_calendar(rule); !valid)
        return valid.error();
    if (not_before >= rule.valid_until)
        return std::optional<GoalTime>{};
    // Include the largest possible overlap in the snapshot so a fold's
    // earlier UTC interpretation cannot fall outside the known rules.
    if (not_before < rule.valid_from + 2 * kOffsetLimit)
        return calendar_error("goal calendar needs two days of preceding offset coverage");
    const auto last = floor<days>(rule.valid_until + kOffsetLimit);
    std::optional<GoalTime> best;
    for (auto date = floor<days>(not_before - kOffsetLimit); date <= last; date += days{1}) {
        const GoalTime local = date + rule.local_time_of_day;
        if (best && local - kOffsetLimit >= *best)
            break;
        const auto bit = 1U << (weekday{date}.iso_encoding() - 1U);
        if ((rule.weekdays & bit) == 0U)
            continue;
        const auto candidate = earliest_utc(rule, local);
        if (candidate && *candidate >= not_before && (!best || *candidate < *best))
            best = candidate;
    }
    return best;
}

} // namespace mira
