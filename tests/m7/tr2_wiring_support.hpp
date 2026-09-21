// M7 TR2 shared test fixtures for the runtime wiring gates (DEC-040
// section 18; M7 sections 4.7/5.7): fixed identities, fixture tools,
// definition/history builders, event scanning helpers and the mounted
// workflow scenario used by both halves of the split suite:
//   m7_tool_runtime_wiring_test.cpp  - reference expression + admission
//                                      domain (gates M7-TR2-G1..G3),
//   m7_tool_runtime_skill_test.cpp   - skill execution + Procedure wiring
//                                      domain (gates M7-TR2-G4..G6).
// Pure move from the pre-split single TU: every identity is fixed hex, every
// digest that reaches an assertion is content-derived, and no clock,
// randomness or generated run id reaches asserted data.
//
// Test-only header; consumed solely from tests/m7.

#pragma once

#include "../support/m13_support.hpp"
#include "../support/test.hpp"

#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_tool.hpp>
#include <mira/tool_executor.hpp>
#include <mira/tool_reference.hpp>
#include <mira/tool_skill.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_runtime.hpp>
#include <mira/workflow_versioning.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mira::testing {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// Shared fixtures (all identities fixed; no generate() reaches asserted data)
// ---------------------------------------------------------------------------

[[noreturn]] inline void fixture_failed(const std::string &what) {
    std::cerr << "fixture failure: " << what << '\n';
    std::abort();
}

inline JsonValue json_or_abort(std::string_view text) {
    auto parsed = parse_json(text);
    if (!parsed.has_value()) {
        fixture_failed("fixture JSON does not parse: " + std::string(text));
    }
    return parsed.value();
}

template <typename T> T must(const Result<T> &result, const char *what) {
    if (!result.has_value()) {
        fixture_failed(std::string(what) + ": " + result.error().safe_message);
    }
    return result.value();
}

inline void must(const Result<void> &result, const char *what) {
    if (!result.has_value()) {
        fixture_failed(std::string(what) + ": " + result.error().safe_message);
    }
}

// Fixed workflow identities.
const WorkflowId kWorkflowAlpha = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a1");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowBeta = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a2");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowGamma = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a3");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowDelta = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a4");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowEpsilon = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a5");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowZeta = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a6");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowEta = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a7");
    return id.has_value() ? *id : WorkflowId{};
}();

inline StepId fixed_step(const char *hex) {
    const auto id = StepId::parse(hex);
    return id.has_value() ? *id : StepId{};
}
inline const char *kStepLookup = "000000000000000000000000000000b1";
inline const char *kStepSecond = "000000000000000000000000000000b2";

inline ToolId fixed_tool(const char *hex) {
    const auto id = ToolId::parse(hex);
    return id.has_value() ? *id : ToolId{};
}

// Fixed content-derived digests for hand-built views and reference pins.
const Hash kDigestLookup = digest_string("m7-tr2/fixture/lookup-spec/v1");
const Hash kDigestForeign = digest_string("m7-tr2/fixture/foreign-spec");
const Sha256Digest kForeignDefinitionDigest = digest_string("m7-tr2/fixture/foreign-definition");

// Fixed schemas for the registry-backed fixture tool.
inline const char *kLookupSchema = R"json({
    "type": "object",
    "properties": {"key": {"type": "string"}},
    "required": ["key"],
    "additionalProperties": false
})json";
// Evolution that the recorded skeleton can no longer satisfy: a new required
// member the workflow's arguments never carry.
inline const char *kEvolvedRequiredSchema = R"json({
    "type": "object",
    "properties": {"key": {"type": "string"}, "token": {"type": "string"}},
    "required": ["key", "token"],
    "additionalProperties": false
})json";

// Error helpers for the two domains exercised here.
inline bool failed_in_reference_domain(const Error &error, std::int32_t domain_code) {
    return error.domain == "mira.tool_reference" && error.domain_code == domain_code &&
           error.code == ErrorCode::InvalidArgument && !error.safe_message.empty();
}

inline bool wiring_failure(const Error &error, ErrorCode code) {
    return error.domain == "mira.workflow" && error.code == code && !error.safe_message.empty();
}

// A registry-backed read-only fixture tool with a fixed identity: counts
// dispatches and answers "sent"; the first `failures_first` dispatches return
// failed records instead.
struct FixtureTool final {
    std::atomic<int> dispatches{0};
    int failures_first = 0;
    const char *tool_hex = "000000000000000000000000000000e1";
    std::string wire_name = "delta.lookup";
    std::string description = "wiring lookup tool v1";
    const char *schema_json = kLookupSchema;

    [[nodiscard]] BuiltinToolRegistration registration() {
        BuiltinToolRegistration registration;
        registration.spec.tool_id = fixed_tool(tool_hex);
        registration.spec.version = SemanticVersion{1, 0, 0};
        registration.spec.wire_name = wire_name;
        registration.spec.description = description;
        registration.spec.parameters_schema = JsonSchema{json_or_abort(schema_json)};
        registration.spec.has_side_effects = false;
        registration.handler = [this](const JsonValue &,
                                      const OperationContext &) -> Result<JsonValue> {
            const auto count = ++dispatches;
            if (count <= failures_first) {
                Error error;
                error.code = ErrorCode::PlatformError;
                error.domain = "mira.test";
                error.safe_message = "scripted tool failure";
                return error;
            }
            return JsonValue{std::string("sent")};
        };
        return registration;
    }
};

