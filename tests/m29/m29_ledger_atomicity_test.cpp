#include "m29_support.hpp"
#include <filesystem>

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

int main() {
    auto store = std::make_shared<FaultStore>();
    const auto runtime = RuntimeId::generate();
    EventStoreInputLedger ledger(store, runtime);
    InputEnvelope envelope;
    envelope.principal = make_principal();
    envelope.input_id = "durable";
    MIRA_CHECK(ledger.admit(envelope));
    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.task_id = TaskId::generate();
    transition.session_id = SessionId::generate();
    transition.intent = RouteIntent::NewTask;
    store->fail = true;
    MIRA_CHECK(!ledger.advance(envelope.input_id, transition));
    auto entry = ledger.lookup(envelope.input_id);
    MIRA_CHECK(entry && entry.value().stage == InputStage::Accepted && !entry.value().task_id);
    store->fail = false;
    MIRA_CHECK(ledger.advance(envelope.input_id, transition));
    auto conflicting = transition;
    conflicting.task_id = TaskId::generate();
    MIRA_CHECK(!ledger.advance(envelope.input_id, conflicting));
    MIRA_CHECK(ledger.lookup(envelope.input_id).value().task_id == transition.task_id);
    LedgerTransition answered;
    answered.stage = InputStage::Answered;
    answered.note = "report ready";
    MIRA_CHECK(ledger.advance(envelope.input_id, answered));
    LedgerTransition late;
    late.clarification = "late question";
    late.candidates = {*transition.task_id};
    MIRA_CHECK(ledger.advance(envelope.input_id, late).value().stage == InputStage::Answered);
    MIRA_CHECK(!ledger.lookup(envelope.input_id).value().clarification);

    InputEnvelope ambiguous = envelope;
    ambiguous.input_id = "ambiguous";
    MIRA_CHECK(ledger.admit(ambiguous));
    MIRA_CHECK(ledger.advance(ambiguous.input_id, late));
    EventStoreInputLedger rebuilt(store, runtime);
    MIRA_CHECK(rebuilt.rebuild());
    MIRA_CHECK(rebuilt.admit(envelope).value().duplicate);
    auto foreign = envelope;
    foreign.principal = make_principal();
    MIRA_CHECK(!rebuilt.admit(foreign));
    MIRA_CHECK(rebuilt.lookup(ambiguous.input_id).value().candidates == late.candidates);
    MIRA_CHECK(rebuilt.lookup(envelope.input_id).value().stage == InputStage::Answered);
    InputLedgerOptions bounded;
    bounded.max_entries = 1;
    EventStoreInputLedger too_small(store, runtime, bounded);
    MIRA_CHECK(!too_small.rebuild() && too_small.size() == 0);

    // An insufficient durability ack does not publish an acceptance.
    auto short_store = std::make_shared<FaultStore>();
    short_store->buffered = true;
    EventStoreInputLedger short_ledger(short_store, runtime);
    MIRA_CHECK(!short_ledger.admit(envelope));
    MIRA_CHECK(!short_ledger.lookup(envelope.input_id));

    // Actual filesystem reopen (not a simulated durability acknowledgement).
    const auto root =
        std::filesystem::temp_directory_path() / ("mira-m29-" + Id128::generate().to_string());
    {
        auto disk = std::make_shared<FileEventStore>(root);
        MIRA_CHECK(disk->recover({}));
        EventStoreInputLedger persisted(disk, runtime);
        MIRA_CHECK(persisted.admit(envelope));
        MIRA_CHECK(persisted.advance(envelope.input_id, transition));
    }
    {
        auto disk = std::make_shared<FileEventStore>(root);
        MIRA_CHECK(disk->recover({}));
        EventStoreInputLedger reopened(disk, RuntimeId::generate());
        MIRA_CHECK(reopened.rebuild());
        const auto duplicate = reopened.admit(envelope);
        MIRA_CHECK(duplicate && duplicate.value().duplicate &&
                   duplicate.value().task_id == transition.task_id);
        MIRA_CHECK(!reopened.admit(foreign));
    }
    std::filesystem::remove_all(root);

    // A failed route append cannot get ahead of its durable association.
    CoordinatorFixture fixture;
    auto durable = std::make_shared<FaultStore>();
    auto coordinator = std::make_unique<InteractionCoordinator>(
        CoordinatorConfig{}, *fixture.runtime_, fixture.executor_);
    coordinator->set_event_store(durable, runtime);
    coordinator->set_environment_provider([&]() -> Result<std::shared_ptr<IEnvironment>> {
        return std::shared_ptr<IEnvironment>(fixture.environment_);
    });
    MIRA_CHECK(coordinator->initialize());
    const auto initial =
        coordinator->submit_input(fixture.envelope("initial", "report", envelope.principal));
    MIRA_CHECK(initial && initial.value().task_id);
    auto sink = std::make_shared<FailingSink>();
    MIRA_CHECK(coordinator->register_sink(*initial.value().task_id, sink));
    coordinator->set_route_resolver(
        [&](const InputEnvelope &, const PrincipalState &) -> std::optional<RouteSuggestion> {
            durable->fail = true; // Acceptance has succeeded; route append now fails.
            return std::nullopt;
        });
    const auto failed = coordinator->submit_input(
        fixture.envelope("failed-route", "new constraint", envelope.principal));
    MIRA_CHECK(!failed);
    MIRA_CHECK(sink->ids == std::vector<std::string>{"initial"});
    const auto unchanged = coordinator->input_record("failed-route");
    MIRA_CHECK(unchanged && unchanged.value().stage == InputStage::Accepted);
    const auto state = coordinator->principal_state(envelope.principal);
    MIRA_CHECK(state && state.value().tasks.front().constraints.empty());
    MIRA_CHECK(coordinator->close());
    return 0;
}
