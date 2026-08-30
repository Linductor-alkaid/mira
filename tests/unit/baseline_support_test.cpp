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

    mira::test::FakeEnvironment environment("frame");
    MIRA_CHECK(environment.observe().content == "frame");
    const mira::InputSequence input{{"tap", "10,20"}};
    MIRA_CHECK(environment.execute(input).accepted);
    MIRA_CHECK(environment.actions().size() == 1);
    environment.interrupt();
    MIRA_CHECK(environment.interrupted());
    return 0;
}
