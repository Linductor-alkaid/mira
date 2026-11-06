#include <mira/core_contracts.hpp>

namespace mira {
namespace {

// One table for diagnostics and recovery. Names are persisted; changing an
// existing spelling requires a versioned compatibility decision (DEC-079).
constexpr std::pair<TaskState, std::string_view> kTaskStateNames[] = {
    {TaskState::Idle, "Idle"},
    {TaskState::Observing, "Observing"},
    {TaskState::Reasoning, "Reasoning"},
    {TaskState::Planning, "Planning"},
    {TaskState::Acting, "Acting"},
    {TaskState::Verifying, "Verifying"},
    {TaskState::Recovering, "Recovering"},
    {TaskState::Pausing, "Pausing"},
    {TaskState::Paused, "Paused"},
    {TaskState::TakeoverSettling, "TakeoverSettling"},
    {TaskState::SuspendedForTakeover, "SuspendedForTakeover"},
    {TaskState::Cancelling, "Cancelling"},
    {TaskState::Completed, "Completed"},
    {TaskState::Failed, "Failed"},
    {TaskState::Cancelled, "Cancelled"},
};

} // namespace

std::string_view task_state_name(TaskState state) noexcept {
    for (const auto &[value, name] : kTaskStateNames) {
        if (value == state) {
            return name;
        }
    }
    return "Unknown";
}

std::optional<TaskState> task_state_from_name(std::string_view name) noexcept {
    for (const auto &[value, spelling] : kTaskStateNames) {
        if (spelling == name) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace mira
