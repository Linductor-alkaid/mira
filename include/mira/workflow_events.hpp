#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
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

} // namespace mira
