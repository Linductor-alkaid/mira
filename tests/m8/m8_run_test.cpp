#include "../support/test.hpp"

#include <mira/workflow_ir.hpp>
#include <mira/workflow_run.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

using namespace mira;

[[nodiscard]] WorkflowRunView make_view(WorkflowRunState state, std::uint64_t epoch) {
    WorkflowRunView view;
    view.run_id = WorkflowRunId::generate();
    view.workflow_id = WorkflowId::generate();
    view.ir_digest = digest_string("ir");
    view.state = state;
    view.run_epoch = epoch;
    view.current_step = StepId::generate();
    return view;
}

int transition_table_matches_the_frozen_contract() {
    // Legal edges per DEC-020 §1.
    MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::Created, WorkflowRunState::Running));
    MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::Created, WorkflowRunState::Cancelled));
    for (auto target : {WorkflowRunState::Paused, WorkflowRunState::WaitingUser,
                        WorkflowRunState::WaitingAgent, WorkflowRunState::Completed,
                        WorkflowRunState::Failed, WorkflowRunState::Cancelled}) {
        MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::Running, target));
    }
    MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::Paused, WorkflowRunState::Running));
    MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::Paused, WorkflowRunState::Cancelled));
    MIRA_CHECK(
        valid_workflow_run_transition(WorkflowRunState::WaitingUser, WorkflowRunState::Running));
    MIRA_CHECK(
        valid_workflow_run_transition(WorkflowRunState::WaitingUser, WorkflowRunState::Cancelled));
    MIRA_CHECK(
        valid_workflow_run_transition(WorkflowRunState::WaitingAgent, WorkflowRunState::Running));
    MIRA_CHECK(valid_workflow_run_transition(WorkflowRunState::WaitingAgent,
                                             WorkflowRunState::WaitingUser));
    MIRA_CHECK(
        valid_workflow_run_transition(WorkflowRunState::WaitingAgent, WorkflowRunState::Failed));
    MIRA_CHECK(
        valid_workflow_run_transition(WorkflowRunState::WaitingAgent, WorkflowRunState::Cancelled));

    // Exhaustive illegal edges: everything the table above does not list.
    const WorkflowRunState all[] = {WorkflowRunState::Created,   WorkflowRunState::Running,
                                    WorkflowRunState::Paused,    WorkflowRunState::WaitingUser,
                                    WorkflowRunState::WaitingAgent, WorkflowRunState::Completed,
                                    WorkflowRunState::Failed,    WorkflowRunState::Cancelled};
    struct Edge final {
        WorkflowRunState from;
        WorkflowRunState to;
    };
    const Edge legal[] = {
        {WorkflowRunState::Created, WorkflowRunState::Running},
        {WorkflowRunState::Created, WorkflowRunState::Cancelled},
        {WorkflowRunState::Running, WorkflowRunState::Paused},
        {WorkflowRunState::Running, WorkflowRunState::WaitingUser},
        {WorkflowRunState::Running, WorkflowRunState::WaitingAgent},
        {WorkflowRunState::Running, WorkflowRunState::Completed},
        {WorkflowRunState::Running, WorkflowRunState::Failed},
        {WorkflowRunState::Running, WorkflowRunState::Cancelled},
        {WorkflowRunState::Paused, WorkflowRunState::Running},
        {WorkflowRunState::Paused, WorkflowRunState::Cancelled},
        {WorkflowRunState::WaitingUser, WorkflowRunState::Running},
        {WorkflowRunState::WaitingUser, WorkflowRunState::Cancelled},
        {WorkflowRunState::WaitingAgent, WorkflowRunState::Running},
        {WorkflowRunState::WaitingAgent, WorkflowRunState::WaitingUser},
        {WorkflowRunState::WaitingAgent, WorkflowRunState::Failed},
        {WorkflowRunState::WaitingAgent, WorkflowRunState::Cancelled},
    };
    for (const auto from : all) {
        for (const auto to : all) {
            if (from == to) {
                MIRA_CHECK(!valid_workflow_run_transition(from, to));
                continue;
            }
            const bool listed =
                std::any_of(std::begin(legal), std::end(legal), [&](const Edge &edge) {
                    return edge.from == from && edge.to == to;
                });
            MIRA_CHECK(valid_workflow_run_transition(from, to) == listed);
        }
    }
    return 0;
}

