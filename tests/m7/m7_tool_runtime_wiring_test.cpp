// M7 TR2 runtime wiring verification: gates M7-TR2-G1..G6 (frozen in
// docs/plans/m7-tools-evaluation-platform-v1.md sections 4.7/5.7; contract
// source docs/design/tool_reference_and_skill_design.md section 18).
//
// Covers the WorkflowRuntime consumption of the TR0 reference layer (IR v1.1
// reference expression, the mount table, the publish-gate auto extraction,
// the create_run tool-compatibility admission gate with its Degraded audit
// event) and of the TR1 Skill publication layer (skill execution adapters
// over the Tool channel, the Procedure memory sync). G6 is the consumer
// closure, re-run through the mira_minimal_consumer ctest entry.
//
// Deterministic by construction: every value that reaches an assertion or the
// --report output is content-derived. Workflow/step/tool identities are parsed
// from fixed hex strings, spec digests on hand-built views derive from fixed
// content, registry-backed spec digests derive from fixed specs (fixed tool
// ids), and no clock, randomness or generated run id reaches asserted data, so
// reports reproduce byte-for-byte across processes and build trees.

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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// Shared fixtures (all identities fixed; no generate() reaches asserted data)
// ---------------------------------------------------------------------------

[[noreturn]] void fixture_failed(const std::string &what) {
    std::cerr << "fixture failure: " << what << '\n';
    std::abort();
}

JsonValue json_or_abort(std::string_view text) {
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

void must(const Result<void> &result, const char *what) {
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

StepId fixed_step(const char *hex) {
    const auto id = StepId::parse(hex);
    return id.has_value() ? *id : StepId{};
}
const char *kStepLookup = "000000000000000000000000000000b1";
const char *kStepSecond = "000000000000000000000000000000b2";

ToolId fixed_tool(const char *hex) {
    const auto id = ToolId::parse(hex);
    return id.has_value() ? *id : ToolId{};
}

// Fixed content-derived digests for hand-built views and reference pins.
const Hash kDigestLookup = digest_string("m7-tr2/fixture/lookup-spec/v1");
const Hash kDigestForeign = digest_string("m7-tr2/fixture/foreign-spec");
const Sha256Digest kForeignDefinitionDigest = digest_string("m7-tr2/fixture/foreign-definition");

// Fixed schemas for the registry-backed fixture tool.
const char *kLookupSchema = R"json({
    "type": "object",
    "properties": {"key": {"type": "string"}},
    "required": ["key"],
    "additionalProperties": false
})json";
// Evolution that the recorded skeleton can no longer satisfy: a new required
// member the workflow's arguments never carry.
const char *kEvolvedRequiredSchema = R"json({
    "type": "object",
    "properties": {"key": {"type": "string"}, "token": {"type": "string"}},
    "required": ["key", "token"],
    "additionalProperties": false
})json";

// Error helpers for the two domains exercised here.
bool failed_in_reference_domain(const Error &error, std::int32_t domain_code) {
    return error.domain == "mira.tool_reference" && error.domain_code == domain_code &&
           error.code == ErrorCode::InvalidArgument && !error.safe_message.empty();
}

