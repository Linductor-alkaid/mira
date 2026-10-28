// M28 (DEC-068) deterministic routing decision - gate M28-G3
// (milestone plan §6.1: the eight RouteIntent values each have a reproducible
// case, a terminal task is never revived, an ambiguous reference asks a
// clarifying question instead of failing, and a missing capability fails
// closed).
//
// The frozen surface pinned here:
//   - the closed RouteIntent set and its names;
//   - the decision is deterministic: the same state and the same input
//     always produce the same intent;
//   - a host suggestion is only adopted after it is validated against the
//     principal state (DEC-068 R1);
//   - a terminal task becomes ResumeTask or NewTask, never a revival (R2);
//   - an ambiguous reference is a clarification, not an error code (R4);
//   - the emergency stop never depends on a model routing round (R6);
//   - the permission check fails closed (R9).

#include "m28_support.hpp"

#include <mira/interaction_coordinator.hpp>

#include <string>
#include <vector>

namespace mira::m28 {
namespace {

[[nodiscard]] bool intent_names_are_frozen() {
    return route_intent_name(RouteIntent::ContinueCurrent) == "ContinueCurrent" &&
           route_intent_name(RouteIntent::SupplementTask) == "SupplementTask" &&
           route_intent_name(RouteIntent::NewTask) == "NewTask" &&
           route_intent_name(RouteIntent::ResumeTask) == "ResumeTask" &&
           route_intent_name(RouteIntent::PauseTask) == "PauseTask" &&
           route_intent_name(RouteIntent::CancelTask) == "CancelTask" &&
           route_intent_name(RouteIntent::Clarify) == "Clarify" &&
           route_intent_name(RouteIntent::Reject) == "Reject";
}

} // namespace

int run() {
    if (!intent_names_are_frozen()) {
        return 1;
    }

    CoordinatorFixture fixture;
    const auto started = fixture.start();
    if (!started) {
        return 1;
    }
    const auto principal = make_principal();

    // --- NewTask: the principal has no live work yet ---------------------
    const auto created =
        fixture.submit(fixture.envelope("input-1", "write the summary", principal));
    if (!created || created.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto first_task = created.value().task_id.value();
    if (fixture.runtime_->task_snapshot(first_task).value().state != TaskState::Idle) {
        return 1;
    }

    // --- ContinueCurrent: exactly one live task --------------------------
    const auto continued =
        fixture.submit(fixture.envelope("input-2", "make it shorter", principal));
    if (!continued || continued.value().stage != InputStage::Routed) {
        return 1;
    }
    if (continued.value().task_id.value() != first_task) {
        std::cerr << "the single live task must receive the continuation\n";
        return 1;
    }

    // --- SupplementTask: a validated host suggestion ---------------------
    fixture.resolver = [](const InputEnvelope &,
                          const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::SupplementTask;
        return suggestion;
    };
    // The suggestion names no task, so it does not survive the validation and
    // the structural rule decides again.
    const auto supplemented =
        fixture.submit(fixture.envelope("input-3", "also include the numbers", principal));
    if (!supplemented || supplemented.value().task_id.value() != first_task) {
        return 1;
    }

    // --- the deterministic decision is reproducible -----------------------
    const auto before = fixture.coordinator_->principal_state(principal);
    const auto first_state = before.value();
    const auto repeated =
        fixture.submit(fixture.envelope("input-4", "make it shorter again", principal));
    if (!repeated || repeated.value().task_id.value() != first_task) {
        return 1;
    }
    const auto after = fixture.coordinator_->principal_state(principal);
    if (after.value().tasks.size() != first_state.tasks.size()) {
        return 1;
    }

    // --- NewTask by suggestion: a second live task -----------------------
    fixture.resolver = [](const InputEnvelope &,
                          const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::NewTask;
        suggestion.rationale = "the host starts a separate piece of work";
        return suggestion;
    };
    const auto second =
        fixture.submit(fixture.envelope("input-5", "draft the cover letter", principal));
    if (!second || second.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto second_task = second.value().task_id.value();
    if (second_task == first_task) {
        return 1;
    }

    // --- Clarify: two live tasks make the reference ambiguous -------------
    fixture.resolver = nullptr;
    const auto ambiguous = fixture.submit(fixture.envelope("input-6", "stop that", principal));
    if (!ambiguous) {
        return 1;
    }
    if (ambiguous.value().stage == InputStage::Rejected) {
        std::cerr << "an ambiguous reference is a clarification, not a rejection\n";
        return 1;
    }
    if (!ambiguous.value().clarification.has_value()) {
        std::cerr << "an ambiguous reference must carry a clarification\n";
        return 1;
    }
    if (ambiguous.value().error.has_value()) {
        return 1;
    }
    // The clarification is a normal reply: the input is still accepted.
    if (ambiguous.value().stage != InputStage::Accepted) {
        return 1;
    }

    // --- PauseTask: the deterministic control path ------------------------
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::PauseTask;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto paused = fixture.submit(fixture.envelope("input-7", "hold on", principal));
    if (!paused || paused.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto paused_state = fixture.runtime_->task_snapshot(first_task);
    if (!paused_state || paused_state.value().state != TaskState::Paused) {
        std::cerr << "the pause decision must reach the runtime\n";
        return 1;
    }

    // --- CancelTask: the emergency stop is deterministic ------------------
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::CancelTask;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto cancelled = fixture.submit(fixture.envelope("input-8", "stop that one", principal));
    if (!cancelled || cancelled.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto cancelled_state = fixture.runtime_->task_snapshot(first_task);
    if (!cancelled_state || cancelled_state.value().state != TaskState::Cancelled) {
        return 1;
    }

    // --- a terminal task is never revived ---------------------------------
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::ContinueCurrent;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto resumed = fixture.submit(fixture.envelope("input-9", "carry on", principal));
    if (!resumed || resumed.value().stage != InputStage::Routed) {
        return 1;
    }
    if (resumed.value().task_id.value() == first_task) {
        std::cerr << "a terminal task must never be revived\n";
        return 1;
    }
    const auto still_cancelled = fixture.runtime_->task_snapshot(first_task);
    if (!still_cancelled || still_cancelled.value().state != TaskState::Cancelled) {
        return 1;
    }

    // --- Reject: an explicit host rejection -------------------------------
    fixture.resolver = [](const InputEnvelope &,
                          const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::Reject;
        suggestion.rationale = "the host refuses this request";
        return suggestion;
    };
    const auto rejected = fixture.submit(fixture.envelope("input-10", "no", principal));
    if (!rejected || rejected.value().stage != InputStage::Rejected) {
        return 1;
    }
    if (!rejected.value().error.has_value()) {
        return 1;
    }
    if (rejected.value().error.value().domain_code !=
        static_cast<std::int32_t>(InteractionDomainCode::RouteRejected)) {
        return 1;
    }

    // --- the emergency stop does not need a routing round -----------------
    fixture.resolver = nullptr;
    const auto emergency = fixture.coordinator_->cancel_work(second_task);
    if (!emergency) {
        return 1;
    }
    const auto outcome = emergency.value().outcome(std::chrono::seconds(2));
    if (!outcome || outcome.value().status != SettlementStatus::Applied) {
        return 1;
    }
    const auto stopped = fixture.runtime_->task_snapshot(second_task);
    if (!stopped || stopped.value().state != TaskState::Cancelled) {
        return 1;
    }

    // --- a missing capability fails closed --------------------------------
    const auto denied =
        fixture.submit(fixture.envelope("input-11", "let me in", make_ungranted_principal()));
    if (!denied || denied.value().stage != InputStage::Rejected) {
        return 1;
    }
    if (denied.value().error.value().code != ErrorCode::PermissionDenied) {
        return 1;
    }

    // --- an expired grant fails closed as well ----------------------------
    auto expiring = make_principal();
    expiring.grants.front().expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
    const auto expired_grant = fixture.submit(fixture.envelope("input-12", "and me", expiring));
    if (!expired_grant || expired_grant.value().stage != InputStage::Rejected) {
        std::cerr << "an expired grant must fail closed\n";
        return 1;
    }

    // --- a mismatched identity version fails closed ------------------------
    auto stale = make_principal();
    stale.identity_version = 99;
    CoordinatorConfig pinned;
    pinned.required_identity_version = 1;
    CoordinatorFixture pinned_fixture(pinned);
    if (!pinned_fixture.start()) {
        return 1;
    }
    const auto mismatched =
        pinned_fixture.submit(pinned_fixture.envelope("input-13", "and me too", stale));
    if (!mismatched || mismatched.value().stage != InputStage::Rejected) {
        std::cerr << "a mismatched identity version must fail closed\n";
        return 1;
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
