#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Workflow execution policy (DEC-020 §3) and IR support enums
// ---------------------------------------------------------------------------

enum class WorkflowPolicy : std::uint8_t {
    Strict,
    Recoverable,
    AgentAssisted,
    Interactive,
    DryRun,
};

// Closed set of policy names on the wire; unknown names fail closed.
[[nodiscard]] std::string workflow_policy_name(WorkflowPolicy policy);
[[nodiscard]] Result<WorkflowPolicy> parse_workflow_policy(std::string_view name);
[[nodiscard]] bool workflow_policy_allows_agent(WorkflowPolicy policy) noexcept;
[[nodiscard]] bool workflow_policy_dispatches_side_effects(WorkflowPolicy policy) noexcept;

enum class WorkflowParameterType : std::uint8_t { String, Integer, Number, Boolean };
[[nodiscard]] std::string workflow_parameter_type_name(WorkflowParameterType type);
[[nodiscard]] Result<WorkflowParameterType> parse_workflow_parameter_type(std::string_view name);

// Deterministic parameter binding error codes (DEC-019 §2). Stable domain_code
// symbols; the safe_message carries the offending parameter name only.
enum class WorkflowBindError : std::int32_t {
    UnknownParameter = 1,
    MissingRequired = 2,
    TypeMismatch = 3,
    ConstraintViolated = 4,
    InvalidDefault = 5,
    InvalidArguments = 6,
};

[[nodiscard]] Error make_workflow_bind_error(WorkflowBindError code, std::string detail);

// ---------------------------------------------------------------------------
// Workflow IR (DEC-019): limits, structures, serialization
// ---------------------------------------------------------------------------

// RULE-08 limits enforced while decoding and validating an IR document. The
// defaults can be tightened per call but never relaxed past the ceiling
// constants below.
struct WorkflowLimits final {
    std::size_t max_document_bytes = 256 * 1024;
    std::size_t max_depth = 16;
    std::size_t max_steps = 256;
    std::size_t max_parameters = 64;
    std::size_t max_arguments_bytes = 64 * 1024;
    std::size_t max_string_bytes = 8 * 1024;
    std::size_t max_enum_values = 64;
    std::size_t max_pattern_bytes = 256;
};

extern const WorkflowLimits kDefaultWorkflowLimits;

// v1 predicate DSL (DEC-019 §1 / workflow_runtime_design §4.1). One closed
// assertion form: a signal reference, a comparison and a scalar value.
enum class WorkflowPredicateOp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge, Contains, Exists };
[[nodiscard]] std::string workflow_predicate_op_name(WorkflowPredicateOp op);
[[nodiscard]] Result<WorkflowPredicateOp> parse_workflow_predicate_op(std::string_view name);

struct WorkflowPredicate final {
    // "<signal-kind>:<ref>" with kind in {run_parameter, step_result,
    // screen_state}; screen_state is not evaluable before phase E.
    std::string signal;
    WorkflowPredicateOp op = WorkflowPredicateOp::Eq;
    JsonValue value; // Null only for Exists.
    [[nodiscard]] bool operator==(const WorkflowPredicate &) const noexcept = default;
};

enum class WorkflowStepKind : std::uint8_t { ToolCall, Navigate, Verify, Control };
[[nodiscard]] std::string workflow_step_kind_name(WorkflowStepKind kind);
[[nodiscard]] Result<WorkflowStepKind> parse_workflow_step_kind(std::string_view name);

struct WorkflowRecoveryHook final {
    enum class Mode : std::uint8_t { None, Retry, FallbackStep, AgentEscalation };
    Mode mode = Mode::None;
    std::uint32_t max_retries = 0;           // Retry; 0..8, bounded by attempts.
    std::optional<StepId> fallback_step;     // FallbackStep; must come later.
    [[nodiscard]] bool operator==(const WorkflowRecoveryHook &) const noexcept = default;
};

struct WorkflowStep final {
    StepId id;
    std::string name;
    WorkflowStepKind kind = WorkflowStepKind::ToolCall;
    // Explicit loop-head annotation; only loop heads may be Control targets.
    bool loop_head = false;
    // Step arguments. May contain parameter references encoded as the single
    // closed form {"$param": "<name>"} anywhere in the tree. Navigate steps
    // carry their opaque navigation target as arguments["target"] (string).
    JsonValue arguments;
    std::optional<WorkflowPredicate> precondition;
    // Required for Verify steps, optional elsewhere.
    std::optional<WorkflowPredicate> verification;
    std::optional<WorkflowRecoveryHook> recovery;
    std::uint32_t max_attempts = 1; // 1..8
    // Control steps only: backward jump to a loop_head step declared earlier
    // in the sequence, with the bounded iteration budget.
    std::optional<StepId> jump_to;
    std::uint32_t max_iterations = 1; // 1..64
    [[nodiscard]] bool operator==(const WorkflowStep &) const noexcept = default;
};

