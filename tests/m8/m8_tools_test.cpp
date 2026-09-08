#include "../support/test.hpp"

#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>
#include <mira/workflow_tools.hpp>

#include <string>

namespace {

using namespace mira;

constexpr const char *kWorkflowId = "0123456789abcdef0123456789abcdef";
constexpr const char *kRunId = "fedcba9876543210fedcba9876543210";
constexpr const char *kPatchId = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char *kDigest64 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

[[nodiscard]] JsonValue base_arguments() {
    JsonValue arguments{JsonValue::Object{}};
    arguments.set("workflow_id", JsonValue{std::string{kWorkflowId}});
    arguments.set("run_id", JsonValue{std::string{kRunId}});
    return arguments;
}

int spec_set_is_frozen_and_subset_clean() {
    const auto &specs = workflow_operation_specs();
    MIRA_CHECK(specs.size() == 5);
    for (const auto &spec : specs) {
        MIRA_CHECK(gate_schema_subset(spec.parameters_schema).has_value());
        MIRA_CHECK(gate_schema_subset(spec.result_schema).has_value());
        MIRA_CHECK(gate_schema_subset(spec.details_schema).has_value());
        MIRA_CHECK(gate_schema_subset(spec.error_schema).has_value());
        MIRA_CHECK(!spec.description.empty());
        MIRA_CHECK(spec.error_schema.valid());
    }

    // Wire names are unique, resolvable and never hosted provider names.
    for (std::size_t index = 0; index < specs.size(); ++index) {
        const auto name = workflow_operation_wire_name(specs[index].operation);
        MIRA_CHECK(!is_known_hosted_tool_name(name));
        for (std::size_t other = index + 1; other < specs.size(); ++other) {
            MIRA_CHECK(name != workflow_operation_wire_name(specs[other].operation));
        }
        auto resolved = workflow_operation_from_wire_name(name);
        MIRA_CHECK(resolved.has_value() && resolved.value() == specs[index].operation);
    }

    // Side-effect flags per DEC-021 §1.
    MIRA_CHECK(workflow_operation_spec(WorkflowOperation::RunWorkflow).has_side_effects);
    MIRA_CHECK(!workflow_operation_spec(WorkflowOperation::PatchWorkflow).has_side_effects);
    MIRA_CHECK(!workflow_operation_spec(WorkflowOperation::PauseWorkflow).has_side_effects);
    MIRA_CHECK(!workflow_operation_spec(WorkflowOperation::ResumeWorkflow).has_side_effects);
    MIRA_CHECK(!workflow_operation_spec(WorkflowOperation::CancelWorkflow).has_side_effects);
    MIRA_CHECK(!workflow_operation_from_wire_name("explode_workflow").has_value());
    return 0;
}

int control_operations_validate_by_schema() {
    for (auto operation :
         {WorkflowOperation::PauseWorkflow, WorkflowOperation::ResumeWorkflow,
          WorkflowOperation::CancelWorkflow}) {
        MIRA_CHECK(validate_workflow_operation(operation, base_arguments()).has_value());

        // Missing run_id fails.
        JsonValue missing{JsonValue::Object{}};
        missing.set("workflow_id", JsonValue{std::string{kWorkflowId}});
        auto rejected = validate_workflow_operation(operation, missing);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);

        // Malformed run_id fails.
        JsonValue malformed = base_arguments();
        malformed.set("run_id", JsonValue{std::string("nope")});
        MIRA_CHECK(!validate_workflow_operation(operation, malformed).has_value());

        // Unknown fields fail closed.
        JsonValue unknown = base_arguments();
        unknown.set("urgency", JsonValue{static_cast<std::int64_t>(11)});
        MIRA_CHECK(!validate_workflow_operation(operation, unknown).has_value());
    }
    return 0;
}

int run_workflow_validates_shape_not_semantics() {
    JsonValue arguments{JsonValue::Object{}};
    arguments.set("workflow_id", JsonValue{std::string{kWorkflowId}});
    arguments.set("ir_digest", JsonValue{std::string{kDigest64}});
    MIRA_CHECK(validate_workflow_operation(WorkflowOperation::RunWorkflow, arguments).has_value());

    // Bad digest hex shape fails.
    JsonValue bad_digest = arguments;
    bad_digest.set("ir_digest", JsonValue{std::string("zz")});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::RunWorkflow, bad_digest).has_value());

    // Policy must name a known member; membership in a specific workflow's
    // allowed set stays a runtime check (documented boundary).
    JsonValue with_policy = arguments;
    with_policy.set("policy", JsonValue{std::string("dry_run")});
    MIRA_CHECK(
        validate_workflow_operation(WorkflowOperation::RunWorkflow, with_policy).has_value());
    with_policy.set("policy", JsonValue{std::string("turbo")});
    MIRA_CHECK(
        !validate_workflow_operation(WorkflowOperation::RunWorkflow, with_policy).has_value());

    // Parameters must be an object when present.
    JsonValue parameters = arguments;
    parameters.set("parameters", JsonValue{JsonValue::Object{}});
    MIRA_CHECK(validate_workflow_operation(WorkflowOperation::RunWorkflow, parameters).has_value());
    parameters.set("parameters", JsonValue{std::string("contact=li si")});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::RunWorkflow, parameters).has_value());

    // Missing ir_digest fails.
    JsonValue missing = arguments;
    missing.set("ir_digest", JsonValue{JsonValue{nullptr}});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::RunWorkflow, missing).has_value());
    return 0;
}

