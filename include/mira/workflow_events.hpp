#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_run.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mira {

// ---------------------------------------------------------------------------
// Workflow event payloads (DEC-022 §2/§5, workflow_runtime_design §5)
// ---------------------------------------------------------------------------

// Every payload carries a versioned schema string ("mira.workflow.<type>.v1")
// and fails closed on unknown fields, missing members, malformed ids/digests
// and unknown enum names. Payloads reference parameters and proposal content
// by digest only: user text never travels inline (DEC-022 §5). Summary fields
// are length-bounded and pre-sanitized by the caller.

inline constexpr std::size_t kWorkflowEventMaxSummaryBytes = 2048;

[[nodiscard]] bool is_workflow_event_type(std::string_view type);

// --- WorkflowRunStarted (Critical) -----------------------------------------

struct WorkflowRunStartedEvent final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    Sha256Digest ir_digest{};
    Sha256Digest parameters_digest{};
    WorkflowPolicy policy = WorkflowPolicy::Strict;
};

// --- WorkflowStepStarted (State) -------------------------------------------

struct WorkflowStepStartedEvent final {
    WorkflowRunId run_id;
    StepId step_id;
    WorkflowStepKind kind = WorkflowStepKind::ToolCall;
    std::uint32_t attempt = 1;
};

// --- WorkflowStepSettled (State) -------------------------------------------

enum class WorkflowStepDisposition : std::uint8_t { Completed, Skipped, Failed, Stale };

[[nodiscard]] std::string workflow_step_disposition_name(WorkflowStepDisposition disposition);
[[nodiscard]] Result<WorkflowStepDisposition>
parse_workflow_step_disposition(std::string_view name);

struct WorkflowStepSettledEvent final {
    WorkflowRunId run_id;
    StepId step_id;
    WorkflowStepDisposition disposition = WorkflowStepDisposition::Completed;
    // Verification outcome summary: "satisfied" | "not_satisfied" |
    // "not_evaluable" | "none"; bounded diagnostic string.
    std::string verification;
    std::string safe_summary;
};

// --- WorkflowRunSettled (Critical) -----------------------------------------

struct WorkflowRunSettledEvent final {
    WorkflowRunId run_id;
    WorkflowRunState terminal_state = WorkflowRunState::Failed;
    std::uint64_t run_epoch = 0;
    std::string safe_summary;
};

// --- Workflow patch lifecycle (State, DEC-022 §2) --------------------------

enum class WorkflowPatchTarget : std::uint8_t {
    RunParameters,
    StepArguments,
    ExecutionPolicy,
};

[[nodiscard]] std::string workflow_patch_target_name(WorkflowPatchTarget target);
[[nodiscard]] Result<WorkflowPatchTarget> parse_workflow_patch_target(std::string_view name);

struct WorkflowPatchProposedEvent final {
    WorkflowPatchId patch_id;
    WorkflowRunId run_id;
    Sha256Digest patch_digest{};
    WorkflowPatchTarget target = WorkflowPatchTarget::RunParameters;
    // Stable machine-readable reason/classification code, not user text.
    std::string reason_code;
};

struct WorkflowPatchAppliedEvent final {
    WorkflowPatchId patch_id;
    WorkflowRunId run_id;
    std::uint32_t run_patch_epoch = 0;
};

struct WorkflowPatchRejectedEvent final {
    WorkflowPatchId patch_id;
    WorkflowRunId run_id;
    // Machine-readable rejection code (e.g. "validation-failed",
    // "id-conflict", "ambiguous-target-confirmed-cancel").
    std::string reason_code;
};

// --- WorkflowPolicySwitched (State) ----------------------------------------

struct WorkflowPolicySwitchedEvent final {
    WorkflowRunId run_id;
    WorkflowPolicy from = WorkflowPolicy::Strict;
    WorkflowPolicy to = WorkflowPolicy::Strict;
};

// --- WaitingUser decision points (State, DEC-022 §3) -----------------------

struct WorkflowDecisionRaisedEvent final {
    WorkflowRunId run_id;
    WorkflowDecisionId decision_id;
    Sha256Digest payload_digest{};
};

