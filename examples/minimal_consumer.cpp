#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/version.hpp>

#include <chrono>

int main() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::single_display()};

    mira::ObservationRequest request;
    request.required.screen = true;
    mira::OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    const auto observation = environment.observe(request, context);
    if (!observation.has_value() || !observation.value().screen.has_value()) {
        return 1;
    }

    mira::InputSequence sequence;
    sequence.events.push_back(mira::InputEvent{"tap", "0.5,0.5"});
    const auto receipt = environment.execute(sequence, context);
    if (!receipt.has_value() || receipt.value().status != mira::ExecutionStatus::Completed) {
        return 2;
    }

    mira::RuntimeBaseline runtime;
    if (!runtime.initialize()) {
        return 3;
    }
    const auto submission = runtime.submit({1, 1, 0, mira::BaselineCommandKind::Command});
    if (!submission.admitted) {
        return 4;
    }
    const auto result = runtime.wait(1, std::chrono::seconds(2));
    if (!runtime.request_shutdown()) {
        return 5;
    }
    runtime.finish_shutdown();
    return result.code == mira::BaselineResultCode::Applied ? 0 : 6;
}
