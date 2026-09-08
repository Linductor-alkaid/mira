#include <mira/workflow_ir.hpp>

#include <algorithm>
#include <functional>
#include <regex>
#include <span>
#include <utility>

namespace mira {
namespace {

// Reader capability: documents must share the major and not exceed the
// supported minor (DEC-019 §4).
constexpr SchemaVersion kWorkflowIrReaderVersion{1, 0};

// Hard ceilings behind WorkflowLimits; callers may tighten, never relax.
constexpr std::size_t kCeilingDocumentBytes = 256 * 1024;
constexpr std::size_t kCeilingDepth = 16;
constexpr std::size_t kCeilingSteps = 256;
constexpr std::size_t kCeilingParameters = 64;
constexpr std::size_t kCeilingArgumentsBytes = 64 * 1024;
constexpr std::size_t kCeilingStringBytes = 8 * 1024;
constexpr std::size_t kCeilingEnumValues = 64;
constexpr std::size_t kCeilingPatternBytes = 256;
constexpr std::uint32_t kCeilingMaxAttempts = 8;
constexpr std::uint32_t kCeilingMaxIterations = 64;
constexpr std::uint32_t kCeilingMaxRetries = 8;

const WorkflowLimits &clamp_limits(const WorkflowLimits &limits) {
    static thread_local WorkflowLimits clamped;
    clamped = limits;
    clamped.max_document_bytes = std::min(clamped.max_document_bytes, kCeilingDocumentBytes);
    clamped.max_depth = std::min(clamped.max_depth, kCeilingDepth);
    clamped.max_steps = std::min(clamped.max_steps, kCeilingSteps);
    clamped.max_parameters = std::min(clamped.max_parameters, kCeilingParameters);
    clamped.max_arguments_bytes = std::min(clamped.max_arguments_bytes, kCeilingArgumentsBytes);
    clamped.max_string_bytes = std::min(clamped.max_string_bytes, kCeilingStringBytes);
    clamped.max_enum_values = std::min(clamped.max_enum_values, kCeilingEnumValues);
    clamped.max_pattern_bytes = std::min(clamped.max_pattern_bytes, kCeilingPatternBytes);
    return clamped;
}

[[nodiscard]] Error ir_error(ErrorCode code, std::string_view detail) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = "workflow ir: ";
    error.safe_message += detail;
    return error;
}

// Exact key-set check: every present key must be declared and (unless
// optional) required keys must exist. Unknown fields fail closed (DEC-019).
[[nodiscard]] Result<void> check_keys(const JsonValue &object, std::span<const std::string_view> required,
           std::span<const std::string_view> optional, std::string_view where) {
    if (!object.is_object()) {
        return ir_error(ErrorCode::InvalidArgument, std::string{where} + " must be an object");
    }
    for (const auto &member : *object.as_object()) {
        const bool known = std::any_of(required.begin(), required.end(),
                                       [&](std::string_view key) { return key == member.first; }) ||
                           std::any_of(optional.begin(), optional.end(),
                                       [&](std::string_view key) { return key == member.first; });
        if (!known) {
            return ir_error(ErrorCode::UnsupportedVersion,
                            std::string{where} + ": unknown field '" + member.first + "'");
        }
    }
    for (const auto key : required) {
        if (object.find(key) == nullptr) {
            return ir_error(ErrorCode::InvalidArgument,
                            std::string{where} + ": missing field '" + std::string{key} + "'");
        }
    }
    return Result<void>{};
}

[[nodiscard]] std::optional<std::string> bounded_string(const JsonValue &value,
                                                        std::size_t max_bytes) {
    const auto *text = value.as_string();
    if (text == nullptr || text->empty() || text->size() > max_bytes) {
        return std::nullopt;
    }
    return *text;
}

[[nodiscard]] std::optional<Id128> parse_id(const JsonValue &value) {
    const auto *text = value.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    return Id128::parse(*text);
}

[[nodiscard]] Result<void> check_depth(const JsonValue &value, std::size_t max_depth) {
    std::function<std::size_t(const JsonValue &, std::size_t)> depth_of =
        [&](const JsonValue &node, std::size_t current) -> std::size_t {
        std::size_t child_max = current;
        if (node.is_object()) {
            for (const auto &member : *node.as_object()) {
                child_max = std::max(child_max, depth_of(member.second, current + 1));
            }
        } else if (node.is_array()) {
            for (const auto &item : *node.as_array()) {
                child_max = std::max(child_max, depth_of(item, current + 1));
            }
        }
        return child_max;
    };
    if (depth_of(value, 1) > max_depth) {
        return ir_error(ErrorCode::ResourceExhausted, "workflow ir: nesting depth exceeds limit");
    }
    return Result<void>{};
}

[[nodiscard]] Result<std::optional<WorkflowPredicate>>
parse_predicate(const JsonValue &json, const WorkflowLimits &limits) {
    if (json.is_null()) {
        return std::optional<WorkflowPredicate>{};
    }
    static constexpr std::string_view kRequired[] = {"signal", "op"};
    static constexpr std::string_view kOptional[] = {"value"};
    if (auto check = check_keys(json, kRequired, kOptional, "predicate"); !check.has_value()) {
        return check.error();
    }
    WorkflowPredicate predicate;
    const auto signal = bounded_string(*json.find("signal"), limits.max_string_bytes);
    if (!signal) {
        return ir_error(ErrorCode::InvalidArgument, "predicate signal must be a bounded string");
    }
    predicate.signal = *signal;
    const auto *op_text = json.find("op");
    if (op_text == nullptr || !op_text->is_string()) {
        return ir_error(ErrorCode::InvalidArgument, "predicate op must be a string");
    }
    auto op = parse_workflow_predicate_op(*op_text->as_string());
    if (!op.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "predicate op is unknown");
    }
    predicate.op = op.value();
    const auto *value = json.find("value");
    if (predicate.op == WorkflowPredicateOp::Exists) {
        if (value != nullptr && !value->is_null()) {
            return ir_error(ErrorCode::InvalidArgument,
                            "predicate value must be absent for op 'exists'");
        }
        predicate.value = JsonValue{};
    } else {
        if (value == nullptr) {
            return ir_error(ErrorCode::InvalidArgument, "predicate value is required");
        }
        if (value->is_array() || value->is_object()) {
            return ir_error(ErrorCode::InvalidArgument, "predicate value must be a scalar");
        }
        predicate.value = *value;
    }
    if (auto valid = validate_workflow_predicate(predicate, limits); !valid.has_value()) {
        return valid.error();
    }
    return std::optional<WorkflowPredicate>{predicate};
}