enum class WorkflowDecisionResolution : std::uint8_t { Accept, Reject, CancelRun };

[[nodiscard]] std::string workflow_decision_resolution_name(WorkflowDecisionResolution resolution);
[[nodiscard]] Result<WorkflowDecisionResolution>
parse_workflow_decision_resolution(std::string_view name);

struct WorkflowDecisionResolvedEvent final {
    WorkflowRunId run_id;
    WorkflowDecisionId decision_id;
    WorkflowDecisionResolution resolution = WorkflowDecisionResolution::Accept;
};

// --- Workflow publish gate lifecycle (State, DEC-025 §3, stage D) -----------

// Session-scoped (no run yet): the audit trail of the stage-D publish gate.
struct WorkflowPublishProposedEvent final {
    WorkflowId workflow_id;
    Sha256Digest ir_digest{};
    std::optional<WorkflowRunId> source_run_id;
};

struct WorkflowPublishAppliedEvent final {
    WorkflowId workflow_id;
    Sha256Digest ir_digest{};
    Sha256Digest evidence{};
    WorkflowRunId dry_run_id;
};

struct WorkflowPublishRejectedEvent final {
    WorkflowId workflow_id;
    Sha256Digest ir_digest{};
    // Machine-readable gate rejection code (e.g. "validation-failed",
    // "gate-run-failed", "publish-dryrun-failed", "append-failed").
    std::string reason_code;
};

// --- Workflow navigation (State, DEC-028 §4, stage E) -----------------------

// One navigation plan resolution (DryRun and dispatching policies alike).
struct WorkflowNavigationPlannedEvent final {
    WorkflowRunId run_id;
    StepId step_id;
    std::string from_state;
    std::string to_state;
    std::uint64_t edge_count = 0;
    Sha256Digest plan_digest{};
    double total_cost = 0.0;
    std::uint64_t guards_blocked = 0;
    std::uint64_t guards_unevaluable = 0;
};

// One real transition observation with the post-update edge confidence.
// Emitted only when the run actually dispatched the edge action.
struct WorkflowNavigationObservedEvent final {
    WorkflowRunId run_id;
    StepId step_id;
    std::string transition_id;
    std::string from_state;
    std::string to_state;
    bool success = false;
    double confidence = 0.0;
};

// --- Workflow learning audit (State, DEC-030 §5, stage F) -------------------

// One settlement-time episode memory write attempt. `recorded` carries the
// episode digest; `failed` carries a bounded reason code instead. DryRun
// settlements skip recording by design and emit nothing.
struct WorkflowEpisodeRecordedEvent final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    Sha256Digest episode_digest{};
    std::string outcome; // recorded | failed
    // Machine-readable reason code (e.g. "memory-unavailable"); empty when
    // recorded.
    std::string reason_code;
};

// One host-initiated recovery-lesson memory write attempt, same shape.
struct WorkflowLessonRecordedEvent final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    Sha256Digest lesson_digest{};
    std::string outcome; // recorded | failed
    std::string reason_code;
};

// --- Workflow recovery orchestration (State, DEC-031 §7, M14) ---------------

// The settled outcome of one agent-side recovery attempt. PatchedAndResumed /
// ResumedWithoutPatch / CancelRequested changed the run through the existing
// public exits; DeferredToHost asks the host to decide (Run stays
// WaitingAgent); Aborted ended on an external condition (cancel, takeover,
// shutdown, state drift, model unavailable, duplicate notification) and may
// be retried after the condition clears.
enum class WorkflowRecoveryOutcome : std::uint8_t {
    PatchedAndResumed,
    ResumedWithoutPatch,
    CancelRequested,
    DeferredToHost,
    Aborted,
};

[[nodiscard]] std::string workflow_recovery_outcome_name(WorkflowRecoveryOutcome outcome);
[[nodiscard]] Result<WorkflowRecoveryOutcome>
parse_workflow_recovery_outcome(std::string_view name);

