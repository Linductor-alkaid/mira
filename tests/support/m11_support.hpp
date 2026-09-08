#pragma once

// Shared fixtures for the M11 stage-D tests: trajectory compilation, task
// induction and the publish gate. Builds on the M10 fixtures; consumers:
// tests/m11/*.cpp.

#include "m10_support.hpp"

#include <mira/workflow_compiler.hpp>

#include <atomic>

namespace mira::testing {

// A counting tool whose schema accepts both a payload and a channel, so
// induction scenarios have a constant leaf next to a varying one.
struct ChannelTool final {
    std::atomic<int> dispatches{0};

    [[nodiscard]] BuiltinToolRegistration
    registration(const std::string &wire_name = "channeler") {
        BuiltinToolRegistration registration;
        registration.spec.wire_name = wire_name;
        registration.spec.description = "m11 channel tool";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}, "channel": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        registration.handler = [this](const JsonValue &,
                                      const OperationContext &) -> Result<JsonValue> {
            ++dispatches;
            return JsonValue{"sent"};
        };
        return registration;
    }
};

// A parameterized tool step whose payload references the declared "mode"
// parameter; trajectories of two different modes differ exactly there while
// the channel leaf stays constant.
[[nodiscard]] inline WorkflowStep parameterized_tool_step(
    const std::string &parameter, const std::string &wire = "channeler",
    bool side_effect_tool = false, std::optional<WorkflowPredicate> verification = {}) {
    WorkflowStep step;
    step.id = StepId::generate();
    step.name = "call-" + wire;
    step.kind = WorkflowStepKind::ToolCall;
    JsonValue::Object arguments;
    arguments.emplace_back("tool", wire);
    arguments.emplace_back("payload", JsonValue{JsonValue::Object{{"$param", JsonValue{parameter}}}});
    arguments.emplace_back("channel", "default-channel");
    step.arguments = JsonValue{std::move(arguments)};
    step.verification = std::move(verification);
    step.max_attempts = side_effect_tool ? 2 : 1;
    return step;
}

// Compiles a definition whose steps call the counting tool with a literal
// payload; pass with_parameter to declare the optional "mode" parameter
// (default "run") of the base fixture.
[[nodiscard]] inline WorkflowDefinition compilable_definition(
    const std::vector<WorkflowStep> &steps, std::string name = "m11-flow",
    bool with_parameter = false) {
    auto definition = base_definition(std::move(name), with_parameter);
    definition.steps = steps;
    return definition;
}

// The optional "mode" parameter of the base fixture as a standalone spec.
[[nodiscard]] inline WorkflowParameterSpec mode_parameter(JsonValue default_value) {
    WorkflowParameterSpec parameter;
    parameter.name = "mode";
    parameter.type = WorkflowParameterType::String;
    parameter.required = false;
    parameter.default_value = std::move(default_value);
    parameter.max_length = 16;
    return parameter;
}

// Drives one Strict run to completion and returns its run id.
[[nodiscard]] inline Result<WorkflowRunId> run_to_completion(WorkflowRuntime &workflow,
                                                             const WorkflowDefinition &definition,
                                                             JsonValue parameters) {
    const auto created =
        workflow.create_run(definition, std::move(parameters), WorkflowPolicy::Strict);
    if (!created.has_value()) {
        return created.error();
    }
    const auto driven = workflow.execute_run(created.value().run_id, drive_context());
    if (!driven.has_value() || driven.value().state != WorkflowRunState::Completed) {
        Error error;
        error.code = ErrorCode::Internal;
        error.domain = "mira.test";
        error.safe_message = "scenario run did not complete";
        return error;
    }
    return created.value().run_id;
}

} // namespace mira::testing