[[nodiscard]] JsonValue predicate_to_json(const WorkflowPredicate &predicate) {
    JsonValue::Object object;
    object.emplace_back("signal", predicate.signal);
    object.emplace_back("op", workflow_predicate_op_name(predicate.op));
    if (predicate.op != WorkflowPredicateOp::Exists) {
        object.emplace_back("value", predicate.value);
    }
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<std::optional<WorkflowRecoveryHook>>
parse_recovery(const JsonValue &json) {
    if (json.is_null()) {
        return std::optional<WorkflowRecoveryHook>{};
    }
    static constexpr std::string_view kRequired[] = {"mode"};
    static constexpr std::string_view kOptional[] = {"max_retries", "fallback_step"};
    if (auto check = check_keys(json, kRequired, kOptional, "recovery"); !check.has_value()) {
        return check.error();
    }
    WorkflowRecoveryHook hook;
    const auto *mode_text = json.find("mode");
    if (mode_text == nullptr || !mode_text->is_string()) {
        return ir_error(ErrorCode::InvalidArgument, "recovery mode must be a string");
    }
    const auto mode = *mode_text->as_string();
    const auto *max_retries = json.find("max_retries");
    const auto *fallback = json.find("fallback_step");
    if (mode == "none") {
        hook.mode = WorkflowRecoveryHook::Mode::None;
        if (max_retries != nullptr || fallback != nullptr) {
            return ir_error(ErrorCode::InvalidArgument,
                            "recovery 'none' admits no further fields");
        }
    } else if (mode == "retry") {
        hook.mode = WorkflowRecoveryHook::Mode::Retry;
        if (max_retries == nullptr || !max_retries->is_integer()) {
            return ir_error(ErrorCode::InvalidArgument, "recovery retry requires max_retries");
        }
        const auto retries = max_retries->as_integer().value();
        if (retries < 1 || retries > kCeilingMaxRetries) {
            return ir_error(ErrorCode::InvalidArgument, "recovery max_retries out of range");
        }
        hook.max_retries = static_cast<std::uint32_t>(retries);
        if (fallback != nullptr) {
            return ir_error(ErrorCode::InvalidArgument, "recovery retry admits no fallback_step");
        }
    } else if (mode == "fallback_step") {
        hook.mode = WorkflowRecoveryHook::Mode::FallbackStep;
        const auto fallback_id = (fallback != nullptr) ? parse_id(*fallback) : std::nullopt;
        if (!fallback_id || fallback_id->is_nil()) {
            return ir_error(ErrorCode::InvalidArgument,
                            "recovery fallback_step requires a step id");
        }
        hook.fallback_step = StepId{*fallback_id};
        if (max_retries != nullptr) {
            return ir_error(ErrorCode::InvalidArgument,
                            "recovery fallback_step admits no max_retries");
        }
    } else if (mode == "agent_escalation") {
        hook.mode = WorkflowRecoveryHook::Mode::AgentEscalation;
        if (max_retries != nullptr || fallback != nullptr) {
            return ir_error(ErrorCode::InvalidArgument,
                            "recovery agent_escalation admits no further fields");
        }
    } else {
        return ir_error(ErrorCode::InvalidArgument, "recovery mode is unknown");
    }
    return std::optional<WorkflowRecoveryHook>{hook};
}

[[nodiscard]] JsonValue recovery_to_json(const WorkflowRecoveryHook &hook) {
    JsonValue::Object object;
    switch (hook.mode) {
    case WorkflowRecoveryHook::Mode::None:
        object.emplace_back("mode", "none");
        break;
    case WorkflowRecoveryHook::Mode::Retry:
        object.emplace_back("mode", "retry");
        object.emplace_back("max_retries", static_cast<std::int64_t>(hook.max_retries));
        break;
    case WorkflowRecoveryHook::Mode::FallbackStep:
        object.emplace_back("mode", "fallback_step");
        object.emplace_back("fallback_step", hook.fallback_step.value_or(StepId{}).to_string());
        break;
    case WorkflowRecoveryHook::Mode::AgentEscalation:
        object.emplace_back("mode", "agent_escalation");
        break;
    }
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<WorkflowParameterSpec>
parse_parameter(const JsonValue &json, const WorkflowLimits &limits) {
    static constexpr std::string_view kRequired[] = {"name", "type", "required"};
    static constexpr std::string_view kOptional[] = {"default", "constraints", "summary"};
    if (auto check = check_keys(json, kRequired, kOptional, "parameter"); !check.has_value()) {
        return check.error();
    }
    WorkflowParameterSpec spec;
    const auto name = bounded_string(*json.find("name"), limits.max_string_bytes);
    if (!name) {
        return ir_error(ErrorCode::InvalidArgument, "parameter name must be a bounded string");
    }
    spec.name = *name;
    const auto *type_text = json.find("type");
    if (type_text == nullptr || !type_text->is_string()) {
        return ir_error(ErrorCode::InvalidArgument, "parameter type must be a string");
    }
    auto type = parse_workflow_parameter_type(*type_text->as_string());
    if (!type.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "parameter type is unknown");
    }
    spec.type = type.value();
    const auto *required = json.find("required");
    if (required == nullptr || !required->is_boolean()) {
        return ir_error(ErrorCode::InvalidArgument, "parameter required must be a boolean");
    }
    spec.required = *required->as_boolean();
    const auto *summary = json.find("summary");
    if (summary != nullptr) {
        const auto text = bounded_string(*summary, limits.max_string_bytes);
        if (!text) {
            return ir_error(ErrorCode::InvalidArgument, "parameter summary is not bounded");
        }
        spec.summary = *text;
    }
    if (spec.required) {
        if (json.find("default") != nullptr) {
            return ir_error(ErrorCode::InvalidArgument,
                            "required parameter must not declare a default");
        }
    }
    const auto *default_value = json.find("default");
    if (default_value != nullptr && !default_value->is_null()) {
        if (spec.required) {
            return ir_error(ErrorCode::InvalidArgument,
                            "required parameter must not declare a default");
        }
        spec.default_value = *default_value;
    }
    const auto *constraints = json.find("constraints");
    if (constraints != nullptr && !constraints->is_null()) {
        static constexpr std::string_view kConstraintKeys[] = {
            "minimum", "maximum", "min_length", "max_length", "pattern", "enum"};
        if (!constraints->is_object()) {
            return ir_error(ErrorCode::InvalidArgument, "constraints must be an object");
        }
        for (const auto &member : *constraints->as_object()) {
            const bool known = std::any_of(std::begin(kConstraintKeys), std::end(kConstraintKeys),
                                            [&](std::string_view key) { return key == member.first; });
            if (!known) {
                return ir_error(ErrorCode::UnsupportedVersion,
                                std::string{"constraints: unknown field '"} + member.first + "'");
            }
        }
        if (const auto *minimum = constraints->find("minimum");
            minimum != nullptr && minimum->is_number()) {
            if (spec.type == WorkflowParameterType::String ||
                spec.type == WorkflowParameterType::Boolean) {
                return ir_error(ErrorCode::InvalidArgument,
                                "minimum applies to numeric parameters only");
            }
            spec.minimum = minimum->as_number();
        }
        if (const auto *maximum = constraints->find("maximum");
            maximum != nullptr && maximum->is_number()) {
            if (spec.type == WorkflowParameterType::String ||
                spec.type == WorkflowParameterType::Boolean) {
                return ir_error(ErrorCode::InvalidArgument,
                                "maximum applies to numeric parameters only");
            }
            spec.maximum = maximum->as_number();
        }
        if (const auto *min_length = constraints->find("min_length");
            min_length != nullptr && min_length->is_integer()) {
            if (spec.type != WorkflowParameterType::String) {
                return ir_error(ErrorCode::InvalidArgument,
                                "min_length applies to string parameters only");
            }
            if (min_length->as_integer().value() < 0) {
                return ir_error(ErrorCode::InvalidArgument, "min_length must be non-negative");
            }
            spec.min_length = static_cast<std::uint64_t>(min_length->as_integer().value());
        }
        if (const auto *max_length = constraints->find("max_length");
            max_length != nullptr && max_length->is_integer()) {
            if (spec.type != WorkflowParameterType::String) {
                return ir_error(ErrorCode::InvalidArgument,
                                "max_length applies to string parameters only");
            }
            if (max_length->as_integer().value() < 0) {
                return ir_error(ErrorCode::InvalidArgument, "max_length must be non-negative");
            }
            spec.max_length = static_cast<std::uint64_t>(max_length->as_integer().value());
        }
        if (const auto *pattern = constraints->find("pattern");
            pattern != nullptr && pattern->is_string()) {
            if (spec.type != WorkflowParameterType::String) {
                return ir_error(ErrorCode::InvalidArgument,
                                "pattern applies to string parameters only");
            }
            if (pattern->as_string()->size() > limits.max_pattern_bytes) {
                return ir_error(ErrorCode::ResourceExhausted, "pattern exceeds limit");
            }
            try {
                std::regex expression{*pattern->as_string()};
                static_cast<void>(expression);
            } catch (const std::regex_error &) {
                return ir_error(ErrorCode::InvalidArgument, "pattern is not a valid regex");
            }
            spec.pattern = *pattern->as_string();
        }
        if (const auto *enum_values = constraints->find("enum"); enum_values != nullptr) {
            if (!enum_values->is_array() || enum_values->as_array()->empty()) {
                return ir_error(ErrorCode::InvalidArgument, "enum must be a non-empty array");
            }
            if (enum_values->as_array()->size() > limits.max_enum_values) {
                return ir_error(ErrorCode::ResourceExhausted, "enum exceeds limit");
            }
            for (const auto &item : *enum_values->as_array()) {
                if (item.is_array() || item.is_object()) {
                    return ir_error(ErrorCode::InvalidArgument, "enum values must be scalars");
                }
                spec.enum_values.push_back(item);
            }
        }
    }
    return spec;
}

[[nodiscard]] JsonValue parameter_to_json(const WorkflowParameterSpec &spec) {
    JsonValue::Object object;
    object.emplace_back("name", spec.name);
    object.emplace_back("type", workflow_parameter_type_name(spec.type));
    object.emplace_back("required", spec.required);
    if (!spec.required && !spec.default_value.is_null()) {
        object.emplace_back("default", spec.default_value);
    }
    JsonValue::Object constraints;
    if (spec.minimum.has_value()) {
        constraints.emplace_back("minimum", *spec.minimum);
    }
    if (spec.maximum.has_value()) {
        constraints.emplace_back("maximum", *spec.maximum);
    }
    if (spec.min_length.has_value()) {
        constraints.emplace_back("min_length", static_cast<std::int64_t>(*spec.min_length));
    }
    if (spec.max_length.has_value()) {
        constraints.emplace_back("max_length", static_cast<std::int64_t>(*spec.max_length));
    }
    if (spec.pattern.has_value()) {
        constraints.emplace_back("pattern", *spec.pattern);
    }
    if (!spec.enum_values.empty()) {
        JsonValue::Array values;
        for (const auto &item : spec.enum_values) {
            values.push_back(item);
        }
        constraints.emplace_back("enum", JsonValue{std::move(values)});
    }
    if (!constraints.empty()) {
        object.emplace_back("constraints", JsonValue{std::move(constraints)});
    }
    if (!spec.summary.empty()) {
        object.emplace_back("summary", spec.summary);
    }
    return JsonValue{std::move(object)};
}

[[nodiscard]] bool value_matches_type(const JsonValue &value, WorkflowParameterType type) {
    switch (type) {
    case WorkflowParameterType::String:
        return value.is_string();
    case WorkflowParameterType::Integer:
        return value.is_integer();
    case WorkflowParameterType::Number:
        return value.is_number();
    case WorkflowParameterType::Boolean:
        return value.is_boolean();
    }
    return false;
}

[[nodiscard]] Result<void> check_constraint(const WorkflowParameterSpec &spec,
                                            const JsonValue &value) {
    const std::string where = "parameter '" + spec.name + "'";
    if (!value_matches_type(value, spec.type)) {
        Error error = make_workflow_bind_error(WorkflowBindError::TypeMismatch, where);
        return error;
    }
    if (spec.type == WorkflowParameterType::String) {
        const auto &text = *value.as_string();
        if (spec.min_length.has_value() && text.size() < *spec.min_length) {
            return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                            where + " below min_length");
        }
        if (spec.max_length.has_value() && text.size() > *spec.max_length) {
            return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                            where + " above max_length");
        }
        if (spec.pattern.has_value()) {
            try {
                const std::regex expression{*spec.pattern};
                if (!std::regex_search(text, expression)) {
                    return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                                    where + " violates pattern");
                }
            } catch (const std::regex_error &) {
                return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                                where + " pattern invalid");
            }
        }
    }
    if (spec.type == WorkflowParameterType::Integer || spec.type == WorkflowParameterType::Number) {
        const auto number = value.as_number();
        if (spec.minimum.has_value() && number < *spec.minimum) {
            return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                            where + " below minimum");
        }
        if (spec.maximum.has_value() && number > *spec.maximum) {
            return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                            where + " above maximum");
        }
    }
    if (!spec.enum_values.empty()) {
        const bool member = std::any_of(spec.enum_values.begin(), spec.enum_values.end(),
                                        [&](const JsonValue &candidate) {
                                            return candidate == value;
                                        });
        if (!member) {
            return make_workflow_bind_error(WorkflowBindError::ConstraintViolated,
                                            where + " outside enum");
        }
    }
    return Result<void>{};
}