bool wiring_failure(const Error &error, ErrorCode code) {
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

std::shared_ptr<BuiltinToolRegistry> registry_with(FixtureTool &tool) {
    auto registry = std::make_shared<BuiltinToolRegistry>();
    auto registration = tool.registration();
    must(registry->register_tool(std::move(registration.spec), std::move(registration.handler)),
         "register fixture tool");
    return registry;
}

// A hand-built exposure view entry with a caller-fixed spec digest.
ExposedToolSpec hand_spec(const std::string &wire, const std::string &description,
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

WorkflowStep toolcall_step(const char *step_hex, const JsonValue &arguments) {
    WorkflowStep step;
    step.id = fixed_step(step_hex);
    step.name = "step-" + std::string(step_hex).substr(28);
    step.kind = WorkflowStepKind::ToolCall;
    step.arguments = arguments;
    return step;
}

WorkflowStep lookup_step(const char *step_hex, const std::string &tool_member) {
    const std::string text =
        std::string(R"json({"key":{"$param":"key"},"tool":")json") + tool_member + "\"}";
    return toolcall_step(step_hex, json_or_abort(text));
}

WorkflowParameterSpec key_param() {
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

WorkflowDefinition wiring_definition(const WorkflowId &workflow_id, SchemaVersion version,
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
WorkflowDefinition make_lookup_definition(const WorkflowId &workflow_id, SchemaVersion version,
                                          const std::string &summary) {
    return wiring_definition(workflow_id, version, "fixture.lookup", summary, {key_param()},
                             {lookup_step(kStepLookup, "delta.lookup")});
}

WorkflowDefinition make_v11_definition(const WorkflowId &workflow_id, const std::string &summary,
                                       const std::string &tool_member) {
    return wiring_definition(workflow_id, SchemaVersion{1, 1}, "fixture.lookup.v11", summary,
                             {key_param()}, {lookup_step(kStepLookup, tool_member)});
}

WorkflowVersionRecord make_version_record(const SemanticVersion &version,
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

WorkflowVersionHistory single_version_history(const WorkflowId &workflow_id,
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

WorkflowVersionHistory two_version_history(const WorkflowDefinition &definition_v1,
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

WorkflowToolRefManifest extract_or_abort(const WorkflowDefinition &definition,
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

std::vector<TypedEvent> typed_events(const MemoryEventStore &store, const SessionId &session,
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

OperationContext plain_context() {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

// Builds a DEC-015-consistent proposal for one exposed registry entry.
ToolProposal proposal_for(const ExposedToolSpec &spec, const JsonValue &arguments,
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
bool manifests_equal(const WorkflowToolRefManifest &lhs, const WorkflowToolRefManifest &rhs) {
    return lhs.workflow_id == rhs.workflow_id && lhs.definition_digest == rhs.definition_digest &&
           lhs.entries == rhs.entries && lhs.digest == rhs.digest;
}

bool projections_equal(const WorkflowToolCompatProjection &lhs,
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

MountedScenario mount_lookup_workflow(const WorkflowId &workflow_id, const std::string &summary) {
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

// ---------------------------------------------------------------------------
// --report scenario (pure content-derived values only)
// ---------------------------------------------------------------------------

std::string build_wiring_report() {
    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.workflow.tool_runtime_wiring.report.v1"));

    const std::vector<ExposedToolSpec> view = hand_view();

    // IR v1.1 reference expression: decode/round trip and extraction for both
    // reference forms; the v1.0 golden stays self-consistent; v1.2 rejects.
    const WorkflowDefinition v11_follow =
        make_v11_definition(kWorkflowAlpha, "report v1.1 follow summary", "toolref:delta.lookup");
    const JsonValue follow_json = workflow_definition_to_json(v11_follow);
    const auto follow_decoded = must(workflow_definition_from_json(follow_json), "report decode");
    const WorkflowToolRefManifest follow_refs = extract_or_abort(follow_decoded, view);

    const WorkflowDefinition v11_pinned =
        make_v11_definition(kWorkflowBeta, "report v1.1 pinned summary",
                            "toolref:delta.lookup@" + kDigestLookup.to_string());
    const JsonValue pinned_json = workflow_definition_to_json(v11_pinned);
    const auto pinned_decoded = must(workflow_definition_from_json(pinned_json), "report decode");
    const WorkflowToolRefManifest pinned_refs = extract_or_abort(pinned_decoded, view);

    JsonValue::Object ir;
    ir.emplace_back("v11_follow_manifest",
                    canonical_json_string(workflow_tool_refs_to_json(follow_refs)));
    ir.emplace_back("v11_pinned_manifest",
                    canonical_json_string(workflow_tool_refs_to_json(pinned_refs)));
    {
        const WorkflowDefinition v10 =
            make_lookup_definition(kWorkflowGamma, SchemaVersion{1, 0}, "report v1.0 summary");
        const auto decoded =
            must(workflow_definition_from_json(workflow_definition_to_json(v10)), "report v1.0");
        ir.emplace_back("v10_digest", JsonValue{workflow_definition_digest(v10).to_string()});
        ir.emplace_back("v10_roundtrip_digest",
                        JsonValue{workflow_definition_digest(decoded).to_string()});
        ir.emplace_back("v10_minor",
                        JsonValue{static_cast<std::int64_t>(decoded.schema_version.minor)});
    }
    {
        JsonValue mutated = workflow_definition_to_json(v11_follow);
        JsonValue::Object version;
        version.emplace_back("major", JsonValue{std::int64_t{1}});
        version.emplace_back("minor", JsonValue{std::int64_t{2}});
        mutated.set("schema_version", JsonValue{std::move(version)});
        const auto rejected = workflow_definition_from_json(mutated);
        ir.emplace_back("v12_rejected", JsonValue{!rejected.has_value()});
        ir.emplace_back(
            "v12_code",
            JsonValue{static_cast<std::int64_t>(
                rejected.has_value() ? 0 : static_cast<std::int64_t>(rejected.error().code))});
    }
    report.emplace_back("ir", JsonValue{std::move(ir)});

    // Compatibility projection matrix over the fixed view set.
    const std::vector<ExposedToolSpec> evolved_view = {
        hand_spec("delta.lookup", "wiring lookup tool v2",
                  digest_string("m7-tr2/fixture/lookup-spec/v2"),
                  fixed_tool("000000000000000000000000000000e1")),
        hand_spec("other.tool", "wiring other tool", kDigestForeign,
                  fixed_tool("000000000000000000000000000000e2"))};
    const std::vector<ExposedToolSpec> required_view = {
        hand_spec("delta.lookup", "wiring lookup tool v3",
                  digest_string("m7-tr2/fixture/lookup-spec/v3"),
                  fixed_tool("000000000000000000000000000000e1"), kEvolvedRequiredSchema),
        hand_spec("other.tool", "wiring other tool", kDigestForeign,
                  fixed_tool("000000000000000000000000000000e2"))};
    const std::vector<ExposedToolSpec> vanished_view = {
        hand_spec("other.tool", "wiring other tool", kDigestForeign,
                  fixed_tool("000000000000000000000000000000e2"))};

    const WorkflowDefinition v10 =
        make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "report projection summary");
    const WorkflowToolRefManifest pinned_view_refs = extract_or_abort(v10, view);
    ToolRefExtractionOptions follow_options;
    follow_options.default_mode = ToolReferenceMode::FollowLatest;
    const WorkflowToolRefManifest follow_view_refs =
        must(extract_workflow_tool_references(v10, view, follow_options), "report follow extract");

    JsonValue::Object projection;
    const auto emit_projection = [&](const char *name, const WorkflowToolCompatProjection &value) {
        JsonValue::Object entry;
        entry.emplace_back("state", std::string(workflow_tool_compat_state_name(value.state)));
        entry.emplace_back("digest", JsonValue{value.digest.to_string()});
        const auto decision = admit_workflow_run_by_tool_compat(value);
        entry.emplace_back("admitted", JsonValue{decision.admitted});
        entry.emplace_back("reason", JsonValue{decision.reason});
        projection.emplace_back(name, JsonValue{std::move(entry)});
    };
    emit_projection("pin_equal",
                    must(project_workflow_tool_compatibility(v10, pinned_view_refs, view),
                         "report project pin equal"));
    emit_projection("pin_mismatch_bindable",
                    must(project_workflow_tool_compatibility(v10, pinned_view_refs, evolved_view),
                         "report project degraded"));
    emit_projection("pin_mismatch_unbindable",
                    must(project_workflow_tool_compatibility(v10, pinned_view_refs, required_view),
                         "report project invalid"));
    emit_projection("wire_vanished",
                    must(project_workflow_tool_compatibility(v10, pinned_view_refs, vanished_view),
                         "report project vanished"));
    emit_projection("follow_resolved",
                    must(project_workflow_tool_compatibility(v10, follow_view_refs, evolved_view),
                         "report project follow"));
    emit_projection("pin_equal_again",
                    must(project_workflow_tool_compatibility(v10, pinned_view_refs, view),
                         "report project repeat"));
    report.emplace_back("projection", JsonValue{std::move(projection)});

    // Runtime mount table: publish_validated auto-mounts a manifest anchored
    // to the registry-backed view (fixed tool ids keep the spec digests
    // content-derived); attach rejections are reported as booleans.
    JsonValue::Object mount;
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        const auto published = must(workflow->publish_validated(v10, "fixture", "report mount"),
                                    "report mount publish");
        const auto readback =
            must(workflow->workflow_tool_refs(kWorkflowAlpha, published.ir_digest),
                 "report mount readback");
        mount.emplace_back("auto_mount_manifest",
                           canonical_json_string(workflow_tool_refs_to_json(readback)));
        // Idempotent re-mount of the identical manifest is a NoOp.
        mount.emplace_back("attach_idempotent",
                           JsonValue{workflow->attach_workflow_tool_refs(readback).has_value()});
        // A different manifest for the same key rejects and leaves the mount.
        const auto conflicting = workflow->attach_workflow_tool_refs(follow_view_refs);
        mount.emplace_back("attach_conflict_rejected", JsonValue{!conflicting.has_value()});
        const auto after = must(workflow->workflow_tool_refs(kWorkflowAlpha, published.ir_digest),
                                "report mount after conflict");
        mount.emplace_back("mount_stable", JsonValue{after.digest == readback.digest});
    }
    report.emplace_back("mount", JsonValue{std::move(mount)});

    // Skill surface identity: descriptor digest over a fixed view is content.
    JsonValue::Object skill;
    {
        const WorkflowToolRefManifest refs = extract_or_abort(v10, view);
        const auto descriptor = must(
            make_skill_descriptor(v10, refs, view, "report.skill", {1, 0, 0}), "report descriptor");
        skill.emplace_back("descriptor_digest", JsonValue{descriptor.digest.to_string()});
        skill.emplace_back("surface_description", JsonValue{descriptor.surface.description});
        skill.emplace_back("has_side_effects", JsonValue{descriptor.surface.has_side_effects});
    }
    report.emplace_back("skill", JsonValue{std::move(skill)});

    // Procedure sync counters over two published skills (counts only; the
    // memory records carry wall-clock fields that never enter the report).
    JsonValue::Object procedure;
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
        auto workflow = fixture.make_workflow();
        must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
             "report learning context");
        SkillPublicationRegistry skills;
        int link = 0;
        for (const WorkflowId id : {kWorkflowAlpha, kWorkflowBeta}) {
            ++link;
            const WorkflowDefinition definition =
                make_lookup_definition(id, SchemaVersion{1, 0}, "report procedure summary");
            const auto published =
                must(workflow->publish_validated(definition, "fixture", "report procedure"),
                     "report procedure publish");
            const WorkflowToolRefManifest refs = must(
                workflow->workflow_tool_refs(id, published.ir_digest), "report procedure refs");
            const WorkflowVersionHistory history =
                single_version_history(id, definition, WorkflowValidationResult::DryRunPassed);
            const auto descriptor =
                must(make_skill_descriptor(definition, refs, view,
                                           "report.skill_" + std::to_string(link), {1, 0, 0}),
                     "report procedure descriptor");
            must(skills.publish_skill(descriptor, definition, refs, view, history),
                 "report procedure publish skill");
        }
        const auto first = must(workflow->sync_skill_procedure_index(skills), "report sync one");
        const auto second = must(workflow->sync_skill_procedure_index(skills), "report sync two");
        procedure.emplace_back("applied_first",
                               JsonValue{static_cast<std::int64_t>(first.applied)});
        procedure.emplace_back("idempotent_second",
                               JsonValue{static_cast<std::int64_t>(second.idempotent)});
        procedure.emplace_back("records",
                               JsonValue{static_cast<std::int64_t>(memory->stored_records())});
        const auto index =
            must(project_skill_procedure_index(skills.publications()), "report index");
        procedure.emplace_back("index_digest", JsonValue{index.digest.to_string()});
    }
    report.emplace_back("procedure", JsonValue{std::move(procedure)});

    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G1: IR v1.1 reference expression and extraction (M7-TR2-G1)
// ---------------------------------------------------------------------------

int g1_ir_version_evolution() {
    const std::vector<ExposedToolSpec> view = hand_view();

    // v1.1 documents decode and round trip losslessly (follow form), both from
    // a struct and from raw text.
    const WorkflowDefinition follow =
        make_v11_definition(kWorkflowAlpha, "v1.1 follow summary", "toolref:delta.lookup");
    MIRA_CHECK(follow.schema_version.major == 1 && follow.schema_version.minor == 1);
    const JsonValue follow_json = workflow_definition_to_json(follow);
    {
        const auto decoded = must(workflow_definition_from_json(follow_json), "follow decode");
        MIRA_CHECK(decoded.schema_version.major == 1 && decoded.schema_version.minor == 1);
        MIRA_CHECK(workflow_definition_digest(decoded) == workflow_definition_digest(follow));
        MIRA_CHECK(decoded.steps.size() == 1 && decoded.steps.front() == follow.steps.front());
        const auto reparsed = parse_workflow_definition(to_json_string(follow_json));
        MIRA_CHECK(reparsed.has_value());
        MIRA_CHECK(reparsed.value().schema_version.minor == 1);
        MIRA_CHECK(workflow_definition_digest(reparsed.value()) ==
                   workflow_definition_digest(follow));
        MIRA_CHECK(validate_workflow_definition(follow).has_value());
    }
    // Pinned form round trips as well.
    const WorkflowDefinition pinned = make_v11_definition(
        kWorkflowBeta, "v1.1 pinned summary", "toolref:delta.lookup@" + kDigestLookup.to_string());
    {
        const auto decoded = must(
            workflow_definition_from_json(workflow_definition_to_json(pinned)), "pinned decode");
        MIRA_CHECK(decoded.schema_version.minor == 1);
        MIRA_CHECK(workflow_definition_digest(decoded) == workflow_definition_digest(pinned));
        const auto *tool = decoded.steps.front().arguments.find("tool");
        MIRA_CHECK(tool != nullptr && tool->is_string() &&
                   *tool->as_string() ==
                       std::string("toolref:delta.lookup@") + kDigestLookup.to_string());
    }

    // The v1.0 golden: a fixed document decodes with unchanged semantics and
    // re-encodes to a self-consistent digest; extraction keeps the pre-TR2
    // pinned-by-default behavior.
    {
        const std::string golden = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "000000000000000000000000000000a3",
            "name": "golden-v10",
            "summary": "v1.0 golden document",
            "parameters": [
                {"name": "key", "type": "string", "required": true,
                 "constraints": {"min_length": 1, "max_length": 32}}
            ],
            "steps": [
                {"step_id": "000000000000000000000000000000b1", "kind": "tool_call",
                 "name": "call-lookup",
                 "arguments": {"tool": "delta.lookup", "key": {"$param": "key"}}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        const auto parsed = parse_workflow_definition(golden);
        MIRA_CHECK(parsed.has_value());
        MIRA_CHECK(parsed.value().schema_version.major == 1 &&
                   parsed.value().schema_version.minor == 0);
        MIRA_CHECK(parsed.value().steps.size() == 1);
        MIRA_CHECK(parsed.value().steps.front().kind == WorkflowStepKind::ToolCall);
        const JsonValue encoded = workflow_definition_to_json(parsed.value());
        const auto reparsed = must(workflow_definition_from_json(encoded), "golden redecode");
        MIRA_CHECK(workflow_definition_digest(reparsed) ==
                   workflow_definition_digest(parsed.value()));
        MIRA_CHECK(reparsed.schema_version.minor == 0);
        const WorkflowToolRefManifest refs = extract_or_abort(parsed.value(), view);
        MIRA_CHECK(refs.entries.size() == 1);
        MIRA_CHECK(refs.entries.front().mode == ToolReferenceMode::PinnedDigest);
        MIRA_CHECK(refs.entries.front().pinned_spec_digest.has_value());
        MIRA_CHECK(*refs.entries.front().pinned_spec_digest == kDigestLookup);
    }

    // {1,2} and a newer major are rejected fail closed.
    for (const SchemaVersion version : {SchemaVersion{1, 2}, SchemaVersion{2, 0}}) {
        JsonValue mutated = workflow_definition_to_json(follow);
        JsonValue::Object version_object;
        version_object.emplace_back("major", JsonValue{static_cast<std::int64_t>(version.major)});
        version_object.emplace_back("minor", JsonValue{static_cast<std::int64_t>(version.minor)});
        mutated.set("schema_version", JsonValue{std::move(version_object)});
        const auto rejected = workflow_definition_from_json(mutated);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::UnsupportedVersion);
        const auto text_rejected = parse_workflow_definition(to_json_string(mutated));
        MIRA_CHECK(!text_rejected.has_value());
        MIRA_CHECK(text_rejected.error().code == ErrorCode::UnsupportedVersion);
    }
    return 0;
}

int g1_v1_0_extraction_unchanged() {
    const std::vector<ExposedToolSpec> view = hand_view();

    // Bare names under v1.0 behave exactly as before TR2: default pinned
    // extraction against the view observation.
    {
        const WorkflowDefinition v10 =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "bare v1.0");
        const WorkflowToolRefManifest refs = extract_or_abort(v10, view);
        MIRA_CHECK(refs.workflow_id == kWorkflowAlpha);
        MIRA_CHECK(refs.definition_digest == workflow_definition_digest(v10));
        MIRA_CHECK(refs.entries.size() == 1);
        MIRA_CHECK(refs.entries.front().step_id == fixed_step(kStepLookup).to_string());
        MIRA_CHECK(refs.entries.front().wire_name == "delta.lookup");
        MIRA_CHECK(refs.entries.front().mode == ToolReferenceMode::PinnedDigest);
        MIRA_CHECK(refs.entries.front().pinned_spec_digest.has_value());
        MIRA_CHECK(*refs.entries.front().pinned_spec_digest == kDigestLookup);
        MIRA_CHECK(verify_workflow_tool_refs(refs, kWorkflowAlpha, workflow_definition_digest(v10))
                       .has_value());
    }
    // A v1.0 definition carrying the toolref: glyph fails closed on the
    // vocabulary charset (the reference scheme is not v1.0 syntax).
    {
        WorkflowDefinition glyph =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "glyph v1.0");
        glyph.steps.front().arguments =
            json_or_abort(R"json({"tool":"toolref:delta.lookup","key":"k"})json");
        const auto rejected = extract_workflow_tool_references(glyph, view);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected.error(), 2));
        // Options cannot smuggle reference syntax into a v1.0 definition.
        ToolRefExtractionOptions follow;
        follow.default_mode = ToolReferenceMode::FollowLatest;
        const auto rejected_follow = extract_workflow_tool_references(glyph, view, follow);
        MIRA_CHECK(!rejected_follow.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected_follow.error(), 2));
    }
    return 0;
}

int g1_v1_1_reference_extraction() {
    const std::vector<ExposedToolSpec> view = hand_view();

    // Follow form: entry records FollowLatest and no digest even though the
    // default options ask for pinning: the reference's own mode wins.
    {
        const WorkflowDefinition definition =
            make_v11_definition(kWorkflowAlpha, "extract follow", "toolref:delta.lookup");
        ToolRefExtractionOptions options;
        options.default_mode = ToolReferenceMode::PinnedDigest;
        const auto refs =
            must(extract_workflow_tool_references(definition, view, options), "follow extraction");
        MIRA_CHECK(refs.entries.size() == 1);
        MIRA_CHECK(refs.entries.front().wire_name == "delta.lookup");
        MIRA_CHECK(refs.entries.front().mode == ToolReferenceMode::FollowLatest);
        MIRA_CHECK(!refs.entries.front().pinned_spec_digest.has_value());
    }
    // Pinned form: the entry records the digest the REFERENCE pins, not the
    // view observation (here they deliberately differ).
    {
        const WorkflowDefinition definition = make_v11_definition(
            kWorkflowAlpha, "extract pinned", "toolref:delta.lookup@" + kDigestForeign.to_string());
        const WorkflowToolRefManifest refs = extract_or_abort(definition, view);
        MIRA_CHECK(refs.entries.size() == 1);
        MIRA_CHECK(refs.entries.front().mode == ToolReferenceMode::PinnedDigest);
        MIRA_CHECK(refs.entries.front().pinned_spec_digest.has_value());
        MIRA_CHECK(*refs.entries.front().pinned_spec_digest == kDigestForeign);
        MIRA_CHECK(*refs.entries.front().pinned_spec_digest != kDigestLookup);
    }
    // Mixed definitions: bare names take the options/override mode, explicit
    // references keep their own.
    {
        WorkflowDefinition definition =
            make_v11_definition(kWorkflowAlpha, "mixed extraction", "toolref:delta.lookup");
        definition.steps.push_back(toolcall_step(
            kStepSecond, json_or_abort(R"json({"tool":"other.tool","payload":"p"})json")));
        MIRA_CHECK(validate_workflow_definition(definition).has_value());
        ToolRefExtractionOptions options;
        options.default_mode = ToolReferenceMode::PinnedDigest;
        options.per_tool_modes = {{"other.tool", ToolReferenceMode::FollowLatest}};
        const auto refs =
            must(extract_workflow_tool_references(definition, view, options), "mixed extraction");
        MIRA_CHECK(refs.entries.size() == 2);
        // Sorted by step id: b1 (explicit follow) then b2 (bare + override).
        MIRA_CHECK(refs.entries.front().step_id == fixed_step(kStepLookup).to_string());
        MIRA_CHECK(refs.entries.front().mode == ToolReferenceMode::FollowLatest);
        MIRA_CHECK(refs.entries.back().step_id == fixed_step(kStepSecond).to_string());
        MIRA_CHECK(refs.entries.back().wire_name == "other.tool");
        MIRA_CHECK(refs.entries.back().mode == ToolReferenceMode::FollowLatest);
        MIRA_CHECK(!refs.entries.back().pinned_spec_digest.has_value());
        // The bare name without an override takes the default mode and the
        // view observation digest.
        ToolRefExtractionOptions pinned_default;
        pinned_default.default_mode = ToolReferenceMode::PinnedDigest;
        const auto pinned_refs =
            must(extract_workflow_tool_references(definition, view, pinned_default), "mixed pin");
        MIRA_CHECK(pinned_refs.entries.back().mode == ToolReferenceMode::PinnedDigest);
        MIRA_CHECK(pinned_refs.entries.back().pinned_spec_digest.has_value());
        MIRA_CHECK(*pinned_refs.entries.back().pinned_spec_digest == kDigestForeign);
    }
    // Invalid references reject the whole group with the reference domain.
    {
        const std::vector<std::string> bad = {
            "toolref:delta.lookup@ABCDEFGH",                // uppercase hex
            "toolref:delta.lookup@" + std::string(63, 'a'), // short digest
            "toolref:delta.lookup@" + std::string(65, 'a'), // long digest
            "toolref:delta.lookup@zzzz",                    // non-hex digest
            "toolref:Delta.Lookup",                         // charset violation
            "toolref:",                                     // empty name
        };
        for (const std::string &member : bad) {
            const WorkflowDefinition definition =
                make_v11_definition(kWorkflowAlpha, "bad reference", member);
            const auto rejected = extract_workflow_tool_references(definition, view);
            MIRA_CHECK(!rejected.has_value());
            MIRA_CHECK(failed_in_reference_domain(rejected.error(), 2));
        }
        // A wrong scheme is not reference syntax at all: v1.1 still fails
        // closed on the vocabulary charset for the bare-name path.
        const WorkflowDefinition wrong_scheme =
            make_v11_definition(kWorkflowAlpha, "wrong scheme", "toolver:delta.lookup");
        const auto rejected_scheme = extract_workflow_tool_references(wrong_scheme, view);
        MIRA_CHECK(!rejected_scheme.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected_scheme.error(), 2));
        // A reference naming an absent view entry is unresolvable.
        const WorkflowDefinition absent =
            make_v11_definition(kWorkflowAlpha, "absent reference", "toolref:ghost.tool");
        const auto rejected_absent = extract_workflow_tool_references(absent, view);
        MIRA_CHECK(!rejected_absent.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected_absent.error(), 2));
    }
    // Manifest binding: verify passes for the exact identity pair and fails
    // for any mismatch.
    {
        const WorkflowDefinition definition =
            make_v11_definition(kWorkflowAlpha, "binding extraction", "toolref:delta.lookup");
        const WorkflowToolRefManifest refs = extract_or_abort(definition, view);
        MIRA_CHECK(
            verify_workflow_tool_refs(refs, kWorkflowAlpha, workflow_definition_digest(definition))
                .has_value());
        MIRA_CHECK(
            !verify_workflow_tool_refs(refs, kWorkflowBeta, workflow_definition_digest(definition))
                 .has_value());
        MIRA_CHECK(
            !verify_workflow_tool_refs(refs, kWorkflowAlpha, kForeignDefinitionDigest).has_value());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G2: mount table, publish-gate auto extraction and create_run admission
// (M7-TR2-G2)
// ---------------------------------------------------------------------------

int g2_attach_negative_matrix() {
    const std::vector<ExposedToolSpec> view = hand_view();

    // 1. Workflow never published -> NotFound; readback stays empty.
    {
        WorkflowFixture fixture;
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition gamma =
            make_lookup_definition(kWorkflowGamma, SchemaVersion{1, 0}, "attach gamma");
        const auto refs = extract_or_abort(gamma, view);
        const auto rejected = workflow->attach_workflow_tool_refs(refs);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(wiring_failure(rejected.error(), ErrorCode::NotFound));
        MIRA_CHECK(
            !workflow->workflow_tool_refs(kWorkflowGamma, refs.definition_digest).has_value());
    }
    // 2. Manifest digest resolves to no library version -> NotFound.
    {
        WorkflowFixture fixture;
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition beta =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "attach beta");
        must(workflow->publish_workflow(beta, "fixture", "attach publish"), "publish beta");
        const WorkflowDefinition other =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "attach beta other");
        const auto refs = extract_or_abort(other, view);
        MIRA_CHECK(workflow_definition_digest(other) != workflow_definition_digest(beta));
        const auto rejected = workflow->attach_workflow_tool_refs(refs);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::NotFound);
        MIRA_CHECK(
            !workflow->workflow_tool_refs(kWorkflowBeta, refs.definition_digest).has_value());
    }
    // 3. Manifest claims another workflow id than the pinned definition
    // content: fail closed (the contract text pins InvalidArgument for the
    // mismatch branch; through the public surface the version cannot resolve
    // in the claimed history, so the rejection surfaces as NotFound — either
    // way the mount is refused and the state is unchanged).
    {
        WorkflowFixture fixture;
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition beta =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "attach beta");
        must(workflow->publish_workflow(beta, "fixture", "attach publish"), "publish beta");
        const WorkflowDefinition alpha =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "attach alpha");
        auto refs = extract_or_abort(alpha, view);
        refs.workflow_id = kWorkflowBeta; // lie about the workflow identity
        const auto rejected = workflow->attach_workflow_tool_refs(refs);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::NotFound ||
                   rejected.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(
            !workflow->workflow_tool_refs(kWorkflowBeta, refs.definition_digest).has_value());
        MIRA_CHECK(
            !workflow->workflow_tool_refs(kWorkflowAlpha, refs.definition_digest).has_value());
    }
    // 4. Same key, different manifest -> InvalidState; readback unchanged;
    // identical re-mount is an idempotent NoOp.
    {
        WorkflowFixture fixture;
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition beta =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "attach beta");
        must(workflow->publish_workflow(beta, "fixture", "attach publish"), "publish beta");
        const auto pinned = extract_or_abort(beta, view);
        must(workflow->attach_workflow_tool_refs(pinned), "attach pinned");
        ToolRefExtractionOptions follow;
        follow.default_mode = ToolReferenceMode::FollowLatest;
        const auto conflicting =
            must(extract_workflow_tool_references(beta, view, follow), "conflicting refs");
        MIRA_CHECK(conflicting.digest != pinned.digest);
        const auto rejected = workflow->attach_workflow_tool_refs(conflicting);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(wiring_failure(rejected.error(), ErrorCode::InvalidState));
        const auto readback =
            must(workflow->workflow_tool_refs(kWorkflowBeta, pinned.definition_digest), "readback");
        MIRA_CHECK(manifests_equal(readback, pinned));
        // Idempotent re-mount.
        must(workflow->attach_workflow_tool_refs(pinned), "re-attach");
        MIRA_CHECK(workflow->attach_workflow_tool_refs(conflicting).error().code ==
                   ErrorCode::InvalidState);
    }
    // 5. Capacity: a tight mount table rejects further mounts with
    // ResourceExhausted (retryable); the idempotent replay still passes and
    // the key check precedes the capacity check.
    {
        WorkflowFixture fixture;
        WorkflowRuntimeConfig config;
        config.max_mounted_tool_refs = 1;
        auto workflow = fixture.make_workflow(config);
        const WorkflowDefinition beta =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "attach beta");
        const WorkflowDefinition alpha =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "attach alpha capped");
        must(workflow->publish_workflow(beta, "fixture", "attach publish"), "publish beta");
        must(workflow->publish_workflow(alpha, "fixture", "attach publish"), "publish alpha");
        const auto first = extract_or_abort(beta, view);
        must(workflow->attach_workflow_tool_refs(first), "mount first");
        const auto second = extract_or_abort(alpha, view);
        const auto rejected = workflow->attach_workflow_tool_refs(second);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(wiring_failure(rejected.error(), ErrorCode::ResourceExhausted));
        MIRA_CHECK(rejected.error().retryable);
        MIRA_CHECK(
            !workflow->workflow_tool_refs(kWorkflowAlpha, second.definition_digest).has_value());
        // Idempotent replay at capacity is still a NoOp, and the conflicting
        // manifest for the mounted key still rejects with InvalidState (the
        // key check precedes the capacity check).
        must(workflow->attach_workflow_tool_refs(first), "replay at capacity");
        ToolRefExtractionOptions follow;
        follow.default_mode = ToolReferenceMode::FollowLatest;
        const auto conflicting =
            must(extract_workflow_tool_references(beta, view, follow), "conflicting capped");
        MIRA_CHECK(workflow->attach_workflow_tool_refs(conflicting).error().code ==
                   ErrorCode::InvalidState);
    }
    return 0;
}

