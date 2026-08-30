#include "support/test.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/environment.hpp>

#include <memory>

namespace {

using mira::ErrorCode;
using mira::ObservationRequest;
using mira::OperationContext;

OperationContext request_context() {
    OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    return context;
}

int check_request_driven_observe() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::single_display()};

    ObservationRequest request;
    request.required.screen = true;
    request.required.structure = true;

    const auto first = environment.observe(request, request_context());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(!first.value().id.is_nil());
    MIRA_CHECK(first.value().screen.has_value());
    MIRA_CHECK(first.value().structure.has_value());
    MIRA_CHECK(environment.observation_count() == 1);

    // A required component the environment cannot deliver fails the request
    // instead of returning a silently incomplete observation.
    ObservationRequest impossible = request;
    impossible.required.perception = 2;
    const auto rejected = environment.observe(impossible, request_context());
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::UnsupportedCapability);
    return 0;
}

int check_capabilities_and_execute() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::dual_display()};
    const auto capabilities = environment.capabilities();
    MIRA_CHECK(capabilities.screen_capture);
    MIRA_CHECK(capabilities.ui_tree);
    MIRA_CHECK(capabilities.discrete_input);
    MIRA_CHECK(capabilities.epoch_invalidation);

    const auto topology = environment.topology();
    MIRA_CHECK(topology.has_value());
    MIRA_CHECK(topology.value().displays.size() == 2);

    mira::InputSequence sequence;
    sequence.events.push_back(mira::InputEvent{"tap", "0.5,0.5"});
    sequence.events.push_back(mira::InputEvent{"type", "redacted fixture"});
    const auto receipt = environment.execute(sequence, request_context());
    MIRA_CHECK(receipt.has_value());
    MIRA_CHECK(receipt.value().status == mira::ExecutionStatus::Completed);
    MIRA_CHECK(!receipt.value().safe_message.empty());
    MIRA_CHECK(environment.executed_inputs().size() == 1);

    // Malformed canonical coordinates are rejected, never guessed.
    mira::InputSequence malformed;
    malformed.events.push_back(mira::InputEvent{"tap", "2.5"});
    const auto refused = environment.execute(malformed, request_context());
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int check_interrupt_and_cancellation() {
    mira::adapters::simulator::SimulatorEnvironment environment;
    MIRA_CHECK(environment.interrupt(mira::make_control_context()).has_value());
    MIRA_CHECK(environment.interrupted());
    // Interrupt is idempotent.
    MIRA_CHECK(environment.interrupt(mira::make_control_context()).has_value());

    bool cancelled = false;
    OperationContext context = request_context();
    context.cancellation_requested = [&cancelled]() noexcept { return cancelled; };
    ObservationRequest request;
    request.required.screen = true;
    cancelled = true;
    const auto observed = environment.observe(request, context);
    MIRA_CHECK(!observed.has_value());
    MIRA_CHECK(observed.error().code == ErrorCode::Cancelled);
    return 0;
}

} // namespace

int main() {
    if (const int code = check_request_driven_observe(); code != 0)
        return code;
    if (const int code = check_capabilities_and_execute(); code != 0)
        return code;
    if (const int code = check_interrupt_and_cancellation(); code != 0)
        return code;
    return 0;
}
