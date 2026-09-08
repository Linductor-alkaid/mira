#include <mira/workflow_tools.hpp>

#include <mira/model_schema.hpp>

#include <array>
#include <span>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error tool_error(ErrorCode code, std::string_view detail) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = "workflow tool: ";
    error.safe_message += detail;
    return error;
}

[[nodiscard]] JsonValue id_pattern() {
    JsonValue::Object pattern;
    pattern.emplace_back("type", "string");
    pattern.emplace_back("pattern", "^[0-9a-fA-F]{32}$");
    pattern.emplace_back("description", "128-bit id as 32 hex characters");
    return JsonValue{std::move(pattern)};
}

[[nodiscard]] JsonValue digest_pattern() {
    JsonValue::Object pattern;
    pattern.emplace_back("type", "string");
    pattern.emplace_back("pattern", "^[0-9a-fA-F]{64}$");
    pattern.emplace_back("description", "SHA-256 content digest as 64 hex characters");
    return JsonValue{std::move(pattern)};
}

[[nodiscard]] JsonValue enum_members(std::span<const std::string_view> names) {
    JsonValue::Array values;
    for (const auto name : names) {
        values.emplace_back(std::string{name});
    }
    return JsonValue{std::move(values)};
}

constexpr std::array<std::string_view, 5> kPolicyNames = {"strict", "recoverable",
                                                          "agent_assisted", "interactive",
                                                          "dry_run"};
constexpr std::array<std::string_view, 8> kRunStateNames = {
    "created", "running", "paused", "waiting_user", "waiting_agent", "completed", "failed",
    "cancelled"};
constexpr std::array<std::string_view, 3> kPatchTargets = {"run_parameters", "step_arguments",
                                                           "execution_policy"};
constexpr std::array<std::string_view, 3> kPatchOps = {"set", "unset", "skip"};

[[nodiscard]] JsonSchema schema_of(JsonValue root) {
    JsonSchema schema;
    schema.root = std::move(root);
    return schema;
}