int g2_publish_gate_auto_mount() {
    const WorkflowDefinition alpha =
        make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "auto mount");

    // 1. Registry installed: publish_validated extracts with the default
    // options and mounts; the manifest reads back bound to the version.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        const auto published =
            must(workflow->publish_validated(alpha, "fixture", "auto mount"), "publish");
        const auto mounted = must(workflow->workflow_tool_refs(kWorkflowAlpha, published.ir_digest),
                                  "mount readback");
        MIRA_CHECK(mounted.workflow_id == kWorkflowAlpha);
        MIRA_CHECK(mounted.definition_digest == published.ir_digest);
        MIRA_CHECK(mounted.entries.size() == 1);
        MIRA_CHECK(mounted.entries.front().mode == ToolReferenceMode::PinnedDigest);
        // Re-publish is an idempotent NoOp and the mount survives.
        const auto replay = must(workflow->publish_validated(alpha, "fixture", "auto mount replay"),
                                 "replay publish");
        MIRA_CHECK(replay.idempotent);
        const auto after = must(workflow->workflow_tool_refs(kWorkflowAlpha, replay.ir_digest),
                                "mount after replay");
        MIRA_CHECK(manifests_equal(after, mounted));
    }
    // 2. Registry installed but the definition references a view-external
    // tool: publish rejects with the tool-refs-unresolvable audit event and
    // the library is unchanged.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        WorkflowDefinition ghost =
            make_lookup_definition(kWorkflowEta, SchemaVersion{1, 0}, "ghost reference");
        ghost.steps.front().arguments = json_or_abort(R"json({"tool":"ghost.tool","key":"k"})json");
        const auto rejected = workflow->publish_validated(ghost, "fixture", "ghost publish");
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected.error(), 2));
        const auto events =
            typed_events(*fixture.events_, fixture.session_id_, "WorkflowPublishRejected");
        MIRA_CHECK(events.size() == 1);
        const auto *reason = events.front().payload.find("reason_code");
        MIRA_CHECK(reason != nullptr && reason->is_string() &&
                   *reason->as_string() == "tool-refs-unresolvable");
        // Library zero change: no mount, no version record.
        const Sha256Digest ghost_digest = workflow_definition_digest(ghost);
        MIRA_CHECK(!workflow->workflow_tool_refs(kWorkflowEta, ghost_digest).has_value());
        const auto unresolved = workflow->create_run(kWorkflowEta, ghost_digest,
                                                     JsonValue{JsonValue::Object{}}, std::nullopt);
        MIRA_CHECK(!unresolved.has_value());
        MIRA_CHECK(unresolved.error().code == ErrorCode::NotFound);
    }
    // 3. Registry not installed: publish keeps its pre-TR2 behavior and does
    // not mount anything.
    {
        WorkflowFixture fixture;
        auto workflow = std::make_unique<WorkflowRuntime>(
            fixture.executor_, *fixture.runtime_, fixture.session_id_, fixture.environment_);
        workflow->set_event_store(fixture.events_);
        const auto published = must(workflow->publish_validated(alpha, "fixture", "no registry"),
                                    "publish no registry");
        MIRA_CHECK(!workflow->workflow_tool_refs(kWorkflowAlpha, published.ir_digest).has_value());
        MIRA_CHECK(workflow->workflow_tool_refs(kWorkflowAlpha, published.ir_digest).error().code ==
                   ErrorCode::NotFound);
        const auto applied =
            typed_events(*fixture.events_, fixture.session_id_, "WorkflowPublishApplied");
        MIRA_CHECK(applied.size() == 1);
        static_cast<void>(workflow->shutdown());
    }
    return 0;
}

