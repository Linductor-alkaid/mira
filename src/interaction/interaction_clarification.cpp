#include "interaction_coordinator_support.hpp"

namespace mira {
Result<LedgerEntry>
InteractionCoordinator::Impl::finish_clarification(const LedgerEntry &answer,
                                                   const LedgerEntry &original) {
    if (answer.tenant_id != original.tenant_id || answer.user_id != original.user_id ||
        original.resolved_by != answer.input_id || !original.task_id || !original.session_id)
        return interaction_support::recovery_error(
            ErrorCode::InvalidState, "the clarification answer is not bound to the original input",
            InteractionDomainCode::InputConflict);
    if (answer.stage != InputStage::Accepted)
        return answer;
    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.task_id = original.task_id;
    transition.session_id = original.session_id;
    transition.intent = original.intent;
    transition.delivery = InputDeliveryState::NotRequired;
    transition.note = "clarification answer bound to original input; no second delivery";
    return ledger->advance(answer.input_id, transition);
}
} // namespace mira