[[nodiscard]] Result<WorkflowStep> parse_step(const JsonValue &json, const WorkflowLimits &limits) {
    static constexpr std::string_view kRequired[] = {"step_id", "kind"};
    static constexpr std::string_view kOptional[] = {"name",
                                                     "loop_head",
                                                     "arguments",
                                                     "precondition",
                                                     "verification",
                                                     "recovery",
                                                     "max_attempts",
                                                     "jump_to",
                                                     "max_iterations"};
    if (auto check = check_keys(json, kRequired, kOptional, "step"); !check.has_value()) {
        return check.error();
    }
    WorkflowStep step;
    const auto step_id = parse_id(*json.find("step_id"));
    if (!step_id || step_id->is_nil()) {
        return ir_error(ErrorCode::InvalidArgument, "step_id must be a non-nil id");
    }
    step.id = StepId{*step_id};
    const auto *name = json.find("name");
    if (name != nullptr && !name->is_null()) {
        const auto text = bounded_string(*name, limits.max_string_bytes);
        if (!text) {
            return ir_error(ErrorCode::InvalidArgument, "step name must be a bounded string");
        }
        step.name = *text;
    }
    const auto *kind_text = json.find("kind");
    if (kind_text == nullptr || !kind_text->is_string()) {
        return ir_error(ErrorCode::InvalidArgument, "step kind must be a string");
    }
    auto kind = parse_workflow_step_kind(*kind_text->as_string());
    if (!kind.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "step kind is unknown");
    }
    step.kind = kind.value();
    if (const auto *loop_head = json.find("loop_head"); loop_head != nullptr) {
        if (!loop_head->is_boolean()) {
            return ir_error(ErrorCode::InvalidArgument, "loop_head must be a boolean");
        }
        step.loop_head = *loop_head->as_boolean();
    }
    if (const auto *arguments = json.find("arguments");
        arguments != nullptr && !arguments->is_null()) {
        if (!arguments->is_object()) {
            return ir_error(ErrorCode::InvalidArgument, "step arguments must be an object");
        }
        if (to_json_string(*arguments).size() > limits.max_arguments_bytes) {
            return ir_error(ErrorCode::ResourceExhausted, "step arguments exceed limit");
        }
        step.arguments = *arguments;
    }
    if (const auto *precondition = json.find("precondition"); precondition != nullptr) {
        auto predicate = parse_predicate(*precondition, limits);
        if (!predicate.has_value()) {
            return predicate.error();
        }
        step.precondition = predicate.value();
    }
    if (const auto *verification = json.find("verification"); verification != nullptr) {
        auto predicate = parse_predicate(*verification, limits);
        if (!predicate.has_value()) {
            return predicate.error();
        }
        step.verification = predicate.value();
    }
    if (const auto *recovery = json.find("recovery"); recovery != nullptr) {
        auto hook = parse_recovery(*recovery);
        if (!hook.has_value()) {
            return hook.error();
        }
        step.recovery = hook.value();
    }
    if (const auto *max_attempts = json.find("max_attempts"); max_attempts != nullptr) {
        if (!max_attempts->is_integer()) {
            return ir_error(ErrorCode::InvalidArgument, "max_attempts must be an integer");
        }
        const auto attempts = max_attempts->as_integer().value();
        if (attempts < 1 || attempts > kCeilingMaxAttempts) {
            return ir_error(ErrorCode::InvalidArgument, "max_attempts out of range");
        }
        step.max_attempts = static_cast<std::uint32_t>(attempts);
    }
    if (const auto *jump = json.find("jump_to"); jump != nullptr && !jump->is_null()) {
        const auto target = parse_id(*jump);
        if (!target || target->is_nil()) {
            return ir_error(ErrorCode::InvalidArgument, "jump_to must be a step id");
        }
        step.jump_to = StepId{*target};
    }
    if (const auto *iterations = json.find("max_iterations"); iterations != nullptr) {
        if (!iterations->is_integer()) {
            return ir_error(ErrorCode::InvalidArgument, "max_iterations must be an integer");
        }
        const auto count = iterations->as_integer().value();
        if (count < 1 || count > kCeilingMaxIterations) {
            return ir_error(ErrorCode::InvalidArgument, "max_iterations out of range");
        }
        step.max_iterations = static_cast<std::uint32_t>(count);
    }
    if (step.kind == WorkflowStepKind::Verify && !step.verification.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "verify step requires verification");
    }
    if (step.kind == WorkflowStepKind::Navigate) {
        const auto *target = step.arguments.find("target");
        if (target == nullptr || !target->is_string() || target->as_string()->empty() ||
            target->as_string()->size() > limits.max_string_bytes) {
            return ir_error(ErrorCode::InvalidArgument,
                            "navigate step requires a bounded target argument");
        }
    }
    if (step.kind == WorkflowStepKind::Control) {
        if (!step.jump_to.has_value()) {
            return ir_error(ErrorCode::InvalidArgument, "control step requires jump_to");
        }
    } else if (step.jump_to.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "jump_to only allowed on control steps");
    }
    return step;
}