int g2_admission_invalid_and_dryrun() {
    // Mounted manifest + evolved registry (new required argument): the
    // skeleton no longer binds, so the Strict admission rejects with the
    // deterministic message prefix while DryRun keeps its pre-TR2 behavior.
    const std::string summary = "invalid admission";
    // Strict path.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, summary);
        auto evolved = std::make_shared<FixtureTool>();
        evolved->description = "wiring lookup tool v3";
        evolved->schema_json = kEvolvedRequiredSchema;
        scenario.workflow->set_tool_registry(registry_with(*evolved));
        const auto rejected = scenario.workflow->create_run(
            scenario.definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(
            rejected.error().safe_message.rfind("tool compatibility admission rejected: ", 0) == 0);
        MIRA_CHECK(typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                "WorkflowToolCompatDegraded")
                       .empty());
    }
    // DryRun path: the gate does not engage, the run is admitted without an
    // event even though the mounted projection is Invalid.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, summary);
        auto evolved = std::make_shared<FixtureTool>();
        evolved->description = "wiring lookup tool v3";
        evolved->schema_json = kEvolvedRequiredSchema;
        scenario.workflow->set_tool_registry(registry_with(*evolved));
        const auto created = scenario.workflow->create_run(
            scenario.definition, json_or_abort(R"json({"key":"k"})json"), WorkflowPolicy::DryRun);
        MIRA_CHECK(created.has_value());
        MIRA_CHECK(typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                "WorkflowToolCompatDegraded")
                       .empty());
    }
    return 0;
}

