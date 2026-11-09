#include "m29_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

int main() {
    CoordinatorFixture fixture;
    MIRA_CHECK(fixture.start());
    const auto principal = make_principal();
    const auto first = fixture.submit(fixture.envelope("one", "report", principal));
    MIRA_CHECK(first && first.value().task_id);
    const auto task = *first.value().task_id;
    MIRA_CHECK(fixture.submit(fixture.envelope("two", "appendix", principal)));
    MIRA_CHECK(fixture.submit(fixture.envelope("three", "latest totals", principal)));
    auto sink = std::make_shared<FailingSink>();
    sink->blocked = "two";
    sink->fail = true;
    MIRA_CHECK(!fixture.coordinator_->register_sink(task, sink));
    MIRA_CHECK(sink->ids == std::vector<std::string>{"one"});
    // A newly submitted input cannot bypass the failed head through the bound sink.
    MIRA_CHECK(fixture.submit(fixture.envelope("four", "compare", principal)));
    MIRA_CHECK(sink->ids == std::vector<std::string>{"one"});
    sink->fail = false;
    MIRA_CHECK(fixture.coordinator_->register_sink(task, sink));
    MIRA_CHECK(sink->ids == std::vector<std::string>({"one", "two", "three", "four"}));
    MIRA_CHECK(fixture.coordinator_->register_sink(task, sink));
    MIRA_CHECK(sink->ids.size() == 4);
    sink->blocked = "refused";
    sink->fail = true;
    const auto refused =
        fixture.submit(fixture.envelope("refused", "must not become a constraint", principal));
    MIRA_CHECK(refused && refused.value().stage == InputStage::Rejected);
    auto state = fixture.coordinator_->principal_state(principal);
    MIRA_CHECK(state && state.value().tasks.front().constraints.size() == 3);
    sink->fail = false;
    MIRA_CHECK(fixture.coordinator_->register_sink(task, sink));
    MIRA_CHECK(sink->ids.size() == 4);

    // A throwing sink may already have accepted the item: stop blind retry.
    MIRA_CHECK(fixture.coordinator_->unregister_sink(task));
    MIRA_CHECK(fixture.submit(fixture.envelope("uncertain", "pending instruction", principal)));
    MIRA_CHECK(fixture.submit(fixture.envelope("tail", "keep this too", principal)));
    sink->blocked = "uncertain";
    sink->throwing = true;
    const auto uncertain = fixture.coordinator_->register_sink(task, sink);
    MIRA_CHECK(!uncertain &&
               uncertain.error().domain_code ==
                   static_cast<std::int32_t>(InteractionDomainCode::DeliveryUncertain));
    const auto attempts = sink->attempts;
    MIRA_CHECK(!fixture.coordinator_->register_sink(task, sink));
    MIRA_CHECK(sink->attempts == attempts);
    const auto diagnostic = fixture.coordinator_->input_record("uncertain");
    MIRA_CHECK(diagnostic && diagnostic.value().stage == InputStage::Routed &&
               diagnostic.value().error);
    EventStoreInputLedger rebuilt(fixture.events_, RuntimeId::generate());
    MIRA_CHECK(rebuilt.rebuild());
    MIRA_CHECK(rebuilt.lookup("uncertain").value().error->domain_code ==
               static_cast<std::int32_t>(InteractionDomainCode::DeliveryUncertain));
    const auto blocked = fixture.submit(fixture.envelope("blocked", "new input", principal));
    MIRA_CHECK(blocked && blocked.value().stage == InputStage::Rejected);
    MIRA_CHECK(blocked.value().error->safe_message.find("private") == std::string::npos);

    CoordinatorConfig config;
    config.max_pending_deliveries = 2;
    CoordinatorFixture bounded(config);
    MIRA_CHECK(bounded.start());
    const auto initial = bounded.submit(bounded.envelope("one", "report", principal));
    MIRA_CHECK(initial);
    MIRA_CHECK(bounded.submit(bounded.envelope("two", "appendix", principal)));
    const auto full = bounded.submit(bounded.envelope("three", "excess", principal));
    MIRA_CHECK(full && full.value().stage == InputStage::Rejected);
    auto ready = std::make_shared<FailingSink>();
    MIRA_CHECK(bounded.coordinator_->register_sink(*initial.value().task_id, ready));
    MIRA_CHECK(ready->ids == std::vector<std::string>({"one", "two"}));
    MIRA_CHECK(!bounded.coordinator_->register_sink(TaskId::generate(), ready));
    return 0;
}