int patch_entries_follow_the_closed_semantics() {
    JsonValue set_entry{JsonValue::Object{}};
    set_entry.set("target", JsonValue{std::string("run_parameters")});
    set_entry.set("op", JsonValue{std::string("set")});
    set_entry.set("path", JsonValue{std::string("contact")});
    set_entry.set("value", JsonValue{std::string("li si")});

    JsonValue arguments = base_arguments();
    arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    arguments.set("patch_entries", JsonValue{JsonValue::Array{set_entry}});
    MIRA_CHECK(validate_workflow_operation(WorkflowOperation::PatchWorkflow, arguments).has_value());

    // Policy switch entries are set-only on the fixed path.
    JsonValue policy_entry{JsonValue::Object{}};
    policy_entry.set("target", JsonValue{std::string("execution_policy")});
    policy_entry.set("op", JsonValue{std::string("set")});
    policy_entry.set("path", JsonValue{std::string("policy")});
    policy_entry.set("value", JsonValue{std::string("interactive")});
    JsonValue policy_arguments = base_arguments();
    policy_arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    policy_arguments.set("patch_entries", JsonValue{JsonValue::Array{policy_entry}});
    MIRA_CHECK(
        validate_workflow_operation(WorkflowOperation::PatchWorkflow, policy_arguments).has_value());

    JsonValue bad_policy_value = policy_entry;
    bad_policy_value.set("value", JsonValue{std::string("fast")});
    JsonValue bad_policy_arguments = base_arguments();
    bad_policy_arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    bad_policy_arguments.set("patch_entries", JsonValue{JsonValue::Array{bad_policy_value}});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::PatchWorkflow, bad_policy_arguments)
                   .has_value());

    JsonValue unset_policy = policy_entry;
    unset_policy.set("op", JsonValue{std::string("unset")});
    unset_policy.set("value", JsonValue{nullptr});
    JsonValue unset_arguments = base_arguments();
    unset_arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    unset_arguments.set("patch_entries", JsonValue{JsonValue::Array{unset_policy}});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::PatchWorkflow, unset_arguments)
                   .has_value());

    // Skip applies to step_arguments only.
    JsonValue skip_step{JsonValue::Object{}};
    skip_step.set("target", JsonValue{std::string("step_arguments")});
    skip_step.set("op", JsonValue{std::string("skip")});
    skip_step.set("path", JsonValue{std::string("11111111111111111111111111111111")});
    JsonValue skip_arguments = base_arguments();
    skip_arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    skip_arguments.set("patch_entries", JsonValue{JsonValue::Array{skip_step}});
    MIRA_CHECK(
        validate_workflow_operation(WorkflowOperation::PatchWorkflow, skip_arguments).has_value());

    JsonValue skip_param = skip_step;
    skip_param.set("target", JsonValue{std::string("run_parameters")});
    JsonValue skip_param_arguments = base_arguments();
    skip_param_arguments.set("patch_id", JsonValue{std::string{kPatchId}});
    skip_param_arguments.set("patch_entries", JsonValue{JsonValue::Array{skip_param}});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::PatchWorkflow, skip_param_arguments)
                   .has_value());

    // Empty entry lists fail the schema (minItems 1).
    JsonValue empty_entries = base_arguments();
    empty_entries.set("patch_id", JsonValue{std::string{kPatchId}});
    empty_entries.set("patch_entries", JsonValue{JsonValue::Array{}});
    MIRA_CHECK(!validate_workflow_operation(WorkflowOperation::PatchWorkflow, empty_entries)
                   .has_value());
    return 0;
}

int patch_digest_is_the_idempotency_identity() {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::RunParameters;
    entry.op = WorkflowPatchOp::Set;
    entry.path = "contact";
    entry.value = JsonValue{std::string("li si")};

    const WorkflowPatchId patch_id = WorkflowPatchId::generate();
    const Sha256Digest digest = workflow_patch_digest(patch_id, {entry});
    MIRA_CHECK(digest == workflow_patch_digest(patch_id, {entry}));

    // Entry order is significant: a different order is a different patch.
    WorkflowPatchEntry other;
    other.target = WorkflowPatchTarget::StepArguments;
    other.op = WorkflowPatchOp::Skip;
    other.path = "step";
    MIRA_CHECK(workflow_patch_digest(patch_id, {entry, other}) !=
               workflow_patch_digest(patch_id, {other, entry}));

    // A different patch id never collides.
    MIRA_CHECK(workflow_patch_digest(WorkflowPatchId::generate(), {entry}) != digest);
    return 0;
}

int error_envelope_is_schema_clean() {
    Error error;
    error.code = ErrorCode::InvalidState;
    error.domain = "mira.workflow";
    error.domain_code = 7;
    error.retryable = false;
    error.safe_message = "run transition is not legal";
    error.operation_id = OperationId::generate();

    const JsonValue envelope = workflow_operation_error_envelope(error);
    const auto &spec = workflow_operation_spec(WorkflowOperation::RunWorkflow);
    const auto violations = validate_instance_against_schema(envelope, spec.error_schema);
    MIRA_CHECK(violations.empty());

    // Without an operation id the optional member is simply absent.
    error.operation_id.reset();
    const JsonValue bare = workflow_operation_error_envelope(error);
    MIRA_CHECK(validate_instance_against_schema(bare, spec.error_schema).empty());
    return 0;
}

} // namespace

int main() {
    if (spec_set_is_frozen_and_subset_clean() != 0) {
        return 1;
    }
    if (control_operations_validate_by_schema() != 0) {
        return 1;
    }
    if (run_workflow_validates_shape_not_semantics() != 0) {
        return 1;
    }
    if (patch_entries_follow_the_closed_semantics() != 0) {
        return 1;
    }
    if (patch_digest_is_the_idempotency_identity() != 0) {
        return 1;
    }
    if (error_envelope_is_schema_clean() != 0) {
        return 1;
    }
    return 0;
}
