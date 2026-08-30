#include <mira/model_schema.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <regex>
#include <string>
#include <utility>

namespace mira {

const SchemaSubsetLimits kDefaultSchemaSubsetLimits{};

namespace {

[[nodiscard]] Error schema_error(std::string message) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.model.schema";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] bool supported_keyword(std::string_view keyword) {
    return std::find(kSupportedSchemaKeywords.begin(), kSupportedSchemaKeywords.end(),
                     keyword) != kSupportedSchemaKeywords.end();
}

struct TypeNames {
    std::vector<std::string> accepted;
};

[[nodiscard]] bool instance_matches_type(const JsonValue &instance, std::string_view type) {
    if (type == "object") {
        return instance.is_object();
    }
    if (type == "array") {
        return instance.is_array();
    }
    if (type == "string") {
        return instance.is_string();
    }
    if (type == "boolean") {
        return instance.is_boolean();
    }
    if (type == "null") {
        return instance.is_null();
    }
    if (type == "integer") {
        return instance.is_integer();
    }
    if (type == "number") {
        return instance.is_number();
    }
    return false;
}

[[nodiscard]] std::string instance_type_name(const JsonValue &instance) {
    switch (instance.kind()) {
    case JsonValue::Kind::Null:
        return "null";
    case JsonValue::Kind::Boolean:
        return "boolean";
    case JsonValue::Kind::Integer:
        return "integer";
    case JsonValue::Kind::Number:
        return "number";
    case JsonValue::Kind::String:
        return "string";
    case JsonValue::Kind::Array:
        return "array";
    case JsonValue::Kind::Object:
        return "object";
    }
    return "unknown";
}

struct Validator final {
    std::vector<SchemaViolation> violations;
    SchemaSubsetLimits limits;