inline std::shared_ptr<BuiltinToolRegistry> registry_with(FixtureTool &tool) {
    auto registry = std::make_shared<BuiltinToolRegistry>();
    auto registration = tool.registration();
    must(registry->register_tool(std::move(registration.spec), std::move(registration.handler)),
         "register fixture tool");
    return registry;
}

// A hand-built exposure view entry with a caller-fixed spec digest.
inline ExposedToolSpec hand_spec(const std::string &wire, const std::string &description,
                          const Hash &spec_digest, const ToolId &tool_id,
                          const char *schema_json = kLookupSchema) {
    ExposedToolSpec spec;
    spec.tool_id = tool_id;
    spec.version = SemanticVersion{1, 0, 0};
    spec.wire_name = wire;
    spec.description = description;
    spec.parameters_schema = JsonSchema{json_or_abort(schema_json)};
    spec.spec_digest = spec_digest;
    spec.has_side_effects = false;
    return spec;
}

// The standard two-tool hand view: one lookup tool plus an unreferenced
// companion, both with fixed digests.
std::vector<ExposedToolSpec> hand_view() {
    return {hand_spec("delta.lookup", "wiring lookup tool v1", kDigestLookup,
                      fixed_tool("000000000000000000000000000000e1")),
            hand_spec("other.tool", "wiring other tool", kDigestForeign,
                      fixed_tool("000000000000000000000000000000e2"))};
}

inline WorkflowStep toolcall_step(const char *step_hex, const JsonValue &arguments) {
    WorkflowStep step;
    step.id = fixed_step(step_hex);
    step.name = "step-" + std::string(step_hex).substr(28);
    step.kind = WorkflowStepKind::ToolCall;
    step.arguments = arguments;
    return step;
}

inline WorkflowStep lookup_step(const char *step_hex, const std::string &tool_member) {
    const std::string text =
        std::string(R"json({"key":{"$param":"key"},"tool":")json") + tool_member + "\"}";
    return toolcall_step(step_hex, json_or_abort(text));
}

inline WorkflowParameterSpec key_param() {
    WorkflowParameterSpec parameter;
    parameter.name = "key";
    parameter.type = WorkflowParameterType::String;
    // Default-complete: the publish gate drives drafts with empty parameters
    // (DEC-025 section 3), so the fixture definitions must bind without input.
    parameter.required = false;
    parameter.default_value = JsonValue{std::string("k")};
    parameter.summary = "lookup key";
    parameter.min_length = 1;
    parameter.max_length = 32;
    return parameter;
}

inline WorkflowDefinition wiring_definition(const WorkflowId &workflow_id, SchemaVersion version,
                                     const std::string &name, const std::string &summary,
                                     std::vector<WorkflowParameterSpec> parameters,
                                     std::vector<WorkflowStep> steps) {
    WorkflowDefinition definition;
    definition.schema_version = version;
    definition.workflow_id = workflow_id;
    definition.name = name;
    definition.summary = summary;
    definition.parameters = std::move(parameters);
    definition.steps = std::move(steps);
    definition.default_policy = WorkflowPolicy::Strict;
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::DryRun};
    return definition;
}

// The standard v1.0 lookup definition: one read-only ToolCall step with a
// bound parameter.
inline WorkflowDefinition make_lookup_definition(const WorkflowId &workflow_id, SchemaVersion version,
                                          const std::string &summary) {
    return wiring_definition(workflow_id, version, "fixture.lookup", summary, {key_param()},
                             {lookup_step(kStepLookup, "delta.lookup")});
}

inline WorkflowDefinition make_v11_definition(const WorkflowId &workflow_id, const std::string &summary,
                                       const std::string &tool_member) {
    return wiring_definition(workflow_id, SchemaVersion{1, 1}, "fixture.lookup.v11", summary,
                             {key_param()}, {lookup_step(kStepLookup, tool_member)});
}

inline WorkflowVersionRecord make_version_record(const SemanticVersion &version,
                                          const Sha256Digest &content_digest,
                                          WorkflowValidationResult validation,
                                          const Sha256Digest &parent_digest = Sha256Digest{}) {
    WorkflowVersionRecord record;
    record.version = version;
    record.actor = "fixture";
    record.reason = "fixture version";
    record.content_digest = content_digest;
    record.parent_digest = parent_digest;
    record.validation = validation;
    if (validation != WorkflowValidationResult::NotValidated) {
        record.validation_evidence = content_digest;
    }
    return record;
}

inline WorkflowVersionHistory single_version_history(const WorkflowId &workflow_id,
                                              const WorkflowDefinition &definition,
                                              WorkflowValidationResult validation) {
    WorkflowVersionHistory history;
    history.workflow_id = workflow_id;
    must(append_workflow_version(
             history,
             make_version_record({1, 0, 0}, workflow_definition_digest(definition), validation)),
         "history fixture");
    return history;
}