[[nodiscard]] JsonValue step_to_json(const WorkflowStep &step) {
    JsonValue::Object object;
    object.emplace_back("step_id", step.id.to_string());
    if (!step.name.empty()) {
        object.emplace_back("name", step.name);
    }
    object.emplace_back("kind", workflow_step_kind_name(step.kind));
    if (step.loop_head) {
        object.emplace_back("loop_head", true);
    }
    if (!step.arguments.is_null()) {
        object.emplace_back("arguments", step.arguments);
    }
    if (step.precondition.has_value()) {
        object.emplace_back("precondition", predicate_to_json(*step.precondition));
    }
    if (step.verification.has_value()) {
        object.emplace_back("verification", predicate_to_json(*step.verification));
    }
    if (step.recovery.has_value()) {
        object.emplace_back("recovery", recovery_to_json(*step.recovery));
    }
    if (step.max_attempts != 1) {
        object.emplace_back("max_attempts", static_cast<std::int64_t>(step.max_attempts));
    }
    if (step.jump_to.has_value()) {
        object.emplace_back("jump_to", step.jump_to->to_string());
        object.emplace_back("max_iterations",
                            static_cast<std::int64_t>(step.max_iterations));
    }
    return JsonValue{std::move(object)};
}

} // namespace

const WorkflowLimits kDefaultWorkflowLimits{};

