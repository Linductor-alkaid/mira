#include "support/controlled_provider.hpp"
#include "support/deterministic_id.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_environment.hpp"
#include "support/test.hpp"

#include <chrono>

int main() {
    mira::test::FakeClock clock;
    clock.advance(std::chrono::milliseconds(25));
    MIRA_CHECK(clock.now().time_since_epoch() == std::chrono::milliseconds(25));

    mira::test::DeterministicIdGenerator ids(41);
    MIRA_CHECK(ids.next() == 41);
    MIRA_CHECK(ids.next() == 42);

    mira::test::ControlledProvider provider;
    provider.push("controlled");
    MIRA_CHECK(provider.invoke() == "controlled");

    mira::test::FakeEnvironment environment;
    mira::ObservationRequest request;
    mira::OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    const auto observed = environment.observe(request, context);
    MIRA_CHECK(observed.has_value());
    mira::InputSequence input;
    input.events.push_back(mira::InputEvent{"tap", "0.5,0.5"});
    const auto receipt = environment.execute(input, context);
    MIRA_CHECK(receipt.has_value());
    MIRA_CHECK(receipt.value().status == mira::ExecutionStatus::Completed);
    MIRA_CHECK(environment.executed_event_count() == 1);
    MIRA_CHECK(environment.interrupt(mira::make_control_context()).has_value());
    MIRA_CHECK(environment.interrupted());
    return 0;
}
