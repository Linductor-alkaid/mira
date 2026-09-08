#pragma once

// Shared fixtures for the M10 stage-C tests: intervention and the full
// policy set. Builds on the M9 fixtures; consumers: tests/m10/*.cpp.

#include "m9_support.hpp"

namespace mira::testing {

// A definition whose allowed set names all five policies (the M9 fixture
// leaves AgentAssisted out).
[[nodiscard]] inline WorkflowDefinition full_policy_definition(std::string name = "m10-flow",
                                                               bool with_parameters = true) {
    auto definition = base_definition(std::move(name), with_parameters);
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::DryRun,
                                   WorkflowPolicy::Recoverable,
                                   WorkflowPolicy::AgentAssisted, WorkflowPolicy::Interactive};
    return definition;
}

// --- patch entry builders (DEC-021 §2 wire shape) ---------------------------

[[nodiscard]] inline WorkflowPatchEntry parameter_set(const std::string &name, JsonValue value) {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::RunParameters;
    entry.op = WorkflowPatchOp::Set;
    entry.path = name;
    entry.value = std::move(value);
    return entry;
}

[[nodiscard]] inline WorkflowPatchEntry parameter_unset(const std::string &name) {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::RunParameters;
    entry.op = WorkflowPatchOp::Unset;
    entry.path = name;
    return entry;
}

[[nodiscard]] inline WorkflowPatchEntry step_skip(const WorkflowStep &step) {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::StepArguments;
    entry.op = WorkflowPatchOp::Skip;
    entry.path = step.id.to_string();
    return entry;
}

[[nodiscard]] inline WorkflowPatchEntry step_unset(const WorkflowStep &step) {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::StepArguments;
    entry.op = WorkflowPatchOp::Unset;
    entry.path = step.id.to_string();
    return entry;
}

[[nodiscard]] inline WorkflowPatchEntry policy_set(WorkflowPolicy policy) {
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::ExecutionPolicy;
    entry.op = WorkflowPatchOp::Set;
    entry.path = "policy";
    entry.value = JsonValue{workflow_policy_name(policy)};
    return entry;
}

// Collects the parsed payloads of one event type for one session, in order.
[[nodiscard]] inline std::vector<JsonValue> session_event_payloads(const IEventStore &store,
                                                                    const SessionId &session,
                                                                    const std::string &type) {
    std::vector<JsonValue> payloads;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            if (envelope.payload.type == type) {
                if (auto parsed = parse_json(envelope.payload.data); parsed.has_value()) {
                    payloads.push_back(parsed.value());
                }
            }
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return payloads;
}

} // namespace mira::testing