int g2_admission_degraded_event() {
    MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, "degraded admission");
    // Evolve the registry: different spec digest (description), the recorded
    // skeleton still binds -> Degraded admission with exactly one audit event.
    auto evolved = std::make_shared<FixtureTool>();
    evolved->description = "wiring lookup tool v2";
    auto evolved_registry = registry_with(*evolved);
    scenario.workflow->set_tool_registry(evolved_registry);

    const auto created = scenario.workflow->create_run(
        scenario.definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto driven = must(
        scenario.workflow->execute_run(created.value().run_id, plain_context()), "degraded drive");
    MIRA_CHECK(driven.state == WorkflowRunState::Completed);
    MIRA_CHECK(evolved->dispatches.load() == 1);

    const auto events = typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                     "WorkflowToolCompatDegraded");
    MIRA_CHECK(events.size() == 1);
    MIRA_CHECK(events.front().classification == EventClass::State);
    // Closed payload field set and strict parse.
    {
        bool saw_extra = false;
        const auto &payload = events.front().payload;
        MIRA_CHECK(payload.is_object());
        for (const auto &member : *payload.as_object()) {
            const bool known = member.first == "schema" || member.first == "run_id" ||
                               member.first == "workflow_id" || member.first == "ir_digest" ||
                               member.first == "projection";
            if (!known) {
                saw_extra = true;
            }
        }
        MIRA_CHECK(!saw_extra);
        EventPayload wire;
        wire.type = events.front().type;
        wire.data = to_json_string(events.front().payload);
        const auto parsed = must(parse_workflow_tool_compat_degraded(wire), "degraded parse");
        MIRA_CHECK(parsed.run_id == created.value().run_id);
        MIRA_CHECK(parsed.workflow_id == kWorkflowAlpha);
        MIRA_CHECK(parsed.ir_digest == scenario.digest);
        // The embedded projection is a strictly valid tool-compat artifact and
        // matches a fresh recomputation over the same inputs.
        const auto projection =
            must(workflow_tool_compat_from_json(parsed.projection), "embedded projection");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);
        const auto fresh =
            must(project_workflow_tool_compatibility(scenario.definition, scenario.mounted,
                                                     evolved_registry->exposed_tools()),
                 "fresh projection");
        MIRA_CHECK(fresh.digest == projection.digest);
        MIRA_CHECK(canonical_json_string(parsed.projection) ==
                   canonical_json_string(workflow_tool_compat_to_json(fresh)));
        // Sanitized: no schema bodies, no tool descriptions.
        const std::string serialized = to_json_string(events.front().payload);
        for (const char *marker : {"properties", "description", "wiring lookup tool", "$param"}) {
            MIRA_CHECK(serialized.find(marker) == std::string::npos);
        }
    }
    // A second run under the same inputs is admitted with exactly one more
    // event carrying the same projection digest (determinism; see G3).
    const auto second = scenario.workflow->create_run(
        scenario.definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
    MIRA_CHECK(second.has_value());
    const auto events_after = typed_events(
        *scenario.fixture->events_, scenario.fixture->session_id_, "WorkflowToolCompatDegraded");
    MIRA_CHECK(events_after.size() == 2);
    return 0;
}

int g2_admission_runnable_and_zero_drift() {
    const JsonValue parameters = json_or_abort(R"json({"key":"k"})json");

    // Runnable: mounted manifest pinned to the current view admits without an
    // event.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, "runnable admission");
        const auto created =
            scenario.workflow->create_run(scenario.definition, parameters, std::nullopt);
        MIRA_CHECK(created.has_value());
        MIRA_CHECK(typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                "WorkflowToolCompatDegraded")
                       .empty());
        const auto driven =
            must(scenario.workflow->execute_run(created.value().run_id, plain_context()),
                 "runnable drive");
        MIRA_CHECK(driven.state == WorkflowRunState::Completed);
    }
    // Unmounted v1.0: zero drift — the gate does not engage and the run
    // behaves exactly as before TR2.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition v10 =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "zero drift");
        const auto created =
            must(workflow->create_run(v10, parameters, std::nullopt), "drift create");
        const auto driven =
            must(workflow->execute_run(created.run_id, plain_context()), "drift drive");
        MIRA_CHECK(driven.state == WorkflowRunState::Completed);
        MIRA_CHECK(typed_events(*fixture.events_, fixture.session_id_, "WorkflowToolCompatDegraded")
                       .empty());
        const auto types = session_event_types(*fixture.events_, fixture.session_id_);
        MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowRunSettled") != types.end());
    }
    // Unmounted v1.1: the reference form resolves on the fly (follow) and the
    // execution path only ever sees the bare wire name.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        const WorkflowDefinition v11 =
            make_v11_definition(kWorkflowAlpha, "bare wire execution", "toolref:delta.lookup");
        const auto created =
            must(workflow->create_run(v11, parameters, std::nullopt), "v11 create");
        const auto driven =
            must(workflow->execute_run(created.run_id, plain_context()), "v11 drive");
        MIRA_CHECK(driven.state == WorkflowRunState::Completed);
        MIRA_CHECK(driven.steps.size() == 1);
        MIRA_CHECK(driven.steps.front().disposition == WorkflowStepDisposition::Completed);
        MIRA_CHECK(tool.dispatches.load() == 1);
        // No toolref glyph anywhere in the run's step records or event store.
        for (const auto &record : driven.steps) {
            MIRA_CHECK(record.safe_summary.find("toolref") == std::string::npos);
        }
        EventQuery query;
        query.session_id = fixture.session_id_;
        const auto page = must(fixture.events_->read(query), "event read");
        for (const auto &envelope : page.events) {
            MIRA_CHECK(envelope.payload.data.find("toolref") == std::string::npos);
        }
        MIRA_CHECK(typed_events(*fixture.events_, fixture.session_id_, "WorkflowToolCompatDegraded")
                       .empty());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G3: pin evolution matrix and determinism (M7-TR2-G3)
// ---------------------------------------------------------------------------

int g3_pin_evolution_projection_matrix() {
    const std::vector<ExposedToolSpec> view = hand_view();
    const WorkflowDefinition definition =
        make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "projection matrix");
    const WorkflowToolRefManifest pinned = extract_or_abort(definition, view);
    ToolRefExtractionOptions follow_options;
    follow_options.default_mode = ToolReferenceMode::FollowLatest;
    const WorkflowToolRefManifest follow =
        must(extract_workflow_tool_references(definition, view, follow_options), "follow manifest");

    const auto project = [&](const WorkflowToolRefManifest &refs,
                             std::span<const ExposedToolSpec> current) {
        return project_workflow_tool_compatibility(definition, refs, current);
    };

    // Pin equal -> Runnable, admitted without a reason.
    {
        const auto projection = must(project(pinned, view), "pin equal");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
        const auto decision = admit_workflow_run_by_tool_compat(projection);
        MIRA_CHECK(decision.admitted);
        MIRA_CHECK(decision.reason.empty());
    }
    // Pin mismatch with a bindable skeleton -> Degraded, admitted.
    {
        std::vector<ExposedToolSpec> evolved = view;
        evolved.front() = hand_spec("delta.lookup", "wiring lookup tool v2",
                                    digest_string("m7-tr2/fixture/lookup-spec/v2"),
                                    fixed_tool("000000000000000000000000000000e1"));
        const auto projection = must(project(pinned, evolved), "pin mismatch bindable");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);
        MIRA_CHECK(projection.entries.size() == 1);
        MIRA_CHECK(projection.entries.front().status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(admit_workflow_run_by_tool_compat(projection).admitted);
    }
    // Pin mismatch the skeleton cannot satisfy (new required member) ->
    // Invalid, rejected with a reason naming the entry.
    {
        std::vector<ExposedToolSpec> required = view;
        required.front() = hand_spec(
            "delta.lookup", "wiring lookup tool v3", digest_string("m7-tr2/fixture/lookup-spec/v3"),
            fixed_tool("000000000000000000000000000000e1"), kEvolvedRequiredSchema);
        const auto projection = must(project(pinned, required), "pin mismatch unbindable");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Invalid);
        MIRA_CHECK(projection.entries.front().status == ToolReferenceCompat::EvolvedIncompatible);
        const auto decision = admit_workflow_run_by_tool_compat(projection);
        MIRA_CHECK(!decision.admitted);
        MIRA_CHECK(!decision.reason.empty());
    }
    // Wire name vanished -> Invalid (Unresolved), rejected.
    {
        const std::vector<ExposedToolSpec> vanished = {view.back()};
        const auto projection = must(project(pinned, vanished), "wire vanished");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Invalid);
        MIRA_CHECK(projection.entries.front().status == ToolReferenceCompat::Unresolved);
        MIRA_CHECK(!admit_workflow_run_by_tool_compat(projection).admitted);
    }
    // Follow -> Resolved against any view that still carries the wire name.
    {
        std::vector<ExposedToolSpec> evolved = view;
        evolved.front() = hand_spec("delta.lookup", "wiring lookup tool v2",
                                    digest_string("m7-tr2/fixture/lookup-spec/v2"),
                                    fixed_tool("000000000000000000000000000000e1"));
        const auto projection = must(project(follow, evolved), "follow resolved");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
        MIRA_CHECK(projection.entries.front().status == ToolReferenceCompat::Resolved);
    }
    // Determinism: same inputs, same projection and digest; the artifact
    // survives its strict inverse.
    {
        const auto first = must(project(pinned, view), "determinism one");
        const auto second = must(project(pinned, view), "determinism two");
        MIRA_CHECK(first.entries == second.entries);
        MIRA_CHECK(first.digest == second.digest);
        const auto decoded = must(
            workflow_tool_compat_from_json(workflow_tool_compat_to_json(first)), "compat inverse");
        MIRA_CHECK(projections_equal(decoded, first));
    }
    return 0;
}