[[nodiscard]] JsonValue run_workflow_parameters_schema() {
    JsonValue::Object properties;
    properties.emplace_back("workflow_id", id_pattern());
    properties.emplace_back("ir_digest", digest_pattern());
    JsonValue::Object parameters;
    parameters.emplace_back("type", "object");
    parameters.emplace_back("description",
                            "Caller arguments; binding against the workflow parameter schema "
                            "is a runtime check, not a tool-layer check");
    properties.emplace_back("parameters", JsonValue{std::move(parameters)});
    JsonValue::Object policy;
    policy.emplace_back("type", "string");
    policy.emplace_back("enum", enum_members(kPolicyNames));
    policy.emplace_back("description",
                        "Requested policy; membership in the workflow allowed set is a "
                        "runtime check");
    properties.emplace_back("policy", JsonValue{std::move(policy)});
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("workflow_id");
    required.emplace_back("ir_digest");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue control_parameters_schema() {
    JsonValue::Object properties;
    properties.emplace_back("workflow_id", id_pattern());
    properties.emplace_back("run_id", id_pattern());
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("workflow_id");
    required.emplace_back("run_id");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue patch_workflow_parameters_schema() {
    JsonValue::Object entry_properties;
    entry_properties.emplace_back("target", [&] {
        JsonValue::Object target;
        target.emplace_back("type", "string");
        target.emplace_back("enum", enum_members(kPatchTargets));
        return JsonValue{std::move(target)};
    }());
    entry_properties.emplace_back("op", [&] {
        JsonValue::Object op;
        op.emplace_back("type", "string");
        op.emplace_back("enum", enum_members(kPatchOps));
        return JsonValue{std::move(op)};
    }());
    JsonValue::Object path;
    path.emplace_back("type", "string");
    path.emplace_back("minLength", static_cast<std::int64_t>(1));
    path.emplace_back("maxLength", static_cast<std::int64_t>(256));
    entry_properties.emplace_back("path", JsonValue{std::move(path)});
    JsonValue::Object value;
    value.emplace_back("description", "Scalar or structured value; required for op=set");
    entry_properties.emplace_back("value", JsonValue{std::move(value)});
    JsonValue::Object entry;
    entry.emplace_back("type", "object");
    entry.emplace_back("properties", JsonValue{std::move(entry_properties)});
    JsonValue::Array entry_required;
    entry_required.emplace_back("target");
    entry_required.emplace_back("op");
    entry_required.emplace_back("path");
    entry.emplace_back("required", JsonValue{std::move(entry_required)});
    entry.emplace_back("additionalProperties", false);
    JsonValue::Object items;
    items.emplace_back("type", "array");
    items.emplace_back("minItems", static_cast<std::int64_t>(1));
    items.emplace_back("maxItems", static_cast<std::int64_t>(32));
    items.emplace_back("items", JsonValue{std::move(entry)});
    JsonValue::Object properties;
    properties.emplace_back("workflow_id", id_pattern());
    properties.emplace_back("run_id", id_pattern());
    properties.emplace_back("patch_id", id_pattern());
    properties.emplace_back("patch_entries", JsonValue{std::move(items)});
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("workflow_id");
    required.emplace_back("run_id");
    required.emplace_back("patch_id");
    required.emplace_back("patch_entries");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue run_result_schema() {
    JsonValue::Object properties;
    properties.emplace_back("run_id", id_pattern());
    JsonValue::Object state;
    state.emplace_back("type", "string");
    state.emplace_back("enum", enum_members(kRunStateNames));
    properties.emplace_back("state", JsonValue{std::move(state)});
    JsonValue::Object summary;
    summary.emplace_back("type", "string");
    summary.emplace_back("maxLength", static_cast<std::int64_t>(2048));
    properties.emplace_back("safe_summary", JsonValue{std::move(summary)});
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("run_id");
    required.emplace_back("state");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue cancel_result_schema() {
    JsonValue root = run_result_schema();
    if (auto *properties = root.find("properties"); properties != nullptr) {
        JsonValue::Object already_terminal;
        already_terminal.emplace_back("type", "boolean");
        already_terminal.emplace_back(
            "description", "True when the run was already terminal; cancel is idempotent");
        properties->set("already_terminal", JsonValue{std::move(already_terminal)});
    }
    return root;
}

[[nodiscard]] JsonValue patch_result_schema() {
    JsonValue::Object properties;
    properties.emplace_back("run_id", id_pattern());
    JsonValue::Object applied;
    applied.emplace_back("type", "boolean");
    properties.emplace_back("applied", JsonValue{std::move(applied)});
    JsonValue::Object epoch;
    epoch.emplace_back("type", "integer");
    epoch.emplace_back("minimum", static_cast<std::int64_t>(0));
    properties.emplace_back("run_patch_epoch", JsonValue{std::move(epoch)});
    JsonValue::Object summary;
    summary.emplace_back("type", "string");
    summary.emplace_back("maxLength", static_cast<std::int64_t>(2048));
    properties.emplace_back("safe_summary", JsonValue{std::move(summary)});
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("run_id");
    required.emplace_back("applied");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

// Host-facing details envelope shared by all five operations.
[[nodiscard]] JsonValue details_schema() {
    JsonValue::Object properties;
    JsonValue::Object summary;
    summary.emplace_back("type", "string");
    summary.emplace_back("maxLength", static_cast<std::int64_t>(2048));
    properties.emplace_back("safe_summary", JsonValue{std::move(summary)});
    JsonValue::Object diagnostics;
    diagnostics.emplace_back("type", "array");
    JsonValue::Object items;
    items.emplace_back("type", "string");
    diagnostics.emplace_back("items", JsonValue{std::move(items)});
    properties.emplace_back("diagnostics", JsonValue{std::move(diagnostics)});
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    root.emplace_back("required", JsonValue::Array{JsonValue{"safe_summary"}});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

// Unified error envelope (DEC-021 §2).
[[nodiscard]] JsonValue error_schema() {
    JsonValue::Object properties;
    JsonValue::Object code;
    code.emplace_back("type", "integer");
    properties.emplace_back("code", JsonValue{std::move(code)});
    JsonValue::Object domain;
    domain.emplace_back("type", "string");
    properties.emplace_back("domain", JsonValue{std::move(domain)});
    JsonValue::Object domain_code;
    domain_code.emplace_back("type", "integer");
    properties.emplace_back("domain_code", JsonValue{std::move(domain_code)});
    JsonValue::Object retryable;
    retryable.emplace_back("type", "boolean");
    properties.emplace_back("retryable", JsonValue{std::move(retryable)});
    JsonValue::Object message;
    message.emplace_back("type", "string");
    message.emplace_back("maxLength", static_cast<std::int64_t>(2048));
    properties.emplace_back("safe_message", JsonValue{std::move(message)});
    properties.emplace_back("operation_id", id_pattern());
    JsonValue::Object root;
    root.emplace_back("type", "object");
    root.emplace_back("properties", JsonValue{std::move(properties)});
    JsonValue::Array required;
    required.emplace_back("code");
    required.emplace_back("domain");
    required.emplace_back("domain_code");
    required.emplace_back("retryable");
    required.emplace_back("safe_message");
    root.emplace_back("required", JsonValue{std::move(required)});
    root.emplace_back("additionalProperties", false);
    return JsonValue{std::move(root)};
}

[[nodiscard]] WorkflowOperationSpec make_spec(WorkflowOperation operation, std::string description,
                                              JsonValue parameters, JsonValue result,
                                              bool has_side_effects) {
    WorkflowOperationSpec spec;
    spec.operation = operation;
    spec.description = std::move(description);
    spec.parameters_schema = schema_of(std::move(parameters));
    spec.result_schema = schema_of(std::move(result));
    spec.details_schema = schema_of(details_schema());
    spec.error_schema = schema_of(error_schema());
    spec.has_side_effects = has_side_effects;
    return spec;
}

[[nodiscard]] std::vector<WorkflowOperationSpec> build_specs() {
    std::vector<WorkflowOperationSpec> specs;
    specs.push_back(make_spec(
        WorkflowOperation::RunWorkflow,
        "Start one run of a workflow pinned to an ir digest; drives environment actions "
        "through the single execution channel",
        run_workflow_parameters_schema(), run_result_schema(), true));
    specs.push_back(make_spec(WorkflowOperation::PatchWorkflow,
                              "Submit an idempotent patch to a running workflow",
                              patch_workflow_parameters_schema(), patch_result_schema(), false));
    specs.push_back(make_spec(WorkflowOperation::PauseWorkflow,
                              "Request a pause; settles after in-flight actions converge and "
                              "platform input is released",
                              control_parameters_schema(), run_result_schema(), false));
    specs.push_back(make_spec(WorkflowOperation::ResumeWorkflow,
                              "Resume a paused or waiting run from its step boundary with a "
                              "fresh observation",
                              control_parameters_schema(), run_result_schema(), false));
    specs.push_back(make_spec(WorkflowOperation::CancelWorkflow,
                              "Cancel a run; terminal states answer idempotently",
                              control_parameters_schema(), cancel_result_schema(), false));
    return specs;
}

} // namespace

std::string_view workflow_operation_wire_name(WorkflowOperation operation) {
    switch (operation) {
    case WorkflowOperation::RunWorkflow:
        return "run_workflow";
    case WorkflowOperation::PatchWorkflow:
        return "patch_workflow";
    case WorkflowOperation::PauseWorkflow:
        return "pause_workflow";
    case WorkflowOperation::ResumeWorkflow:
        return "resume_workflow";
    case WorkflowOperation::CancelWorkflow:
        return "cancel_workflow";
    }
    return "unknown_workflow_operation";
}

Result<WorkflowOperation> workflow_operation_from_wire_name(std::string_view name) {
    for (auto operation : {WorkflowOperation::RunWorkflow, WorkflowOperation::PatchWorkflow,
                           WorkflowOperation::PauseWorkflow, WorkflowOperation::ResumeWorkflow,
                           WorkflowOperation::CancelWorkflow}) {
        if (workflow_operation_wire_name(operation) == name) {
            return operation;
        }
    }
    return tool_error(ErrorCode::InvalidArgument, "unknown workflow operation name");
}

const std::vector<WorkflowOperationSpec> &workflow_operation_specs() {
    static const std::vector<WorkflowOperationSpec> specs = build_specs();
    return specs;
}

const WorkflowOperationSpec &workflow_operation_spec(WorkflowOperation operation) {
    const auto &specs = workflow_operation_specs();
    for (const auto &spec : specs) {
        if (spec.operation == operation) {
            return spec;
        }
    }
    return specs.front();
}

std::string workflow_patch_op_name(WorkflowPatchOp op) {
    switch (op) {
    case WorkflowPatchOp::Set:
        return "set";
    case WorkflowPatchOp::Unset:
        return "unset";
    case WorkflowPatchOp::Skip:
        return "skip";
    }
    return "unknown";
}

Result<WorkflowPatchOp> parse_workflow_patch_op(std::string_view name) {
    for (auto op : {WorkflowPatchOp::Set, WorkflowPatchOp::Unset, WorkflowPatchOp::Skip}) {
        if (workflow_patch_op_name(op) == name) {
            return op;
        }
    }
    return tool_error(ErrorCode::InvalidArgument, "unknown patch op");
}

Result<void> validate_workflow_patch_entry(const WorkflowPatchEntry &entry,
                                           const WorkflowLimits &limits) {
    if (entry.path.empty() || entry.path.size() > limits.max_string_bytes) {
        return tool_error(ErrorCode::InvalidArgument, "patch path is not bounded");
    }
    switch (entry.target) {
    case WorkflowPatchTarget::RunParameters:
        if (entry.op == WorkflowPatchOp::Skip) {
            return tool_error(ErrorCode::InvalidArgument,
                              "skip applies to step_arguments only");
        }
        break;
    case WorkflowPatchTarget::StepArguments:
        break;
    case WorkflowPatchTarget::ExecutionPolicy:
        if (entry.op != WorkflowPatchOp::Set) {
            return tool_error(ErrorCode::InvalidArgument,
                              "execution_policy only supports set");
        }
        if (entry.path != "policy") {
            return tool_error(ErrorCode::InvalidArgument,
                              "execution_policy path must be 'policy'");
        }
        if (!entry.value.is_string() || !parse_workflow_policy(*entry.value.as_string()).has_value()) {
            return tool_error(ErrorCode::InvalidArgument,
                              "execution_policy value must name a known policy");
        }
        break;
    }
    if (entry.op == WorkflowPatchOp::Set && entry.value.is_null()) {
        return tool_error(ErrorCode::InvalidArgument, "set entries require a value");
    }
    if (entry.op != WorkflowPatchOp::Set && !entry.value.is_null()) {
        return tool_error(ErrorCode::InvalidArgument, "only set entries carry a value");
    }
    return Result<void>{};
}

Sha256Digest workflow_patch_digest(const WorkflowPatchId &patch_id,
                                   const std::vector<WorkflowPatchEntry> &entries) {
    JsonValue::Array entry_values;
    for (const auto &entry : entries) {
        JsonValue::Object object;
        object.emplace_back("target", workflow_patch_target_name(entry.target));
        object.emplace_back("op", workflow_patch_op_name(entry.op));
        object.emplace_back("path", entry.path);
        if (!entry.value.is_null()) {
            object.emplace_back("value", entry.value);
        }
        entry_values.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("patch_id", patch_id.to_string());
    root.emplace_back("entries", JsonValue{std::move(entry_values)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

Result<void> validate_workflow_operation(WorkflowOperation operation, const JsonValue &arguments) {
    const WorkflowOperationSpec &spec = workflow_operation_spec(operation);
    if (auto gate = gate_schema_subset(spec.parameters_schema); !gate.has_value()) {
        return gate.error();
    }
    const auto violations = validate_instance_against_schema(arguments, spec.parameters_schema);
    if (!violations.empty()) {
        Error error = tool_error(ErrorCode::InvalidArgument,
                                 "workflow operation arguments failed schema validation");
        error.safe_message += ": ";
        error.safe_message += violations.front().message;
        return error;
    }
    if (operation == WorkflowOperation::PatchWorkflow) {
        const auto *entries = arguments.find("patch_entries");
        for (const auto &item : *entries->as_array()) {
            WorkflowPatchEntry entry;
            auto target = parse_workflow_patch_target(*item.find("target")->as_string());
            if (!target.has_value()) {
                return target.error();
            }
            entry.target = target.value();
            auto op = parse_workflow_patch_op(*item.find("op")->as_string());
            if (!op.has_value()) {
                return op.error();
            }
            entry.op = op.value();
            const auto *path = item.find("path");
            entry.path = *path->as_string();
            if (const auto *value = item.find("value"); value != nullptr) {
                entry.value = *value;
            }
            if (auto check = validate_workflow_patch_entry(entry); !check.has_value()) {
                return check;
            }
        }
    }
    return Result<void>{};
}

JsonValue workflow_operation_error_envelope(const Error &error) {
    JsonValue::Object object;
    object.emplace_back("code", static_cast<std::int64_t>(error.code));
    object.emplace_back("domain", error.domain);
    object.emplace_back("domain_code", error.domain_code);
    object.emplace_back("retryable", error.retryable);
    object.emplace_back("safe_message", error.safe_message);
    if (error.operation_id) {
        object.emplace_back("operation_id", error.operation_id->to_string());
    }
    return JsonValue{std::move(object)};
}

} // namespace mira
