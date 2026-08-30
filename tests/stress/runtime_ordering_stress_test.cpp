#include "support/test.hpp"

#include <mira/runtime_baseline.hpp>

#include <chrono>
#include <cstdint>

int main() {
    using namespace std::chrono_literals;
    constexpr std::uint64_t kCommandCount = 10'000;

    mira::RuntimeBaseline runtime({2, 8, kCommandCount + 8});
    MIRA_CHECK(runtime.initialize());

    for (std::uint64_t command_id = 1; command_id <= kCommandCount; ++command_id) {
        mira::BaselineCommandKind kind = command_id % 2 == 0
                                             ? mira::BaselineCommandKind::Completion
                                             : mira::BaselineCommandKind::Command;
        if (command_id == kCommandCount - 1) {
            kind = mira::BaselineCommandKind::CompleteTask;
        }
        const auto submission = runtime.submit({command_id, 7, 11, kind});
        MIRA_CHECK(submission.admitted);
    }

    std::uint64_t prior_sequence = 0;
    for (std::uint64_t command_id = 1; command_id <= kCommandCount; ++command_id) {
        const auto result = runtime.wait(command_id, 30s);
        if (result.control_sequence != prior_sequence + 1) {
            std::cerr << "command_id=" << command_id << " expected_sequence="
                      << prior_sequence + 1 << " actual_sequence=" << result.control_sequence
                      << " code=" << static_cast<int>(result.code)
                      << " message=" << result.safe_message << '\n';
        }
        MIRA_CHECK(result.control_sequence == prior_sequence + 1);
        MIRA_CHECK(result.code == mira::BaselineResultCode::Applied ||
                   result.code == mira::BaselineResultCode::StaleCompletionIgnored);
        prior_sequence = result.control_sequence;
    }
    MIRA_CHECK(runtime.status().unobserved_results == 0);
    MIRA_CHECK(runtime.request_shutdown());
    runtime.finish_shutdown();
    MIRA_CHECK(runtime.status().state == mira::BaselineRuntimeState::Stopped);
    return 0;
}