int g3_runtime_pin_follow_and_evolution() {
    // Mounted pinned manifest + vanished wire name: the admission gate rejects
    // before any per-step NotFound could leak.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, "vanished mount");
        auto vanished_registry = std::make_shared<BuiltinToolRegistry>();
        auto other = std::make_shared<FixtureTool>();
        other->wire_name = "other.tool";
        auto registration = other->registration();
        must(vanished_registry->register_tool(std::move(registration.spec),
                                              std::move(registration.handler)),
             "register other tool");
        scenario.workflow->set_tool_registry(vanished_registry);
        const auto rejected = scenario.workflow->create_run(
            scenario.definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(
            rejected.error().safe_message.rfind("tool compatibility admission rejected: ", 0) == 0);
    }
    // Mounted follow manifest: the mount survives registry evolution because
    // the follow reference re-resolves; no Degraded event is emitted.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, "follow mount");
        const WorkflowDefinition definition =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "follow mount");
        must(scenario.workflow->publish_workflow(definition, "fixture", "follow mount"),
             "publish follow");
        ToolRefExtractionOptions options;
        options.default_mode = ToolReferenceMode::FollowLatest;
        const auto follow_refs =
            must(extract_workflow_tool_references(definition, hand_view(), options), "follow refs");
        must(scenario.workflow->attach_workflow_tool_refs(follow_refs), "mount follow");
        const auto readback = must(scenario.workflow->workflow_tool_refs(
                                       kWorkflowBeta, workflow_definition_digest(definition)),
                                   "follow readback");
        MIRA_CHECK(readback.entries.front().mode == ToolReferenceMode::FollowLatest);
        // Evolve the view: the follow reference still resolves.
        auto evolved = std::make_shared<FixtureTool>();
        evolved->description = "wiring lookup tool v2";
        scenario.workflow->set_tool_registry(registry_with(*evolved));
        const auto created = scenario.workflow->create_run(
            definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
        MIRA_CHECK(created.has_value());
        MIRA_CHECK(typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                "WorkflowToolCompatDegraded")
                       .empty());
    }
    // Degraded determinism: two runs over identical inputs produce events
    // carrying the same projection digest.
    {
        MountedScenario scenario = mount_lookup_workflow(kWorkflowAlpha, "degraded determinism");
        auto evolved = std::make_shared<FixtureTool>();
        evolved->description = "wiring lookup tool v2";
        scenario.workflow->set_tool_registry(registry_with(*evolved));
        for (int index = 0; index < 2; ++index) {
            const auto created = scenario.workflow->create_run(
                scenario.definition, json_or_abort(R"json({"key":"k"})json"), std::nullopt);
            MIRA_CHECK(created.has_value());
        }
        const auto events = typed_events(*scenario.fixture->events_, scenario.fixture->session_id_,
                                         "WorkflowToolCompatDegraded");
        MIRA_CHECK(events.size() == 2);
        EventPayload first_payload;
        first_payload.type = events.front().type;
        first_payload.data = to_json_string(events.front().payload);
        EventPayload second_payload;
        second_payload.type = events.back().type;
        second_payload.data = to_json_string(events.back().payload);
        const auto first = must(parse_workflow_tool_compat_degraded(first_payload), "parse one");
        const auto second = must(parse_workflow_tool_compat_degraded(second_payload), "parse two");
        const auto first_projection =
            must(workflow_tool_compat_from_json(first.projection), "projection one");
        const auto second_projection =
            must(workflow_tool_compat_from_json(second.projection), "projection two");
        MIRA_CHECK(first_projection.digest == second_projection.digest);
        MIRA_CHECK(first_projection.entries == second_projection.entries);
    }
    return 0;
}

int g3_report_deterministic_in_process() {
    const std::string first = build_wiring_report();
    const std::string second = build_wiring_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// G4: skill execution over the Tool channel (M7-TR2-G4)
// ---------------------------------------------------------------------------

// Publishes one skill whose source workflow runs through the runtime library
// path and returns the pinned scenario inputs.
struct SkillSetup final {
    WorkflowDefinition definition;
    SkillDescriptor descriptor;
    WorkflowToolRefManifest refs;
    WorkflowVersionHistory history;
};

SkillSetup publish_skill_source(WorkflowRuntime &workflow, std::span<const ExposedToolSpec> view,
                                SkillPublicationRegistry &skills, const WorkflowId &workflow_id,
                                const std::string &skill_name, const std::string &summary,
                                const std::string &tool_member, SemanticVersion version) {
    SkillSetup setup;
    setup.definition =
        wiring_definition(workflow_id, SchemaVersion{1, 0}, "fixture.skill_source", summary,
                          {key_param()}, {lookup_step(kStepLookup, tool_member)});
    const auto published =
        must(workflow.publish_validated(setup.definition, "fixture", "skill source"),
             "skill source publish");
    setup.refs =
        must(workflow.workflow_tool_refs(workflow_id, published.ir_digest), "skill source refs");
    setup.history = single_version_history(workflow_id, setup.definition,
                                           WorkflowValidationResult::DryRunPassed);
    setup.descriptor =
        must(make_skill_descriptor(setup.definition, setup.refs, view, skill_name, version),
             "skill descriptor");
    must(skills.publish_skill(setup.descriptor, setup.definition, setup.refs, view, setup.history),
         "skill publish");
    return setup;
}

// Registers every not-yet-registered Published skill tool into the registry
// (skill_tool_registrations re-lists the whole publication set each call).
void register_skill_tools(BuiltinToolRegistry &registry, WorkflowRuntime &workflow,
                          SkillPublicationRegistry &skills) {
    std::vector<std::string> known;
    for (const auto &entry : registry.exposed_tools()) {
        known.push_back(entry.wire_name);
    }
    for (auto registration : workflow.skill_tool_registrations(skills)) {
        if (std::find(known.begin(), known.end(), registration.spec.wire_name) != known.end()) {
            continue;
        }
        must(registry.register_tool(std::move(registration.spec), std::move(registration.handler)),
             "register skill tool");
    }
}

int g4_registration_surface_and_identity_gate() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto workflow = fixture.make_workflow();
    SkillPublicationRegistry skills;
    const SkillSetup setup =
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});

    const auto registrations = workflow->skill_tool_registrations(skills);
    MIRA_CHECK(registrations.size() == 1);
    const BuiltinToolRegistration &registration = registrations.front();
    // The spec carries the descriptor surface field by field.
    MIRA_CHECK(registration.spec.wire_name == setup.descriptor.name);
    MIRA_CHECK(registration.spec.version == setup.descriptor.version);
    MIRA_CHECK(registration.spec.description == setup.descriptor.surface.description);
    MIRA_CHECK(registration.spec.parameters_schema.root ==
               setup.descriptor.surface.parameters_schema.root);
    MIRA_CHECK(registration.spec.has_side_effects == setup.descriptor.surface.has_side_effects);
    // A side-effecting source surface maps as well (registration only; the
    // source workflow must carry the W-02 verification predicate).
    {
        FixtureTool render_tool;
        render_tool.tool_hex = "000000000000000000000000000000e2";
        render_tool.wire_name = "delta.render";
        render_tool.description = "wiring render tool";
        auto render_registration = render_tool.registration();
        render_registration.spec.has_side_effects = true;
        must(fixture.registry_->register_tool(std::move(render_registration.spec),
                                              std::move(render_registration.handler)),
             "register render tool");
        SkillPublicationRegistry render_skills;
        WorkflowDefinition render_definition =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "render skill source");
        render_definition.steps.front().arguments =
            json_or_abort(R"json({"tool":"delta.render","key":{"$param":"key"}})json");
        render_definition.steps.front().verification = step_result_predicate(
            fixed_step(kStepLookup), WorkflowPredicateOp::Eq, JsonValue{std::string("sent")});
        const auto published =
            must(workflow->publish_validated(render_definition, "fixture", "render source"),
                 "render publish");
        const auto render_view = fixture.registry_->exposed_tools();
        const auto render_refs =
            must(workflow->workflow_tool_refs(kWorkflowBeta, published.ir_digest), "render refs");
        const auto render_descriptor =
            must(make_skill_descriptor(render_definition, render_refs, render_view,
                                       "fixture.render_skill", {1, 0, 0}),
                 "render descriptor");
        MIRA_CHECK(render_descriptor.surface.has_side_effects);
        const auto render_history = single_version_history(kWorkflowBeta, render_definition,
                                                           WorkflowValidationResult::DryRunPassed);
        must(render_skills.publish_skill(render_descriptor, render_definition, render_refs,
                                         render_view, render_history),
             "render skill publish");
        const auto render_registrations = workflow->skill_tool_registrations(render_skills);
        MIRA_CHECK(render_registrations.size() == 1);
        MIRA_CHECK(render_registrations.front().spec.has_side_effects);
        MIRA_CHECK(render_registrations.front().spec.wire_name == "fixture.render_skill");
    }
    // Registration passes the DEC-015 identity gate: only a proposal built
    // from the exposed spec executes.
    {
        auto owned = registrations.front();
        must(fixture.registry_->register_tool(std::move(owned.spec), std::move(owned.handler)),
             "register skill");
    }
    const auto exposed = fixture.registry_->exposed_tools();
    const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
    MIRA_CHECK(skill_spec != nullptr);
    {
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"), "call-ok", context),
            context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(!outcome.value().failed);
        const auto *state = outcome.value().result.find("state");
        MIRA_CHECK(state != nullptr && state->is_string() && *state->as_string() == "completed");
        MIRA_CHECK(tool.dispatches.load() == 1);
    }
    // A stale identity is rejected exactly as any other registry entry.
    {
        const OperationContext context = plain_context();
        ToolProposal stale = proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"),
                                          "call-stale", context);
        stale.tool_id = fixed_tool("000000000000000000000000000000f1");
        const auto outcome = fixture.registry_->execute(stale, context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::NotFound);
    }
    return 0;
}

