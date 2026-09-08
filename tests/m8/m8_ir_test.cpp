#include "../support/test.hpp"

#include <mira/workflow_ir.hpp>

#include <string>

namespace {

using namespace mira;

constexpr const char *kWorkflowId = "0123456789abcdef0123456789abcdef";
constexpr const char *kStepA = "11111111111111111111111111111111";
constexpr const char *kStepB = "22222222222222222222222222222222";
constexpr const char *kStepC = "33333333333333333333333333333333";

[[nodiscard]] std::string minimal_ir() {
    std::string text = R"({
        "schema_version": {"major": 1, "minor": 0},
        "workflow_id": ")";
    text += kWorkflowId;
    text += R"(",
        "name": "send-daily-report",
        "summary": "reference fixture",
        "parameters": [
            {"name": "contact", "type": "string", "required": true,
             "constraints": {"min_length": 1, "max_length": 64}},
            {"name": "copies", "type": "integer", "required": false,
             "default": 1, "constraints": {"minimum": 1, "maximum": 8}},
            {"name": "channel", "type": "string", "required": false,
             "default": "wechat", "constraints": {"enum": ["wechat", "dingtalk"]}}
        ],
        "steps": [
            {"step_id": ")";
    text += kStepA;
    text += R"(", "kind": "tool_call", "name": "open-chat",
             "arguments": {"text": {"$param": "contact"}, "channel": {"$param": "channel"}}},
            {"step_id": ")";
    text += kStepB;
    text += R"(", "kind": "verify",
             "verification": {"signal": "step_result:)";
    text += kStepA;
    text += R"(", "op": "eq", "value": "sent"}},
            {"step_id": ")";
    text += kStepC;
    text += R"(", "kind": "navigate",
             "arguments": {"target": "ChatPage"}}
        ],
        "default_policy": "recoverable",
        "allowed_policies": ["strict", "recoverable", "interactive", "dry_run"]
    })";
    return text;
}

int round_trip_is_lossless() {
    auto parsed = parse_workflow_definition(minimal_ir());
    MIRA_CHECK(parsed.has_value());
    const WorkflowDefinition &definition = parsed.value();
    MIRA_CHECK(definition.name == "send-daily-report");
    MIRA_CHECK(definition.parameters.size() == 3);
    MIRA_CHECK(definition.steps.size() == 3);
    MIRA_CHECK(definition.steps[0].kind == WorkflowStepKind::ToolCall);
    MIRA_CHECK(definition.steps[1].kind == WorkflowStepKind::Verify);
    MIRA_CHECK(definition.steps[1].verification.has_value());
    MIRA_CHECK(definition.steps[2].kind == WorkflowStepKind::Navigate);
    MIRA_CHECK(definition.default_policy == WorkflowPolicy::Recoverable);

    const JsonValue json = workflow_definition_to_json(definition);
    auto reparsed = workflow_definition_from_json(json);
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(reparsed.value().steps.size() == definition.steps.size());
    MIRA_CHECK(reparsed.value().steps[0] == definition.steps[0]);
    MIRA_CHECK(reparsed.value().steps[1] == definition.steps[1]);
    MIRA_CHECK(reparsed.value().steps[2] == definition.steps[2]);
    MIRA_CHECK(reparsed.value().parameters.size() == definition.parameters.size());
    MIRA_CHECK(reparsed.value().parameters[1].default_value ==
               definition.parameters[1].default_value);
    MIRA_CHECK(workflow_definition_digest(reparsed.value()) ==
               workflow_definition_digest(definition));
    return 0;
}

int unknown_fields_fail_closed() {
    std::string text = minimal_ir();
    text.erase(text.rfind('}'), 1);
    text += R"(, "future_field": true })";
    auto parsed = parse_workflow_definition(text);
    MIRA_CHECK(!parsed.has_value());
    MIRA_CHECK(parsed.error().code == ErrorCode::UnsupportedVersion ||
               parsed.error().code == ErrorCode::InvalidArgument);

    // Unknown nested field inside a step.
    std::string nested = minimal_ir();
    const auto position = nested.find("\"kind\": \"verify\"");
    MIRA_CHECK(position != std::string::npos);
    nested.insert(position, "\"surprise\": 1, ");
    MIRA_CHECK(!parse_workflow_definition(nested).has_value());

    // Unknown constraint keyword.
    std::string constraint = minimal_ir();
    MIRA_CHECK(constraint.find("\"max_length\": 64}") != std::string::npos);
    constraint.replace(constraint.find("\"max_length\": 64}"),
                       std::string("\"max_length\": 64}").size(),
                       "\"max_length\": 64, \"exclusiveMin\": 1}");
    auto result = parse_workflow_definition(constraint);
    MIRA_CHECK(!result.has_value());
    MIRA_CHECK(result.error().code == ErrorCode::UnsupportedVersion);
    return 0;
}

