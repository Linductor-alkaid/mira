#include "support/test.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/environment.hpp>

#include <memory>

int main() {
    std::unique_ptr<mira::IEnvironment> environment =
        std::make_unique<mira::adapters::simulator::SimulatorEnvironment>("initial");
    const auto first = environment->observe();
    const auto second = environment->observe();
    MIRA_CHECK(first.sequence == 1);
    MIRA_CHECK(second.sequence == 2);
    MIRA_CHECK(first.content == "initial");

    const mira::InputSequence input{{"tap", "1,2"}, {"type", "redacted fixture"}};
    const auto receipt = environment->execute(input);
    MIRA_CHECK(receipt.accepted);
    MIRA_CHECK(receipt.sequence == 1);
    environment->interrupt();
    return 0;
}
