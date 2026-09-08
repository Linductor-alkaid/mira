#pragma once

#include <mira/core_contracts.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Workflow operation tool specs (DEC-021): schema freeze, validation only
// ---------------------------------------------------------------------------

enum class WorkflowOperation : std::uint8_t {
    RunWorkflow,
    PatchWorkflow,
    PauseWorkflow,
    ResumeWorkflow,
    CancelWorkflow,
};

[[nodiscard]] std::string_view workflow_operation_wire_name(WorkflowOperation operation);
[[nodiscard]] Result<WorkflowOperation>
workflow_operation_from_wire_name(std::string_view name);

// The wire schemas of one operation: arguments the model supplies, the
// model-facing output, the host-facing details envelope and the unified error
// envelope. All schemas stay within the JsonSchema subset gate
// (gate_schema_subset); validation shares that subset with
// resolve_tool_calls instead of reimplementing a second semantics.
struct WorkflowOperationSpec final {
    WorkflowOperation operation = WorkflowOperation::RunWorkflow;
    std::string description;
    JsonSchema parameters_schema;
    JsonSchema result_schema;
    JsonSchema details_schema;
    JsonSchema error_schema;
    bool has_side_effects = false;
};

// The frozen v1 spec set (mira.workflow.tool.v1). Five entries, one per
// operation, in enum order.
[[nodiscard]] const std::vector<WorkflowOperationSpec> &workflow_operation_specs();

// The spec for one operation; UnknownOperation never occurs because the
// overload takes the closed enum.
[[nodiscard]] const WorkflowOperationSpec &
workflow_operation_spec(WorkflowOperation operation);

// ---------------------------------------------------------------------------
// Patch entry shape (DEC-021 §2 / DEC-022 §2)
// ---------------------------------------------------------------------------

enum class WorkflowPatchOp : std::uint8_t { Set, Unset, Skip };

[[nodiscard]] std::string workflow_patch_op_name(WorkflowPatchOp op);
[[nodiscard]] Result<WorkflowPatchOp> parse_workflow_patch_op(std::string_view name);

struct WorkflowPatchEntry final {
    WorkflowPatchTarget target = WorkflowPatchTarget::RunParameters;
    WorkflowPatchOp op = WorkflowPatchOp::Set;
    // "path" on the wire: parameter name, step id or the fixed "policy".
    std::string path;
    JsonValue value; // Required for Set; ignored by Unset/Skip.
};

// Validates the closed patch-entry semantics the tool layer can check
// locally: op/target combinations (Set/Unset for run_parameters and
// step_arguments, Set-only for execution_policy, Skip only for
// step_arguments), non-empty bounded paths and policy values naming a known
// policy. Whether the path names an actual parameter or step is a runtime
// check (phase B); the schema documents that boundary.
[[nodiscard]] Result<void> validate_workflow_patch_entry(const WorkflowPatchEntry &entry,
                                                          const WorkflowLimits &limits = kDefaultWorkflowLimits);

// Canonical digest of a patch: ordered entries through canonical JSON. Used
// as the idempotency identity (same patch_id + same digest => NoOp).
[[nodiscard]] Sha256Digest workflow_patch_digest(const WorkflowPatchId &patch_id,
                                                 const std::vector<WorkflowPatchEntry> &entries);

// ---------------------------------------------------------------------------
// Local operation validation (validate only, never execute)
// ---------------------------------------------------------------------------

// Fail-closed local validation of one operation call: JSON Schema subset
// validation of the arguments plus semantic checks (id/digest hex shape,
// known enum members, required run_id per operation, patch entry rules for
// patch_workflow, policy enum shape for run_workflow). Membership of the
// policy in a specific workflow's allowed set and parameter binding are
// runtime checks and stay out of this layer (DEC-021 §2/§4).
[[nodiscard]] Result<void> validate_workflow_operation(WorkflowOperation operation,
                                                       const JsonValue &arguments);

// Builds the unified error envelope value for a failed operation result:
// {code, domain, domain_code, retryable, safe_message, operation_id}. The
// message must already be sanitized; it is embedded verbatim.
[[nodiscard]] JsonValue workflow_operation_error_envelope(const Error &error);

// ---------------------------------------------------------------------------
// request_user_input tool spec (DEC-024 §4, stage C)
// ---------------------------------------------------------------------------

// The agent-side seat for raising a WaitingUser decision point on one
// Interactive run. The tool call returns once the decision point is raised;
// resolution is asynchronous through WorkflowRuntime::resolve_decision and
// flows back via events and the conversation projection.
struct WorkflowUserInputToolSpec final {
    std::string description;
    JsonSchema parameters_schema;
    JsonSchema result_schema;
    JsonSchema error_schema;
};

[[nodiscard]] const WorkflowUserInputToolSpec &workflow_request_user_input_spec();

// Fail-closed local validation of one request_user_input call: schema subset
// validation plus prompt bounds and the same patch-entry rules as
// patch_workflow for the optional proposal. Run admission (policy, state,
// single pending decision) is a runtime check and stays out of this layer.
[[nodiscard]] Result<void> validate_request_user_input_arguments(const JsonValue &arguments);

// Decodes and validates a raw array of patch entries (the shared shape of
// patch_workflow's patch_entries and request_user_input's proposal).
// Unknown targets, ops and shapes fail closed.
[[nodiscard]] Result<std::vector<WorkflowPatchEntry>>
parse_workflow_patch_entries(const JsonValue &items);

// Decodes the proposal entries of a validated request_user_input call.
// Mirrors the patch_workflow entry parsing; unknown shapes fail closed.
[[nodiscard]] Result<std::vector<WorkflowPatchEntry>>
request_user_input_proposal(const JsonValue &arguments);

} // namespace mira