    void validate(const JsonValue &instance, const JsonValue &schema, const std::string &path,
                  std::size_t depth) {
        if (violations.size() >= 32) {
            return; // Bounded error reporting.
        }
        if (depth >= limits.max_depth) {
            violations.push_back({path, "depth", "schema nesting depth exceeded"});
            return;
        }
        if (const auto *type = schema.find("type")) {
            bool matches = false;
            if (type->is_string()) {
                matches = instance_matches_type(instance, *type->as_string());
            } else if (type->is_array()) {
                for (const auto &candidate : *type->as_array()) {
                    if (candidate.is_string() &&
                        instance_matches_type(instance, *candidate.as_string())) {
                        matches = true;
                        break;
                    }
                }
            }
            if (!matches) {
                violations.push_back({path, "type",
                                      "instance type " + instance_type_name(instance) +
                                          " does not match the schema type"});
            }
        }
        if (const auto *required = schema.find("required"); required != nullptr &&
                                                              required->is_array() &&
                                                              instance.is_object()) {
            for (const auto &name : *required->as_array()) {
                if (name.is_string() && instance.find(*name.as_string()) == nullptr) {
                    violations.push_back(
                        {path, "required", "missing required property '" + *name.as_string() + "'"});
                }
            }
        }
        if (const auto *properties = schema.find("properties");
            properties != nullptr && properties->is_object() && instance.is_object()) {
            const auto *additional = schema.find("additionalProperties");
            const bool allow_additional =
                additional != nullptr && additional->is_boolean() && *additional->as_boolean();
            const auto *object = instance.as_object();
            for (const auto &member : *object) {
                const auto *property_schema = properties->find(member.first);
                if (property_schema == nullptr) {
                    if (!allow_additional) {
                        violations.push_back(
                            {path.empty() ? member.first : path + "." + member.first,
                             "additionalProperties",
                             "property '" + member.first + "' is not declared in the schema"});
                    }
                    continue;
                }
                validate(member.second, *property_schema,
                         path.empty() ? member.first : path + "." + member.first, depth + 1);
            }
        }
        if (const auto *items = schema.find("items"); items != nullptr && instance.is_array()) {
            const auto *array = instance.as_array();
            for (std::size_t index = 0; index < array->size(); ++index) {
                validate((*array)[index], *items, path + "[" + std::to_string(index) + "]",
                         depth + 1);
            }
        }
        if (instance.is_array()) {
            const auto *array = instance.as_array();
            if (const auto *min_items = schema.find("minItems");
                min_items != nullptr && min_items->is_integer() &&
                array->size() < static_cast<std::size_t>(min_items->as_integer().value())) {
                violations.push_back({path, "minItems", "array is shorter than minItems"});
            }
            if (const auto *max_items = schema.find("maxItems");
                max_items != nullptr && max_items->is_integer() &&
                array->size() > static_cast<std::size_t>(max_items->as_integer().value())) {
                violations.push_back({path, "maxItems", "array is longer than maxItems"});
            }
        }
        if (instance.is_string()) {
            const auto size = instance.as_string()->size();
            if (const auto *min_length = schema.find("minLength");
                min_length != nullptr && min_length->is_integer() &&
                size < static_cast<std::size_t>(min_length->as_integer().value())) {
                violations.push_back({path, "minLength", "string is shorter than minLength"});
            }
            if (const auto *max_length = schema.find("maxLength");
                max_length != nullptr && max_length->is_integer() &&
                size > static_cast<std::size_t>(max_length->as_integer().value())) {
                violations.push_back({path, "maxLength", "string is longer than maxLength"});
            }
            if (const auto *pattern = schema.find("pattern");
                pattern != nullptr && pattern->is_string()) {
                try {
                    const std::regex expression(*pattern->as_string());
                    if (!std::regex_search(*instance.as_string(), expression)) {
                        violations.push_back({path, "pattern", "string does not match pattern"});
                    }
                } catch (const std::regex_error &) {
                    violations.push_back({path, "pattern", "schema pattern is not a valid regex"});
                }
            }
        }
        if (instance.is_number()) {
            const auto value = instance.as_number().value();
            if (const auto *minimum = schema.find("minimum");
                minimum != nullptr && minimum->is_number() && value < minimum->as_number().value()) {
                violations.push_back({path, "minimum", "number is below the minimum"});
            }
            if (const auto *maximum = schema.find("maximum");
                maximum != nullptr && maximum->is_number() && value > maximum->as_number().value()) {
                violations.push_back({path, "maximum", "number is above the maximum"});
            }
        }
        if (const auto *enum_values = schema.find("enum");
            enum_values != nullptr && enum_values->is_array()) {
            bool found = false;
            for (const auto &candidate : *enum_values->as_array()) {
                if (candidate == instance) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                violations.push_back({path, "enum", "value is not one of the enum members"});
            }
        }
        if (const auto *constant = schema.find("const"); constant != nullptr &&
                                                           !(*constant == instance)) {
            violations.push_back({path, "const", "value does not equal the const"});
        }
    }
};

[[nodiscard]] std::optional<std::string> first_text(const ModelResponse &response) {
    for (const auto &item : response.output) {
        if (const auto *message = std::get_if<MessageOutput>(&item)) {
            for (const auto &part : message->content) {
                if (const auto *text = std::get_if<OutputTextPart>(&part)) {
                    if (!text->text.empty()) {
                        return text->text;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<JsonValue> parse_embedded_json(const std::string &text) {
    // Structured text output is a single JSON document, optionally fenced.
    std::string_view view = text;
    const auto trim = [](std::string_view &value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\n' ||
                                  value.front() == '\r' || value.front() == '\t' ||
                                  value.front() == '`')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n' ||
                                  value.back() == '\r' || value.back() == '\t' ||
                                  value.back() == '`')) {
            value.remove_suffix(1);
        }
    };
    trim(view);
    // A fenced document may carry a language tag line ("```json").
    const auto first_brace = view.find('{');
    if (first_brace == std::string_view::npos) {
        return std::nullopt;
    }
    view.remove_prefix(first_brace);
    auto parsed = parse_json(view);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(parsed).value();
}

} // namespace

Result<void> gate_schema_subset(const JsonSchema &schema, const SchemaSubsetLimits &limits) {
    if (!schema.valid()) {
        return schema_error("schema root must be an object");
    }
    const auto serialized = canonical_json_string(schema.root);
    if (serialized.size() > limits.max_schema_bytes) {
        return schema_error("schema exceeds the size limit");
    }

    // Iterative walk; keeps failure messages precise per node.
    std::vector<std::pair<const JsonValue *, std::size_t>> pending{
        {&schema.root, 0}};
    while (!pending.empty()) {
        const auto [node, depth] = pending.back();
        pending.pop_back();
        if (depth >= limits.max_depth) {
            return schema_error("schema nesting exceeds the subset depth limit");
        }
        if (!node->is_object()) {
            return schema_error("every schema node must be an object");
        }
        for (const auto &member : *node->as_object()) {
            if (!supported_keyword(member.first)) {
                return schema_error("schema keyword is outside the M3 subset: " + member.first);
            }
        }
        if (const auto *properties = node->find("properties");
            properties != nullptr && properties->is_object() &&
            properties->as_object()->size() > limits.max_properties) {
            return schema_error("schema declares more properties than the subset allows");
        }
        if (const auto *enum_values = node->find("enum");
            enum_values != nullptr && enum_values->is_array() &&
            enum_values->as_array()->size() > limits.max_enum_values) {
            return schema_error("schema enum exceeds the subset limit");
        }
        if (const auto *pattern = node->find("pattern");
            pattern != nullptr && pattern->is_string() &&
            pattern->as_string()->size() > limits.max_pattern_bytes) {
            return schema_error("schema pattern exceeds the subset limit");
        }
        if (const auto *type = node->find("type"); type != nullptr && !type->is_string() &&
                                                     !type->is_array()) {
            return schema_error("schema type must be a string or array of strings");
        }
        if (const auto *required = node->find("required");
            required != nullptr && !required->is_array()) {
            return schema_error("schema required must be an array");
        }
        if (const auto *properties = node->find("properties")) {
            if (properties->is_object()) {
                for (const auto &member : *properties->as_object()) {
                    pending.emplace_back(&member.second, depth + 1);
                }
            }
        }
        if (const auto *items = node->find("items")) {
            pending.emplace_back(items, depth + 1);
        }
    }
    return Result<void>{};
}

std::vector<SchemaViolation> validate_instance_against_schema(const JsonValue &instance,
                                                              const JsonSchema &schema) {
    Validator validator;
    validator.limits = kDefaultSchemaSubsetLimits;
    validator.validate(instance, schema.root, std::string(), 0);
    return validator.violations;
}

DecisionParseResult parse_decision(const ModelRequest &request, const ModelResponse &response) {
    DecisionParseResult result;
    switch (response.status) {
    case ModelCompletionStatus::Refused:
        result.outcome = DecisionParseOutcome::Refused;
        return result;
    case ModelCompletionStatus::ContentFiltered:
        result.outcome = DecisionParseOutcome::ContentFiltered;
        return result;
    case ModelCompletionStatus::Incomplete:
        result.outcome = DecisionParseOutcome::Incomplete;
        return result;
    case ModelCompletionStatus::Failed:
    case ModelCompletionStatus::Cancelled:
    case ModelCompletionStatus::Unknown:
        result.outcome = DecisionParseOutcome::Failed;
        return result;
    case ModelCompletionStatus::Completed:
        break;
    }

    const auto &contract = request.output_contract;
    std::size_t tool_calls = 0;
    std::size_t executable = 0;
    for (const auto &item : response.output) {
        if (std::get_if<ToolCallOutput>(&item) != nullptr) {
            // Unresolved tool identities are rejected by the tool bridge with
            // a precise hosted/unknown-name diagnosis.
            ++tool_calls;
        }
        if (std::get_if<MessageOutput>(&item) != nullptr) {
            ++executable;
        }
    }

    if (contract.mode == OutputMode::StrictFunctionTool ||
        (tool_calls > 0 && contract.mode != OutputMode::Text)) {
        if (tool_calls > 0) {
            // Tool proposals are resolved (IDs, duplicates, hosted names) by
            // the tool bridge before the gateway admits them.
            if (executable > 0) {
                for (const auto &item : response.output) {
                    if (const auto *message = std::get_if<MessageOutput>(&item)) {
                        for (const auto &part : message->content) {
                            if (std::get_if<OutputTextPart>(&part) != nullptr &&
                                !std::get_if<OutputTextPart>(&part)->text.empty()) {
                                result.outcome = DecisionParseOutcome::Ambiguous;
                                result.safe_summary =
                                    "response mixes tool calls with decision text";
                                return result;
                            }
                        }
                    }
                }
            }
            result.outcome = DecisionParseOutcome::ToolProposals;
            return result;
        }
        result.outcome = DecisionParseOutcome::Malformed;
        result.safe_summary = "strict tool mode requires a tool call";
        return result;
    }

    if (tool_calls > 0) {
        result.outcome = DecisionParseOutcome::Ambiguous;
        result.safe_summary = "tool call appears in a non-tool output contract";
        return result;
    }

    const auto text = first_text(response);
    if (!text.has_value()) {
        result.outcome = DecisionParseOutcome::NoExecutableOutput;
        result.safe_summary = "completed response carries no executable output";
        return result;
    }
    if (contract.mode == OutputMode::Text) {
        result.outcome = DecisionParseOutcome::NoExecutableOutput;
        result.safe_summary = "text output mode never yields a decision";
        return result;
    }

    auto payload = parse_embedded_json(*text);
    if (!payload || !payload->is_object()) {
        result.outcome = DecisionParseOutcome::Malformed;
        result.safe_summary = "structured text output is not a JSON object";
        return result;
    }
    auto violations = validate_instance_against_schema(*payload, contract.schema);
    if (!violations.empty()) {
        result.outcome = DecisionParseOutcome::Malformed;
        result.violations = std::move(violations);
        result.safe_summary = "structured output violates the decision schema";
        return result;
    }
    result.outcome = DecisionParseOutcome::Decision;
    result.source = contract.mode == OutputMode::JsonObject ? DecisionSource::JsonObject
                                                           : DecisionSource::TextJson;
    DecisionCandidate candidate;
    candidate.schema_id = contract.schema_id;
    candidate.schema_version = contract.schema_version;
    candidate.value = std::move(*payload);
    candidate.decision_digest_field =
        decision_digest(contract.schema_id, contract.schema_version, candidate.value);
    result.decision = std::move(candidate);
    return result;
}

Result<ModelRequest> build_schema_repair_request(const ModelRequest &original,
                                                const DecisionParseResult &failure,
                                                const RepairPolicy &policy,
                                                const RepairBudget &budget) {
    if (budget.exhausted(policy)) {
        return schema_error("schema repair budget is exhausted");
    }
    if (failure.outcome != DecisionParseOutcome::Malformed) {
        return schema_error("repair is only applicable to malformed structured output");
    }
    if (!original.output_contract.schema.valid()) {
        return schema_error("repair requires the original decision schema");
    }
    ModelRequest repair = original;
    repair.request_id = ModelRequestId::generate();
    repair.continuation = std::nullopt;

    // The repair prompt quotes a bounded, redacted validation summary and the
    // schema itself; raw model output is never embedded.
    JsonValue::Object summary;
    summary.emplace_back("repair_of_request", original.request_id.to_string());
    summary.emplace_back("attempt", static_cast<std::int64_t>(budget.attempts_used + 1));
    JsonValue::Array violations;
    for (const auto &violation : failure.violations) {
        if (violations.size() >= 16) {
            break;
        }
        violations.emplace_back(JsonValue::Object{
            {"path", violation.path}, {"keyword", violation.keyword}, {"message", violation.message}});
    }
    summary.emplace_back("violations", std::move(violations));
    auto summary_text = to_json_string(JsonValue(std::move(summary)));
    if (summary_text.size() > policy.max_error_summary_bytes) {
        summary_text.resize(policy.max_error_summary_bytes);
    }

    ModelInputItem repair_item;
    repair_item.role = ModelRole::User;
    repair_item.provenance.source = "mira.decision-repair.v1";
    repair_item.authority = Sensitivity::Internal;
    TextPart part;
    part.text = "The previous structured output failed local validation. Repair the output so it "
                "conforms to the decision schema. Validation summary: " +
                summary_text;
    part.sensitivity = Sensitivity::Internal;
    repair_item.content.emplace_back(std::move(part));
    repair.input = {std::move(repair_item)};
    return repair;
}

} // namespace mira
