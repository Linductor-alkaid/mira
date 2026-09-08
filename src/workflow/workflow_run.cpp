#include <mira/workflow_run.hpp>

#include <algorithm>

namespace mira {

std::string workflow_run_state_name(WorkflowRunState state) {
    switch (state) {
    case WorkflowRunState::Created:
        return "created";
    case WorkflowRunState::Running:
        return "running";
    case WorkflowRunState::Paused:
        return "paused";
    case WorkflowRunState::WaitingUser:
        return "waiting_user";
    case WorkflowRunState::WaitingAgent:
        return "waiting_agent";
    case WorkflowRunState::Completed:
        return "completed";
    case WorkflowRunState::Failed:
        return "failed";
    case WorkflowRunState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

Result<WorkflowRunState> parse_workflow_run_state(std::string_view name) {
    for (auto state : {WorkflowRunState::Created, WorkflowRunState::Running,
                       WorkflowRunState::Paused, WorkflowRunState::WaitingUser,
                       WorkflowRunState::WaitingAgent, WorkflowRunState::Completed,
                       WorkflowRunState::Failed, WorkflowRunState::Cancelled}) {
        if (workflow_run_state_name(state) == name) {
            return state;
        }
    }
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.safe_message = "unknown workflow run state";
    return error;
}

bool valid_workflow_run_transition(WorkflowRunState from, WorkflowRunState to) noexcept {
    switch (from) {
    case WorkflowRunState::Created:
        return to == WorkflowRunState::Running || to == WorkflowRunState::Cancelled;
    case WorkflowRunState::Running:
        return to == WorkflowRunState::Paused || to == WorkflowRunState::WaitingUser ||
               to == WorkflowRunState::WaitingAgent || to == WorkflowRunState::Completed ||
               to == WorkflowRunState::Failed || to == WorkflowRunState::Cancelled;
    case WorkflowRunState::Paused:
    case WorkflowRunState::WaitingUser:
        // Both wait states share the resume/cancel edges; WaitingUser is
        // distinguished by its pending decision point, not by transitions.
        return to == WorkflowRunState::Running || to == WorkflowRunState::Cancelled;
    case WorkflowRunState::WaitingAgent:
        return to == WorkflowRunState::Running || to == WorkflowRunState::WaitingUser ||
               to == WorkflowRunState::Failed || to == WorkflowRunState::Cancelled;
    case WorkflowRunState::Completed:
    case WorkflowRunState::Failed:
    case WorkflowRunState::Cancelled:
        // Terminal idempotency is handled by apply_workflow_run_transition;
        // the table itself has no outgoing edges from terminal states.
        return false;
    }
    return false;
}

WorkflowRunTransitionResult apply_workflow_run_transition(const WorkflowRunView &view,
                                                          WorkflowRunState target) {
    WorkflowRunTransitionResult result;
    result.view = view;
    if (is_terminal(view.state)) {
        if (view.state == target) {
            // Terminal idempotency: re-submitting the same terminal state is a
            // legal NoOp; the epoch does not advance (DEC-020 §1).
            result.status = WorkflowRunTransitionStatus::NoOpTerminal;
            return result;
        }
        result.error.code = ErrorCode::InvalidState;
        result.error.domain = "mira.workflow";
        result.error.safe_message = "terminal workflow run state has no outgoing transition";
        return result;
    }
    if (!valid_workflow_run_transition(view.state, target)) {
        result.error.code = ErrorCode::InvalidState;
        result.error.domain = "mira.workflow";
        result.error.safe_message = "workflow run transition is not legal";
        return result;
    }
    result.view.state = target;
    result.view.run_epoch += 1;
    if (view.state == WorkflowRunState::WaitingUser) {
        // Leaving WaitingUser settles the decision point (answered, replaced
        // or cancelled); entering WaitingUser keeps the decision the caller
        // staged before the transition (DEC-022 §3).
        result.view.pending_decision.reset();
    }
    result.status = WorkflowRunTransitionStatus::Applied;
    return result;
}

TaskState task_state_for_run_state(WorkflowRunState state) noexcept {
    switch (state) {
    case WorkflowRunState::Created:
        return TaskState::Idle;
    case WorkflowRunState::Running:
        return TaskState::Acting;
    case WorkflowRunState::Paused:
    case WorkflowRunState::WaitingUser:
        // Waiting on a user decision point carries the pause-family safety
        // semantics: no autonomous actions, platform input released
        // (DEC-020 §2).
        return TaskState::Paused;
    case WorkflowRunState::WaitingAgent:
        return TaskState::Recovering;
    case WorkflowRunState::Completed:
        return TaskState::Completed;
    case WorkflowRunState::Failed:
        return TaskState::Failed;
    case WorkflowRunState::Cancelled:
        return TaskState::Cancelled;
    }
    return TaskState::Idle;
}

bool run_task_state_compatible(WorkflowRunState run, TaskState task) noexcept {
    if (is_terminal(run)) {
        return task_state_for_run_state(run) == task;
    }
    switch (run) {
    case WorkflowRunState::Created:
        return task == TaskState::Idle || task == TaskState::Cancelling;
    case WorkflowRunState::Running:
        // While the run view is Running the task may be executing any phase or
        // settling a control request (pause/cancel/takeover); the run view
        // only moves once the in-flight step has settled (DEC-020 §2).
        switch (task) {
        case TaskState::Observing:
        case TaskState::Reasoning:
        case TaskState::Planning:
        case TaskState::Acting:
        case TaskState::Verifying:
        case TaskState::Pausing:
        case TaskState::Cancelling:
        case TaskState::TakeoverSettling:
            return true;
        default:
            return false;
        }
    case WorkflowRunState::Paused:
    case WorkflowRunState::WaitingUser:
        // SuspendedForTakeover is the takeover-specific carrier of the pause
        // semantics (platform input released, no autonomous actions).
        return task == TaskState::Paused || task == TaskState::SuspendedForTakeover ||
               task == TaskState::Cancelling;
    case WorkflowRunState::WaitingAgent:
        return task == TaskState::Recovering || task == TaskState::Cancelling;
    case WorkflowRunState::Completed:
    case WorkflowRunState::Failed:
    case WorkflowRunState::Cancelled:
        break;
    }
    return false;
}

WorkflowRunCompletionDisposition
admit_workflow_run_completion(const WorkflowRunView &view, std::uint64_t signal_epoch) noexcept {
    // Terminal runs never revive: any arrival settles stale regardless of the
    // epoch it carries (W-01/RULE-03).
    if (is_terminal(view.state)) {
        return WorkflowRunCompletionDisposition::Stale;
    }
    // Only the epoch the signal was issued under is admissible; anything
    // older (or unexpected) is stale. Runs advance monotonically.
    if (signal_epoch != view.run_epoch) {
        return WorkflowRunCompletionDisposition::Stale;
    }
    return WorkflowRunCompletionDisposition::Accepted;
}

Result<void> validate_workflow_policy_compatibility(const WorkflowDefinition &definition,
                                                    WorkflowPolicy policy) {
    const bool allowed = std::any_of(definition.allowed_policies.begin(),
                                     definition.allowed_policies.end(),
                                     [&](WorkflowPolicy entry) { return entry == policy; });
    if (!allowed) {
        Error error;
        error.code = ErrorCode::InvalidArgument;
        error.domain = "mira.workflow";
        error.safe_message = "policy is not declared in the workflow allowed set";
        return error;
    }
    if (!workflow_policy_allows_agent(policy)) {
        for (const auto &step : definition.steps) {
            if (step.recovery.has_value() &&
                step.recovery->mode == WorkflowRecoveryHook::Mode::AgentEscalation) {
                Error error;
                error.code = ErrorCode::InvalidArgument;
                error.domain = "mira.workflow";
                error.safe_message =
                    "agent escalation hook requires an agent-capable policy";
                return error;
            }
        }
    }
    return Result<void>{};
}

} // namespace mira