int version_mismatch_fails_closed() {
    std::string text = minimal_ir();
    const auto minor_position = text.find("\"minor\": 0");
    MIRA_CHECK(minor_position != std::string::npos);
    text.replace(minor_position, std::string("\"minor\": 0").size(), "\"minor\": 2");
    auto newer = parse_workflow_definition(text);
    MIRA_CHECK(!newer.has_value());
    MIRA_CHECK(newer.error().code == ErrorCode::UnsupportedVersion);

    std::string older = minimal_ir();
    const auto major_position = older.find("\"major\": 1");
    MIRA_CHECK(major_position != std::string::npos);
    older.replace(major_position, std::string("\"major\": 1").size(), "\"major\": 2");
    MIRA_CHECK(!parse_workflow_definition(older).has_value());
    return 0;
}

int limits_fail_closed() {
    // Oversize document rejected at the text boundary (RULE-08).
    WorkflowLimits tight;
    tight.max_document_bytes = 32;
    MIRA_CHECK(!parse_workflow_definition(minimal_ir(), tight).has_value());

    // Too many steps.
    std::string many = minimal_ir();
    const auto steps_position = many.find("\"steps\": [");
    MIRA_CHECK(steps_position != std::string::npos);
    std::string bulk;
    for (int index = 0; index < 40; ++index) {
        bulk += R"({"step_id": "4444444444444444444444444444)" +
                std::to_string(1000 + index) + R"(", "kind": "verify", "verification": )"
                + R"({"signal": "run_parameter:copies", "op": "ge", "value": 1}},)";
    }
    many.insert(steps_position + std::string("\"steps\": [").size(), bulk);
    WorkflowLimits step_cap;
    step_cap.max_steps = 16;
    auto parsed = parse_workflow_definition(many, step_cap);
    MIRA_CHECK(!parsed.has_value());
    MIRA_CHECK(parsed.error().code == ErrorCode::ResourceExhausted);

    // Nesting beyond the depth limit: build a legal document first, then
    // deepen one step's arguments past the ceiling and round-trip it.
    auto base = parse_workflow_definition(minimal_ir());
    MIRA_CHECK(base.has_value());
    WorkflowDefinition deep_definition = base.value();
    JsonValue layer{JsonValue::Object{{"leaf", JsonValue{true}}}};
    for (int index = 0; index < 14; ++index) {
        layer = JsonValue{JsonValue::Object{
            {"layer_" + std::to_string(index), layer}}};
    }
    deep_definition.steps[0].arguments = layer;
    auto deep_result =
        workflow_definition_from_json(workflow_definition_to_json(deep_definition));
    MIRA_CHECK(!deep_result.has_value());
    MIRA_CHECK(deep_result.error().code == ErrorCode::ResourceExhausted);
    return 0;
}