int g4_skill_execution_outcomes() {
    const JsonValue arguments = json_or_abort(R"json({"key":"k"})json");

    // Completed, cancelled and revoked outcomes over one healthy fixture.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        MIRA_CHECK(skill_spec != nullptr);

        // Child run Completed -> structured JSON result.
        {
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-completed", context), context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
            const auto *run_id = outcome.value().result.find("run_id");
            MIRA_CHECK(run_id != nullptr && run_id->is_string());
            MIRA_CHECK(WorkflowRunId::parse(*run_id->as_string()).has_value());
            const auto *state = outcome.value().result.find("state");
            MIRA_CHECK(state != nullptr && state->is_string() &&
                       *state->as_string() == "completed");
            const auto *safe = outcome.value().result.find("safe_summary");
            MIRA_CHECK(safe != nullptr && safe->is_string() && !safe->as_string()->empty());
            MIRA_CHECK(outcome.value().result.is_object() &&
                       outcome.value().result.as_object()->size() == 3);
        }
        // A withdrawn caller never starts a child run: Cancelled.
        {
            OperationContext context = plain_context();
            context.cancellation_requested = [] { return true; };
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-cancelled", context), context);
            MIRA_CHECK(!outcome.has_value());
            MIRA_CHECK(outcome.error().code == ErrorCode::Cancelled);
        }
        // Revoked publication -> NotFound failed record.
        {
            must(skills.revoke_skill("fixture.skill", "outcome revoke"), "revoke");
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-revoked", context), context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(outcome.value().safe_error_summary.find("not published") !=
                       std::string::npos);
        }
    }
    // Descriptor drift on a healthy publication (upgrade without
    // re-registering) -> InvalidState failed record.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        const SkillSetup v1 = publish_skill_source(
            *workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha, "fixture.skill",
            "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        // Upgrade the publication without re-registering the tool.
        const WorkflowDefinition v2 =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "wiring skill source v2");
        const auto v2_view = fixture.registry_->exposed_tools();
        const auto v2_refs = extract_or_abort(v2, v2_view);
        const auto v2_descriptor =
            must(make_skill_descriptor(v2, v2_refs, v2_view, "fixture.skill", {2, 0, 0}),
                 "descriptor v2");
        const auto both = two_version_history(v1.definition, v2);
        must(skills.upgrade_skill("fixture.skill", v2_descriptor, v2, v2_refs, v2_view, both),
             "upgrade skill");
        MIRA_CHECK(skills.find("fixture.skill")->descriptor.digest != v1.descriptor.digest);

        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, arguments, "call-drift", context), context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().failed);
        MIRA_CHECK(outcome.value().safe_error_summary.find("drifted") != std::string::npos);
    }
    // Child run Failed (internal tool failure) -> InvalidState failed record.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        tool.failures_first = 1;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, arguments, "call-failed", context), context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().failed);
        MIRA_CHECK(outcome.value().safe_error_summary.find("failed") != std::string::npos);
        MIRA_CHECK(tool.dispatches.load() == 1);
    }
    return 0;
}

// Publishes one link of the skill chain: its source workflow calls the next
// skill by wire name (or the plain tool at the bottom of the chain).
void publish_chain_link(WorkflowFixture &fixture, WorkflowRuntime &workflow,
                        SkillPublicationRegistry &skills, const WorkflowId &workflow_id,
                        const std::string &skill_name, const std::string &summary,
                        const std::string &tool_member) {
    SkillSetup setup;
    setup.definition =
        wiring_definition(workflow_id, SchemaVersion{1, 0}, "fixture.chain_link", summary,
                          {key_param()}, {lookup_step(kStepLookup, tool_member)});
    const auto published = must(
        workflow.publish_validated(setup.definition, "fixture", "chain link"), "chain publish");
    const auto view = fixture.registry_->exposed_tools();
    setup.refs = must(workflow.workflow_tool_refs(workflow_id, published.ir_digest), "chain refs");
    setup.history = single_version_history(workflow_id, setup.definition,
                                           WorkflowValidationResult::DryRunPassed);
    setup.descriptor =
        must(make_skill_descriptor(setup.definition, setup.refs, view, skill_name, {1, 0, 0}),
             "chain descriptor");
    must(skills.publish_skill(setup.descriptor, setup.definition, setup.refs, view, setup.history),
         "chain skill publish");
    register_skill_tools(*fixture.registry_, workflow, skills);
}

int g4_skill_depth_matrix() {
    // Default bound (max_skill_call_depth = 2): the depth-1 direct call and
    // the depth-2 nested call run; the depth-3 chain is refused.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_chain_link(fixture, *workflow, skills, kWorkflowDelta, "fixture.skill.innermost",
                           "chain innermost", "delta.lookup");
        publish_chain_link(fixture, *workflow, skills, kWorkflowEpsilon, "fixture.skill.inner",
                           "chain inner", "fixture.skill.innermost");
        publish_chain_link(fixture, *workflow, skills, kWorkflowZeta, "fixture.skill.outer",
                           "chain outer", "fixture.skill.inner");
        MIRA_CHECK(fixture.registry_->size() == 4); // lookup + three skills
        const auto exposed = fixture.registry_->exposed_tools();

        for (const char *wire : {"fixture.skill.innermost", "fixture.skill.inner"}) {
            const ExposedToolSpec *spec = find_exposed(exposed, wire);
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"),
                             std::string("call-") + wire, context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
        }
        // The depth-3 chain fails closed.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.outer");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth3",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(!outcome.value().safe_error_summary.empty());
        }
        MIRA_CHECK(tool.dispatches.load() >= 2);
    }
    // Tightened bound (max_skill_call_depth = 1): a single skill still runs,
    // any nested skill call is refused with the depth reason.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        WorkflowRuntimeConfig config;
        config.max_skill_call_depth = 1;
        auto workflow = fixture.make_workflow(config);
        SkillPublicationRegistry skills;
        publish_chain_link(fixture, *workflow, skills, kWorkflowEpsilon, "fixture.skill.inner",
                           "tight inner", "delta.lookup");
        publish_chain_link(fixture, *workflow, skills, kWorkflowZeta, "fixture.skill.outer",
                           "tight outer", "fixture.skill.inner");
        const auto exposed = fixture.registry_->exposed_tools();
        // The single-level call succeeds.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.inner");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth1",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
        }
        // The nested call is refused with the configured bound.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.outer");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth2",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(outcome.value().safe_error_summary.find("depth exceeds") !=
                       std::string::npos);
        }
    }
    return 0;
}

int g4_skill_child_passes_compat_gate() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto workflow = fixture.make_workflow();
    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.skill", "gated skill source", "delta.lookup", {1, 0, 0});
    // Build the evolved registry holding both the evolved lookup tool and the
    // already-registered skill tool, so the skill stays dispatchable while its
    // source workflow's mounted manifest turns Invalid.
    auto skill_regs = workflow->skill_tool_registrations(skills);
    MIRA_CHECK(skill_regs.size() == 1);
    auto evolved_registry = std::make_shared<BuiltinToolRegistry>();
    auto evolved = std::make_shared<FixtureTool>();
    evolved->description = "wiring lookup tool v3";
    evolved->schema_json = kEvolvedRequiredSchema;
    auto evolved_registration = evolved->registration();
    must(evolved_registry->register_tool(std::move(evolved_registration.spec),
                                         std::move(evolved_registration.handler)),
         "register evolved lookup");
    must(evolved_registry->register_tool(std::move(skill_regs.front().spec),
                                         std::move(skill_regs.front().handler)),
         "re-register skill tool");
    workflow->set_tool_registry(evolved_registry);

    const auto exposed = evolved_registry->exposed_tools();
    const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
    MIRA_CHECK(skill_spec != nullptr);
    const OperationContext context = plain_context();
    const auto outcome = evolved_registry->execute(
        proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"), "call-gated", context),
        context);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().failed);
    MIRA_CHECK(outcome.value().safe_error_summary.find("tool compatibility admission rejected") !=
               std::string::npos);
    MIRA_CHECK(evolved->dispatches.load() == 0);
    MIRA_CHECK(tool.dispatches.load() == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// G5: Procedure memory wiring (M7-TR2-G5)
// ---------------------------------------------------------------------------

int g5_procedure_sync_wiring() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
    auto workflow = fixture.make_workflow();
    // The learning scope is the Agent scope; the User scope is rejected.
    must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
         "learning context");
    {
        MemoryScope user_scope;
        user_scope.kind = MemoryScopeKind::User;
        user_scope.subject_id = "mira.test.user";
        const auto rejected = workflow->set_learning_context(memory, user_scope);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
    }

    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.proc_alpha", "procedure sync alpha", "delta.lookup", {1, 0, 0});
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowBeta,
                         "fixture.proc_beta", "procedure sync beta", "delta.lookup", {1, 0, 0});
    const auto index = must(project_skill_procedure_index(skills.publications()), "sync index");
    MIRA_CHECK(index.entries.size() == 2);
    MIRA_CHECK(index.entries.front().status == SkillPublicationStatus::Published);

    // First sync: one applied Add per Published entry.
    const auto first = must(workflow->sync_skill_procedure_index(skills), "first sync");
    MIRA_CHECK(first.applied == 2);
    MIRA_CHECK(first.idempotent == 0);
    MIRA_CHECK(first.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Record shape: deterministic identity, Procedure kind, learning scope,
    // byte-identical statement, timestamped, capped confidence.
    {
        const auto records = memory->snapshot();
        for (const auto &entry : index.entries) {
            const std::string seed = "mira.workflow.skill_procedure|" + entry.name + "|1.0.0|" +
                                     entry.descriptor_digest.to_string();
            const MemoryId expected_id{learning_id_from_seed(seed)};
            const auto found =
                std::find_if(records.begin(), records.end(),
                             [&](const MemoryRecord &record) { return record.id == expected_id; });
            MIRA_CHECK(found != records.end());
            MIRA_CHECK(found->kind == MemoryKind::Procedure);
            MIRA_CHECK(found->scope == mira::testing::learning_scope());
            MIRA_CHECK(found->statement == entry.statement);
            MIRA_CHECK(found->recorded_at.time_since_epoch().count() != 0);
            MIRA_CHECK(found->confidence == 0.3F);
            MIRA_CHECK(found->status == MemoryStatus::Active);
        }
    }

    // Second sync: idempotent replay, no duplicates.
    const auto second = must(workflow->sync_skill_procedure_index(skills), "second sync");
    MIRA_CHECK(second.applied == 0);
    MIRA_CHECK(second.idempotent == 2);
    MIRA_CHECK(second.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Revoked entries are never written; the surviving entry replays.
    must(skills.revoke_skill(index.entries.back().name, "sync revoke"), "sync revoke");
    const auto third = must(workflow->sync_skill_procedure_index(skills), "third sync");
    MIRA_CHECK(third.applied == 0);
    MIRA_CHECK(third.idempotent == 1);
    MIRA_CHECK(third.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Each sync emits exactly one State-level audit event with the closed
    // payload set; its id is the evidence anchor of every mutation in the
    // batch, and it binds the observed index digest and publication count.
    {
        const auto sync_events =
            typed_events(*fixture.events_, fixture.session_id_, "WorkflowProceduresSynced");
        MIRA_CHECK(sync_events.size() == 3);
        for (const auto &event : sync_events) {
            MIRA_CHECK(event.classification == EventClass::State);
            MIRA_CHECK(event.payload.is_object());
            for (const auto &member : *event.payload.as_object()) {
                const bool known = member.first == "schema" ||
                                   member.first == "procedure_index_digest" ||
                                   member.first == "published_count";
                MIRA_CHECK(known);
            }
        }
        const auto *digest = sync_events.front().payload.find("procedure_index_digest");
        MIRA_CHECK(digest != nullptr && digest->is_string() &&
                   *digest->as_string() == index.digest.to_string());
        const auto *count = sync_events.front().payload.find("published_count");
        MIRA_CHECK(count != nullptr && count->as_integer().has_value() &&
                   *count->as_integer() == 2);
        // The stored payload survives the strict parser.
        EventPayload wire;
        wire.type = sync_events.front().type;
        wire.data = to_json_string(sync_events.front().payload);
        const auto parsed = must(parse_workflow_procedures_synced(wire), "synced parse");
        MIRA_CHECK(parsed.procedure_index_digest == index.digest);
        MIRA_CHECK(parsed.published_count == 2);
    }
    // Without an installed learning context the sync refuses.
    {
        auto bare = std::make_unique<WorkflowRuntime>(fixture.executor_, *fixture.runtime_,
                                                      fixture.session_id_, fixture.environment_);
        bare->set_event_store(fixture.events_);
        const auto rejected = bare->sync_skill_procedure_index(skills);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidState);
        static_cast<void>(bare->shutdown());
    }
    // Without an event store there is no mutation evidence anchor: the sync
    // refuses instead of writing untraceable memory.
    {
        auto storeless = std::make_unique<WorkflowRuntime>(
            fixture.executor_, *fixture.runtime_, fixture.session_id_, fixture.environment_);
        must(storeless->set_learning_context(memory, mira::testing::learning_scope()),
             "storeless learning context");
        const auto rejected = storeless->sync_skill_procedure_index(skills);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable);
        MIRA_CHECK(memory->stored_records() == 2); // nothing extra written
        static_cast<void>(storeless->shutdown());
    }
    return 0;
}