inline WorkflowVersionHistory two_version_history(const WorkflowDefinition &definition_v1,
                                           const WorkflowDefinition &definition_v2) {
    WorkflowVersionHistory history;
    history.workflow_id = definition_v1.workflow_id;
    must(append_workflow_version(
             history, make_version_record({1, 0, 0}, workflow_definition_digest(definition_v1),
                                          WorkflowValidationResult::DryRunPassed)),
         "history v1 fixture");
    must(append_workflow_version(
             history, make_version_record({2, 0, 0}, workflow_definition_digest(definition_v2),
                                          WorkflowValidationResult::DryRunPassed,
                                          workflow_definition_digest(definition_v1))),
         "history v2 fixture");
    return history;
}

inline WorkflowToolRefManifest extract_or_abort(const WorkflowDefinition &definition,
                                         std::span<const ExposedToolSpec> view) {
    return must(extract_workflow_tool_references(definition, view), "extraction fixture");
}

// ---------------------------------------------------------------------------
// Event helpers over the fixture MemoryEventStore
// ---------------------------------------------------------------------------

struct TypedEvent final {
    std::string type;
    EventClass classification = EventClass::State;
    JsonValue payload;
};

inline std::vector<TypedEvent> typed_events(const MemoryEventStore &store, const SessionId &session,
                                     std::string_view type) {
    std::vector<TypedEvent> events;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            if (envelope.payload.type != type) {
                continue;
            }
            TypedEvent event;
            event.type = envelope.payload.type;
            event.classification = envelope.payload.classification;
            auto parsed = parse_json(envelope.payload.data);
            if (parsed.has_value()) {
                event.payload = std::move(parsed.value());
            }
            events.push_back(std::move(event));
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return events;
}

inline OperationContext plain_context() {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

// Builds a DEC-015-consistent proposal for one exposed registry entry.
inline ToolProposal proposal_for(const ExposedToolSpec &spec, const JsonValue &arguments,
                          const std::string &call_id, const OperationContext &context) {
    ToolProposal proposal;
    proposal.provider_call_id = ProviderToolCallId{call_id};
    proposal.tool_id = spec.tool_id;
    proposal.wire_name = spec.wire_name;
    proposal.tool_version = spec.version;
    proposal.arguments = arguments;
    proposal.arguments_digest = canonical_json_digest(arguments);
    proposal.operation_id = context.operation;
    proposal.has_side_effects = spec.has_side_effects;
    return proposal;
}

const ExposedToolSpec *find_exposed(std::span<const ExposedToolSpec> view,
                                    const std::string &wire_name) {
    const auto found = std::find_if(view.begin(), view.end(), [&](const ExposedToolSpec &entry) {
        return entry.wire_name == wire_name;
    });
    return found == view.end() ? nullptr : &*found;
}

// Field-wise equality for the two projection structs that predate defaulted
// comparisons.
inline bool manifests_equal(const WorkflowToolRefManifest &lhs, const WorkflowToolRefManifest &rhs) {
    return lhs.workflow_id == rhs.workflow_id && lhs.definition_digest == rhs.definition_digest &&
           lhs.entries == rhs.entries && lhs.digest == rhs.digest;
}

inline bool projections_equal(const WorkflowToolCompatProjection &lhs,
                       const WorkflowToolCompatProjection &rhs) {
    return lhs.workflow_id == rhs.workflow_id && lhs.definition_digest == rhs.definition_digest &&
           lhs.state == rhs.state && lhs.entries == rhs.entries && lhs.digest == rhs.digest;
}

// ---------------------------------------------------------------------------
// Shared scenario: one published+mounted lookup workflow on registry v1
// ---------------------------------------------------------------------------

struct MountedScenario final {
    std::unique_ptr<WorkflowFixture> fixture;
    std::shared_ptr<FixtureTool> tool; // keeps the registered handler alive
    std::unique_ptr<WorkflowRuntime> workflow;
    WorkflowDefinition definition;
    Sha256Digest digest{};
    WorkflowToolRefManifest mounted;
};

inline MountedScenario mount_lookup_workflow(const WorkflowId &workflow_id, const std::string &summary) {
    MountedScenario scenario;
    scenario.fixture = std::make_unique<WorkflowFixture>();
    scenario.tool = std::make_shared<FixtureTool>();
    scenario.fixture->registry_ = registry_with(*scenario.tool);
    scenario.workflow = scenario.fixture->make_workflow();
    scenario.definition = make_lookup_definition(workflow_id, SchemaVersion{1, 0}, summary);
    const auto published =
        must(scenario.workflow->publish_validated(scenario.definition, "fixture", "mount scenario"),
             "publish scenario");
    scenario.digest = published.ir_digest;
    scenario.mounted =
        must(scenario.workflow->workflow_tool_refs(workflow_id, scenario.digest), "mount readback");
    return scenario;
}

} // namespace mira::testing