std::string workflow_policy_name(WorkflowPolicy policy) {
    switch (policy) {
    case WorkflowPolicy::Strict:
        return "strict";
    case WorkflowPolicy::Recoverable:
        return "recoverable";
    case WorkflowPolicy::AgentAssisted:
        return "agent_assisted";
    case WorkflowPolicy::Interactive:
        return "interactive";
    case WorkflowPolicy::DryRun:
        return "dry_run";
    }
    return "unknown";
}

Result<WorkflowPolicy> parse_workflow_policy(std::string_view name) {
    for (auto policy : {WorkflowPolicy::Strict, WorkflowPolicy::Recoverable,
                        WorkflowPolicy::AgentAssisted, WorkflowPolicy::Interactive,
                        WorkflowPolicy::DryRun}) {
        if (workflow_policy_name(policy) == name) {
            return policy;
        }
    }
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.safe_message = "unknown workflow policy name";
    return error;
}

bool workflow_policy_allows_agent(WorkflowPolicy policy) noexcept {
    return policy == WorkflowPolicy::Recoverable || policy == WorkflowPolicy::AgentAssisted ||
           policy == WorkflowPolicy::Interactive;
}

bool workflow_policy_dispatches_side_effects(WorkflowPolicy policy) noexcept {
    return policy != WorkflowPolicy::DryRun;
}

std::string workflow_parameter_type_name(WorkflowParameterType type) {
    switch (type) {
    case WorkflowParameterType::String:
        return "string";
    case WorkflowParameterType::Integer:
        return "integer";
    case WorkflowParameterType::Number:
        return "number";
    case WorkflowParameterType::Boolean:
        return "boolean";
    }
    return "unknown";
}

Result<WorkflowParameterType> parse_workflow_parameter_type(std::string_view name) {
    for (auto type : {WorkflowParameterType::String, WorkflowParameterType::Integer,
                      WorkflowParameterType::Number, WorkflowParameterType::Boolean}) {
        if (workflow_parameter_type_name(type) == name) {
            return type;
        }
    }
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.safe_message = "unknown workflow parameter type";
    return error;
}

Error make_workflow_bind_error(WorkflowBindError code, std::string detail) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = "workflow bind: " + detail;
    return error;
}

std::string workflow_predicate_op_name(WorkflowPredicateOp op) {
    switch (op) {
    case WorkflowPredicateOp::Eq:
        return "eq";
    case WorkflowPredicateOp::Ne:
        return "ne";
    case WorkflowPredicateOp::Lt:
        return "lt";
    case WorkflowPredicateOp::Le:
        return "le";
    case WorkflowPredicateOp::Gt:
        return "gt";
    case WorkflowPredicateOp::Ge:
        return "ge";
    case WorkflowPredicateOp::Contains:
        return "contains";
    case WorkflowPredicateOp::Exists:
        return "exists";
    }
    return "unknown";
}

Result<WorkflowPredicateOp> parse_workflow_predicate_op(std::string_view name) {
    for (auto op : {WorkflowPredicateOp::Eq, WorkflowPredicateOp::Ne, WorkflowPredicateOp::Lt,
                    WorkflowPredicateOp::Le, WorkflowPredicateOp::Gt, WorkflowPredicateOp::Ge,
                    WorkflowPredicateOp::Contains, WorkflowPredicateOp::Exists}) {
        if (workflow_predicate_op_name(op) == name) {
            return op;
        }
    }
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.safe_message = "unknown workflow predicate op";
    return error;
}

std::string workflow_step_kind_name(WorkflowStepKind kind) {
    switch (kind) {
    case WorkflowStepKind::ToolCall:
        return "tool_call";
    case WorkflowStepKind::Navigate:
        return "navigate";
    case WorkflowStepKind::Verify:
        return "verify";
    case WorkflowStepKind::Control:
        return "control";
    }
    return "unknown";
}

Result<WorkflowStepKind> parse_workflow_step_kind(std::string_view name) {
    for (auto kind : {WorkflowStepKind::ToolCall, WorkflowStepKind::Navigate,
                      WorkflowStepKind::Verify, WorkflowStepKind::Control}) {
        if (workflow_step_kind_name(kind) == name) {
            return kind;
        }
    }
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.safe_message = "unknown workflow step kind";
    return error;
}