int g5_episode_path_intact() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
    auto workflow = fixture.make_workflow();
    must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
         "learning context");
    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.proc", "episode path source", "delta.lookup", {1, 0, 0});
    must(workflow->sync_skill_procedure_index(skills), "sync");
    MIRA_CHECK(memory->stored_records() == 1);

    // The settlement-time episode loop keeps working after procedure syncs:
    // a Completed Strict run records its episode into the same memory.
    const WorkflowDefinition episode_definition =
        make_lookup_definition(kWorkflowGamma, SchemaVersion{1, 0}, "episode path run");
    const auto created =
        must(workflow->create_run(episode_definition, json_or_abort(R"json({"key":"k"})json"),
                                  std::nullopt),
             "episode create");
    const auto driven =
        must(workflow->execute_run(created.run_id, plain_context()), "episode drive");
    MIRA_CHECK(driven.state == WorkflowRunState::Completed);
    MIRA_CHECK(memory->stored_records() == 2);
    const auto records = memory->snapshot();
    MIRA_CHECK(std::any_of(records.begin(), records.end(), [](const MemoryRecord &record) {
        return record.kind == MemoryKind::Episode;
    }));
    const auto events =
        typed_events(*fixture.events_, fixture.session_id_, "WorkflowEpisodeRecorded");
    MIRA_CHECK(events.size() == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// G6: consumer closure surface (M7-TR2-G6; consumer binary run via ctest)
// ---------------------------------------------------------------------------

int g6_public_surface_closure() {
    // The new public vocabulary is self-describing and the degraded event
    // round trip is usable from a standalone TU (the full closure runs as the
    // mira_minimal_consumer test).
    MIRA_CHECK(kWorkflowToolRefsSchema == "mira.workflow.tool_refs.v1");
    MIRA_CHECK(kWorkflowToolCompatSchema == "mira.workflow.tool_compat.v1");
    MIRA_CHECK(kToolReferenceScheme == "toolref:");
    MIRA_CHECK(is_workflow_event_type("WorkflowToolCompatDegraded"));

    WorkflowRuntimeConfig defaults;
    MIRA_CHECK(defaults.max_mounted_tool_refs == 1024);
    MIRA_CHECK(defaults.max_skill_call_depth == 2);

    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::PinnedDigest) == "pinned_digest");
    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::FollowLatest) == "follow_latest");
    MIRA_CHECK(parse_tool_reference_mode("follow_latest").has_value());

    // Degraded event round trip over a pure projection.
    const std::vector<ExposedToolSpec> view = hand_view();
    const WorkflowDefinition definition =
        make_v11_definition(kWorkflowAlpha, "closure v1.1", "toolref:delta.lookup");
    const auto refs = extract_or_abort(definition, view);
    const auto projection =
        must(project_workflow_tool_compatibility(definition, refs, view), "closure projection");
    MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
    WorkflowToolCompatDegradedEvent event;
    event.run_id = WorkflowRunId::generate();
    event.workflow_id = definition.workflow_id;
    event.ir_digest = workflow_definition_digest(definition);
    event.projection = workflow_tool_compat_to_json(projection);
    const auto payload = to_event_payload(event);
    MIRA_CHECK(payload.type == "WorkflowToolCompatDegraded");
    const auto parsed = must(parse_workflow_tool_compat_degraded(payload), "closure parse");
    MIRA_CHECK(parsed.run_id == event.run_id);
    const auto decoded = must(workflow_tool_compat_from_json(parsed.projection), "closure inverse");
    MIRA_CHECK(projections_equal(decoded, projection));

    // Procedures-synced payload round trip over the public surface.
    MIRA_CHECK(is_workflow_event_type("WorkflowProceduresSynced"));
    WorkflowProceduresSyncedEvent synced;
    synced.procedure_index_digest = digest_string("m7-tr2/closure/sync");
    synced.published_count = 3;
    const auto synced_payload = to_event_payload(synced);
    MIRA_CHECK(synced_payload.type == "WorkflowProceduresSynced");
    MIRA_CHECK(synced_payload.classification == EventClass::State);
    const auto synced_parsed =
        must(parse_workflow_procedures_synced(synced_payload), "synced closure parse");
    MIRA_CHECK(synced_parsed.procedure_index_digest == synced.procedure_index_digest);
    MIRA_CHECK(synced_parsed.published_count == 3);
    // Unknown fields fail closed.
    {
        JsonValue mutated = must(parse_json(synced_payload.data), "synced payload parse");
        mutated.set("extra", JsonValue{true});
        EventPayload tampered;
        tampered.type = synced_payload.type;
        tampered.data = to_json_string(mutated);
        MIRA_CHECK(!parse_workflow_procedures_synced(tampered).has_value());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report [path]: canonical report of the fixed scenarios for
    // cross-process byte comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const std::string report = build_wiring_report();
        if (argc > 2) {
            std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "cannot open report path: " << argv[2] << '\n';
                return 2;
            }
            out << report << '\n';
            return out.good() ? 0 : 2;
        }
        std::cout << report << '\n';
        return 0;
    }

    struct Gate {
        const char *name;
        int (*run)();
    };
    const Gate gates[] = {
        {"G1 ir version evolution", g1_ir_version_evolution},
        {"G1 v1.0 extraction unchanged", g1_v1_0_extraction_unchanged},
        {"G1 v1.1 reference extraction", g1_v1_1_reference_extraction},
        {"G2 attach negative matrix", g2_attach_negative_matrix},
        {"G2 publish gate auto mount", g2_publish_gate_auto_mount},
        {"G2 admission invalid and dryrun", g2_admission_invalid_and_dryrun},
        {"G2 admission degraded event", g2_admission_degraded_event},
        {"G2 admission runnable and zero drift", g2_admission_runnable_and_zero_drift},
        {"G3 pin evolution projection matrix", g3_pin_evolution_projection_matrix},
        {"G3 runtime pin follow and evolution", g3_runtime_pin_follow_and_evolution},
        {"G3 report deterministic in-process", g3_report_deterministic_in_process},
        {"G4 registration surface and identity gate", g4_registration_surface_and_identity_gate},
        {"G4 skill execution outcomes", g4_skill_execution_outcomes},
        {"G4 skill depth matrix", g4_skill_depth_matrix},
        {"G4 skill child passes compat gate", g4_skill_child_passes_compat_gate},
        {"G5 procedure sync wiring", g5_procedure_sync_wiring},
        {"G5 episode path intact", g5_episode_path_intact},
        {"G6 public surface closure", g6_public_surface_closure},
    };

    std::cout << "M7 TR2 tool runtime wiring verification (" << sizeof(gates) / sizeof(gates[0])
              << " gates)\n";
    for (const Gate &gate : gates) {
        if (gate.run() != 0) {
            std::cerr << "FAILED gate: " << gate.name << '\n';
            return 1;
        }
        std::cout << "[ok] " << gate.name << '\n';
    }
    std::cout << "all gates passed\n";
    return 0;
}