struct WorkflowParameterSpec final {
    std::string name;
    WorkflowParameterType type = WorkflowParameterType::String;
    bool required = false;
    JsonValue default_value; // Null when absent; optional parameters only.
    std::optional<double> minimum;         // Integer/Number.
    std::optional<double> maximum;         // Integer/Number.
    std::optional<std::uint64_t> min_length; // String.
    std::optional<std::uint64_t> max_length; // String.
    std::optional<std::string> pattern;    // String; std::regex, search semantics.
    std::vector<JsonValue> enum_values;    // Any scalar type.
    std::string summary;
};

struct WorkflowDefinition final {
    SchemaVersion schema_version{1, 0};
    WorkflowId workflow_id;
    std::string name;
    std::string summary;
    std::vector<WorkflowParameterSpec> parameters;
    std::vector<WorkflowStep> steps;
    WorkflowPolicy default_policy = WorkflowPolicy::Strict; // Provisional default.
    std::vector<WorkflowPolicy> allowed_policies;
};

// Serialization and strict decoding. Unknown fields, unsupported versions and
// limit violations fail closed (DEC-019 §4/§5); round trips are lossless.
[[nodiscard]] JsonValue workflow_definition_to_json(const WorkflowDefinition &definition);

// Structural validation of an in-memory definition with the same semantics as
// decoding: the definition is canonicalized and re-decoded, so struct-built
// definitions (never parsed from JSON) face the identical fail-closed checks
// (control jumps, fallback targets, policy sets, predicate shapes, limits).
[[nodiscard]] Result<void>
validate_workflow_definition(const WorkflowDefinition &definition,
                             const WorkflowLimits &limits = kDefaultWorkflowLimits);
[[nodiscard]] Result<WorkflowDefinition>
workflow_definition_from_json(const JsonValue &json,
                              const WorkflowLimits &limits = kDefaultWorkflowLimits);
[[nodiscard]] Result<WorkflowDefinition>
parse_workflow_definition(std::string_view text, const WorkflowLimits &limits = kDefaultWorkflowLimits);

// Content-addressed identity: canonical JSON digest of the definition.
[[nodiscard]] Sha256Digest workflow_definition_digest(const WorkflowDefinition &definition);

// ---------------------------------------------------------------------------
// Parameter binding and step argument resolution (DEC-019 §2/§3, pure)
// ---------------------------------------------------------------------------

struct WorkflowParameterBindings final {
    // Effective parameter values (defaults applied), keyed by parameter name.
    JsonValue values;
    Sha256Digest digest{};
};

// Validates that declared defaults satisfy their own constraints.
[[nodiscard]] Result<void> validate_workflow_parameter_specs(const WorkflowDefinition &definition);

// Binds caller arguments against the declared parameter schema. Pure function;
// rejects unknown parameters, missing required values, type mismatches and
// constraint violations with the deterministic WorkflowBindError codes.
[[nodiscard]] Result<WorkflowParameterBindings>
bind_workflow_parameters(const WorkflowDefinition &definition, const JsonValue &input);

// Substitutes {"$param": name} references with bound values. Unknown
// references, nested references and unknown keys inside the reference object
// fail closed. Values without references pass through unchanged.
[[nodiscard]] Result<JsonValue> resolve_step_arguments(const WorkflowParameterBindings &bindings,
                                                       const JsonValue &arguments);

// ---------------------------------------------------------------------------
// Predicate evaluation (pure; the runtime drives it from phase B)
// ---------------------------------------------------------------------------

enum class WorkflowPredicateResult : std::uint8_t { Satisfied, NotSatisfied, NotEvaluable };

// Evaluates one predicate against a context object whose keys are full signal
// references ("run_parameter:contact", "step_result:send", "screen_state:x").
// Absent signals and type mismatches are NotEvaluable (fail closed), except
// Exists which answers presence directly.
[[nodiscard]] WorkflowPredicateResult
evaluate_workflow_predicate(const WorkflowPredicate &predicate, const JsonValue &context);

// Validates the predicate's signal reference shape ("kind:ref", non-empty ref).
[[nodiscard]] Result<void> validate_workflow_predicate(const WorkflowPredicate &predicate,
                                                       const WorkflowLimits &limits = kDefaultWorkflowLimits);

} // namespace mira
