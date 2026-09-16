#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/tool_module.hpp>
#include <mira/version.hpp>

#include <chrono>
#include <vector>

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
    const int baseline_status = result.code == mira::BaselineResultCode::Applied ? 0 : 6;
    if (baseline_status != 0) {
        return baseline_status;
    }

    // M7-TM0-G6 consumer closure: the new public tool module header must be
    // includable and linkable from a minimal external consumer.
    const mira::CapabilityCatalog &catalog = mira::CapabilityCatalog::core();
    const mira::CapabilityDescriptor *capability = catalog.find("env.screen.capture");
    if (capability == nullptr || capability->kind != mira::CapabilityKind::Boolean) {
        return 7;
    }
    const std::vector<mira::ModuleSnapshot> active;
    const mira::ModuleNegotiationResult negotiation =
        mira::negotiate_modules(active, mira::EnvironmentCapabilities{}, catalog);
    if (!negotiation.modules.empty()) {
        return 7;
    }
    return 0;
}