int binding_is_deterministic() {
    auto parsed = parse_workflow_definition(minimal_ir());
    MIRA_CHECK(parsed.has_value());

    JsonValue input{JsonValue::Object{}};
    input.set("contact", JsonValue{std::string("zhang san")});
    input.set("copies", JsonValue{static_cast<std::int64_t>(3)});
    auto bound = bind_workflow_parameters(parsed.value(), input);
    MIRA_CHECK(bound.has_value());
    const auto *contact = bound.value().values.find("contact");
    MIRA_CHECK(contact != nullptr && contact->as_string() &&
               *contact->as_string() == "zhang san");
    // Default applied for the untouched parameter.
    const auto *channel = bound.value().values.find("channel");
    MIRA_CHECK(channel != nullptr && channel->as_string() &&
               *channel->as_string() == "wechat");
    // Digest stability: same input, same digest.
    auto again = bind_workflow_parameters(parsed.value(), input);
    MIRA_CHECK(again.has_value() && again.value().digest == bound.value().digest);

    // Unknown parameter.
    JsonValue unknown{JsonValue::Object{}};
    unknown.set("ghost", JsonValue{std::string("x")});
    auto rejected = bind_workflow_parameters(parsed.value(), unknown);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::UnknownParameter));

    // Missing required.
    JsonValue missing{JsonValue::Object{}};
    missing.set("copies", JsonValue{static_cast<std::int64_t>(2)});
    auto lost = bind_workflow_parameters(parsed.value(), missing);
    MIRA_CHECK(!lost.has_value());
    MIRA_CHECK(lost.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::MissingRequired));

    // Type mismatch.
    JsonValue mismatched{JsonValue::Object{}};
    mismatched.set("contact", JsonValue{static_cast<std::int64_t>(9)});
    auto wrong_type = bind_workflow_parameters(parsed.value(), mismatched);
    MIRA_CHECK(!wrong_type.has_value());
    MIRA_CHECK(wrong_type.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::TypeMismatch));

    // Constraint violations: above maximum, below min_length, outside enum.
    JsonValue above_max{JsonValue::Object{}};
    above_max.set("contact", JsonValue{std::string("zhang san")});
    above_max.set("copies", JsonValue{static_cast<std::int64_t>(9)});
    auto over = bind_workflow_parameters(parsed.value(), above_max);
    MIRA_CHECK(!over.has_value());
    MIRA_CHECK(over.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::ConstraintViolated));

    JsonValue bad_channel{JsonValue::Object{}};
    bad_channel.set("contact", JsonValue{std::string("li si")});
    bad_channel.set("channel", JsonValue{std::string("sms")});
    auto outside_enum = bind_workflow_parameters(parsed.value(), bad_channel);
    MIRA_CHECK(!outside_enum.has_value());
    MIRA_CHECK(outside_enum.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::ConstraintViolated));

    // Non-object input.
    MIRA_CHECK(!bind_workflow_parameters(parsed.value(), JsonValue{JsonValue::Array{}}).has_value());

    // Optional without a value stays unset and references to it fail closed.
    JsonValue only_contact{JsonValue::Object{}};
    only_contact.set("contact", JsonValue{std::string("ok")});
    auto partial = bind_workflow_parameters(parsed.value(), only_contact);
    MIRA_CHECK(partial.has_value());
    MIRA_CHECK(partial.value().values.find("channel") != nullptr); // has default
    return 0;
}

int defaults_must_satisfy_their_own_constraints() {
    std::string text = minimal_ir();
    const auto default_position = text.find("\"default\": 1");
    MIRA_CHECK(default_position != std::string::npos);
    text.replace(default_position, std::string("\"default\": 1").size(), "\"default\": 99");
    auto parsed = parse_workflow_definition(text);
    MIRA_CHECK(!parsed.has_value());
    MIRA_CHECK(parsed.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::InvalidDefault));
    return 0;
}

int parameter_references_resolve_and_fail_closed() {
    auto parsed = parse_workflow_definition(minimal_ir());
    MIRA_CHECK(parsed.has_value());
    JsonValue input{JsonValue::Object{}};
    input.set("contact", JsonValue{std::string("li si")});
    auto bound = bind_workflow_parameters(parsed.value(), input);
    MIRA_CHECK(bound.has_value());

    auto resolved = resolve_step_arguments(bound.value(), parsed.value().steps[0].arguments);
    MIRA_CHECK(resolved.has_value());
    const auto *text = resolved.value().find("text");
    MIRA_CHECK(text != nullptr && text->as_string() && *text->as_string() == "li si");
    const auto *channel = resolved.value().find("channel");
    MIRA_CHECK(channel != nullptr && channel->as_string() &&
               *channel->as_string() == "wechat");

    // Unknown parameter reference.
    JsonValue arguments{JsonValue::Object{}};
    arguments.set("x", JsonValue{JsonValue::Object{{"$param", JsonValue{std::string("ghost")}}}});
    auto ghost = resolve_step_arguments(bound.value(), arguments);
    MIRA_CHECK(!ghost.has_value());
    MIRA_CHECK(ghost.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::UnknownParameter));

    // Extra members inside the reference object fail closed.
    JsonValue extra{JsonValue::Object{}};
    extra.set("x", JsonValue{JsonValue::Object{
                       {"$param", JsonValue{std::string("contact")}},
                       {"$other", JsonValue{std::string("contact")}}}});
    MIRA_CHECK(!resolve_step_arguments(bound.value(), extra).has_value());

    // Unbound optional parameter (no value, no default) fails closed.
    WorkflowDefinition definition;
    definition.workflow_id = WorkflowId::generate();
    definition.name = "unset";
    definition.steps.push_back(WorkflowStep{});
    definition.steps.front().id = StepId::generate();
    definition.allowed_policies.push_back(WorkflowPolicy::Strict);
    definition.default_policy = WorkflowPolicy::Strict;
    WorkflowParameterSpec maybe;
    maybe.name = "maybe";
    maybe.required = false;
    definition.parameters.push_back(maybe);
    JsonValue empty_input{JsonValue::Object{}};
    auto unset = bind_workflow_parameters(definition, empty_input);
    MIRA_CHECK(unset.has_value());
    JsonValue reference{JsonValue::Object{}};
    reference.set("y", JsonValue{JsonValue::Object{{"$param", JsonValue{std::string("maybe")}}}});
    MIRA_CHECK(!resolve_step_arguments(unset.value(), reference).has_value());
    return 0;
}