int terminal_states_are_idempotent() {
    for (auto terminal : {WorkflowRunState::Completed, WorkflowRunState::Failed,
                          WorkflowRunState::Cancelled}) {
        // Re-submitting the same terminal state is a legal NoOp.
        const auto view = make_view(terminal, 7);
        auto same = apply_workflow_run_transition(view, terminal);
        MIRA_CHECK(same.status == WorkflowRunTransitionStatus::NoOpTerminal);
        MIRA_CHECK(same.view.state == terminal);
        MIRA_CHECK(same.view.run_epoch == view.run_epoch);

        // Any other target is rejected; the view is unchanged.
        for (auto other : {WorkflowRunState::Created, WorkflowRunState::Running,
                           WorkflowRunState::Paused, WorkflowRunState::WaitingUser,
                           WorkflowRunState::WaitingAgent, WorkflowRunState::Completed,
                           WorkflowRunState::Failed, WorkflowRunState::Cancelled}) {
            if (other == terminal) {
                continue;
            }
            auto revived = apply_workflow_run_transition(view, other);
            MIRA_CHECK(revived.status == WorkflowRunTransitionStatus::Rejected);
            MIRA_CHECK(revived.error.code == ErrorCode::InvalidState);
            MIRA_CHECK(revived.view.state == terminal);
        }
    }

    // Illegal non-terminal edges are rejected without mutating the view.
    auto paused = apply_workflow_run_transition(make_view(WorkflowRunState::Paused, 2),
                                                WorkflowRunState::Failed);
    MIRA_CHECK(paused.status == WorkflowRunTransitionStatus::Rejected);

    // Legal transitions apply and advance the epoch.
    auto running = apply_workflow_run_transition(make_view(WorkflowRunState::Created, 0),
                                                 WorkflowRunState::Running);
    MIRA_CHECK(running.status == WorkflowRunTransitionStatus::Applied);
    MIRA_CHECK(running.view.state == WorkflowRunState::Running);
    MIRA_CHECK(running.view.run_epoch == 1);

    // A pending decision survives staying in WaitingUser (replace) but is
    // dropped when the run moves on to a terminal state.
    WorkflowRunView waiting = make_view(WorkflowRunState::WaitingUser, 4);
    WorkflowPendingDecision decision;
    decision.decision_id = WorkflowDecisionId::generate();
    decision.payload_digest = digest_string("decision");
    waiting.pending_decision = decision;
    auto cancelled = apply_workflow_run_transition(waiting, WorkflowRunState::Cancelled);
    MIRA_CHECK(cancelled.status == WorkflowRunTransitionStatus::Applied);
    MIRA_CHECK(!cancelled.view.pending_decision.has_value());
    return 0;
}

int late_completions_settle_stale() {
    // Live run, current epoch: accepted.
    MIRA_CHECK(admit_workflow_run_completion(make_view(WorkflowRunState::Running, 3), 3) ==
               WorkflowRunCompletionDisposition::Accepted);

    // Live run, stale epoch: stale.
    MIRA_CHECK(admit_workflow_run_completion(make_view(WorkflowRunState::Running, 3), 2) ==
               WorkflowRunCompletionDisposition::Stale);
    MIRA_CHECK(admit_workflow_run_completion(make_view(WorkflowRunState::Running, 3), 4) ==
               WorkflowRunCompletionDisposition::Stale);

    // Terminal run: any epoch, even the current one, is stale (no revival).
    for (auto terminal : {WorkflowRunState::Completed, WorkflowRunState::Failed,
                          WorkflowRunState::Cancelled}) {
        for (std::uint64_t epoch = 0; epoch < 4; ++epoch) {
            MIRA_CHECK(admit_workflow_run_completion(make_view(terminal, 5), epoch) ==
                       WorkflowRunCompletionDisposition::Stale);
        }
    }
    return 0;
}

