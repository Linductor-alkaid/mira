#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_digest.hpp>
#include <mira/model_schema.hpp>

#include <string>

namespace {

using namespace mira;

[[nodiscard]] JsonSchema parse_schema(const std::string &text) {
    auto parsed = parse_json(text);
    return JsonSchema{std::move(parsed).value()};
}

int subset_gate_rejects_unsupported_keywords() {
    MIRA_CHECK(gate_schema_subset(parse_schema(
                   R"({"type":"object","properties":{"a":{"type":"string"}},"additionalProperties":false})"))
                   .has_value());
    MIRA_CHECK(!gate_schema_subset(parse_schema(
                    R"({"type":"object","properties":{"a":{"anyOf":[{"type":"string"}]}}})"))
                    .has_value());
    MIRA_CHECK(!gate_schema_subset(
                   parse_schema(R"({"type":"object","properties":{"a":{"$ref":"#/x"}}})"))
                   .has_value());
    MIRA_CHECK(!gate_schema_subset(parse_schema(
                    R"({"type":"object","properties":{"a":{"oneOf":[{"type":"string"}]}}})"))
                    .has_value());
    // Deeply nested schemas are rejected before the request is sent.
    std::string deep = R"({"type":"object","properties":{"a":)";
    for (int index = 0; index < 12; ++index) {
        deep += R"({"type":"object","properties":{"a":)";
    }
    deep += R"({"type":"string"})";
    for (int index = 0; index < 12; ++index) {
        deep += "}}";
    }
    deep += "}}";
    MIRA_CHECK(!gate_schema_subset(parse_schema(deep)).has_value());
    return 0;
}

int validator_covers_the_subset() {
    const auto schema = parse_schema(
        R"({
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["tap", "done"]},
                "x": {"type": "number", "minimum": 0, "maximum": 1},
                "tags": {"type": "array", "items": {"type": "string"}, "minItems": 1},
                "note": {"type": "string", "maxLength": 4}
            },
            "required": ["action"],
            "additionalProperties": false
        })");
    MIRA_CHECK(validate_instance_against_schema(
                   parse_json(R"({"action":"tap","x":0.5,"tags":["a"],"note":"abcd"})").value(),
                   schema)
                   .empty());
    MIRA_CHECK(!validate_instance_against_schema(parse_json(R"({"action":"swipe"})").value(),
                                                 schema)
                    .empty()); // enum
    MIRA_CHECK(!validate_instance_against_schema(parse_json(R"({"x":2})").value(), schema)
                    .empty()); // missing required + range
    MIRA_CHECK(!validate_instance_against_schema(parse_json(R"({"action":"tap","zz":1})").value(),
                                                 schema)
                    .empty()); // additional property
    MIRA_CHECK(!validate_instance_against_schema(
                    parse_json(R"({"action":"tap","tags":[]})").value(), schema)
                    .empty()); // minItems
    MIRA_CHECK(!validate_instance_against_schema(
                    parse_json(R"({"action":"tap","note":"abcde"})").value(), schema)
                    .empty()); // maxLength
    MIRA_CHECK(!validate_instance_against_schema(parse_json(R"([])").value(), schema).empty());
    return 0;
}

[[nodiscard]] ModelRequest decision_request(const std::string &mode_text) {
    auto request = testing::make_profile(ProtocolDialect::OpenAIResponsesV1, "https://x.test");
    ModelRequest model_request;
    model_request.contract_version = SchemaVersion{1, 0};
    model_request.request_id = ModelRequestId::generate();
    model_request.operation_id = OperationId::generate();
    model_request.task_id = TaskId::generate();
    model_request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart text;
    text.text = "s";
    system_item.content.emplace_back(std::move(text));
    model_request.input = {std::move(system_item)};
    model_request.output_contract.mode = OutputMode::StrictJsonSchema;
    model_request.output_contract.schema_id = SchemaId::generate();
    model_request.output_contract.schema = agent_decision_schema();
    model_request.output_contract.canonical_schema_digest =
        canonical_json_digest(model_request.output_contract.schema.root);
    model_request.data_policy.store = false;
    static_cast<void>(mode_text);
    return model_request;
}

