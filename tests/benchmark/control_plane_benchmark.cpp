#include "support/test.hpp"

#include <mira/runtime_baseline.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    constexpr std::uint64_t kSamples = 1'000;

    mira::RuntimeBaseline runtime({2, 32, kSamples + 4});
    MIRA_CHECK(runtime.initialize());
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t command_id = 1; command_id <= kSamples; ++command_id) {
        MIRA_CHECK(runtime.submit({command_id, command_id, 0, mira::BaselineCommandKind::Command})
                       .admitted);
    }
    for (std::uint64_t command_id = 1; command_id <= kSamples; ++command_id) {
        MIRA_CHECK(runtime.wait(command_id, 10s).code == mira::BaselineResultCode::Applied);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::cout << "samples=" << kSamples << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << '\n';
    MIRA_CHECK(runtime.request_shutdown());
    runtime.finish_shutdown();
    return 0;
}