int predicates_evaluate_fail_closed() {
    WorkflowPredicate predicate;
    predicate.signal = "run_parameter:copies";
    predicate.op = WorkflowPredicateOp::Ge;
    predicate.value = JsonValue{static_cast<std::int64_t>(1)};

    JsonValue context{JsonValue::Object{}};
    context.set("run_parameter:copies", JsonValue{static_cast<std::int64_t>(3)});
    MIRA_CHECK(evaluate_workflow_predicate(predicate, context) ==
               WorkflowPredicateResult::Satisfied);
    context.set("run_parameter:copies", JsonValue{static_cast<std::int64_t>(0)});
    MIRA_CHECK(evaluate_workflow_predicate(predicate, context) ==
               WorkflowPredicateResult::NotSatisfied);

    // Missing signal is NotEvaluable; exists answers presence.
    JsonValue empty_context{JsonValue::Object{}};
    MIRA_CHECK(evaluate_workflow_predicate(predicate, empty_context) ==
               WorkflowPredicateResult::NotEvaluable);
    WorkflowPredicate exists;
    exists.signal = "screen_state:ChatPage";
    exists.op = WorkflowPredicateOp::Exists;
    MIRA_CHECK(evaluate_workflow_predicate(exists, empty_context) ==
               WorkflowPredicateResult::NotSatisfied);
    empty_context.set("screen_state:ChatPage", JsonValue{std::string("idle")});
    MIRA_CHECK(evaluate_workflow_predicate(exists, empty_context) ==
               WorkflowPredicateResult::Satisfied);

    // Type mismatch on ordered comparison is NotEvaluable (fail closed).
    WorkflowPredicate compare;
    compare.signal = "step_result:send";
    compare.op = WorkflowPredicateOp::Lt;
    compare.value = JsonValue{static_cast<std::int64_t>(5)};
    JsonValue stringy{JsonValue::Object{}};
    stringy.set("step_result:send", JsonValue{std::string("sent")});
    MIRA_CHECK(evaluate_workflow_predicate(compare, stringy) ==
               WorkflowPredicateResult::NotEvaluable);

    // Signal shape validation.
    WorkflowPredicate bad;
    bad.signal = "nocolon";
    bad.op = WorkflowPredicateOp::Eq;
    bad.value = JsonValue{static_cast<std::int64_t>(1)};
    MIRA_CHECK(!validate_workflow_predicate(bad).has_value());
    bad.signal = "unknown_kind:ref";
    MIRA_CHECK(!validate_workflow_predicate(bad).has_value());
    bad.signal = "run_parameter:";
    MIRA_CHECK(!validate_workflow_predicate(bad).has_value());
    return 0;
}

