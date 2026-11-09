#include "m29_recovery_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

int main() {
    for (bool completed : {false, true}) {
        RecoveryRoot root;
        const auto principal = make_principal();
        TaskId task;
        SessionId session;
        {
            RecoveryHost host(root.path);
            MIRA_CHECK(host.start());
            const auto initial = host.create("goal", "recover safely", principal);
            MIRA_CHECK(initial);
            task = *initial.value().task_id;
            session = *initial.value().session_id;
            MIRA_CHECK(seed_uncertain(host, task, session, completed));
            MIRA_CHECK(host.coordinator().close());
        }
        {
            RecoveryHost host(root.path);
            MIRA_CHECK(host.start());
            const auto observed = host.coordinator().recover_work(
                principal, task, host.core.environment_, recovery_context());
            MIRA_CHECK(observed && observed.value().observation &&
                       !observed.value().ready_for_new_task);
            MIRA_CHECK(observed.value().recovery.checkpoint->verification_pending);
            const auto still_pinned = RecoveryPlanner(*host.events, *host.checkpoints)
                                          .plan(task, session, Timestamp::now());
            MIRA_CHECK(still_pinned && still_pinned.value().checkpoint->verification_pending);
            MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
            const auto denied = host.coordinator().submit_input(
                host.input("before-verify", "continue", principal, task));
            MIRA_CHECK(denied && denied.value().stage == InputStage::Rejected);
            bool verified = false;
            const auto recovered = host.coordinator().recover_work(
                principal, task, host.core.environment_, recovery_context(),
                [&](const WorkRecoveryReport &report, const Observation &,
                    const OperationContext &) -> Result<bool> {
                    verified = report.recovery.checkpoint->verification_pending;
                    return true;
                });
            MIRA_CHECK(recovered && recovered.value().ready_for_new_task && verified);
            MIRA_CHECK(recovered.value().recovery.pending_side_effects.empty());
            MIRA_CHECK(!recovered.value().recovery.checkpoint->verification_pending);
            const auto resumed = host.coordinator().submit_input(
                host.input("after-verify", "finish using the verified state", principal, task));
            MIRA_CHECK(resumed && resumed.value().stage == InputStage::Routed &&
                       resumed.value().task_id != task);
            MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
        }
    }
    // Settled work in the same Runtime must not bypass pending journal pins.
    RecoveryRoot root;
    const auto principal = make_principal();
    RecoveryHost host(root.path);
    MIRA_CHECK(host.start());
    const auto initial =
        host.create("terminal-goal", "settled work with uncertain input", principal);
    MIRA_CHECK(initial);
    const auto task = *initial.value().task_id;
    MIRA_CHECK(seed_uncertain(host, task, *initial.value().session_id));
    const auto cancelled = host.coordinator().cancel_work(task);
    MIRA_CHECK(cancelled && cancelled.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(host.coordinator().record_settlement("terminal-goal", TaskState::Cancelled));
    MIRA_CHECK(host.coordinator().record_settlement("terminal-goal", TaskState::Completed));
    MIRA_CHECK(host.coordinator().input_record("terminal-goal").value().note ==
               "terminal: Cancelled");
    const auto blocked = host.coordinator().submit_input(
        host.input("terminal-blocked", "continue", principal, task));
    MIRA_CHECK(blocked && blocked.value().stage == InputStage::Rejected);
    const auto observed = host.coordinator().recover_work(principal, task, host.core.environment_,
                                                          recovery_context());
    MIRA_CHECK(observed && observed.value().recovery.action == RecoveryAction::AlreadyTerminal);
    MIRA_CHECK(!observed.value().ready_for_new_task);
    const auto verified = host.coordinator().recover_work(
        principal, task, host.core.environment_, recovery_context(),
        [](const WorkRecoveryReport &, const Observation &,
           const OperationContext &) -> Result<bool> { return true; });
    MIRA_CHECK(verified && verified.value().ready_for_new_task);
    const auto resumed = host.coordinator().submit_input(
        host.input("terminal-resumed", "new linked work", principal, task));
    MIRA_CHECK(resumed && resumed.value().task_id != task);
    MIRA_CHECK(host.core.runtime_->task_snapshot(task).value().state == TaskState::Cancelled);
    MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
    return 0;
}
