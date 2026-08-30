#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/version.hpp>

#include <chrono>

int main() {
    mira::adapters::simulator::SimulatorEnvironment environment("ready");
    if (environment.observe().content != "ready") {
        return 1;
    }

    mira::RuntimeBaseline runtime;
    if (!runtime.initialize()) {
        return 2;
    }
    const auto submission = runtime.submit({1, 1, 0, mira::BaselineCommandKind::Command});
    if (!submission.admitted) {
        return 3;
    }
    const auto result = runtime.wait(1, std::chrono::seconds(2));
    if (!runtime.request_shutdown()) {
        return 5;
    }
    runtime.finish_shutdown();
    return result.code == mira::BaselineResultCode::Applied ? 0 : 4;
}
