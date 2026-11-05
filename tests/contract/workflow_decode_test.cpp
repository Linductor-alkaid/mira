#include "support/test.hpp"
#include <mira/workflow_ir.hpp>
#include <mira/workflow_learning.hpp>
#include <mira/workflow_navigation.hpp>

namespace {
using namespace mira;

template <typename T>
int expect_error(const Result<T> &result, ErrorCode code, std::int32_t domain_code,
                 std::string_view message) {
    MIRA_CHECK(!result.has_value());
    MIRA_CHECK(result.error().code == code);
    MIRA_CHECK(result.error().domain == "mira.workflow");
    MIRA_CHECK(result.error().domain_code == domain_code);
    MIRA_CHECK(result.error().safe_message == message);
    return 0;
}

int key_sets_keep_schema_errors() {
    // Unknown fields take precedence over missing fields, even when all
    // required fields are absent. Pin each schema's public error mapping.
    const JsonValue unknown{JsonValue::Object{{"future", true}}};
    const JsonValue empty{JsonValue::Object{}};
    MIRA_CHECK(expect_error(workflow_definition_from_json(unknown), ErrorCode::UnsupportedVersion,
                            0, "workflow ir: workflow definition: unknown field 'future'") == 0);
    MIRA_CHECK(expect_error(app_model_from_json(unknown), ErrorCode::UnsupportedVersion,
                            static_cast<std::int32_t>(AppModelError::UnknownField),
                            "app model: app model: unknown field 'future'") == 0);
    MIRA_CHECK(expect_error(failure_signature_from_json(unknown), ErrorCode::InvalidArgument,
                            static_cast<std::int32_t>(WorkflowLearningError::UnknownField),
                            "workflow learning: failure signature: unknown field 'future'") == 0);
    MIRA_CHECK(expect_error(workflow_definition_from_json(empty), ErrorCode::InvalidArgument, 0,
                            "workflow ir: workflow definition: missing field 'schema_version'") ==
               0);
    MIRA_CHECK(expect_error(app_model_from_json(empty), ErrorCode::InvalidArgument,
                            static_cast<std::int32_t>(AppModelError::InvalidShape),
                            "app model: app model: missing field 'schema_version'") == 0);
    MIRA_CHECK(expect_error(failure_signature_from_json(empty), ErrorCode::InvalidArgument,
                            static_cast<std::int32_t>(WorkflowLearningError::InvalidShape),
                            "workflow learning: failure signature: missing field 'workflow_id'") ==
               0);
    for (const JsonValue &value : {JsonValue{}, JsonValue{true}, JsonValue{JsonValue::Array{}}}) {
        MIRA_CHECK(expect_error(workflow_definition_from_json(value), ErrorCode::InvalidArgument, 0,
                                "workflow ir: workflow definition must be an object") == 0);
        MIRA_CHECK(expect_error(app_model_from_json(value), ErrorCode::InvalidArgument,
                                static_cast<std::int32_t>(AppModelError::InvalidShape),
                                "app model: app model must be an object") == 0);
        MIRA_CHECK(expect_error(failure_signature_from_json(value), ErrorCode::InvalidArgument,
                                static_cast<std::int32_t>(WorkflowLearningError::InvalidShape),
                                "workflow learning: failure signature must be an object") == 0);
    }
    return 0;
}

int strings_keep_nonempty_byte_bounds() {
    WorkflowDefinition definition;
    definition.workflow_id = WorkflowId{Id128{Id128::Bytes{1}}};
    definition.name = "flow";
    definition.allowed_policies = {WorkflowPolicy::Strict};
    WorkflowStep step;
    step.id = StepId{Id128{Id128::Bytes{2}}};
    step.arguments = JsonValue{JsonValue::Object{{"tool", "noop"}}};
    definition.steps.push_back(step);
    auto ir = workflow_definition_to_json(definition);
    AppModel model;
    model.app_id = "app";
    model.name = "name";
    auto app = app_model_to_json(model);
    WorkflowFailureSignature signature;
    signature.workflow_id = "flow";
    signature.reason_code = "failed";
    auto failure = failure_signature_to_json(signature);
    WorkflowLimits ir_limits;
    ir_limits.max_string_bytes = 4;
    AppModelLimits app_limits;
    app_limits.max_string_bytes = 4;
    WorkflowLearningLimits learning_limits;
    learning_limits.max_id_bytes = 4;
    // One UTF-8 code point plus ASCII occupies four bytes. Learning ids retain their
    // stricter ASCII vocabulary, so its boundary case uses ASCII instead.
    for (const auto &text : {std::string{"abcd"}, std::string{"测a"}}) {
        ir.set("name", text);
        app.set("name", text);
        MIRA_CHECK(workflow_definition_from_json(ir, ir_limits).has_value());
        MIRA_CHECK(app_model_from_json(app, app_limits).has_value());
    }
    failure.set("step_id", "abcd");
    MIRA_CHECK(failure_signature_from_json(failure, learning_limits).has_value());
    for (const auto &bad : {JsonValue{""}, JsonValue{"abcde"}, JsonValue{42}}) {
        ir.set("name", bad);
        app.set("name", bad);
        failure.set("step_id", bad);
        MIRA_CHECK(expect_error(workflow_definition_from_json(ir, ir_limits),
                                ErrorCode::InvalidArgument, 0,
                                "workflow ir: workflow name must be a bounded string") == 0);
        MIRA_CHECK(expect_error(app_model_from_json(app, app_limits), ErrorCode::InvalidArgument,
                                static_cast<std::int32_t>(AppModelError::InvalidShape),
                                "app model: app model name must be bounded") == 0);
        MIRA_CHECK(
            expect_error(
                failure_signature_from_json(failure, learning_limits), ErrorCode::InvalidArgument,
                static_cast<std::int32_t>(WorkflowLearningError::InvalidShape),
                "workflow learning: failure signature 'step_id' must be a bounded string") == 0);
    }
    // Optional absence/null remains accepted; empty present values do not.
    failure.set("step_id", JsonValue{});
    MIRA_CHECK(failure_signature_from_json(failure, learning_limits).has_value());
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(key_sets_keep_schema_errors() == 0);
    MIRA_CHECK(strings_keep_nonempty_byte_bounds() == 0);
    return 0;
}