JsonValue workflow_definition_to_json(const WorkflowDefinition &definition) {
    JsonValue::Object root;
    JsonValue::Object version;
    version.emplace_back("major",
                         static_cast<std::int64_t>(definition.schema_version.major));
    version.emplace_back("minor",
                         static_cast<std::int64_t>(definition.schema_version.minor));
    root.emplace_back("schema_version", JsonValue{std::move(version)});
    root.emplace_back("workflow_id", definition.workflow_id.to_string());
    root.emplace_back("name", definition.name);
    // Summary is optional on the wire and rejected when present-but-empty;
    // the canonical form omits it so definitions without one round trip.
    if (!definition.summary.empty()) {
        root.emplace_back("summary", definition.summary);
    }
    JsonValue::Array parameters;
    for (const auto &parameter : definition.parameters) {
        parameters.push_back(parameter_to_json(parameter));
    }
    root.emplace_back("parameters", JsonValue{std::move(parameters)});
    JsonValue::Array steps;
    for (const auto &step : definition.steps) {
        steps.push_back(step_to_json(step));
    }
    root.emplace_back("steps", JsonValue{std::move(steps)});
    root.emplace_back("default_policy", workflow_policy_name(definition.default_policy));
    JsonValue::Array allowed;
    for (const auto policy : definition.allowed_policies) {
        allowed.emplace_back(workflow_policy_name(policy));
    }
    root.emplace_back("allowed_policies", JsonValue{std::move(allowed)});
    return JsonValue{std::move(root)};
}

Result<void> validate_workflow_definition(const WorkflowDefinition &definition,
                                          const WorkflowLimits &limits) {
    // Single source of validation semantics: canonicalize then re-decode. A
    // definition that cannot survive its own round trip (control jumps,
    // fallback targets, policy sets, predicate shapes, limits) is rejected
    // with the decoder's error; struct-built definitions get the identical
    // fail-closed treatment as JSON-decoded ones.
    auto decoded = workflow_definition_from_json(workflow_definition_to_json(definition),
                                                 limits);
    if (!decoded.has_value()) {
        return decoded.error();
    }
    // A successful re-decode of the canonical form is the validation; a
    // struct that does not survive its own round trip is rejected above.
    return Result<void>{};
}

Result<WorkflowDefinition>
workflow_definition_from_json(const JsonValue &json, const WorkflowLimits &limits) {
    const WorkflowLimits &bounded = clamp_limits(limits);
    static constexpr std::string_view kRequired[] = {"schema_version", "workflow_id", "name",
                                                     "steps", "default_policy",
                                                     "allowed_policies"};
    static constexpr std::string_view kOptional[] = {"summary", "parameters"};
    if (auto check = check_keys(json, kRequired, kOptional, "workflow definition");
        !check.has_value()) {
        return check.error();
    }
    if (auto depth = check_depth(json, bounded.max_depth); !depth.has_value()) {
        return depth.error();
    }
    if (to_json_string(json).size() > bounded.max_document_bytes) {
        return ir_error(ErrorCode::ResourceExhausted, "workflow document exceeds limit");
    }
    WorkflowDefinition definition;
    const auto *version = json.find("schema_version");
    if (!version->is_object()) {
        return ir_error(ErrorCode::InvalidArgument, "schema_version must be an object");
    }
    static constexpr std::string_view kVersionKeys[] = {"major", "minor"};
    if (auto check = check_keys(*version, kVersionKeys, {}, "schema_version");
        !check.has_value()) {
        return check.error();
    }
    const auto *major = version->find("major");
    const auto *minor = version->find("minor");
    if (!major->is_integer() || !minor->is_integer()) {
        return ir_error(ErrorCode::InvalidArgument, "schema_version members must be integers");
    }
    const auto incoming_major = static_cast<std::uint16_t>(major->as_integer().value());
    const auto incoming_minor = static_cast<std::uint16_t>(minor->as_integer().value());
    if (incoming_major != kWorkflowIrReaderVersion.major ||
        incoming_minor > kWorkflowIrReaderVersion.minor) {
        Error error;
        error.code = ErrorCode::UnsupportedVersion;
        error.domain = "mira.workflow";
        error.safe_message = "workflow ir schema version is not supported";
        return error;
    }
    definition.schema_version = SchemaVersion{incoming_major, incoming_minor};
    const auto workflow_id = parse_id(*json.find("workflow_id"));
    if (!workflow_id || workflow_id->is_nil()) {
        return ir_error(ErrorCode::InvalidArgument, "workflow_id must be a non-nil id");
    }
    definition.workflow_id = WorkflowId{*workflow_id};
    const auto name = bounded_string(*json.find("name"), bounded.max_string_bytes);
    if (!name) {
        return ir_error(ErrorCode::InvalidArgument, "workflow name must be a bounded string");
    }
    definition.name = *name;
    if (const auto *summary = json.find("summary"); summary != nullptr && !summary->is_null()) {
        const auto text = bounded_string(*summary, bounded.max_string_bytes);
        if (!text) {
            return ir_error(ErrorCode::InvalidArgument, "workflow summary is not bounded");
        }
        definition.summary = *text;
    }
    if (const auto *parameters = json.find("parameters");
        parameters != nullptr && !parameters->is_null()) {
        if (!parameters->is_array()) {
            return ir_error(ErrorCode::InvalidArgument, "parameters must be an array");
        }
        if (parameters->as_array()->size() > bounded.max_parameters) {
            return ir_error(ErrorCode::ResourceExhausted, "parameter count exceeds limit");
        }
        for (const auto &item : *parameters->as_array()) {
            auto spec = parse_parameter(item, bounded);
            if (!spec.has_value()) {
                return spec.error();
            }
            const bool duplicate = std::any_of(
                definition.parameters.begin(), definition.parameters.end(),
                [&](const WorkflowParameterSpec &existing) {
                    return existing.name == spec.value().name;
                });
            if (duplicate) {
                return ir_error(ErrorCode::InvalidArgument, "duplicate parameter name");
            }
            definition.parameters.push_back(spec.value());
        }
    }
    const auto *steps = json.find("steps");
    if (!steps->is_array() || steps->as_array()->empty()) {
        return ir_error(ErrorCode::InvalidArgument, "steps must be a non-empty array");
    }
    if (steps->as_array()->size() > bounded.max_steps) {
        return ir_error(ErrorCode::ResourceExhausted, "step count exceeds limit");
    }
    for (const auto &item : *steps->as_array()) {
        auto step = parse_step(item, bounded);
        if (!step.has_value()) {
            return step.error();
        }
        const bool duplicate = std::any_of(definition.steps.begin(), definition.steps.end(),
                                           [&](const WorkflowStep &existing) {
                                               return existing.id == step.value().id;
                                           });
        if (duplicate) {
            return ir_error(ErrorCode::InvalidArgument, "duplicate step id");
        }
        definition.steps.push_back(step.value());
    }
    const auto *default_policy = json.find("default_policy");
    if (default_policy == nullptr || !default_policy->is_string()) {
        return ir_error(ErrorCode::InvalidArgument, "default_policy must be a string");
    }
    auto policy = parse_workflow_policy(*default_policy->as_string());
    if (!policy.has_value()) {
        return ir_error(ErrorCode::InvalidArgument, "default_policy is unknown");
    }
    definition.default_policy = policy.value();
    const auto *allowed = json.find("allowed_policies");
    if (allowed == nullptr || !allowed->is_array() || allowed->as_array()->empty()) {
        return ir_error(ErrorCode::InvalidArgument,
                        "allowed_policies must be a non-empty array");
    }
    for (const auto &item : *allowed->as_array()) {
        if (!item.is_string()) {
            return ir_error(ErrorCode::InvalidArgument, "allowed_policies members must be strings");
        }
        auto entry = parse_workflow_policy(*item.as_string());
        if (!entry.has_value()) {
            return ir_error(ErrorCode::InvalidArgument, "allowed_policies member is unknown");
        }
        const bool duplicate = std::any_of(definition.allowed_policies.begin(),
                                           definition.allowed_policies.end(),
                                           [&](WorkflowPolicy existing) {
                                               return existing == entry.value();
                                           });
        if (duplicate) {
            return ir_error(ErrorCode::InvalidArgument, "duplicate allowed policy");
        }
        definition.allowed_policies.push_back(entry.value());
    }
    const bool default_allowed = std::any_of(
        definition.allowed_policies.begin(), definition.allowed_policies.end(),
        [&](WorkflowPolicy entry) { return entry == definition.default_policy; });
    if (!default_allowed) {
        return ir_error(ErrorCode::InvalidArgument,
                        "default_policy must be declared in allowed_policies");
    }
    // Structural checks that span steps: control jumps must target an earlier
    // loop head; fallback recovery must target a later step; agent escalation
    // requires an agent-capable declared default policy (DEC-020).
    for (std::size_t index = 0; index < definition.steps.size(); ++index) {
        const auto &step = definition.steps[index];
        if (step.kind == WorkflowStepKind::Control) {
            const auto target = std::find_if(definition.steps.begin(), definition.steps.end(),
                                             [&](const WorkflowStep &candidate) {
                                                 return candidate.id == step.jump_to;
                                             });
            if (target == definition.steps.end() || !target->loop_head ||
                std::distance(definition.steps.begin(), target) >=
                    static_cast<std::ptrdiff_t>(index)) {
                return ir_error(ErrorCode::InvalidArgument,
                                "control jump must target an earlier loop_head step");
            }
        }
        if (step.recovery.has_value() &&
            step.recovery->mode == WorkflowRecoveryHook::Mode::FallbackStep) {
            const auto target = std::find_if(
                definition.steps.begin(), definition.steps.end(),
                [&](const WorkflowStep &candidate) {
                    return candidate.id == step.recovery->fallback_step;
                });
            if (target == definition.steps.end() ||
                std::distance(definition.steps.begin(), target) <=
                    static_cast<std::ptrdiff_t>(index)) {
                return ir_error(ErrorCode::InvalidArgument,
                                "fallback_step must reference a later step");
            }
        }
        if (step.recovery.has_value() &&
            step.recovery->mode == WorkflowRecoveryHook::Mode::AgentEscalation &&
            !workflow_policy_allows_agent(definition.default_policy)) {
            return ir_error(ErrorCode::InvalidArgument,
                            "agent escalation requires an agent-capable default policy");
        }
    }
    if (auto defaults = validate_workflow_parameter_specs(definition); !defaults.has_value()) {
        return defaults.error();
    }
    return definition;
}