int control_flow_and_recovery_are_structurally_validated() {
    // A backward jump to a step that is not a loop head is rejected. The
    // control step is appended after the target so the edge points backwards.
    std::string text = minimal_ir();
    const auto steps_end = text.rfind("\n        ],");
    MIRA_CHECK(steps_end != std::string::npos);
    std::string control_step = R"(,
            {"step_id": "55555555555555555555555555555555", "kind": "control",
             "jump_to": ")" + std::string{kStepC} + R"(", "max_iterations": 4})";
    text.insert(steps_end, control_step);
    MIRA_CHECK(!parse_workflow_definition(text).has_value());

    // Marking the target as a loop head makes the same jump legal.
    std::string legal = text;
    const auto target = legal.find("\"step_id\": \"" + std::string{kStepC} + "\"");
    MIRA_CHECK(target != std::string::npos);
    legal.insert(target + std::string("\"step_id\": \"").size() + std::string{kStepC}.size() +
                     std::string("\"").size(),
                 ", \"loop_head\": true");
    auto marked = parse_workflow_definition(legal);
    MIRA_CHECK(marked.has_value());
    MIRA_CHECK(marked.value().steps.back().kind == WorkflowStepKind::Control);
    MIRA_CHECK(marked.value().steps.back().max_iterations == 4);

    // Jumping forward (to a later step) is rejected even when that step is a
    // marked loop head: control edges may only point backwards.
    std::string forward = legal;
    const auto step_b = forward.find("\"step_id\": \"" + std::string{kStepB} + "\"");
    MIRA_CHECK(step_b != std::string::npos);
    forward.insert(step_b + std::string("\"step_id\": \"").size() +
                       std::string{kStepB}.size() + std::string("\"").size(),
                   ", \"loop_head\": true");
    const auto steps_start = forward.find("\"steps\": [");
    MIRA_CHECK(steps_start != std::string::npos);
    std::string early_control = R"( {"step_id": "77777777777777777777777777777777",
            "kind": "control", "jump_to": ")" + std::string{kStepB} + R"(", "max_iterations": 2},)";
    forward.insert(steps_start + std::string("\"steps\": [").size(), early_control);
    MIRA_CHECK(!parse_workflow_definition(forward).has_value());

    // jump_to on a non-control step is rejected.
    std::string stray_jump = minimal_ir();
    const auto navigate_kind = stray_jump.find("\"kind\": \"navigate\"");
    MIRA_CHECK(navigate_kind != std::string::npos);
    stray_jump.replace(navigate_kind, std::string("\"kind\": \"navigate\"").size(),
                       std::string("\"kind\": \"navigate\", \"jump_to\": \"") +
                           std::string{kStepA} + "\"");
    MIRA_CHECK(!parse_workflow_definition(stray_jump).has_value());

    // Agent escalation is legal under an agent-capable default policy and
    // rejected under strict (DEC-020).
    std::string escalation = minimal_ir();
    const auto verify_kind = escalation.find("\"kind\": \"verify\"");
    MIRA_CHECK(verify_kind != std::string::npos);
    escalation.replace(verify_kind, std::string("\"kind\": \"verify\"").size(),
                       R"("kind": "tool_call")");
    const auto verify_position = escalation.find("\"step_id\": \"" + std::string{kStepB} + "\"");
    const auto after_id = verify_position + std::string("\"step_id\": \"").size() +
                          std::string{kStepB}.size() + std::string("\"").size();
    escalation.insert(after_id, R"(, "recovery": {"mode": "agent_escalation"})");
    MIRA_CHECK(parse_workflow_definition(escalation).has_value());

    std::string strict_version = escalation;
    const auto policy_position = strict_version.find("\"default_policy\": \"recoverable\"");
    MIRA_CHECK(policy_position != std::string::npos);
    strict_version.replace(policy_position,
                           std::string("\"default_policy\": \"recoverable\"").size(),
                           "\"default_policy\": \"strict\"");
    MIRA_CHECK(!parse_workflow_definition(strict_version).has_value());
    return 0;
}

int digest_is_content_addressed() {
    auto parsed = parse_workflow_definition(minimal_ir());
    MIRA_CHECK(parsed.has_value());
    const Sha256Digest digest = workflow_definition_digest(parsed.value());

    WorkflowDefinition mutated = parsed.value();
    mutated.name = "send-daily-report-v2";
    MIRA_CHECK(workflow_definition_digest(mutated) != digest);

    const WorkflowDefinition same = parsed.value();
    MIRA_CHECK(workflow_definition_digest(same) == digest);
    return 0;
}

} // namespace

int main() {
    if (round_trip_is_lossless() != 0) {
        return 1;
    }
    if (unknown_fields_fail_closed() != 0) {
        return 1;
    }
    if (version_mismatch_fails_closed() != 0) {
        return 1;
    }
    if (limits_fail_closed() != 0) {
        return 1;
    }
    if (binding_is_deterministic() != 0) {
        return 1;
    }
    if (defaults_must_satisfy_their_own_constraints() != 0) {
        return 1;
    }
    if (parameter_references_resolve_and_fail_closed() != 0) {
        return 1;
    }
    if (predicates_evaluate_fail_closed() != 0) {
        return 1;
    }
    if (control_flow_and_recovery_are_structurally_validated() != 0) {
        return 1;
    }
    if (digest_is_content_addressed() != 0) {
        return 1;
    }
    return 0;
}