int task_mapping_matches_the_decided_table() {
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Created) == TaskState::Idle);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Running) == TaskState::Acting);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Paused) == TaskState::Paused);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::WaitingUser) == TaskState::Paused);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::WaitingAgent) == TaskState::Recovering);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Completed) == TaskState::Completed);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Failed) == TaskState::Failed);
    MIRA_CHECK(task_state_for_run_state(WorkflowRunState::Cancelled) == TaskState::Cancelled);

    // Running carries every execution phase plus the control transients.
    for (auto task : {TaskState::Observing, TaskState::Reasoning, TaskState::Planning,
                      TaskState::Acting, TaskState::Verifying, TaskState::Pausing,
                      TaskState::Cancelling, TaskState::TakeoverSettling}) {
        MIRA_CHECK(run_task_state_compatible(WorkflowRunState::Running, task));
    }
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::Running, TaskState::Idle));
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::Running, TaskState::Paused));
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::Running, TaskState::Completed));

    // Paused/WaitingUser carry the pause family including takeover suspension.
    for (auto run : {WorkflowRunState::Paused, WorkflowRunState::WaitingUser}) {
        MIRA_CHECK(run_task_state_compatible(run, TaskState::Paused));
        MIRA_CHECK(run_task_state_compatible(run, TaskState::SuspendedForTakeover));
        MIRA_CHECK(run_task_state_compatible(run, TaskState::Cancelling));
        MIRA_CHECK(!run_task_state_compatible(run, TaskState::Acting));
        MIRA_CHECK(!run_task_state_compatible(run, TaskState::Observing));
    }

    // WaitingAgent carries the recovery family.
    MIRA_CHECK(run_task_state_compatible(WorkflowRunState::WaitingAgent, TaskState::Recovering));
    MIRA_CHECK(run_task_state_compatible(WorkflowRunState::WaitingAgent, TaskState::Cancelling));
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::WaitingAgent, TaskState::Acting));

    // The terminal invariant: a terminal run is compatible with exactly its
    // own terminal task state, never with an active one.
    MIRA_CHECK(run_task_state_compatible(WorkflowRunState::Completed, TaskState::Completed));
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::Completed, TaskState::Acting));
    MIRA_CHECK(!run_task_state_compatible(WorkflowRunState::Cancelled, TaskState::Cancelling));

    // State names round trip.
    for (const auto state : {WorkflowRunState::Created, WorkflowRunState::Running,
                             WorkflowRunState::Paused, WorkflowRunState::WaitingUser,
                             WorkflowRunState::WaitingAgent, WorkflowRunState::Completed,
                             WorkflowRunState::Failed, WorkflowRunState::Cancelled}) {
        auto parsed = parse_workflow_run_state(workflow_run_state_name(state));
        MIRA_CHECK(parsed.has_value() && parsed.value() == state);
    }
    MIRA_CHECK(!parse_workflow_run_state("exploded").has_value());
    return 0;
}

int policy_compatibility_gates_run_creation() {
    WorkflowDefinition definition;
    definition.workflow_id = WorkflowId::generate();
    definition.name = "gate";
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::DryRun};
    definition.default_policy = WorkflowPolicy::Strict;
    WorkflowStep step;
    step.id = StepId::generate();
    step.kind = WorkflowStepKind::ToolCall;
    definition.steps.push_back(step);

    MIRA_CHECK(validate_workflow_policy_compatibility(definition, WorkflowPolicy::Strict)
                   .has_value());
    MIRA_CHECK(validate_workflow_policy_compatibility(definition, WorkflowPolicy::DryRun)
                   .has_value());
    // Not in the allowed set.
    auto interactive =
        validate_workflow_policy_compatibility(definition, WorkflowPolicy::Interactive);
    MIRA_CHECK(!interactive.has_value());
    MIRA_CHECK(interactive.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int agent_escalation_requires_agent_capable_policy() {
    WorkflowDefinition definition;
    definition.workflow_id = WorkflowId::generate();
    definition.name = "escalation";
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::Recoverable,
                                   WorkflowPolicy::DryRun};
    definition.default_policy = WorkflowPolicy::Strict;
    WorkflowStep step;
    step.id = StepId::generate();
    step.kind = WorkflowStepKind::ToolCall;
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::AgentEscalation;
    step.recovery = hook;
    definition.steps.push_back(step);

    MIRA_CHECK(!validate_workflow_policy_compatibility(definition, WorkflowPolicy::Strict)
                   .has_value());
    MIRA_CHECK(!validate_workflow_policy_compatibility(definition, WorkflowPolicy::DryRun)
                   .has_value());
    MIRA_CHECK(validate_workflow_policy_compatibility(definition, WorkflowPolicy::Recoverable)
                   .has_value());
    return 0;
}

} // namespace

int main() {
    if (transition_table_matches_the_frozen_contract() != 0) {
        return 1;
    }
    if (terminal_states_are_idempotent() != 0) {
        return 1;
    }
    if (late_completions_settle_stale() != 0) {
        return 1;
    }
    if (task_mapping_matches_the_decided_table() != 0) {
        return 1;
    }
    if (policy_compatibility_gates_run_creation() != 0) {
        return 1;
    }
    if (agent_escalation_requires_agent_capable_policy() != 0) {
        return 1;
    }
    return 0;
}
