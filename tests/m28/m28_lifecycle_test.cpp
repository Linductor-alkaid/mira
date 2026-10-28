// M28 (DEC-068) Executor discipline and lifecycle - gate M28-G6
// (milestone plan §6.1: cancellation, timeout, shutdown, backpressure and
// submission rejection each have a settlement path, no handle stays
// unresolved and no failure is dropped silently).
//
// The frozen surface pinned here:
//   - the coordinator owns no Executor of its own: the injected one is used
//     and the host keeps the ownership;
//   - a refused submission is an explicit, visible failure;
//   - a refused delivery is an explicit, visible failure;
//   - the coordinator stops accepting inputs when it shuts down;
//   - the emergency stop settles through the runtime command plane.

#include "m28_support.hpp"

#include <mira/interaction_coordinator.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace mira::m28 {

int run() {
    // --- a refused delivery is visible, never silently dropped -----------
    {
        CoordinatorFixture fixture;
        if (!fixture.start()) {
            return 1;
        }
        const auto principal = make_principal();
        const auto submitted =
            fixture.submit(fixture.envelope("input-1", "open the report", principal));
        if (!submitted) {
            return 1;
        }
        auto sink = std::make_shared<RecordingSink>();
        const auto registered =
            fixture.coordinator_->register_sink(submitted.value().task_id.value(), sink);
        if (!registered) {
            return 1;
        }
        // The sink refuses the next delivery: the input must fail visibly.
        sink->refuse = true;
        const auto refused =
            fixture.submit(fixture.envelope("input-2", "and the summary", principal));
        if (!refused) {
            return 1;
        }
        if (refused.value().stage != InputStage::Rejected) {
            std::cerr << "a refused delivery must be an explicit rejection\n";
            return 1;
        }
        if (!refused.value().error.has_value()) {
            return 1;
        }
        if (refused.value().error.value().domain_code !=
            static_cast<std::int32_t>(InteractionDomainCode::SinkUnavailable)) {
            return 1;
        }
        // The failure is durable: the record of the input carries it.
        const auto record = fixture.coordinator_->input_record("input-2");
        if (!record || record.value().stage != InputStage::Rejected) {
            return 1;
        }
        if (!record.value().error.has_value()) {
            return 1;
        }
    }

    // --- a refused submission is visible ----------------------------------
    {
        CoordinatorFixture fixture;
        if (!fixture.start()) {
            return 1;
        }
        const auto principal = make_principal();
        // The executor stops under the coordinator: the context preparation
        // can no longer be submitted and the input must fail visibly.
        (void)fixture.executor_.shutdown(true);
        const auto refused =
            fixture.submit(fixture.envelope("input-1", "open the report", principal));
        if (!refused) {
            return 1;
        }
        if (refused.value().stage != InputStage::Rejected) {
            std::cerr << "a refused submission must be an explicit rejection\n";
            return 1;
        }
        if (!refused.value().error.has_value()) {
            return 1;
        }
        if (refused.value().error.value().domain != std::string(kInteractionErrorDomain)) {
            return 1;
        }
    }

    // --- the coordinator stops accepting inputs when it shuts down -------
    {
        CoordinatorFixture fixture;
        if (!fixture.start()) {
            return 1;
        }
        const auto principal = make_principal();
        fixture.coordinator_->shutdown();
        const auto refused = fixture.submit(fixture.envelope("input-1", "too late", principal));
        if (refused.has_value()) {
            std::cerr << "a shut down coordinator must refuse new inputs\n";
            return 1;
        }
        if (refused.error().domain_code !=
            static_cast<std::int32_t>(InteractionDomainCode::RouteRejected)) {
            return 1;
        }
    }

    // --- the emergency stop settles through the command plane --------------
    {
        CoordinatorFixture fixture;
        if (!fixture.start()) {
            return 1;
        }
        const auto principal = make_principal();
        const auto submitted =
            fixture.submit(fixture.envelope("input-1", "open the report", principal));
        if (!submitted) {
            return 1;
        }
        const auto task = submitted.value().task_id.value();
        const auto command = fixture.coordinator_->cancel_work(task);
        if (!command) {
            return 1;
        }
        const auto receipt = command.value().receipt(std::chrono::seconds(2));
        if (!receipt || receipt.value().status != ReceiptStatus::Accepted) {
            return 1;
        }
        const auto outcome = command.value().outcome(std::chrono::seconds(2));
        if (!outcome || outcome.value().status != SettlementStatus::Applied) {
            return 1;
        }
        const auto snapshot = fixture.runtime_->task_snapshot(task);
        if (!snapshot || snapshot.value().state != TaskState::Cancelled) {
            return 1;
        }
        // The terminal state is idempotent: a second stop is a no-op and the
        // task never comes back.
        const auto again = fixture.coordinator_->cancel_work(task);
        if (!again) {
            return 1;
        }
        const auto second_outcome = again.value().outcome(std::chrono::seconds(2));
        if (!second_outcome || second_outcome.value().status != SettlementStatus::NoOp) {
            return 1;
        }
        const auto still = fixture.runtime_->task_snapshot(task);
        if (!still || still.value().state != TaskState::Cancelled) {
            return 1;
        }
    }

    // --- the executor outlives the coordinator with no unresolved handle --
    {
        CoordinatorFixture fixture;
        if (!fixture.start()) {
            return 1;
        }
        const auto principal = make_principal();
        for (int index = 0; index < 8; ++index) {
            const auto id = "input-" + std::to_string(index);
            const auto submitted = fixture.submit(fixture.envelope(id, "work", principal));
            if (!submitted) {
                return 1;
            }
        }
        // The coordinator releases its own state before the executor stops.
        fixture.coordinator_->shutdown();
        const auto shutdown = fixture.executor_.shutdown(true);
        if (shutdown != kairo::ShutdownResult::Completed) {
            std::cerr << "the executor must stop cleanly after the coordinator\n";
            return 1;
        }
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
