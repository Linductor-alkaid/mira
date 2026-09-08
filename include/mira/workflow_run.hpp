#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/workflow_ir.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mira {

// ---------------------------------------------------------------------------
// WorkflowRun state view and transition table (DEC-020 §1/§2)
// ---------------------------------------------------------------------------

enum class WorkflowRunState : std::uint8_t {
    Created,
    Running,
    Paused,
    WaitingUser,
    WaitingAgent,
    Completed,
    Failed,
    Cancelled,
};

[[nodiscard]] std::string workflow_run_state_name(WorkflowRunState state);
[[nodiscard]] Result<WorkflowRunState> parse_workflow_run_state(std::string_view name);
[[nodiscard]] inline bool is_terminal(WorkflowRunState state) noexcept {
    return state == WorkflowRunState::Completed || state == WorkflowRunState::Failed ||
           state == WorkflowRunState::Cancelled;
}

// The frozen transition table (DEC-020 §1). Terminal states have no outgoing
// edges; see apply_workflow_run_transition for terminal idempotency.
[[nodiscard]] bool valid_workflow_run_transition(WorkflowRunState from, WorkflowRunState to) noexcept;

// One pending user decision point (DEC-022 §3): stable identity plus the
// digest of the proposed payload. Answers match on both; mismatching answers
// are rejected rather than misrouted.
struct WorkflowPendingDecision final {
    WorkflowDecisionId decision_id;
    Sha256Digest payload_digest{};
};

// The semantic view of one execution instance. State is submitted only by the
// single-writer control plane (W-01); run_epoch increments on every applied
// transition so late signals settle as stale (RULE-03).
struct WorkflowRunView final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    // Creation-time pinned workflow content (W-03); replay resolves by digest.
    Sha256Digest ir_digest{};
    WorkflowRunState state = WorkflowRunState::Created;
    std::uint64_t run_epoch = 0;
    std::optional<StepId> current_step;
    WorkflowPolicy policy = WorkflowPolicy::Strict;
    std::uint32_t run_patch_epoch = 0;
    std::optional<WorkflowPendingDecision> pending_decision;
    Timestamp created_at;
};

enum class WorkflowRunTransitionStatus : std::uint8_t { Applied, NoOpTerminal, Rejected };

struct WorkflowRunTransitionResult final {
    WorkflowRunTransitionStatus status = WorkflowRunTransitionStatus::Rejected;
    WorkflowRunView view; // Unchanged input view unless status == Applied.
    Error error;
};

// Applies one state transition as a pure function. Legal transitions apply and
// advance run_epoch; re-submitting the current terminal state is the legal
// NoOpTerminal (idempotent, epoch unchanged); everything else is Rejected
// with InvalidState. Pending decisions are dropped when leaving WaitingUser
// towards a terminal or a different wait state only through legal edges.
[[nodiscard]] WorkflowRunTransitionResult
apply_workflow_run_transition(const WorkflowRunView &view, WorkflowRunState target);

// ---------------------------------------------------------------------------
// Run <-> Task state mapping (DEC-020 §2)
// ---------------------------------------------------------------------------

// The representative TaskState for each run state (Running maps to Acting;
// use run_task_state_compatible for the full membership test).
[[nodiscard]] TaskState task_state_for_run_state(WorkflowRunState state) noexcept;

// Table-driven membership test: is `task` a legal carrier for `run`? The
// invariant "one side terminal, the other active" must never occur; control
// transient states (Pausing/Cancelling/TakeoverSettling) are legal carriers
// while the run is still Running because the run view only moves once the
// in-flight step has settled.
[[nodiscard]] bool run_task_state_compatible(WorkflowRunState run, TaskState task) noexcept;

// ---------------------------------------------------------------------------
// Late completion isolation (DEC-020 §1, W-01/RULE-03)
// ---------------------------------------------------------------------------

enum class WorkflowRunCompletionDisposition : std::uint8_t { Accepted, Stale };

// Admits one step-level completion signal carrying the run epoch it was
// issued under. Terminal runs never revive: any epoch settles Stale. On a
// live run only the current epoch is Accepted; older (or unknown newer)
// epochs are Stale.
[[nodiscard]] WorkflowRunCompletionDisposition
admit_workflow_run_completion(const WorkflowRunView &view, std::uint64_t signal_epoch) noexcept;

// Policy compatibility checked when a run is created (DEC-020 §3): the chosen
// policy must be declared in the definition's allowed set, and an
// AgentEscalation recovery hook requires an agent-capable policy.
[[nodiscard]] Result<void> validate_workflow_policy_compatibility(const WorkflowDefinition &definition,
                                                                  WorkflowPolicy policy);

} // namespace mira