// One settled recovery attempt over a WaitingAgent run (DEC-031 §7): the
// whole-chain correlation key linking the model request (model_request_id),
// the decision (decision_digest), the applied patch (patch_id) and the run's
// escalation context (run_id + ordinal). Lesson filtering counters make "no
// lessons" and "lessons offered but all filtered" distinguishable. Payload
// discipline follows DEC-030 §5: ids, digests, enum names and bounded
// counters only; the rationale never travels in events.
struct WorkflowRecoveryAttemptedEvent final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    // Orchestrator-side per-run attempt ordinal, starting at 1.
    std::uint32_t ordinal = 0;
    // The carrier task the recovery model request was attributed to.
    TaskId task_id;
    WorkflowRecoveryOutcome outcome = WorkflowRecoveryOutcome::Aborted;
    // Closed-set reason code (e.g. "not-waiting-agent", "takeover",
    // "decision-invalid", "patch-rejected:<code>", "cancelled"); empty when
    // the outcome itself carries the cause.
    std::string reason_code;
    std::optional<Sha256Digest> decision_digest;
    std::optional<WorkflowPatchId> patch_id;
    std::optional<ModelRequestId> model_request_id;
    std::uint32_t lessons_offered = 0;
    std::uint32_t lessons_stale = 0;
    std::uint32_t lessons_unparseable = 0;
    std::uint32_t lessons_kept = 0;
};

// --- Payload builders and parsers -------------------------------------------

// Builders return ready-to-append payloads with the event type and schema
// string set; classification matches workflow_runtime_design §5.
[[nodiscard]] EventPayload to_event_payload(const WorkflowRunStartedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowStepStartedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowStepSettledEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowRunSettledEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPatchProposedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPatchAppliedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPatchRejectedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPolicySwitchedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowDecisionRaisedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowDecisionResolvedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPublishProposedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPublishAppliedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowPublishRejectedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowNavigationPlannedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowNavigationObservedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowEpisodeRecordedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowLessonRecordedEvent &event);
[[nodiscard]] EventPayload to_event_payload(const WorkflowRecoveryAttemptedEvent &event);

// Parsers fail closed on schema mismatch, unknown fields and malformed ids.
// The payload data is JSON text (EventPayload::data).
[[nodiscard]] Result<WorkflowRunStartedEvent>
parse_workflow_run_started(const EventPayload &payload);
[[nodiscard]] Result<WorkflowStepStartedEvent>
parse_workflow_step_started(const EventPayload &payload);
[[nodiscard]] Result<WorkflowStepSettledEvent>
parse_workflow_step_settled(const EventPayload &payload);
[[nodiscard]] Result<WorkflowRunSettledEvent>
parse_workflow_run_settled(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPatchProposedEvent>
parse_workflow_patch_proposed(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPatchAppliedEvent>
parse_workflow_patch_applied(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPatchRejectedEvent>
parse_workflow_patch_rejected(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPolicySwitchedEvent>
parse_workflow_policy_switched(const EventPayload &payload);
[[nodiscard]] Result<WorkflowDecisionRaisedEvent>
parse_workflow_decision_raised(const EventPayload &payload);
[[nodiscard]] Result<WorkflowDecisionResolvedEvent>
parse_workflow_decision_resolved(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPublishProposedEvent>
parse_workflow_publish_proposed(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPublishAppliedEvent>
parse_workflow_publish_applied(const EventPayload &payload);
[[nodiscard]] Result<WorkflowPublishRejectedEvent>
parse_workflow_publish_rejected(const EventPayload &payload);
[[nodiscard]] Result<WorkflowNavigationPlannedEvent>
parse_workflow_navigation_planned(const EventPayload &payload);
[[nodiscard]] Result<WorkflowNavigationObservedEvent>
parse_workflow_navigation_observed(const EventPayload &payload);
[[nodiscard]] Result<WorkflowEpisodeRecordedEvent>
parse_workflow_episode_recorded(const EventPayload &payload);
[[nodiscard]] Result<WorkflowLessonRecordedEvent>
parse_workflow_lesson_recorded(const EventPayload &payload);
[[nodiscard]] Result<WorkflowRecoveryAttemptedEvent>
parse_workflow_recovery_attempted(const EventPayload &payload);

} // namespace mira
