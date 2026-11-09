#include "m29_support.hpp"

using namespace mira;
using namespace mira::m28;

int main() {
    // Queue pressure causes a bounded wait failure; a late preparation owns
    // values only and close consumes its future before releasing the owner.
    CoordinatorConfig config;
    config.command_timeout = std::chrono::milliseconds(10);
    config.max_retired_preparations = 1;
    CoordinatorFixture host(config);
    MIRA_CHECK(host.start());
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::promise<void> entered_one;
    std::promise<void> entered_two;
    auto one_ready = entered_one.get_future();
    auto two_ready = entered_two.get_future();
    auto first = host.executor_.submit_auto([&] {
        entered_one.set_value();
        gate.wait();
    });
    auto second = host.executor_.submit_auto([&] {
        entered_two.set_value();
        gate.wait();
    });
    // Always release the test blockers before an assertion can return.
    const bool ready = one_ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                       two_ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const auto principal = make_principal();
    const auto timed_out = host.submit(host.envelope("timeout", "bounded preparation", principal));
    const auto full = host.submit(host.envelope("full", "bounded retirement", principal));
    release.set_value();
    first.get();
    second.get();
    MIRA_CHECK(ready);
    MIRA_CHECK(timed_out && timed_out.value().stage == InputStage::Rejected);
    MIRA_CHECK(timed_out.value().error->domain_code ==
               static_cast<std::int32_t>(InteractionDomainCode::RuntimeCommandTimeout));
    MIRA_CHECK(full && full.value().error->domain_code ==
                           static_cast<std::int32_t>(InteractionDomainCode::ExecutorRejected));
    MIRA_CHECK(host.coordinator_->close());
    MIRA_CHECK(host.coordinator_->principal_state(principal).value().tasks.empty());

    CoordinatorFixture failures;
    MIRA_CHECK(failures.start());
    failures.resolver = [](const InputEnvelope &,
                           const PrincipalState &) -> std::optional<RouteSuggestion> {
        throw std::runtime_error("secret exception details");
    };
    const auto failed = failures.submit(failures.envelope("resolver", "test", principal));
    MIRA_CHECK(failed && failed.value().stage == InputStage::Rejected);
    MIRA_CHECK(failed.value().error->safe_message.find("secret") == std::string::npos);
    failures.resolver = {};
    failures.coordinator_->set_environment_provider([]() -> Result<std::shared_ptr<IEnvironment>> {
        throw std::runtime_error("secret environment details");
    });
    const auto unavailable = failures.submit(failures.envelope("environment", "test", principal));
    MIRA_CHECK(unavailable && unavailable.value().stage == InputStage::Rejected);
    MIRA_CHECK(unavailable.value().error->safe_message.find("secret") == std::string::npos);
    return 0;
}
