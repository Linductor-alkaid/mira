#include "support/test.hpp"
#include <mira/core_contracts.hpp>

int main() {
    using namespace mira;
    constexpr std::string_view names[] = {
        "Idle",
        "Observing",
        "Reasoning",
        "Planning",
        "Acting",
        "Verifying",
        "Recovering",
        "Pausing",
        "Paused",
        "TakeoverSettling",
        "SuspendedForTakeover",
        "Cancelling",
        "Completed",
        "Failed",
        "Cancelled",
    };
    // Pin every persisted spelling, not merely agreement between two helpers.
    for (unsigned value = 0; value <= 255; ++value) {
        const auto state = static_cast<TaskState>(value);
        if (value < std::size(names)) {
            MIRA_CHECK(task_state_name(state) == names[value]);
            MIRA_CHECK(task_state_from_name(names[value]) == state);
        } else {
            MIRA_CHECK(task_state_name(state) == "Unknown");
            MIRA_CHECK(!task_state_from_name(task_state_name(state)));
        }
    }
    for (auto invalid : {"", "idle", "IDLE", "Unknown", " Idle", "Idle ", "Completed\n"}) {
        MIRA_CHECK(!task_state_from_name(invalid));
    }
    MIRA_CHECK(!task_state_from_name(std::string_view{"Idle\0suffix", 11}));
    return 0;
}