Result<WorkflowDefinition> parse_workflow_definition(std::string_view text,
                                                     const WorkflowLimits &limits) {
    const WorkflowLimits &bounded = clamp_limits(limits);
    if (text.size() > bounded.max_document_bytes) {
        return ir_error(ErrorCode::ResourceExhausted, "workflow document exceeds limit");
    }
    auto json = parse_json(text);
    if (!json.has_value()) {
        return json.error();
    }
    return workflow_definition_from_json(json.value(), bounded);
}

Sha256Digest workflow_definition_digest(const WorkflowDefinition &definition) {
    return canonical_json_digest(workflow_definition_to_json(definition));
}

Result<void> validate_workflow_parameter_specs(const WorkflowDefinition &definition) {
    for (const auto &spec : definition.parameters) {
        if (!spec.default_value.is_null()) {
            if (auto check = check_constraint(spec, spec.default_value); !check.has_value()) {
                if (check.error().domain_code ==
                    static_cast<std::int32_t>(WorkflowBindError::ConstraintViolated)) {
                    Error error = make_workflow_bind_error(
                        WorkflowBindError::InvalidDefault, "parameter '" + spec.name + "'");
                    return error;
                }
                return check.error();
            }
        }
    }
    return Result<void>{};
}

Result<WorkflowParameterBindings> bind_workflow_parameters(const WorkflowDefinition &definition,
                                                            const JsonValue &input) {
    if (!input.is_null() && !input.is_object()) {
        return make_workflow_bind_error(WorkflowBindError::InvalidArguments,
                                        "input must be an object");
    }
    JsonValue::Object values;
    for (const auto &member : (input.is_object() ? *input.as_object()
                                                 : JsonValue::Object{})) {
        const auto spec = std::find_if(definition.parameters.begin(), definition.parameters.end(),
                                       [&](const WorkflowParameterSpec &candidate) {
                                           return candidate.name == member.first;
                                       });
        if (spec == definition.parameters.end()) {
            return make_workflow_bind_error(WorkflowBindError::UnknownParameter, member.first);
        }
        if (auto check = check_constraint(*spec, member.second); !check.has_value()) {
            return check.error();
        }
        values.emplace_back(member.first, member.second);
    }
    for (const auto &spec : definition.parameters) {
        const bool provided =
            std::any_of(values.begin(), values.end(),
                        [&](const auto &entry) { return entry.first == spec.name; });
        if (provided) {
            continue;
        }
        if (!spec.default_value.is_null()) {
            values.emplace_back(spec.name, spec.default_value);
            continue;
        }
        if (spec.required) {
            return make_workflow_bind_error(WorkflowBindError::MissingRequired, spec.name);
        }
        // Optional without default: stays unset; referencing it later fails
        // closed at resolution time.
    }
    WorkflowParameterBindings bindings;
    bindings.values = JsonValue{std::move(values)};
    bindings.digest = canonical_json_digest(bindings.values);
    return bindings;
}

