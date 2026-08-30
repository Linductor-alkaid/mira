#include "support/test.hpp"

#include <mira/runtime_baseline.hpp>

#include <chrono>

int main() {
    using namespace std::chrono_literals;

    mira::RuntimeBaseline runtime;
    MIRA_CHECK(runtime.initialize());
    MIRA_CHECK(runtime.submit({1, 1, 4, mira::BaselineCommandKind::Command}).admitted);
    MIRA_CHECK(runtime.submit({2, 1, 4, mira::BaselineCommandKind::CompleteTask}).admitted);
    MIRA_CHECK(runtime.submit({3, 1, 4, mira::BaselineCommandKind::Completion}).admitted);

    const auto first = runtime.wait(1, 2s);
    const auto terminal = runtime.wait(2, 2s);
    const auto late = runtime.wait(3, 2s);
    MIRA_CHECK(first.control_sequence == 1);
    MIRA_CHECK(terminal.control_sequence == 2);
    MIRA_CHECK(terminal.task_terminal);
    MIRA_CHECK(late.control_sequence == 3);
    MIRA_CHECK(late.code == mira::BaselineResultCode::StaleCompletionIgnored);
    MIRA_CHECK(late.task_terminal);

    MIRA_CHECK(runtime.request_shutdown());
    runtime.finish_shutdown();
    return 0;
}