int decision_parse_outcomes() {
    const auto request = decision_request("");

    auto parse_with_text = [&request](const std::string &text) {
        return parse_decision(request, testing::text_response(text));
    };

    auto good = parse_with_text(R"({"action":"tap","x":0.2,"y":0.3,"reason":"r"})");
    MIRA_CHECK(good.outcome == DecisionParseOutcome::Decision);
    MIRA_CHECK(good.decision.has_value());
    MIRA_CHECK(good.decision->decision_digest_field ==
               decision_digest(request.output_contract.schema_id,
                               request.output_contract.schema_version,
                               good.decision->value));

    auto malformed_json = parse_with_text("not json at all");
    MIRA_CHECK(malformed_json.outcome == DecisionParseOutcome::Malformed);

    auto schema_violation = parse_with_text(R"({"action":"detonate","reason":"r"})");
    MIRA_CHECK(schema_violation.outcome == DecisionParseOutcome::Malformed);
    MIRA_CHECK(!schema_violation.violations.empty());

    auto fenced = parse_with_text("```json\n{\"action\":\"back\",\"reason\":\"r\"}\n```");
    MIRA_CHECK(fenced.outcome == DecisionParseOutcome::Decision);

    auto refused = parse_decision(request, testing::refused_response());
    MIRA_CHECK(refused.outcome == DecisionParseOutcome::Refused);

    ModelResponse incomplete;
    incomplete.contract_version = SchemaVersion{1, 0};
    incomplete.status = ModelCompletionStatus::Incomplete;
    incomplete.incomplete_reason = IncompleteReason::MaxOutputTokens;
    incomplete.requested_model = "m";
    MIRA_CHECK(parse_decision(request, incomplete).outcome == DecisionParseOutcome::Incomplete);

    ModelResponse filtered;
    filtered.contract_version = SchemaVersion{1, 0};
    filtered.status = ModelCompletionStatus::ContentFiltered;
    filtered.requested_model = "m";
    MIRA_CHECK(parse_decision(request, filtered).outcome ==
               DecisionParseOutcome::ContentFiltered);

    // Text mode never yields a decision.
    auto text_request = decision_request("");
    text_request.output_contract.mode = OutputMode::Text;
    auto text_mode = parse_decision(text_request, testing::text_response("hello"));
    MIRA_CHECK(text_mode.outcome == DecisionParseOutcome::NoExecutableOutput);
    return 0;
}

int repair_policy_is_bounded() {
    const auto request = decision_request("");
    auto failure = parse_decision(request, testing::text_response("not json"));
    MIRA_CHECK(failure.outcome == DecisionParseOutcome::Malformed);

    const RepairPolicy policy{1, 512};
    RepairBudget budget;
    auto repair = build_schema_repair_request(request, failure, policy, budget);
    MIRA_CHECK(repair.has_value());
    MIRA_CHECK(repair.value().request_id != request.request_id);
    // The repair prompt quotes a bounded summary; raw model output never
    // enters the request.
    const auto repair_text = to_json_string(model_request_to_json(repair.value()));
    MIRA_CHECK(repair_text.find("not json") == std::string::npos);
    MIRA_CHECK(repair.value().output_contract.canonical_schema_digest ==
               request.output_contract.canonical_schema_digest);

    budget.attempts_used = 1;
    MIRA_CHECK(!build_schema_repair_request(request, failure, policy, budget).has_value());
    return 0;
}

} // namespace

int main() {
    if (const int status = subset_gate_rejects_unsupported_keywords(); status != 0) {
        return status;
    }
    if (const int status = validator_covers_the_subset(); status != 0) {
        return status;
    }
    if (const int status = decision_parse_outcomes(); status != 0) {
        return status;
    }
    if (const int status = repair_policy_is_bounded(); status != 0) {
        return status;
    }
    return 0;
}