namespace {

[[nodiscard]] Result<JsonValue> resolve_references(const JsonValue &node,
                                                   const JsonValue &values) {
    if (node.is_object()) {
        const auto *reference = node.find("$param");
        if (reference != nullptr) {
            if (node.as_object()->size() != 1) {
                return ir_error(ErrorCode::InvalidArgument,
                                "parameter reference object admits only '$param'");
            }
            if (!reference->is_string()) {
                return ir_error(ErrorCode::InvalidArgument, "'$param' must name a parameter");
            }
            const auto *bound = values.find(*reference->as_string());
            if (bound == nullptr) {
                return make_workflow_bind_error(WorkflowBindError::UnknownParameter,
                                                *reference->as_string());
            }
            return *bound;
        }
        JsonValue::Object resolved;
        for (const auto &member : *node.as_object()) {
            auto value = resolve_references(member.second, values);
            if (!value.has_value()) {
                return value;
            }
            resolved.emplace_back(member.first, value.value());
        }
        return JsonValue{std::move(resolved)};
    }
    if (node.is_array()) {
        JsonValue::Array resolved;
        for (const auto &item : *node.as_array()) {
            auto value = resolve_references(item, values);
            if (!value.has_value()) {
                return value;
            }
            resolved.push_back(value.value());
        }
        return JsonValue{std::move(resolved)};
    }
    return node;
}

} // namespace

Result<JsonValue> resolve_step_arguments(const WorkflowParameterBindings &bindings,
                                         const JsonValue &arguments) {
    return resolve_references(arguments, bindings.values);
}

WorkflowPredicateResult evaluate_workflow_predicate(const WorkflowPredicate &predicate,
                                                    const JsonValue &context) {
    if (!context.is_object()) {
        return WorkflowPredicateResult::NotEvaluable;
    }
    const auto *signal = context.find(predicate.signal);
    if (predicate.op == WorkflowPredicateOp::Exists) {
        return (signal != nullptr && !signal->is_null()) ? WorkflowPredicateResult::Satisfied
                                                         : WorkflowPredicateResult::NotSatisfied;
    }
    if (signal == nullptr) {
        return WorkflowPredicateResult::NotEvaluable;
    }
    const JsonValue &left = *signal;
    const JsonValue &right = predicate.value;
    switch (predicate.op) {
    case WorkflowPredicateOp::Eq:
        return left == right ? WorkflowPredicateResult::Satisfied
                             : WorkflowPredicateResult::NotSatisfied;
    case WorkflowPredicateOp::Ne:
        return !(left == right) ? WorkflowPredicateResult::Satisfied
                                : WorkflowPredicateResult::NotSatisfied;
    case WorkflowPredicateOp::Lt:
    case WorkflowPredicateOp::Le:
    case WorkflowPredicateOp::Gt:
    case WorkflowPredicateOp::Ge: {
        const auto lhs = left.as_number();
        const auto rhs = right.as_number();
        if (!lhs.has_value() || !rhs.has_value()) {
            return WorkflowPredicateResult::NotEvaluable;
        }
        bool satisfied = false;
        switch (predicate.op) {
        case WorkflowPredicateOp::Lt:
            satisfied = *lhs < *rhs;
            break;
        case WorkflowPredicateOp::Le:
            satisfied = *lhs <= *rhs;
            break;
        case WorkflowPredicateOp::Gt:
            satisfied = *lhs > *rhs;
            break;
        case WorkflowPredicateOp::Ge:
            satisfied = *lhs >= *rhs;
            break;
        default:
            break;
        }
        return satisfied ? WorkflowPredicateResult::Satisfied
                         : WorkflowPredicateResult::NotSatisfied;
    }
    case WorkflowPredicateOp::Contains:
        if (left.is_string()) {
            if (!right.is_string()) {
                return WorkflowPredicateResult::NotEvaluable;
            }
            return left.as_string()->find(*right.as_string()) != std::string::npos
                       ? WorkflowPredicateResult::Satisfied
                       : WorkflowPredicateResult::NotSatisfied;
        }
        if (left.is_array()) {
            for (const auto &item : *left.as_array()) {
                if (item == right) {
                    return WorkflowPredicateResult::Satisfied;
                }
            }
            return WorkflowPredicateResult::NotSatisfied;
        }
        return WorkflowPredicateResult::NotEvaluable;
    case WorkflowPredicateOp::Exists:
        break;
    }
    return WorkflowPredicateResult::NotEvaluable;
}

Result<void> validate_workflow_predicate(const WorkflowPredicate &predicate,
                                         const WorkflowLimits &limits) {
    static constexpr std::string_view kSignalKinds[] = {"run_parameter", "step_result",
                                                        "screen_state"};
    const auto separator = predicate.signal.find(':');
    if (separator == std::string::npos) {
        return ir_error(ErrorCode::InvalidArgument, "predicate signal must be 'kind:ref'");
    }
    const std::string kind = predicate.signal.substr(0, separator);
    const std::string reference = predicate.signal.substr(separator + 1);
    const bool known_kind = std::any_of(std::begin(kSignalKinds), std::end(kSignalKinds),
                                        [&](std::string_view candidate) {
                                            return candidate == kind;
                                        });
    if (!known_kind) {
        return ir_error(ErrorCode::InvalidArgument, "predicate signal kind is unknown");
    }
    if (reference.empty() || reference.size() > limits.max_string_bytes) {
        return ir_error(ErrorCode::InvalidArgument, "predicate signal reference is not bounded");
    }
    return Result<void>{};
}

} // namespace mira
