// M28 (DEC-068) idempotency ledger - gate M28-G2
// (milestone plan §6.1: a repeat inside the idempotency window produces no
// second dispatch, a repeat outside the window is an explicit expiry
// rejection, and the same content under a different input id is not a
// retry).
//
// The frozen surface pinned here:
//   - the dedup key is the host-provided input id, never a content digest;
//   - a repeat inside the window returns the existing record with
//     duplicate=true and no second side effect;
//   - a repeat outside the window is rejected with the stable
//     InteractionDomainCode::InputExpired code and is never executed as a
//     new input;
//   - the ledger index is bounded.

#include "m28_support.hpp"

#include <mira/interaction_coordinator.hpp>

#include <chrono>
#include <string>

namespace mira::m28 {

int run() {
    CoordinatorFixture fixture;
    const auto started = fixture.start();
    if (!started) {
        return 1;
    }
    const auto principal = make_principal();

    auto sink = std::make_shared<RecordingSink>();

    // --- a repeat inside the window produces no second dispatch ----------
    const auto first = fixture.submit(fixture.envelope("input-1", "open the report", principal));
    if (!first || first.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto task = first.value().task_id.value();
    const auto registered = fixture.coordinator_->register_sink(task, sink);
    if (!registered) {
        return 1;
    }

    const auto repeat = fixture.submit(fixture.envelope("input-1", "open the report", principal));
    if (!repeat) {
        return 1;
    }
    if (!repeat.value().duplicate) {
        std::cerr << "a repeat inside the window must be flagged as a duplicate\n";
        return 1;
    }
    if (repeat.value().ingress_id != first.value().ingress_id) {
        return 1;
    }
    if (repeat.value().task_id != first.value().task_id) {
        return 1;
    }
    if (repeat.value().stage != InputStage::Routed) {
        return 1;
    }
    // Exactly one delivery: the repeat produced no second dispatch.
    if (sink->deliveries.size() != 1) {
        std::cerr << "the repeat must not produce a second dispatch\n";
        return 1;
    }
    // The ledger recorded the acceptance once, not twice.
    const auto types = fixture.ledger_event_types();
    if (types.size() != 2 || types[0] != "InputAccepted" || types[1] != "InputRouted") {
        return 1;
    }

    // --- the same content under a different id is not a retry ------------
    const auto different =
        fixture.submit(fixture.envelope("input-2", "open the report", principal));
    if (!different) {
        return 1;
    }
    if (different.value().duplicate) {
        std::cerr << "a different input id is never a retry\n";
        return 1;
    }
    if (different.value().ingress_id == first.value().ingress_id) {
        return 1;
    }

    // --- a repeat outside the window is an explicit expiry rejection -----
    CoordinatorConfig expired_config;
    expired_config.idempotency_window = std::chrono::seconds(0);
    expired_config.max_ledger_entries = 8;
    CoordinatorFixture expired(expired_config);
    const auto expired_started = expired.start();
    if (!expired_started) {
        return 1;
    }
    const auto admitted = expired.submit(expired.envelope("input-1", "open the report", principal));
    if (!admitted || admitted.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto expired_repeat =
        expired.submit(expired.envelope("input-1", "open the report", principal));
    if (!expired_repeat) {
        return 1;
    }
    if (expired_repeat.value().stage != InputStage::Rejected) {
        std::cerr << "a repeat outside the window must be rejected\n";
        return 1;
    }
    if (!expired_repeat.value().error.has_value()) {
        return 1;
    }
    if (expired_repeat.value().error.value().domain_code !=
        static_cast<std::int32_t>(InteractionDomainCode::InputExpired)) {
        std::cerr << "the expiry rejection must carry the stable domain code\n";
        return 1;
    }
    // The expired record was not silently dropped and the repeat was not
    // executed: only the acceptance and the routing of the first input are
    // durable.
    const auto expired_types = expired.ledger_event_types();
    if (expired_types.size() != 2 || expired_types[0] != "InputAccepted" ||
        expired_types[1] != "InputRouted") {
        return 1;
    }

    // --- the ledger index is bounded -------------------------------------
    CoordinatorConfig bounded_config;
    bounded_config.idempotency_window = std::chrono::hours(1);
    bounded_config.max_ledger_entries = 2;
    CoordinatorFixture bounded(bounded_config);
    const auto bounded_started = bounded.start();
    if (!bounded_started) {
        return 1;
    }
    const auto one = bounded.submit(bounded.envelope("input-1", "one", principal));
    const auto two = bounded.submit(bounded.envelope("input-2", "two", principal));
    if (!one || !two) {
        return 1;
    }
    const auto three = bounded.submit(bounded.envelope("input-3", "three", principal));
    if (!three) {
        return 1;
    }
    if (three.value().stage != InputStage::Rejected) {
        std::cerr << "a full ledger must reject explicitly\n";
        return 1;
    }
    if (three.value().error.value().domain_code !=
        static_cast<std::int32_t>(InteractionDomainCode::LedgerFull)) {
        return 1;
    }

    // --- the durable record survives a rebuild ----------------------------
    InputLedgerOptions options;
    options.idempotency_window = std::chrono::hours(1);
    auto store = std::make_shared<MemoryEventStore>();
    const auto runtime_id = RuntimeId::generate();
    EventStoreInputLedger ledger(store, runtime_id, options);
    InputEnvelope envelope;
    envelope.principal = principal;
    envelope.input_id = "durable-1";
    envelope.content = "keep me";
    const auto stored = ledger.admit(envelope);
    if (!stored || stored.value().duplicate) {
        return 1;
    }
    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.session_id = SessionId::generate();
    transition.task_id = TaskId::generate();
    transition.intent = RouteIntent::NewTask;
    transition.note = "routed for the rebuild";
    const auto routed = ledger.advance("durable-1", transition);
    if (!routed) {
        return 1;
    }
    EventStoreInputLedger rebuilt(store, runtime_id, options);
    const auto replayed = rebuilt.rebuild();
    if (!replayed) {
        return 1;
    }
    const auto record = rebuilt.lookup("durable-1");
    if (!record) {
        return 1;
    }
    if (record.value().stage != InputStage::Routed) {
        std::cerr << "the rebuild must restore the stage of the input\n";
        return 1;
    }
    if (record.value().task_id != transition.task_id) {
        return 1;
    }
    if (record.value().session_id != transition.session_id) {
        return 1;
    }
    // The rebuilt ledger keeps the idempotency window: a repeat after the
    // rebuild is still a duplicate.
    const auto duplicate = rebuilt.admit(envelope);
    if (!duplicate || !duplicate.value().duplicate) {
        std::cerr << "the rebuilt ledger must keep the idempotency window\n";
        return 1;
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
