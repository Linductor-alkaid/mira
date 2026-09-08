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

} // namespace mira
