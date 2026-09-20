// M7 TR1 skill publication lifecycle verification: gates M7-TR1-G1..G6
// (frozen in docs/plans/m7-tools-evaluation-platform-v1.md section 5.6;
// contract source docs/design/tool_reference_and_skill_design.md section 17).
//
// Deterministic by construction: every value that reaches an assertion or the
// --report output is content-derived. Workflow/step/tool identities are parsed
// from fixed hex strings, view spec digests and descriptor digests derive from
// fixed content, and no clock or randomness enters asserted data, so reports
// reproduce byte-for-byte across processes and build trees.

#include "../support/test.hpp"

#include <mira/json.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>
#include <mira/tool_executor.hpp>
#include <mira/tool_reference.hpp>
#include <mira/tool_skill.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_versioning.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;

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

// Fixed step identities (hex suffixes chosen so lexicographic step_id order is
// decoupled from construction order).
StepId fixed_step(const char *hex) {
    const auto id = StepId::parse(hex);
    return id.has_value() ? *id : StepId{};
}
const char *kStepRender = "000000000000000000000000000000b1";
const char *kStepLookup = "000000000000000000000000000000b2";

ToolId fixed_tool(const char *hex) {
    const auto id = ToolId::parse(hex);
    return id.has_value() ? *id : ToolId{};
}

// Fixed runtime/session identities for the event-sink fixtures.
const RuntimeId kRuntimeId = [] {
    const auto id = RuntimeId::parse("0000000000000000000000000000000b");
    return id.has_value() ? *id : RuntimeId{};
}();
const SessionId kSessionId = [] {
    const auto id = SessionId::parse("0000000000000000000000000000000c");
    return id.has_value() ? *id : SessionId{};
}();

// Fixed content-derived view digests.
const Hash kDigestLookup = digest_string("m7-tr1/fixture/delta.lookup/v1");
const Hash kDigestRender = digest_string("m7-tr1/fixture/delta.render/v1");
const Hash kDigestForeign = digest_string("m7-tr1/fixture/unpublished-definition");

// Error helpers: the mira.skill domain (1 = descriptor/surface, 2 = lifecycle
// or capacity, 3 = binding/consistency).
bool failed_in_skill_domain(const Error &error, std::int32_t domain_code, ErrorCode code) {
    return error.domain == "mira.skill" && error.domain_code == domain_code && error.code == code &&
           !error.safe_message.empty();
}

// Input binding failures re-use the TR0 discipline, so an unbound manifest may
// surface under either the skill domain or the TR0 reference domain; both must
// carry the binding domain code 3.
bool failed_binding_like(const Error &error) {
    const bool skill = error.domain == "mira.skill";
    const bool reference = error.domain == "mira.tool_reference";
    return (skill || reference) && error.domain_code == 3 &&
           error.code == ErrorCode::InvalidArgument && !error.safe_message.empty();
}

// A hand-built exposure view entry: spec digest is supplied by the caller so
// every digest that reaches an assertion is fixed content.
ExposedToolSpec make_spec(const std::string &wire, const std::string &description,
                          const Hash &spec_digest, const ToolId &tool_id, bool side_effects) {
    ExposedToolSpec spec;
    spec.tool_id = tool_id;
    spec.version = SemanticVersion{1, 0, 0};
    spec.wire_name = wire;
    spec.description = description;
    spec.parameters_schema = JsonSchema{json_or_abort(R"json({
        "type": "object",
        "properties": {"key": {"type": "string"}},
        "required": ["key"],
        "additionalProperties": false
    })json")};
    spec.spec_digest = spec_digest;
    spec.has_side_effects = side_effects;
    return spec;
}

// The shared exposure view: one read-only lookup tool and one side-effecting
// render tool, both with fixed identities and digests.
std::vector<ExposedToolSpec> make_view() {
    return {make_spec("delta.lookup", "fixture lookup tool", kDigestLookup,
                      fixed_tool("000000000000000000000000000000e1"), false),
            make_spec("delta.render", "fixture render tool", kDigestRender,
                      fixed_tool("000000000000000000000000000000e2"), true)};
}

WorkflowStep toolcall_step(const char *step_hex, const JsonValue &arguments) {
    WorkflowStep step;
    step.id = fixed_step(step_hex);
    step.name = "step-" + std::string(step_hex).substr(28);
    step.kind = WorkflowStepKind::ToolCall;
    step.arguments = arguments;
    return step;
}

WorkflowParameterSpec make_param(const std::string &name, WorkflowParameterType type, bool required,
                                 const std::string &summary) {
    WorkflowParameterSpec spec;
    spec.name = name;
    spec.type = type;
    spec.required = required;
    spec.summary = summary;
    return spec;
}

WorkflowDefinition base_definition(const WorkflowId &workflow_id, const std::string &name,
                                   const std::string &summary,
                                   std::vector<WorkflowParameterSpec> parameters,
                                   std::vector<WorkflowStep> steps) {
    WorkflowDefinition definition;
    definition.schema_version = SchemaVersion{1, 0};
    definition.workflow_id = workflow_id;
    definition.name = name;
    definition.summary = summary;
    definition.parameters = std::move(parameters);
    definition.steps = std::move(steps);
    definition.default_policy = WorkflowPolicy::Strict;
    definition.allowed_policies = {WorkflowPolicy::Strict};
    return definition;
}

// The standard side-effecting scenario definition: renders through the
// side-effecting tool with a bound parameter, then looks up read-only.
WorkflowDefinition make_alpha_definition(const std::string &summary) {
    WorkflowParameterSpec scene =
        make_param("scene", WorkflowParameterType::String, true, "scene identifier to render");
    scene.min_length = 1;
    scene.max_length = 32;
    return base_definition(
        kWorkflowAlpha, "fixture.alpha", summary, {scene},
        {toolcall_step(
             kStepRender,
             json_or_abort(R"json({"tool":"delta.render","scene":{"$param":"scene"}})json")),
         toolcall_step(kStepLookup,
                       json_or_abort(R"json({"tool":"delta.lookup","key":"k"})json"))});
}

// A read-only scenario definition: only the read-only tool, no parameters.
WorkflowDefinition make_readonly_definition(const WorkflowId &workflow_id,
                                            const std::string &name) {
    return base_definition(
        workflow_id, name, "read-only fixture summary", {},
        {toolcall_step(kStepLookup,
                       json_or_abort(R"json({"tool":"delta.lookup","key":"k"})json"))});
}

// A read-only scenario definition bound to a parametrized lookup: used where
// the exposure view holds only the read-only tool (BuiltinToolRegistry).
WorkflowDefinition make_lookup_definition(const WorkflowId &workflow_id,
                                          const std::string &summary) {
    WorkflowParameterSpec key =
        make_param("key", WorkflowParameterType::String, true, "lookup key to resolve");
    return base_definition(
        workflow_id, "fixture.lookup", summary, {key},
        {toolcall_step(
            kStepLookup,
            json_or_abort(R"json({"tool":"delta.lookup","key":{"$param":"key"}})json"))});
}

WorkflowToolRefManifest extract_or_abort(const WorkflowDefinition &definition,
                                         std::span<const ExposedToolSpec> view) {
    return must(extract_workflow_tool_references(definition, view), "extraction fixture");
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

// A history with one record of the requested validation result.
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

// A two-version runnable history chaining definition_v1 -> definition_v2.
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

SkillDescriptor make_descriptor_or_abort(const WorkflowDefinition &definition,
                                         const WorkflowToolRefManifest &refs,
                                         std::span<const ExposedToolSpec> view,
                                         std::string_view name, SemanticVersion version) {
    return must(make_skill_descriptor(definition, refs, view, name, version), "descriptor fixture");
}

// One bundled publish scenario: definition + bound refs + view + runnable
// history + derived descriptor.
struct PublishScenario {
    WorkflowDefinition definition;
    WorkflowToolRefManifest refs;
    std::vector<ExposedToolSpec> view;
    WorkflowVersionHistory history;
    SkillDescriptor descriptor;
};

PublishScenario make_scenario(const WorkflowId &workflow_id, const std::string &skill_name,
                              const std::string &summary, SemanticVersion skill_version) {
    PublishScenario scenario;
    scenario.view = make_view();
    scenario.definition = make_alpha_definition(summary);
    scenario.definition.workflow_id = workflow_id;
    scenario.refs = extract_or_abort(scenario.definition, scenario.view);
    scenario.history = single_version_history(workflow_id, scenario.definition,
                                              WorkflowValidationResult::DryRunPassed);
    scenario.descriptor = make_descriptor_or_abort(scenario.definition, scenario.refs,
                                                   scenario.view, skill_name, skill_version);
    return scenario;
}

// ---------------------------------------------------------------------------
// Event sink fixtures (mirror the module registry test discipline)
// ---------------------------------------------------------------------------

Error store_error(std::string message) {
    Error error;
    error.code = ErrorCode::Unavailable;
    error.domain = "mira.test";
    error.safe_message = std::move(message);
    return error;
}

struct CapturedEvent {
    std::string type;
    std::string data;
    EventClass classification = EventClass::State;
};

// Capturing sink: records every append so payload shape and redaction can be
// asserted. Only content-derived fields are ever read back.
class CapturingEventStore final : public IEventStore {
  public:
    Result<AppendReceipt> append(const AppendRequest &request) override {
        events.push_back(CapturedEvent{request.payload.type, request.payload.data,
                                       request.payload.classification});
        AppendReceipt receipt;
        receipt.event_id = request.event_id;
        return receipt;
    }
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest>) override {
        return store_error("append_batch unused");
    }
    Result<EventPage> read(const EventQuery &) const override { return store_error("read unused"); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &) override {
        return StoreRecoveryReport{};
    }
    Result<void> flush(Durability) override { return Result<void>{}; }

    std::vector<CapturedEvent> events;
};

// Sink that always fails: mutations must still succeed and the failure must be
// counted, never thrown and never blocking.
class FailingEventStore final : public IEventStore {
  public:
    Result<AppendReceipt> append(const AppendRequest &) override {
        ++failures;
        return store_error("append unavailable");
    }
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest>) override {
        ++failures;
        return store_error("append_batch unavailable");
    }
    Result<EventPage> read(const EventQuery &) const override {
        return store_error("read unavailable");
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &) override {
        return StoreRecoveryReport{};
    }
    Result<void> flush(Durability) override { return Result<void>{}; }

    int failures = 0;
};

// Validates the closed event payload shape; returns the parsed payload so
// callers can assert kind-specific members. On failure MIRA_CHECK exits the
// gate with an error print, so the returned value only matters on success.
JsonValue check_publication_event(const CapturedEvent &event, std::string_view kind,
                                  const SkillDescriptor &descriptor, bool has_reason,
                                  EventClass classification) {
    MIRA_CHECK(event.type == std::string(kSkillPublicationEventSchema));
    MIRA_CHECK(event.classification == classification);
    const auto payload = must(parse_json(event.data), "event payload parse");
    MIRA_CHECK(payload.is_object());
    bool saw_reason = false;
    for (const auto &member : *payload.as_object()) {
        const bool known = member.first == "schema" || member.first == "kind" ||
                           member.first == "name" || member.first == "version" ||
                           member.first == "source_workflow_id" ||
                           member.first == "source_ir_digest" ||
                           member.first == "descriptor_digest" || member.first == "reason";
        MIRA_CHECK(known);
        if (member.first == "reason") {
            saw_reason = true;
        }
    }
    MIRA_CHECK(saw_reason == has_reason);
    const auto *schema = payload.find("schema");
    MIRA_CHECK(schema != nullptr && schema->is_string() &&
               *schema->as_string() == std::string(kSkillPublicationEventSchema));
    const auto *kind_value = payload.find("kind");
    MIRA_CHECK(kind_value != nullptr && kind_value->is_string() &&
               *kind_value->as_string() == kind);
    const auto *name = payload.find("name");
    MIRA_CHECK(name != nullptr && name->is_string() && *name->as_string() == descriptor.name);
    const auto *version = payload.find("version");
    MIRA_CHECK(version != nullptr && version->is_object());
    const auto *workflow_id = payload.find("source_workflow_id");
    MIRA_CHECK(workflow_id != nullptr && workflow_id->is_string() &&
               *workflow_id->as_string() == descriptor.source_workflow_id.to_string());
    const auto *ir_digest = payload.find("source_ir_digest");
    MIRA_CHECK(ir_digest != nullptr && ir_digest->is_string() &&
               *ir_digest->as_string() == descriptor.source_ir_digest.to_string());
    const auto *descriptor_digest = payload.find("descriptor_digest");
    MIRA_CHECK(descriptor_digest != nullptr && descriptor_digest->is_string() &&
               *descriptor_digest->as_string() == descriptor.digest.to_string());
    return payload;
}

// ---------------------------------------------------------------------------
// --report scenario (pure content-derived values only)
// ---------------------------------------------------------------------------

std::string build_skill_report() {
    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.skill.report.v1"));

    const std::vector<ExposedToolSpec> view = make_view();

    // Descriptor anchor: fixed identity, fixed content, canonical encoding.
    const WorkflowDefinition alpha = make_alpha_definition("TR1 report scenario summary");
    const WorkflowToolRefManifest alpha_refs = extract_or_abort(alpha, view);
    const SkillDescriptor alpha_descriptor =
        make_descriptor_or_abort(alpha, alpha_refs, view, "report.alpha_skill", {1, 2, 3});
    report.emplace_back("descriptor_json",
                        canonical_json_string(skill_descriptor_to_json(alpha_descriptor)));
    report.emplace_back("descriptor_digest", JsonValue{alpha_descriptor.digest.to_string()});
    const SkillSurface surface = must(derive_skill_surface(alpha, alpha_refs, view), "surface");
    report.emplace_back("surface_schema", canonical_json_string(surface.parameters_schema.root));
    report.emplace_back("has_side_effects", JsonValue{surface.has_side_effects});

    // Publication scenario: publish two skills, upgrade one, revoke the other.
    // Every captured event payload is deterministic content.
    CapturingEventStore sink;
    SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, kSessionId};
    const WorkflowDefinition alpha_v2 = make_alpha_definition("TR1 report scenario summary v2");
    const WorkflowToolRefManifest alpha_v2_refs = extract_or_abort(alpha_v2, view);
    const SkillDescriptor alpha_v2_descriptor =
        make_descriptor_or_abort(alpha_v2, alpha_v2_refs, view, "report.alpha_skill", {2, 0, 0});
    const WorkflowVersionHistory alpha_history = two_version_history(alpha, alpha_v2);

    const WorkflowDefinition beta = make_readonly_definition(kWorkflowBeta, "fixture.beta");
    const WorkflowToolRefManifest beta_refs = extract_or_abort(beta, view);
    const SkillDescriptor beta_descriptor =
        make_descriptor_or_abort(beta, beta_refs, view, "report.beta_skill", {1, 0, 0});
    const WorkflowVersionHistory beta_history =
        single_version_history(kWorkflowBeta, beta, WorkflowValidationResult::Validated);

    must(registry.publish_skill(alpha_descriptor, alpha, alpha_refs, view, alpha_history),
         "report publish alpha");
    must(registry.publish_skill(beta_descriptor, beta, beta_refs, view, beta_history),
         "report publish beta");
    must(registry.upgrade_skill("report.alpha_skill", alpha_v2_descriptor, alpha_v2, alpha_v2_refs,
                                view, alpha_history),
         "report upgrade alpha");
    must(registry.revoke_skill("report.beta_skill", "report revoke"), "report revoke beta");

    JsonValue::Array events;
    for (const CapturedEvent &captured : sink.events) {
        JsonValue::Object entry;
        entry.emplace_back("type", captured.type);
        entry.emplace_back("classification",
                           JsonValue{captured.classification == EventClass::Critical
                                         ? std::string("critical")
                                         : std::string("state")});
        const auto payload = must(parse_json(captured.data), "report event parse");
        entry.emplace_back("payload", canonical_json_string(payload));
        events.emplace_back(JsonValue{std::move(entry)});
    }
    report.emplace_back("events", JsonValue{std::move(events)});

    const SkillPublicationStats &stats = registry.stats();
    JsonValue::Object stats_json;
    stats_json.emplace_back("published", JsonValue{static_cast<std::int64_t>(stats.published)});
    stats_json.emplace_back("upgrades", JsonValue{static_cast<std::int64_t>(stats.upgrades)});
    stats_json.emplace_back("revocations", JsonValue{static_cast<std::int64_t>(stats.revocations)});
    stats_json.emplace_back("idempotent_noops",
                            JsonValue{static_cast<std::int64_t>(stats.idempotent_noops)});
    stats_json.emplace_back("rejected_sealed",
                            JsonValue{static_cast<std::int64_t>(stats.rejected_sealed)});
    stats_json.emplace_back("rejected_closed",
                            JsonValue{static_cast<std::int64_t>(stats.rejected_closed)});
    stats_json.emplace_back("rejected_other",
                            JsonValue{static_cast<std::int64_t>(stats.rejected_other)});
    stats_json.emplace_back("events_emitted",
                            JsonValue{static_cast<std::int64_t>(stats.events_emitted)});
    stats_json.emplace_back("event_sink_failures",
                            JsonValue{static_cast<std::int64_t>(stats.event_sink_failures)});
    report.emplace_back("stats", JsonValue{std::move(stats_json)});

    // Procedure index projection over the final publication state.
    const SkillProcedureIndex index =
        must(project_skill_procedure_index(registry.publications()), "report index");
    JsonValue::Array statements;
    for (const auto &entry : index.entries) {
        statements.emplace_back(JsonValue{entry.statement});
    }
    report.emplace_back("statements", JsonValue{std::move(statements)});
    report.emplace_back("index_digest", JsonValue{index.digest.to_string()});

    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G1: descriptor and surface derivation matrix (M7-TR1-G1)
// ---------------------------------------------------------------------------

int g1_parameter_type_and_constraint_matrix() {
    const std::vector<ExposedToolSpec> view = make_view();
    WorkflowParameterSpec p_string =
        make_param("p_string", WorkflowParameterType::String, true, "string parameter");
    p_string.min_length = 1;
    p_string.max_length = 64;
    p_string.pattern = "[a-z]+";
    p_string.enum_values = {JsonValue{std::string("fast")}, JsonValue{std::string("slow")}};
    WorkflowParameterSpec p_integer =
        make_param("p_integer", WorkflowParameterType::Integer, true, "integer parameter");
    p_integer.minimum = 0.0;
    p_integer.maximum = 100.0;
    WorkflowParameterSpec p_number =
        make_param("p_number", WorkflowParameterType::Number, false, "number parameter");
    p_number.minimum = -1.5;
    p_number.maximum = 2.5;
    p_number.default_value = JsonValue{0.5};
    WorkflowParameterSpec p_bool =
        make_param("p_bool", WorkflowParameterType::Boolean, false, "boolean parameter");
    p_bool.default_value = JsonValue{false};
    WorkflowParameterSpec p_default =
        make_param("p_default", WorkflowParameterType::String, false, "defaulted parameter");
    p_default.default_value = JsonValue{std::string("fallback")};

    WorkflowDefinition definition = base_definition(
        kWorkflowAlpha, "fixture.matrix", "parameter matrix summary",
        {p_string, p_integer, p_number, p_bool, p_default},
        {toolcall_step(kStepLookup,
                       json_or_abort(R"json({"tool":"delta.lookup","key":"k"})json"))});
    MIRA_CHECK(validate_workflow_definition(definition).has_value());
    const WorkflowToolRefManifest refs = extract_or_abort(definition, view);

    const SkillSurface surface = must(derive_skill_surface(definition, refs, view), "matrix");
    // description = definition.summary, verbatim.
    MIRA_CHECK(surface.description == "parameter matrix summary");
    // Read-only reference only: no side effects.
    MIRA_CHECK(!surface.has_side_effects);
    // The derived schema passes the shared gate subset explicitly.
    MIRA_CHECK(gate_schema_subset(surface.parameters_schema).has_value());

    const JsonValue &schema = surface.parameters_schema.root;
    MIRA_CHECK(schema.is_object());
    const auto *type = schema.find("type");
    MIRA_CHECK(type != nullptr && type->is_string() && *type->as_string() == "object");
    const auto *additional = schema.find("additionalProperties");
    MIRA_CHECK(additional != nullptr && additional->is_boolean() && !*additional->as_boolean());
    // Required carries exactly the required parameters, in declaration order.
    const auto *required = schema.find("required");
    MIRA_CHECK(required != nullptr && required->is_array() && required->as_array()->size() == 2);
    MIRA_CHECK((*required->as_array())[0] == JsonValue{std::string("p_string")});
    MIRA_CHECK((*required->as_array())[1] == JsonValue{std::string("p_integer")});

    const auto *properties = schema.find("properties");
    MIRA_CHECK(properties != nullptr && properties->is_object());
    const auto *string_property = properties->find("p_string");
    MIRA_CHECK(string_property != nullptr && string_property->is_object());
    {
        const auto *value = string_property->find("type");
        MIRA_CHECK(value != nullptr && value->is_string() && *value->as_string() == "string");
        value = string_property->find("minLength");
        MIRA_CHECK(value != nullptr && value->as_integer().has_value() &&
                   *value->as_integer() == 1);
        value = string_property->find("maxLength");
        MIRA_CHECK(value != nullptr && value->as_integer().has_value() &&
                   *value->as_integer() == 64);
        value = string_property->find("pattern");
        MIRA_CHECK(value != nullptr && value->is_string() && *value->as_string() == "[a-z]+");
        value = string_property->find("enum");
        MIRA_CHECK(value != nullptr && value->is_array() && value->as_array()->size() == 2);
        MIRA_CHECK((*value->as_array())[0] == JsonValue{std::string("fast")});
        MIRA_CHECK((*value->as_array())[1] == JsonValue{std::string("slow")});
        value = string_property->find("description");
        MIRA_CHECK(value != nullptr && value->is_string() &&
                   *value->as_string() == "string parameter");
        // Defaults never enter the schema (binding applies them at execution).
        MIRA_CHECK(string_property->find("default") == nullptr);
    }
    {
        const JsonValue &integer_property = *properties->find("p_integer");
        const auto *value = integer_property.find("type");
        MIRA_CHECK(value != nullptr && value->is_string() && *value->as_string() == "integer");
        value = integer_property.find("minimum");
        MIRA_CHECK(value != nullptr && value->as_number().has_value() &&
                   *value->as_number() == 0.0);
        value = integer_property.find("maximum");
        MIRA_CHECK(value != nullptr && value->as_number().has_value() &&
                   *value->as_number() == 100.0);
        MIRA_CHECK(integer_property.find("default") == nullptr);
    }
    {
        const JsonValue &number_property = *properties->find("p_number");
        const auto *value = number_property.find("type");
        MIRA_CHECK(value != nullptr && value->is_string() && *value->as_string() == "number");
        value = number_property.find("minimum");
        MIRA_CHECK(value != nullptr && value->as_number().has_value() &&
                   *value->as_number() == -1.5);
        value = number_property.find("maximum");
        MIRA_CHECK(value != nullptr && value->as_number().has_value() &&
                   *value->as_number() == 2.5);
        MIRA_CHECK(number_property.find("default") == nullptr);
    }
    {
        const auto *value = properties->find("p_bool")->find("type");
        MIRA_CHECK(value != nullptr && value->is_string() && *value->as_string() == "boolean");
        MIRA_CHECK(properties->find("p_bool")->find("default") == nullptr);
    }
    {
        const auto *defaulted = properties->find("p_default");
        MIRA_CHECK(defaulted != nullptr && defaulted->is_object());
        MIRA_CHECK(defaulted->find("default") == nullptr);
        MIRA_CHECK(defaulted->find("type") != nullptr);
    }
    // The derived schema validates a minimal instance (shape sanity for the
    // consumers downstream).
    const auto violations = validate_instance_against_schema(
        json_or_abort(R"json({"p_string":"fast","p_integer":1})json"), surface.parameters_schema);
    MIRA_CHECK(violations.empty());
    return 0;
}

int g1_side_effects_derivation() {
    const std::vector<ExposedToolSpec> view = make_view();

    // All referenced tools read-only -> false.
    {
        const WorkflowDefinition readonly = make_readonly_definition(kWorkflowAlpha, "ro");
        const WorkflowToolRefManifest refs = extract_or_abort(readonly, view);
        const SkillSurface surface =
            must(derive_skill_surface(readonly, refs, view), "readonly surface");
        MIRA_CHECK(!surface.has_side_effects);
        // Unreferenced side-effecting tools in the view do not leak in.
        MIRA_CHECK(surface.parameters_schema.root.find("properties")->as_object()->empty());
    }
    // Any referenced tool with side effects -> true.
    {
        const WorkflowDefinition alpha = make_alpha_definition("side effect summary");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        const SkillSurface surface = must(derive_skill_surface(alpha, refs, view), "alpha surface");
        MIRA_CHECK(surface.has_side_effects);
    }
    // Every reference must resolve in the view (fail closed, not absorbed):
    // refs are extracted against the full view, then the derivation view is
    // missing a referenced tool.
    {
        std::vector<ExposedToolSpec> missing_view;
        missing_view.push_back(view[0]); // delta.lookup only; delta.render vanished.
        const WorkflowDefinition alpha = make_alpha_definition("missing view summary");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        const auto surface = derive_skill_surface(alpha, refs, missing_view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 3, ErrorCode::InvalidArgument));
    }
    return 0;
}

int g1_summary_bounds() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition ok = make_alpha_definition(std::string(2048, 's'));
    const WorkflowToolRefManifest ok_refs = extract_or_abort(ok, view);
    MIRA_CHECK(derive_skill_surface(ok, ok_refs, view).has_value());

    // Empty summary rejects.
    {
        const WorkflowDefinition empty = make_alpha_definition("");
        const WorkflowToolRefManifest refs = extract_or_abort(empty, view);
        const auto surface = derive_skill_surface(empty, refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 1, ErrorCode::InvalidArgument));
    }
    // Summary one byte over the bound rejects.
    {
        const WorkflowDefinition over = make_alpha_definition(std::string(2049, 's'));
        const WorkflowToolRefManifest refs = extract_or_abort(over, view);
        const auto surface = derive_skill_surface(over, refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 1, ErrorCode::InvalidArgument));
    }
    // A per-parameter summary over the bound rejects the whole group.
    {
        WorkflowDefinition over_param = make_alpha_definition("param summary over bound");
        over_param.parameters.push_back(
            make_param("loud", WorkflowParameterType::String, false, std::string(2049, 'p')));
        MIRA_CHECK(validate_workflow_definition(over_param).has_value());
        const WorkflowToolRefManifest refs = extract_or_abort(over_param, view);
        const auto surface = derive_skill_surface(over_param, refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 1, ErrorCode::InvalidArgument));
    }
    // A per-parameter summary exactly at the bound is accepted.
    {
        WorkflowDefinition at_param = make_alpha_definition("param summary at bound");
        at_param.parameters.push_back(
            make_param("loud", WorkflowParameterType::String, false, std::string(2048, 'p')));
        MIRA_CHECK(validate_workflow_definition(at_param).has_value());
        const WorkflowToolRefManifest refs = extract_or_abort(at_param, view);
        const auto surface = must(derive_skill_surface(at_param, refs, view), "at bound");
        const auto *description =
            surface.parameters_schema.root.find("properties")->find("loud")->find("description");
        MIRA_CHECK(description != nullptr && description->is_string() &&
                   description->as_string()->size() == 2048);
    }
    return 0;
}

int g1_binding_fail_closed() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("binding summary");
    const WorkflowToolRefManifest alpha_refs = extract_or_abort(alpha, view);

    // Refs bound to a different definition content reject.
    {
        WorkflowDefinition renamed = alpha;
        renamed.name = "fixture.alpha.renamed";
        MIRA_CHECK(workflow_definition_digest(renamed) != workflow_definition_digest(alpha));
        const auto surface = derive_skill_surface(renamed, alpha_refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_binding_like(surface.error()));
    }
    // Refs bound to a different workflow id reject.
    {
        WorkflowDefinition other = alpha;
        other.workflow_id = kWorkflowBeta;
        const WorkflowToolRefManifest other_refs = extract_or_abort(other, view);
        const auto surface = derive_skill_surface(alpha, other_refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_binding_like(surface.error()));
    }
    // A view with duplicate wire names rejects the whole group.
    {
        std::vector<ExposedToolSpec> duplicated = view;
        duplicated.push_back(make_spec("delta.lookup", "shadow lookup",
                                       digest_string("m7-tr1/fixture/shadow"),
                                       fixed_tool("000000000000000000000000000000e9"), false));
        const auto surface = derive_skill_surface(alpha, alpha_refs, duplicated);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 3, ErrorCode::InvalidArgument));
    }
    // A definition that fails structural validation rejects.
    {
        WorkflowDefinition broken = alpha;
        WorkflowStep verify = toolcall_step(kStepLookup, json_or_abort(R"json({"tool":"x"})json"));
        verify.kind = WorkflowStepKind::Verify;
        verify.verification.reset();
        broken.steps.back() = verify;
        const auto surface = derive_skill_surface(broken, alpha_refs, view);
        MIRA_CHECK(!surface.has_value());
        MIRA_CHECK(failed_in_skill_domain(surface.error(), 3, ErrorCode::InvalidArgument));
    }
    return 0;
}

int g1_descriptor_identity_and_charset() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("identity summary");
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);

    // Source identity comes from the definition; the digest is content.
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.alpha_skill", {1, 4, 3});
    MIRA_CHECK(descriptor.name == "fixture.alpha_skill");
    MIRA_CHECK((descriptor.version == SemanticVersion{1, 4, 3}));
    MIRA_CHECK(descriptor.source_workflow_id == kWorkflowAlpha);
    MIRA_CHECK(descriptor.source_ir_digest == workflow_definition_digest(alpha));
    MIRA_CHECK(descriptor.digest == skill_descriptor_digest(descriptor));
    MIRA_CHECK(descriptor.surface.description == "identity summary");
    MIRA_CHECK(descriptor.surface.has_side_effects);

    // Determinism: same inputs, same digest; every identity carrier changes it.
    const SkillDescriptor again =
        make_descriptor_or_abort(alpha, refs, view, "fixture.alpha_skill", {1, 4, 3});
    MIRA_CHECK(again == descriptor);
    const SkillDescriptor other_version =
        make_descriptor_or_abort(alpha, refs, view, "fixture.alpha_skill", {2, 0, 0});
    MIRA_CHECK(other_version.digest != descriptor.digest);
    const SkillDescriptor other_name =
        make_descriptor_or_abort(alpha, refs, view, "fixture.alpha_skill_b", {1, 4, 3});
    MIRA_CHECK(other_name.digest != descriptor.digest);
    const WorkflowDefinition renamed_summary = make_alpha_definition("identity summary v2");
    const WorkflowToolRefManifest renamed_refs = extract_or_abort(renamed_summary, view);
    const SkillDescriptor other_content = make_descriptor_or_abort(
        renamed_summary, renamed_refs, view, "fixture.alpha_skill", {1, 4, 3});
    MIRA_CHECK(other_content.digest != descriptor.digest);
    MIRA_CHECK(other_content.source_ir_digest == workflow_definition_digest(renamed_summary));

    // Charset breadth: lowercase segments with digits, '_' and '-' inside; the
    // 128-byte length bound is accepted.
    const std::vector<std::string> goldens = {"a", "skill_1", "skill-1.v2", "a1_b2-c3.d4",
                                              std::string(128, 'a')};
    for (const std::string &golden : goldens) {
        const auto parsed = make_skill_descriptor(alpha, refs, view, golden, {1, 0, 0});
        MIRA_CHECK(parsed.has_value());
        MIRA_CHECK(parsed.value().name == golden);
    }
    // Charset violations, empty name and the length bound.
    const std::vector<std::string> bad_names = {
        "",       std::string(129, 'a'), "Skill",      "skill name",  ".skill",
        "skill.", "skill..name",         "skill/name", "skill-name!", "skill\nname"};
    for (const std::string &bad : bad_names) {
        const auto parsed = make_skill_descriptor(alpha, refs, view, bad, {1, 0, 0});
        MIRA_CHECK(!parsed.has_value());
        MIRA_CHECK(failed_in_skill_domain(parsed.error(), 1, ErrorCode::InvalidArgument));
    }
    // Hosted provider tool names are reserved across the whole closed set.
    for (const char *hosted : {"web_search", "web_search_preview", "file_search",
                               "code_interpreter", "computer_use_preview", "computer-use", "mcp",
                               "bash", "shell", "terminal", "image_generation", "canvas"}) {
        MIRA_CHECK(is_known_hosted_tool_name(hosted));
        const auto parsed = make_skill_descriptor(alpha, refs, view, hosted, {1, 0, 0});
        MIRA_CHECK(!parsed.has_value());
        MIRA_CHECK(failed_in_skill_domain(parsed.error(), 1, ErrorCode::InvalidArgument));
    }
    // A non-hosted lookalike stays publishable.
    MIRA_CHECK(make_skill_descriptor(alpha, refs, view, "bash_tool", {1, 0, 0}).has_value());
    return 0;
}

int g1_descriptor_json_round_trip() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("round trip summary");
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.round_trip", {3, 2, 1});

    const JsonValue encoded = skill_descriptor_to_json(descriptor);
    MIRA_CHECK(encoded.is_object());
    const auto decoded = must(skill_descriptor_from_json(encoded), "descriptor decode");
    MIRA_CHECK(decoded == descriptor);
    MIRA_CHECK(canonical_json_string(skill_descriptor_to_json(decoded)) ==
               canonical_json_string(encoded));
    MIRA_CHECK(decoded.digest == skill_descriptor_digest(decoded));

    const auto rejects = [](const JsonValue &value) {
        const auto result = skill_descriptor_from_json(value);
        return !result.has_value() &&
               failed_in_skill_domain(result.error(), 1, ErrorCode::InvalidArgument);
    };

    // Unknown root field fails closed (closed field set scans first).
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member);
        }
        mutated.emplace_back("extra", JsonValue{std::int64_t{1}});
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Wrong schema name.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first,
                                 member.first == "schema"
                                     ? JsonValue{std::string("mira.skill.descriptor.v2")}
                                     : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Stored digest no longer matches the content.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "digest"
                                                   ? JsonValue{std::string(64, '0')}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Tampered name: the stored digest binds the original content, so the
    // mismatch is what rejects.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "name"
                                                   ? JsonValue{std::string("fixture.tampered")}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Unknown surface field rejects the closed surface field set.
    {
        const auto *surface = encoded.find("surface");
        JsonValue::Object mutated_surface;
        for (const auto &member : *surface->as_object()) {
            mutated_surface.emplace_back(member);
        }
        mutated_surface.emplace_back("secrets", JsonValue{std::string("none")});
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "surface"
                                                   ? JsonValue{std::move(mutated_surface)}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Version must decode as a semantic version.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "version"
                                                   ? JsonValue{std::string("1.0.0")}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Source workflow id must parse as an id.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "source_workflow_id"
                                                   ? JsonValue{std::string("not-an-id")}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Source ir digest must parse as a digest.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first, member.first == "source_ir_digest"
                                                   ? JsonValue{std::string("zz")}
                                                   : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Surface members have closed types.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            if (member.first == "surface") {
                JsonValue::Object surface;
                surface.emplace_back("description", JsonValue{std::int64_t{7}});
                surface.emplace_back("parameters_schema", member.second.find("parameters_schema"));
                surface.emplace_back("has_side_effects", member.second.find("has_side_effects"));
                mutated.emplace_back("surface", JsonValue{std::move(surface)});
            } else {
                mutated.emplace_back(member);
            }
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Not an object at all.
    MIRA_CHECK(rejects(JsonValue{JsonValue::Array{}}));
    return 0;
}

// ---------------------------------------------------------------------------
// G2: publication lifecycle matrix (M7-TR1-G2)
// ---------------------------------------------------------------------------

int g2_publish_positive() {
    const PublishScenario scenario =
        make_scenario(kWorkflowAlpha, "fixture.alpha_skill", "publish summary", {1, 0, 0});
    CapturingEventStore sink;
    SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, kSessionId};

    const auto published = registry.publish_skill(scenario.descriptor, scenario.definition,
                                                  scenario.refs, scenario.view, scenario.history);
    MIRA_CHECK(published.has_value());
    MIRA_CHECK(published.value().status == SkillPublicationStatus::Published);
    MIRA_CHECK(published.value().descriptor == scenario.descriptor);
    MIRA_CHECK(published.value().superseded_versions.empty());
    MIRA_CHECK(registry.stats().published == 1);
    MIRA_CHECK(registry.stats().rejected_other == 0);
    MIRA_CHECK(registry.stats().events_emitted == 1);
    MIRA_CHECK(sink.events.size() == 1);
    check_publication_event(sink.events.front(), "published", scenario.descriptor, false,
                            EventClass::State);

    // Reads see the record; publications() is sorted by name.
    const auto found = registry.find("fixture.alpha_skill");
    MIRA_CHECK(found.has_value() && *found == published.value());
    MIRA_CHECK(registry.publications().size() == 1);
    MIRA_CHECK(registry.publications().front() == published.value());
    MIRA_CHECK(!registry.find("fixture.absent").has_value());
    MIRA_CHECK(!registry.sealed());
    MIRA_CHECK(!registry.closed());

    // A Validated source version publishes as well (the runnable set is
    // DryRunPassed | Validated).
    const WorkflowDefinition beta = make_readonly_definition(kWorkflowBeta, "fixture.beta");
    const WorkflowToolRefManifest beta_refs = extract_or_abort(beta, scenario.view);
    const SkillDescriptor beta_descriptor =
        make_descriptor_or_abort(beta, beta_refs, scenario.view, "fixture.beta_skill", {1, 0, 0});
    const WorkflowVersionHistory beta_history =
        single_version_history(kWorkflowBeta, beta, WorkflowValidationResult::Validated);
    const auto validated =
        registry.publish_skill(beta_descriptor, beta, beta_refs, scenario.view, beta_history);
    MIRA_CHECK(validated.has_value());
    MIRA_CHECK(registry.stats().published == 2);
    // publications() stays name-sorted after the second insert.
    MIRA_CHECK(registry.publications().front().descriptor.name == "fixture.alpha_skill");
    MIRA_CHECK(registry.publications().back().descriptor.name == "fixture.beta_skill");
    return 0;
}

int g2_publish_negative_matrix() {
    // Shared view; every case builds its own registry and inputs.
    const std::vector<ExposedToolSpec> view = make_view();

    // 1. Non-runnable source version: NotValidated and Rejected both fail the
    // publish gate (DEC-025: only DryRunPassed/Validated are runnable).
    for (const WorkflowValidationResult validation :
         {WorkflowValidationResult::NotValidated, WorkflowValidationResult::Rejected}) {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("non runnable");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        const SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.non_runnable", {1, 0, 0});
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, validation);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 3, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    // 2. Pinned ir_digest does not match the presented definition content
    // (descriptor re-signed so only the pin is wrong).
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("pin mismatch");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.pin_mismatch", {1, 0, 0});
        descriptor.source_ir_digest = kDigestForeign;
        descriptor.digest = skill_descriptor_digest(descriptor);
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 3, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    // 3. The pinned digest resolves nowhere in the history (the history holds
    // a version of other content under the same workflow id).
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("resolve miss");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        const SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.resolve_miss", {1, 0, 0});
        const WorkflowDefinition other = make_alpha_definition("history other content");
        WorkflowVersionHistory history;
        history.workflow_id = kWorkflowAlpha;
        must(append_workflow_version(
                 history, make_version_record({1, 0, 0}, workflow_definition_digest(other),
                                              WorkflowValidationResult::DryRunPassed)),
             "resolve miss history fixture");
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::NotFound);
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    // 4. History workflow id does not match the descriptor source.
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("history id mismatch");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        const SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.history_id", {1, 0, 0});
        WorkflowVersionHistory history;
        history.workflow_id = kWorkflowBeta;
        must(append_workflow_version(
                 history, make_version_record({1, 0, 0}, workflow_definition_digest(alpha),
                                              WorkflowValidationResult::DryRunPassed)),
             "history id fixture");
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 3, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
    }
    // 5. Descriptor source workflow id does not match definition/history
    // (re-signed so only the identity is wrong).
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("descriptor id mismatch");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.descriptor_id", {1, 0, 0});
        descriptor.source_workflow_id = kWorkflowBeta;
        descriptor.digest = skill_descriptor_digest(descriptor);
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 3, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
    }
    // 6. Descriptor pinned to content that differs from the presented source
    // definition (digest/id mismatch family).
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition renamed = make_alpha_definition("renamed content");
        const WorkflowToolRefManifest renamed_refs = extract_or_abort(renamed, view);
        const SkillDescriptor pinned =
            make_descriptor_or_abort(renamed, renamed_refs, view, "fixture.mismatch", {1, 0, 0});
        const WorkflowDefinition original = make_alpha_definition("original content");
        MIRA_CHECK(workflow_definition_digest(original) != workflow_definition_digest(renamed));
        const WorkflowVersionHistory history = single_version_history(
            kWorkflowAlpha, original, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(pinned, original, renamed_refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 3, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    // 7. Descriptor surface does not match the derived surface (re-signed).
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("surface mismatch");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.surface", {1, 0, 0});
        descriptor.surface.description = "tampered description";
        descriptor.digest = skill_descriptor_digest(descriptor);
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 1, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    // 8. Descriptor digest is not self-consistent.
    {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("digest flip");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.digest", {1, 0, 0});
        descriptor.digest.bytes[0] = static_cast<std::uint8_t>(descriptor.digest.bytes[0] ^ 0xff);
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 1, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
    }
    // 9. Reserved hosted name and charset-violating name on the descriptor.
    for (const char *bad : {"bash", "Bash", "fixture.negative!"}) {
        SkillPublicationRegistry registry;
        const WorkflowDefinition alpha = make_alpha_definition("bad name");
        const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
        SkillDescriptor descriptor =
            make_descriptor_or_abort(alpha, refs, view, "fixture.bad_name", {1, 0, 0});
        descriptor.name = bad;
        const WorkflowVersionHistory history =
            single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
        const auto rejected = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 1, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.publications().empty());
    }
    return 0;
}

int g2_publish_duplicates_and_capacity() {
    const PublishScenario scenario =
        make_scenario(kWorkflowAlpha, "fixture.dup", "duplicate summary", {1, 0, 0});
    SkillPublicationRegistry registry;

    const auto first = registry.publish_skill(scenario.descriptor, scenario.definition,
                                              scenario.refs, scenario.view, scenario.history);
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(registry.stats().published == 1);

    // Same name + same descriptor digest while Published -> idempotent NoOp.
    const auto replay = registry.publish_skill(scenario.descriptor, scenario.definition,
                                               scenario.refs, scenario.view, scenario.history);
    MIRA_CHECK(replay.has_value() && replay.value() == first.value());
    MIRA_CHECK(registry.stats().idempotent_noops == 1);
    MIRA_CHECK(registry.stats().published == 1);
    MIRA_CHECK(registry.stats().rejected_other == 0);
    MIRA_CHECK(registry.publications().size() == 1);

    // Same name + different content while Published -> AlreadyExists. The
    // conflicting descriptor must satisfy the whole fail-closed chain (its pin
    // resolves in a history that holds both versions), so only the name
    // collision is what rejects.
    const WorkflowDefinition evolved = make_alpha_definition("duplicate summary v2");
    const WorkflowToolRefManifest evolved_refs = extract_or_abort(evolved, scenario.view);
    const SkillDescriptor evolved_descriptor =
        make_descriptor_or_abort(evolved, evolved_refs, scenario.view, "fixture.dup", {1, 0, 0});
    MIRA_CHECK(evolved_descriptor.digest != scenario.descriptor.digest);
    const WorkflowVersionHistory evolved_history =
        two_version_history(scenario.definition, evolved);
    const auto conflict = registry.publish_skill(evolved_descriptor, evolved, evolved_refs,
                                                 scenario.view, evolved_history);
    MIRA_CHECK(!conflict.has_value());
    MIRA_CHECK(failed_in_skill_domain(conflict.error(), 2, ErrorCode::AlreadyExists));
    MIRA_CHECK(registry.stats().rejected_other == 1);
    MIRA_CHECK(registry.find("fixture.dup").value().descriptor == scenario.descriptor);

    // Capacity: a tight registry rejects the publish over the limit with
    // ResourceExhausted, and an over-capacity idempotent replay is still a
    // NoOp (the name-existence check precedes the capacity check).
    SkillLimits tight{};
    tight.max_skills = 2;
    SkillPublicationRegistry capped{tight};
    const WorkflowDefinition beta = make_readonly_definition(kWorkflowBeta, "fixture.beta");
    const WorkflowToolRefManifest beta_refs = extract_or_abort(beta, scenario.view);
    const SkillDescriptor beta_descriptor =
        make_descriptor_or_abort(beta, beta_refs, scenario.view, "fixture.beta_skill", {1, 0, 0});
    const WorkflowVersionHistory beta_history =
        single_version_history(kWorkflowBeta, beta, WorkflowValidationResult::DryRunPassed);
    const WorkflowDefinition gamma = make_readonly_definition(kWorkflowGamma, "fixture.gamma");
    const WorkflowToolRefManifest gamma_refs = extract_or_abort(gamma, scenario.view);
    const SkillDescriptor gamma_descriptor = make_descriptor_or_abort(
        gamma, gamma_refs, scenario.view, "fixture.gamma_skill", {1, 0, 0});
    const WorkflowVersionHistory gamma_history =
        single_version_history(kWorkflowGamma, gamma, WorkflowValidationResult::DryRunPassed);
    MIRA_CHECK(capped
                   .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                  scenario.view, scenario.history)
                   .has_value());
    MIRA_CHECK(capped.publish_skill(beta_descriptor, beta, beta_refs, scenario.view, beta_history)
                   .has_value());
    const auto over =
        capped.publish_skill(gamma_descriptor, gamma, gamma_refs, scenario.view, gamma_history);
    MIRA_CHECK(!over.has_value());
    MIRA_CHECK(failed_in_skill_domain(over.error(), 2, ErrorCode::ResourceExhausted));
    MIRA_CHECK(capped.stats().rejected_other == 1);
    MIRA_CHECK(capped.publications().size() == 2);
    const auto capped_replay = capped.publish_skill(scenario.descriptor, scenario.definition,
                                                    scenario.refs, scenario.view, scenario.history);
    MIRA_CHECK(capped_replay.has_value());
    MIRA_CHECK(capped.stats().idempotent_noops == 1);
    MIRA_CHECK(capped.publications().size() == 2);
    return 0;
}

int g2_upgrade_matrix() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition v1 = make_alpha_definition("upgrade summary v1");
    const WorkflowDefinition v2 = make_alpha_definition("upgrade summary v2");
    const WorkflowToolRefManifest v1_refs = extract_or_abort(v1, view);
    const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, view);
    const SkillDescriptor d1 =
        make_descriptor_or_abort(v1, v1_refs, view, "fixture.upgrade", {1, 0, 0});
    const SkillDescriptor d2 =
        make_descriptor_or_abort(v2, v2_refs, view, "fixture.upgrade", {2, 0, 0});
    const WorkflowVersionHistory history = two_version_history(v1, v2);

    CapturingEventStore sink;
    SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, kSessionId};
    const auto published = registry.publish_skill(d1, v1, v1_refs, view, history);
    MIRA_CHECK(published.has_value());

    // Positive upgrade: strictly increasing version, new pin, superseded trail.
    const auto upgraded = registry.upgrade_skill("fixture.upgrade", d2, v2, v2_refs, view, history);
    MIRA_CHECK(upgraded.has_value());
    MIRA_CHECK(upgraded.value().descriptor == d2);
    MIRA_CHECK(upgraded.value().superseded_versions.size() == 1);
    MIRA_CHECK((upgraded.value().superseded_versions.front() == SemanticVersion{1, 0, 0}));
    MIRA_CHECK(registry.stats().upgrades == 1);
    MIRA_CHECK(registry.stats().published == 1);
    MIRA_CHECK(sink.events.size() == 2);
    check_publication_event(sink.events.back(), "upgraded", d2, false, EventClass::State);
    MIRA_CHECK(registry.find("fixture.upgrade")->descriptor == d2);

    // Version not strictly greater rejects (equal and lower), state unchanged.
    {
        const WorkflowDefinition v3 = make_alpha_definition("upgrade summary v3 distinct");
        const WorkflowToolRefManifest v3_refs = extract_or_abort(v3, view);
        const WorkflowDefinition v4 = make_alpha_definition("upgrade summary v4 distinct");
        const WorkflowToolRefManifest v4_refs = extract_or_abort(v4, view);
        WorkflowVersionHistory longer = two_version_history(v1, v2);
        must(append_workflow_version(longer,
                                     make_version_record({3, 0, 0}, workflow_definition_digest(v3),
                                                         WorkflowValidationResult::DryRunPassed,
                                                         workflow_definition_digest(v2))),
             "history v3 fixture");
        must(append_workflow_version(longer,
                                     make_version_record({4, 0, 0}, workflow_definition_digest(v4),
                                                         WorkflowValidationResult::DryRunPassed,
                                                         workflow_definition_digest(v3))),
             "history v4 fixture");
        // d3 is valid content but carries the CURRENT version, so the
        // strictly-increasing gate is what rejects.
        const SkillDescriptor d3 =
            make_descriptor_or_abort(v3, v3_refs, view, "fixture.upgrade", {2, 0, 0});
        const auto not_increasing =
            registry.upgrade_skill("fixture.upgrade", d3, v3, v3_refs, view, longer);
        MIRA_CHECK(!not_increasing.has_value());
        MIRA_CHECK(failed_in_skill_domain(not_increasing.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.stats().rejected_other == 1);
        MIRA_CHECK(registry.find("fixture.upgrade")->descriptor == d2);
        // Lower version with valid new content also rejects.
        const SkillDescriptor d4_lower =
            make_descriptor_or_abort(v4, v4_refs, view, "fixture.upgrade", {1, 0, 0});
        const auto lower =
            registry.upgrade_skill("fixture.upgrade", d4_lower, v4, v4_refs, view, longer);
        MIRA_CHECK(!lower.has_value());
        MIRA_CHECK(failed_in_skill_domain(lower.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.stats().rejected_other == 2);
        MIRA_CHECK(registry.find("fixture.upgrade")->descriptor == d2);
        MIRA_CHECK(registry.find("fixture.upgrade")->superseded_versions.size() == 1);
    }

    // Same-digest upgrade is an idempotent NoOp: no state change, no event.
    {
        const auto noop = registry.upgrade_skill("fixture.upgrade", d2, v2, v2_refs, view, history);
        MIRA_CHECK(noop.has_value() && noop.value().descriptor == d2);
        MIRA_CHECK(registry.stats().idempotent_noops == 1);
        MIRA_CHECK(registry.stats().upgrades == 1);
        MIRA_CHECK(sink.events.size() == 2);
        MIRA_CHECK(registry.find("fixture.upgrade")->superseded_versions.size() == 1);
    }

    // Republishing the CURRENT descriptor after an upgrade is an idempotent
    // NoOp: the frozen contract keys idempotency on name + descriptor digest,
    // so a non-empty superseded trail must not turn the replay into an
    // AlreadyExists rejection. State, counters and events stay untouched.
    {
        const auto replay = registry.publish_skill(d2, v2, v2_refs, view, history);
        MIRA_CHECK(replay.has_value());
        MIRA_CHECK(replay.value().descriptor == d2);
        MIRA_CHECK(replay.value().status == SkillPublicationStatus::Published);
        MIRA_CHECK((replay.value().superseded_versions.front() == SemanticVersion{1, 0, 0}));
        MIRA_CHECK(replay.value().superseded_versions.size() == 1);
        MIRA_CHECK(registry.stats().idempotent_noops == 2);
        MIRA_CHECK(registry.stats().published == 1);
        MIRA_CHECK(registry.stats().upgrades == 1);
        MIRA_CHECK(registry.stats().rejected_other == 2);
        MIRA_CHECK(sink.events.size() == 2);
        MIRA_CHECK(registry.publications().size() == 1);
        MIRA_CHECK(registry.find("fixture.upgrade")->descriptor == d2);
        // The upgraded content still conflicts under the same name.
        const WorkflowDefinition v3 = make_alpha_definition("upgrade summary v3 distinct");
        const WorkflowToolRefManifest v3_refs = extract_or_abort(v3, view);
        const SkillDescriptor d3 =
            make_descriptor_or_abort(v3, v3_refs, view, "fixture.upgrade", {2, 0, 0});
        WorkflowVersionHistory v3_history = two_version_history(v1, v2);
        must(append_workflow_version(v3_history,
                                     make_version_record({3, 0, 0}, workflow_definition_digest(v3),
                                                         WorkflowValidationResult::DryRunPassed,
                                                         workflow_definition_digest(v2))),
             "history v3 fixture");
        const auto conflict = registry.publish_skill(d3, v3, v3_refs, view, v3_history);
        MIRA_CHECK(!conflict.has_value());
        MIRA_CHECK(failed_in_skill_domain(conflict.error(), 2, ErrorCode::AlreadyExists));
        MIRA_CHECK(registry.stats().rejected_other == 3);
        MIRA_CHECK(registry.find("fixture.upgrade")->descriptor == d2);
    }

    // Unknown name -> NotFound; descriptor name mismatch -> invalid input.
    {
        const auto unknown =
            registry.upgrade_skill("fixture.absent", d2, v2, v2_refs, view, history);
        MIRA_CHECK(!unknown.has_value());
        MIRA_CHECK(failed_in_skill_domain(unknown.error(), 2, ErrorCode::NotFound));
        const SkillDescriptor renamed =
            make_descriptor_or_abort(v2, v2_refs, view, "fixture.other_name", {3, 0, 0});
        const auto mismatch =
            registry.upgrade_skill("fixture.upgrade", renamed, v2, v2_refs, view, history);
        MIRA_CHECK(!mismatch.has_value());
        MIRA_CHECK(failed_in_skill_domain(mismatch.error(), 1, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 5);
        MIRA_CHECK(sink.events.size() == 2);
    }

    // Superseded trail bound: a tight registry stops upgrading past the limit.
    {
        SkillLimits tight{};
        tight.max_superseded_versions = 1;
        SkillPublicationRegistry tight_registry{tight};
        const WorkflowDefinition v3 = make_alpha_definition("upgrade summary v3 distinct");
        const WorkflowToolRefManifest v3_refs = extract_or_abort(v3, view);
        const SkillDescriptor first =
            make_descriptor_or_abort(v1, v1_refs, view, "fixture.trail", {1, 0, 0});
        const SkillDescriptor second =
            make_descriptor_or_abort(v2, v2_refs, view, "fixture.trail", {2, 0, 0});
        const SkillDescriptor third =
            make_descriptor_or_abort(v3, v3_refs, view, "fixture.trail", {3, 0, 0});
        WorkflowVersionHistory trail_history = two_version_history(v1, v2);
        must(append_workflow_version(trail_history,
                                     make_version_record({3, 0, 0}, workflow_definition_digest(v3),
                                                         WorkflowValidationResult::DryRunPassed,
                                                         workflow_definition_digest(v2))),
             "trail history fixture");
        MIRA_CHECK(
            tight_registry.publish_skill(first, v1, v1_refs, view, trail_history).has_value());
        MIRA_CHECK(
            tight_registry.upgrade_skill("fixture.trail", second, v2, v2_refs, view, trail_history)
                .has_value());
        const auto overflow =
            tight_registry.upgrade_skill("fixture.trail", third, v3, v3_refs, view, trail_history);
        MIRA_CHECK(!overflow.has_value());
        MIRA_CHECK(failed_in_skill_domain(overflow.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(tight_registry.find("fixture.trail")->descriptor == second);
        MIRA_CHECK(tight_registry.find("fixture.trail")->superseded_versions.size() == 1);
        MIRA_CHECK(tight_registry.stats().rejected_other == 1);
    }
    return 0;
}

int g2_revoke_matrix() {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("revoke summary");
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.revoke", {1, 0, 0});
    const WorkflowVersionHistory history =
        single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);

    CapturingEventStore sink;
    SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, kSessionId};
    MIRA_CHECK(registry.publish_skill(descriptor, alpha, refs, view, history).has_value());

    // Unknown name -> NotFound.
    {
        const auto unknown = registry.revoke_skill("fixture.absent", "unknown revoke");
        MIRA_CHECK(!unknown.has_value());
        MIRA_CHECK(failed_in_skill_domain(unknown.error(), 2, ErrorCode::NotFound));
        MIRA_CHECK(registry.stats().rejected_other == 1);
    }
    // Reason bounds: empty and over-bound reject without state change; the
    // boundary value is accepted later on a second skill.
    {
        const auto empty = registry.revoke_skill("fixture.revoke", "");
        MIRA_CHECK(!empty.has_value());
        MIRA_CHECK(failed_in_skill_domain(empty.error(), 1, ErrorCode::InvalidArgument));
        const auto over = registry.revoke_skill("fixture.revoke", std::string(257, 'r'));
        MIRA_CHECK(!over.has_value());
        MIRA_CHECK(failed_in_skill_domain(over.error(), 1, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.stats().rejected_other == 3);
        MIRA_CHECK(registry.find("fixture.revoke")->status == SkillPublicationStatus::Published);
        MIRA_CHECK(registry.find("fixture.revoke")->descriptor == descriptor);
    }
    // Positive revoke: downgrade to Revoked with a bounded reason and a
    // Critical event.
    {
        const auto revoked = registry.revoke_skill("fixture.revoke", "superseded by host");
        MIRA_CHECK(revoked.has_value());
        MIRA_CHECK(revoked.value().status == SkillPublicationStatus::Revoked);
        MIRA_CHECK(revoked.value().descriptor == descriptor);
        MIRA_CHECK(registry.stats().revocations == 1);
        MIRA_CHECK(sink.events.size() == 2);
        const JsonValue payload = check_publication_event(sink.events.back(), "revoked", descriptor,
                                                          true, EventClass::Critical);
        const auto *reason = payload.find("reason");
        MIRA_CHECK(reason != nullptr && reason->is_string() &&
                   *reason->as_string() == "superseded by host");
    }
    // Repeated revoke is an idempotent NoOp even without a reason.
    {
        const auto repeat = registry.revoke_skill("fixture.revoke", "");
        MIRA_CHECK(repeat.has_value());
        MIRA_CHECK(repeat.value().status == SkillPublicationStatus::Revoked);
        MIRA_CHECK(registry.stats().idempotent_noops == 1);
        MIRA_CHECK(registry.stats().revocations == 1);
        MIRA_CHECK(sink.events.size() == 2);
    }
    // Revocation is final for the window: same-content republish rejects with
    // InvalidState and upgrade is refused as well.
    {
        const auto republish = registry.publish_skill(descriptor, alpha, refs, view, history);
        MIRA_CHECK(!republish.has_value());
        MIRA_CHECK(failed_in_skill_domain(republish.error(), 2, ErrorCode::InvalidState));
        const WorkflowDefinition v2 = make_alpha_definition("revoke summary v2");
        const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, view);
        const SkillDescriptor d2 =
            make_descriptor_or_abort(v2, v2_refs, view, "fixture.revoke", {2, 0, 0});
        const WorkflowVersionHistory v2_history =
            single_version_history(kWorkflowAlpha, v2, WorkflowValidationResult::DryRunPassed);
        const auto upgrade_revoked =
            registry.upgrade_skill("fixture.revoke", d2, v2, v2_refs, view, v2_history);
        MIRA_CHECK(!upgrade_revoked.has_value());
        MIRA_CHECK(failed_in_skill_domain(upgrade_revoked.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.find("fixture.revoke")->status == SkillPublicationStatus::Revoked);
        MIRA_CHECK(registry.stats().rejected_other == 5);
        // Different content under a revoked name is also downgrade-locked: the
        // revoked branch precedes the AlreadyExists branch (regression guard
        // for the idempotency fix).
        const WorkflowVersionHistory both = two_version_history(alpha, v2);
        const auto different_content = registry.publish_skill(d2, v2, v2_refs, view, both);
        MIRA_CHECK(!different_content.has_value());
        MIRA_CHECK(failed_in_skill_domain(different_content.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.stats().rejected_other == 6);
        MIRA_CHECK(registry.find("fixture.revoke")->descriptor == descriptor);
    }
    // Reason exactly at the bound is accepted on a second skill.
    {
        const WorkflowDefinition beta = make_readonly_definition(kWorkflowBeta, "fixture.beta");
        const WorkflowToolRefManifest beta_refs = extract_or_abort(beta, view);
        const SkillDescriptor beta_descriptor =
            make_descriptor_or_abort(beta, beta_refs, view, "fixture.beta_skill", {1, 0, 0});
        const WorkflowVersionHistory beta_history =
            single_version_history(kWorkflowBeta, beta, WorkflowValidationResult::DryRunPassed);
        MIRA_CHECK(registry.publish_skill(beta_descriptor, beta, beta_refs, view, beta_history)
                       .has_value());
        const auto boundary = registry.revoke_skill("fixture.beta_skill", std::string(256, 'r'));
        MIRA_CHECK(boundary.has_value());
        MIRA_CHECK(boundary.value().status == SkillPublicationStatus::Revoked);
        MIRA_CHECK(registry.stats().revocations == 2);
    }
    return 0;
}

int g2_seal_and_close_windows() {
    const PublishScenario scenario =
        make_scenario(kWorkflowAlpha, "fixture.window", "window summary", {1, 0, 0});
    const WorkflowDefinition v2 = make_alpha_definition("window summary v2");
    const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, scenario.view);
    const SkillDescriptor d2 =
        make_descriptor_or_abort(v2, v2_refs, scenario.view, "fixture.window", {2, 0, 0});
    const WorkflowVersionHistory history = two_version_history(scenario.definition, v2);

    SkillPublicationRegistry registry;
    MIRA_CHECK(registry
                   .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                  scenario.view, scenario.history)
                   .has_value());

    // seal(): publish and upgrade reject (counted), revoke stays available.
    registry.seal();
    MIRA_CHECK(registry.sealed());
    {
        const auto publish_sealed = registry.publish_skill(d2, v2, v2_refs, scenario.view, history);
        MIRA_CHECK(!publish_sealed.has_value());
        MIRA_CHECK(failed_in_skill_domain(publish_sealed.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.stats().rejected_sealed == 1);
        const auto upgrade_sealed =
            registry.upgrade_skill("fixture.window", d2, v2, v2_refs, scenario.view, history);
        MIRA_CHECK(!upgrade_sealed.has_value());
        MIRA_CHECK(registry.stats().rejected_sealed == 2);
        MIRA_CHECK(registry.find("fixture.window")->descriptor == scenario.descriptor);
        // Reads still work while sealed.
        MIRA_CHECK(registry.publications().size() == 1);
        const auto revoked_sealed = registry.revoke_skill("fixture.window", "sealed revoke");
        MIRA_CHECK(revoked_sealed.has_value());
        MIRA_CHECK(revoked_sealed.value().status == SkillPublicationStatus::Revoked);
        MIRA_CHECK(registry.stats().revocations == 1);
    }

    // close(): every mutation rejects with the closed counter; reads survive.
    registry.close();
    MIRA_CHECK(registry.closed());
    {
        const auto publish_closed = registry.publish_skill(d2, v2, v2_refs, scenario.view, history);
        MIRA_CHECK(!publish_closed.has_value());
        MIRA_CHECK(failed_in_skill_domain(publish_closed.error(), 2, ErrorCode::InvalidState));
        MIRA_CHECK(registry.stats().rejected_closed == 1);
        const auto upgrade_closed =
            registry.upgrade_skill("fixture.window", d2, v2, v2_refs, scenario.view, history);
        MIRA_CHECK(!upgrade_closed.has_value());
        MIRA_CHECK(registry.stats().rejected_closed == 2);
        const auto revoke_closed = registry.revoke_skill("fixture.window", "closed revoke");
        MIRA_CHECK(!revoke_closed.has_value());
        MIRA_CHECK(registry.stats().rejected_closed == 3);
        MIRA_CHECK(registry.find("fixture.window")->status == SkillPublicationStatus::Revoked);
        // Reads and pure projections still work on a closed registry.
        MIRA_CHECK(registry.publications().size() == 1);
        const auto index =
            must(project_skill_procedure_index(registry.publications()), "closed read");
        MIRA_CHECK(index.entries.size() == 1);
        MIRA_CHECK(index.entries.front().status == SkillPublicationStatus::Revoked);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G3: events, redaction and determinism (M7-TR1-G3)
// ---------------------------------------------------------------------------

int g3_event_closed_set_and_redaction() {
    const std::vector<ExposedToolSpec> view = make_view();
    // Markers that must never appear in any event payload.
    WorkflowDefinition alpha = make_alpha_definition("confidential-summary-marker planned summary");
    alpha.parameters.front() =
        make_param("scene", WorkflowParameterType::String, true, "param-secret-marker detail");
    MIRA_CHECK(validate_workflow_definition(alpha).has_value());
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.events", {1, 0, 0});
    const WorkflowVersionHistory history =
        single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
    const WorkflowDefinition v2 =
        make_alpha_definition("confidential-summary-marker planned summary v2");
    const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, view);
    const SkillDescriptor d2 =
        make_descriptor_or_abort(v2, v2_refs, view, "fixture.events", {2, 0, 0});
    const WorkflowVersionHistory both = two_version_history(alpha, v2);

    CapturingEventStore sink;
    SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, kSessionId};
    MIRA_CHECK(registry.publish_skill(descriptor, alpha, refs, view, history).has_value());
    MIRA_CHECK(registry.upgrade_skill("fixture.events", d2, v2, v2_refs, view, both).has_value());
    MIRA_CHECK(registry.revoke_skill("fixture.events", "event revoke").has_value());
    MIRA_CHECK(sink.events.size() == 3);
    MIRA_CHECK(registry.stats().events_emitted == 3);
    MIRA_CHECK(registry.stats().event_sink_failures == 0);

    check_publication_event(sink.events[0], "published", descriptor, false, EventClass::State);
    check_publication_event(sink.events[1], "upgraded", d2, false, EventClass::State);
    check_publication_event(sink.events[2], "revoked", d2, true, EventClass::Critical);
    // The revoke event pins the descriptor current at revocation time (d2).
    MIRA_CHECK(sink.events[2].data.find(d2.digest.to_string()) != std::string::npos);

    // Redaction: no summary text, no parameter summaries, no schema bodies, no
    // tool descriptions in any captured payload.
    for (const CapturedEvent &event : sink.events) {
        for (const char *marker :
             {"confidential-summary-marker", "param-secret-marker", "parameters_schema",
              "properties", "description", "fixture lookup tool", "fixture render tool"}) {
            MIRA_CHECK(event.data.find(marker) == std::string::npos);
        }
    }
    return 0;
}

int g3_sink_failure_and_nil_sink_isolation() {
    const PublishScenario scenario =
        make_scenario(kWorkflowAlpha, "fixture.sink", "sink summary", {1, 0, 0});
    const WorkflowDefinition v2 = make_alpha_definition("sink summary v2");
    const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, scenario.view);
    const SkillDescriptor d2 =
        make_descriptor_or_abort(v2, v2_refs, scenario.view, "fixture.sink", {2, 0, 0});
    const WorkflowVersionHistory history = two_version_history(scenario.definition, v2);

    // A failing sink never blocks the control plane: mutations succeed and the
    // failures are visible in stats.
    {
        FailingEventStore failing;
        SkillPublicationRegistry registry{kDefaultSkillLimits, &failing, kRuntimeId, kSessionId};
        MIRA_CHECK(registry
                       .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                      scenario.view, history)
                       .has_value());
        MIRA_CHECK(registry.upgrade_skill("fixture.sink", d2, v2, v2_refs, scenario.view, history)
                       .has_value());
        MIRA_CHECK(registry.revoke_skill("fixture.sink", "sink revoke").has_value());
        MIRA_CHECK(registry.stats().published == 1);
        MIRA_CHECK(registry.stats().upgrades == 1);
        MIRA_CHECK(registry.stats().revocations == 1);
        MIRA_CHECK(registry.stats().events_emitted == 0);
        MIRA_CHECK(registry.stats().event_sink_failures == 3);
        MIRA_CHECK(failing.failures == 3);
        MIRA_CHECK(registry.publications().size() == 1);
        MIRA_CHECK(registry.find("fixture.sink")->status == SkillPublicationStatus::Revoked);
    }
    // A null sink and nil runtime/session ids disable emission with the same
    // counting discipline.
    {
        SkillPublicationRegistry registry; // null sink.
        MIRA_CHECK(registry
                       .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                      scenario.view, history)
                       .has_value());
        MIRA_CHECK(registry.stats().events_emitted == 0);
        MIRA_CHECK(registry.stats().event_sink_failures == 1);
    }
    {
        SkillPublicationRegistry registry{kDefaultSkillLimits, nullptr, RuntimeId{}, SessionId{}};
        MIRA_CHECK(registry
                       .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                      scenario.view, history)
                       .has_value());
        MIRA_CHECK(registry.stats().event_sink_failures == 1);
    }
    {
        CapturingEventStore sink;
        SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, RuntimeId{}, kSessionId};
        MIRA_CHECK(registry
                       .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                      scenario.view, history)
                       .has_value());
        MIRA_CHECK(sink.events.empty());
        MIRA_CHECK(registry.stats().event_sink_failures == 1);
    }
    {
        CapturingEventStore sink;
        SkillPublicationRegistry registry{kDefaultSkillLimits, &sink, kRuntimeId, SessionId{}};
        MIRA_CHECK(registry
                       .publish_skill(scenario.descriptor, scenario.definition, scenario.refs,
                                      scenario.view, history)
                       .has_value());
        MIRA_CHECK(sink.events.empty());
        MIRA_CHECK(registry.stats().event_sink_failures == 1);
    }
    return 0;
}

int g3_report_deterministic_in_process() {
    const std::string first = build_skill_report();
    const std::string second = build_skill_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// G4: procedure index projection and rebuild (M7-TR1-G4)
// ---------------------------------------------------------------------------

// Descriptors and definitions for the projection evolution scenario.
struct IndexData {
    WorkflowDefinition alpha_v1_definition;
    WorkflowDefinition alpha_v2_definition;
    WorkflowDefinition beta_definition;
    SkillDescriptor alpha_v1;
    SkillDescriptor alpha_v2;
    SkillDescriptor beta;
};

IndexData make_index_data() {
    const std::vector<ExposedToolSpec> view = make_view();
    IndexData data;
    data.alpha_v1_definition = make_alpha_definition("index summary v1");
    data.alpha_v2_definition = make_alpha_definition("index summary v2");
    data.beta_definition = make_readonly_definition(kWorkflowBeta, "fixture.beta");
    const WorkflowToolRefManifest alpha_v1_refs = extract_or_abort(data.alpha_v1_definition, view);
    const WorkflowToolRefManifest alpha_v2_refs = extract_or_abort(data.alpha_v2_definition, view);
    const WorkflowToolRefManifest beta_refs = extract_or_abort(data.beta_definition, view);
    data.alpha_v1 = make_descriptor_or_abort(data.alpha_v1_definition, alpha_v1_refs, view,
                                             "index.alpha", {1, 0, 0});
    data.alpha_v2 = make_descriptor_or_abort(data.alpha_v2_definition, alpha_v2_refs, view,
                                             "index.alpha", {2, 0, 0});
    data.beta =
        make_descriptor_or_abort(data.beta_definition, beta_refs, view, "index.beta", {1, 0, 0});
    return data;
}

// Drives publish alpha, publish beta, upgrade alpha and revoke beta on the
// caller's registry (the registry is neither copyable nor movable).
void drive_index_lifecycle(SkillPublicationRegistry &registry, const IndexData &data) {
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowToolRefManifest alpha_v1_refs = extract_or_abort(data.alpha_v1_definition, view);
    const WorkflowToolRefManifest alpha_v2_refs = extract_or_abort(data.alpha_v2_definition, view);
    const WorkflowToolRefManifest beta_refs = extract_or_abort(data.beta_definition, view);
    const WorkflowVersionHistory alpha_history =
        two_version_history(data.alpha_v1_definition, data.alpha_v2_definition);
    const WorkflowVersionHistory beta_history = single_version_history(
        kWorkflowBeta, data.beta_definition, WorkflowValidationResult::DryRunPassed);
    must(registry.publish_skill(data.alpha_v1, data.alpha_v1_definition, alpha_v1_refs, view,
                                alpha_history),
         "index publish alpha");
    must(registry.publish_skill(data.beta, data.beta_definition, beta_refs, view, beta_history),
         "index publish beta");
    must(registry.upgrade_skill("index.alpha", data.alpha_v2, data.alpha_v2_definition,
                                alpha_v2_refs, view, alpha_history),
         "index upgrade alpha");
    must(registry.revoke_skill("index.beta", "index revoke"), "index revoke beta");
}

int g4_projection_sorted_deterministic_and_digest() {
    const IndexData data = make_index_data();
    SkillPublicationRegistry registry;
    drive_index_lifecycle(registry, data);
    const std::vector<SkillPublicationRecord> publications = registry.publications();
    MIRA_CHECK(publications.size() == 2);
    // Publications are name-sorted regardless of insertion order.
    MIRA_CHECK(publications[0].descriptor.name == "index.alpha");
    MIRA_CHECK(publications[1].descriptor.name == "index.beta");

    const SkillProcedureIndex first =
        must(project_skill_procedure_index(publications), "index first");
    const SkillProcedureIndex second =
        must(project_skill_procedure_index(publications), "index second");
    MIRA_CHECK(first.entries == second.entries);
    MIRA_CHECK(first.digest == second.digest);
    MIRA_CHECK(!first.digest.to_string().empty());
    // Entries sorted by name; projection input order cannot leak.
    MIRA_CHECK(first.entries[0].name == "index.alpha");
    MIRA_CHECK(first.entries[1].name == "index.beta");

    // The index digest is the canonical digest over the parsed statements.
    JsonValue::Array statements;
    for (const auto &entry : first.entries) {
        statements.emplace_back(must(parse_json(entry.statement), "statement parse"));
    }
    MIRA_CHECK(first.digest == canonical_json_digest(JsonValue{std::move(statements)}));

    // An empty publication set projects to a defined empty index with a digest.
    const SkillProcedureIndex empty =
        must(project_skill_procedure_index(std::vector<SkillPublicationRecord>{}), "empty");
    MIRA_CHECK(empty.entries.empty());
    MIRA_CHECK(empty.empty());
    MIRA_CHECK(empty.digest != Hash{});
    return 0;
}

int g4_statement_shape_and_no_description() {
    const IndexData data = make_index_data();
    SkillPublicationRegistry registry;
    drive_index_lifecycle(registry, data);
    const SkillProcedureIndex index =
        must(project_skill_procedure_index(registry.publications()), "shape index");

    const SkillPublicationRecord alpha = *registry.find("index.alpha");
    const SkillPublicationRecord beta = *registry.find("index.beta");
    const SkillProcedureEntry &alpha_entry = index.entries[0];
    const SkillProcedureEntry &beta_entry = index.entries[1];
    MIRA_CHECK(alpha_entry.name == alpha.descriptor.name);
    MIRA_CHECK(beta_entry.name == beta.descriptor.name);

    // Entries carry the pinned identity and the side-effect derivation.
    MIRA_CHECK(alpha_entry.version == alpha.descriptor.version);
    MIRA_CHECK(alpha_entry.source_workflow_id == alpha.descriptor.source_workflow_id);
    MIRA_CHECK(alpha_entry.source_ir_digest == alpha.descriptor.source_ir_digest);
    MIRA_CHECK(alpha_entry.descriptor_digest == alpha.descriptor.digest);
    MIRA_CHECK(alpha_entry.has_side_effects);
    MIRA_CHECK(alpha_entry.status == SkillPublicationStatus::Published);
    MIRA_CHECK(!beta_entry.has_side_effects);
    MIRA_CHECK(beta_entry.status == SkillPublicationStatus::Revoked);

    for (const auto &entry : index.entries) {
        const auto statement = must(parse_json(entry.statement), "statement parse");
        MIRA_CHECK(statement.is_object());
        bool has_description = false;
        for (const auto &member : *statement.as_object()) {
            const bool known = member.first == "schema" || member.first == "name" ||
                               member.first == "version" || member.first == "source_workflow_id" ||
                               member.first == "source_ir_digest" ||
                               member.first == "descriptor_digest" ||
                               member.first == "has_side_effects" || member.first == "status";
            MIRA_CHECK(known);
            has_description |= member.first == "description";
        }
        MIRA_CHECK(!has_description);
        const auto *schema = statement.find("schema");
        MIRA_CHECK(schema != nullptr && schema->is_string() &&
                   *schema->as_string() == std::string(kSkillProcedureIndexSchema));
        const auto *status = statement.find("status");
        MIRA_CHECK(status != nullptr && status->is_string());
        MIRA_CHECK(*status->as_string() ==
                   std::string(skill_publication_status_name(entry.status)));
        const auto *side_effects = statement.find("has_side_effects");
        MIRA_CHECK(side_effects != nullptr && side_effects->is_boolean() &&
                   *side_effects->as_boolean() == entry.has_side_effects);
        // The statement is canonical: serializing the parsed value again is a
        // byte-identical round trip.
        MIRA_CHECK(canonical_json_string(statement) == entry.statement);
    }
    // The revoked statement names its status explicitly; superseded versions
    // never become separate entries.
    MIRA_CHECK(index.entries[1].statement.find("\"status\":\"revoked\"") != std::string::npos);
    MIRA_CHECK(index.entries.size() == 2);
    return 0;
}

int g4_rebuild_round_trip_and_replay() {
    const IndexData data = make_index_data();
    SkillPublicationRegistry registry;
    drive_index_lifecycle(registry, data);
    const SkillProcedureIndex index =
        must(project_skill_procedure_index(registry.publications()), "rebuild index");

    for (const auto &entry : index.entries) {
        const auto rebuilt =
            must(skill_procedure_entry_from_statement(entry.statement), "statement rebuild");
        MIRA_CHECK(rebuilt == entry);
        // Replay: re-serializing the rebuilt entry's statement is byte-stable.
        MIRA_CHECK(canonical_json_string(must(parse_json(rebuilt.statement), "replay parse")) ==
                   entry.statement);
    }

    // Non-canonical statements fail closed even when the content is right.
    const std::string canonical = index.entries.front().statement;
    {
        const auto parsed = must(parse_json(canonical), "reorder parse");
        JsonValue::Object reversed;
        const auto &members = *parsed.as_object();
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            reversed.emplace_back(*it);
        }
        const std::string non_canonical = to_json_string(JsonValue{std::move(reversed)});
        // Sanity: the canonical form of the reordered content still matches.
        MIRA_CHECK(canonical_json_string(must(parse_json(non_canonical), "reorder reparse")) ==
                   canonical);
        const auto rejected = skill_procedure_entry_from_statement(non_canonical);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 1, ErrorCode::InvalidArgument));
    }
    {
        const std::string padded = " " + canonical;
        const auto rejected = skill_procedure_entry_from_statement(padded);
        MIRA_CHECK(!rejected.has_value());
    }
    // Unknown field, wrong schema, missing member, bad version, bad id, bad
    // digest, bad status and non-object content all fail closed.
    const auto mutate = [&](const std::function<void(JsonValue::Object &)> &mutator) {
        const auto parsed = must(parse_json(canonical), "mutate parse");
        JsonValue::Object object;
        for (const auto &member : *parsed.as_object()) {
            object.emplace_back(member);
        }
        mutator(object);
        return skill_procedure_entry_from_statement(to_json_string(JsonValue{object}));
    };
    {
        const auto rejected = mutate(
            [](JsonValue::Object &object) { object.emplace_back("extra", JsonValue{true}); });
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_skill_domain(rejected.error(), 1, ErrorCode::InvalidArgument));
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "schema") {
                    member.second = JsonValue{std::string("mira.skill.procedure_index.v2")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            JsonValue::Object pruned;
            for (auto &member : object) {
                if (member.first != "descriptor_digest") {
                    pruned.emplace_back(member);
                }
            }
            object = std::move(pruned);
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "version") {
                    member.second = JsonValue{std::string("not-a-version")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "source_workflow_id") {
                    member.second = JsonValue{std::string("not-an-id")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "source_ir_digest") {
                    member.second = JsonValue{std::string("zz")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "status") {
                    member.second = JsonValue{std::string("archived")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    {
        const auto rejected = mutate([](JsonValue::Object &object) {
            for (auto &member : object) {
                if (member.first == "has_side_effects") {
                    member.second = JsonValue{std::string("yes")};
                }
            }
        });
        MIRA_CHECK(!rejected.has_value());
    }
    MIRA_CHECK(!skill_procedure_entry_from_statement("[]").has_value());
    MIRA_CHECK(!skill_procedure_entry_from_statement("not json").has_value());
    return 0;
}

int g4_projection_evolution_and_unpublished_assets() {
    const IndexData data = make_index_data();
    SkillPublicationRegistry registry;
    drive_index_lifecycle(registry, data);
    const SkillProcedureIndex index =
        must(project_skill_procedure_index(registry.publications()), "evolution index");

    // Upgraded skill: the entry pins the NEW digest and version; the
    // superseded version leaves no entry of its own.
    const SkillProcedureEntry &alpha = index.entries[0];
    MIRA_CHECK(alpha.name == "index.alpha");
    MIRA_CHECK((alpha.version == SemanticVersion{2, 0, 0}));
    MIRA_CHECK(alpha.descriptor_digest == data.alpha_v2.digest);
    MIRA_CHECK(alpha.source_ir_digest == data.alpha_v2.source_ir_digest);
    MIRA_CHECK(alpha.descriptor_digest != data.alpha_v1.digest);
    MIRA_CHECK(alpha.status == SkillPublicationStatus::Published);

    // Revoked skill: explicit revoked status with the pinned identity intact.
    const SkillProcedureEntry &beta = index.entries[1];
    MIRA_CHECK(beta.name == "index.beta");
    MIRA_CHECK(beta.status == SkillPublicationStatus::Revoked);
    MIRA_CHECK(beta.descriptor_digest == data.beta.digest);
    MIRA_CHECK((beta.version == SemanticVersion{1, 0, 0}));

    // Unpublished Workflow library assets never enter the index: gamma was
    // never published, so its identity appears nowhere. Descriptions never
    // leak into statements either.
    const std::string gamma_id = kWorkflowGamma.to_string();
    MIRA_CHECK(index.entries.size() == 2);
    for (const auto &entry : index.entries) {
        MIRA_CHECK(entry.source_workflow_id != kWorkflowGamma);
        MIRA_CHECK(entry.statement.find(gamma_id) == std::string::npos);
        MIRA_CHECK(entry.statement.find("description") == std::string::npos);
        MIRA_CHECK(entry.statement.find("index summary") == std::string::npos);
    }
    return 0;
}

int g4_projection_capacity_guard() {
    // Fabricated records are enough: the projection reads identity fields only.
    std::vector<SkillPublicationRecord> records;
    for (int index = 0; index < 3; ++index) {
        SkillDescriptor descriptor;
        descriptor.name = "capacity.skill_" + std::to_string(index);
        descriptor.version = SemanticVersion{1, 0, 0};
        descriptor.source_workflow_id = kWorkflowAlpha;
        descriptor.source_ir_digest = kDigestLookup;
        descriptor.surface.description = "capacity";
        descriptor.digest = skill_descriptor_digest(descriptor);
        SkillPublicationRecord record;
        record.descriptor = descriptor;
        records.push_back(record);
    }
    SkillLimits tight{};
    tight.max_skills = 2;
    const auto over = project_skill_procedure_index(records, tight);
    MIRA_CHECK(!over.has_value());
    MIRA_CHECK(failed_in_skill_domain(over.error(), 2, ErrorCode::ResourceExhausted));
    // At the exact bound the projection succeeds.
    records.pop_back();
    const auto at_bound = must(project_skill_procedure_index(records, tight), "at bound");
    MIRA_CHECK(at_bound.entries.size() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// G5: combination boundary - no execution face (M7-TR1-G5)
// ---------------------------------------------------------------------------

int g5_registry_exposure_and_execution_unchanged() {
    // A real registry holds one read-only tool; publishing a Skill that
    // references it must not change the exposure face or the DEC-015
    // execution-time identity checks.
    BuiltinToolRegistry tool_registry;
    BuiltinToolSpec spec;
    spec.tool_id = fixed_tool("000000000000000000000000000000e1");
    spec.version = SemanticVersion{1, 4, 0};
    spec.wire_name = "delta.lookup";
    spec.description = "fixture lookup tool";
    spec.parameters_schema = JsonSchema{json_or_abort(R"json({
        "type": "object",
        "properties": {"key": {"type": "string"}},
        "required": ["key"],
        "additionalProperties": false
    })json")};
    spec.has_side_effects = false;
    BuiltinToolHandler handler = [](const JsonValue &,
                                    const OperationContext &) -> Result<JsonValue> {
        return JsonValue{JsonValue::Object{{"echo", JsonValue{std::int64_t{1}}}}};
    };
    MIRA_CHECK(tool_registry.register_tool(std::move(spec), std::move(handler)).has_value());
    const std::vector<ExposedToolSpec> view = tool_registry.exposed_tools();
    MIRA_CHECK(view.size() == 1);

    const WorkflowDefinition alpha =
        make_lookup_definition(kWorkflowAlpha, "no execution face summary");
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.face", {1, 0, 0});
    const WorkflowVersionHistory history =
        single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
    SkillPublicationRegistry skill_registry;
    MIRA_CHECK(skill_registry.publish_skill(descriptor, alpha, refs, view, history).has_value());
    const WorkflowDefinition v2 =
        make_lookup_definition(kWorkflowAlpha, "no execution face summary v2");
    const WorkflowToolRefManifest v2_refs = extract_or_abort(v2, view);
    const SkillDescriptor d2 =
        make_descriptor_or_abort(v2, v2_refs, view, "fixture.face", {2, 0, 0});
    const WorkflowVersionHistory both = two_version_history(alpha, v2);
    MIRA_CHECK(
        skill_registry.upgrade_skill("fixture.face", d2, v2, v2_refs, view, both).has_value());
    MIRA_CHECK(skill_registry.revoke_skill("fixture.face", "face revoke").has_value());

    // The exposure face is unchanged by publication, upgrade and revocation.
    const std::vector<ExposedToolSpec> after = tool_registry.exposed_tools();
    MIRA_CHECK(after.size() == 1);
    MIRA_CHECK(after.front().wire_name == "delta.lookup");
    MIRA_CHECK(after.front().tool_id == fixed_tool("000000000000000000000000000000e1"));
    MIRA_CHECK(after.front().spec_digest == view.front().spec_digest);

    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    const ToolId current_id = fixed_tool("000000000000000000000000000000e1");
    const ToolId stale_id = fixed_tool("000000000000000000000000000000f1");
    const auto proposal = [&](const char *call_id, const ToolId &tool_id, SemanticVersion version) {
        ToolProposal item;
        item.provider_call_id = ProviderToolCallId{call_id};
        item.tool_id = tool_id;
        item.wire_name = "delta.lookup";
        item.tool_version = version;
        item.arguments = json_or_abort(R"json({"key":"k"})json");
        item.arguments_digest = digest_string(to_json_string(item.arguments));
        item.operation_id = OperationId::generate();
        item.has_side_effects = false;
        return item;
    };
    // Stale identity still rejected after the whole publication lifecycle.
    {
        const auto outcome = tool_registry.execute(proposal("stale", stale_id, {1, 4, 0}), context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::NotFound);
    }
    {
        const auto outcome =
            tool_registry.execute(proposal("stale_version", current_id, {1, 0, 0}), context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::InvalidState);
    }
    // Positive control: the current identity still dispatches.
    {
        const auto outcome =
            tool_registry.execute(proposal("current", current_id, {1, 4, 0}), context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(!outcome.value().failed);
    }
    // Inputs were never mutated by the publication path.
    MIRA_CHECK(descriptor.source_ir_digest == workflow_definition_digest(alpha));
    MIRA_CHECK(history.records.size() == 1);
    MIRA_CHECK(both.records.size() == 2);
    return 0;
}

int g5_combination_chain_tr0_tr1() {
    // The full publish chain runs on TR0 outputs only:
    // extract_workflow_tool_references -> verify_workflow_tool_refs ->
    // derive_skill_surface -> make_skill_descriptor -> publish.
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("combination summary");
    const auto extracted = extract_workflow_tool_references(alpha, view);
    MIRA_CHECK(extracted.has_value());
    MIRA_CHECK(verify_workflow_tool_refs(extracted.value(), alpha.workflow_id,
                                         workflow_definition_digest(alpha))
                   .has_value());
    const SkillSurface surface =
        must(derive_skill_surface(alpha, extracted.value(), view), "combination surface");
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, extracted.value(), view, "fixture.combo", {1, 0, 0});
    MIRA_CHECK(descriptor.surface == surface);
    const WorkflowVersionHistory history =
        single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
    SkillPublicationRegistry registry;
    const auto published =
        registry.publish_skill(descriptor, alpha, extracted.value(), view, history);
    MIRA_CHECK(published.has_value());
    const auto index =
        must(project_skill_procedure_index(registry.publications()), "combination index");
    MIRA_CHECK(index.entries.size() == 1);
    MIRA_CHECK(index.entries.front().descriptor_digest == descriptor.digest);
    MIRA_CHECK(must(skill_procedure_entry_from_statement(index.entries.front().statement),
                    "combination rebuild") == index.entries.front());
    return 0;
}

// ---------------------------------------------------------------------------
// G6: consumer closure surface (M7-TR1-G6; consumer binary run via ctest)
// ---------------------------------------------------------------------------

int g6_public_surface_closure() {
    // The header closes over its own vocabulary: schemas, status names, limits
    // and the full derive -> publish -> project -> rebuild chain are usable
    // from a standalone TU (examples/minimal_consumer.cpp links the same
    // surface; its TR1 closure runs as the mira_minimal_consumer test).
    MIRA_CHECK(kSkillDescriptorSchema == "mira.skill.descriptor.v1");
    MIRA_CHECK(kSkillPublicationEventSchema == "mira.skill.publication.v1");
    MIRA_CHECK(kSkillProcedureIndexSchema == "mira.skill.procedure_index.v1");

    MIRA_CHECK(skill_publication_status_name(SkillPublicationStatus::Published) == "published");
    MIRA_CHECK(skill_publication_status_name(SkillPublicationStatus::Revoked) == "revoked");

    SkillLimits limits{};
    MIRA_CHECK(limits.max_name_bytes == 128);
    MIRA_CHECK(limits.max_description_bytes == 2 * 1024);
    MIRA_CHECK(limits.max_reason_bytes == 256);
    MIRA_CHECK(limits.max_skills == 256);
    MIRA_CHECK(limits.max_superseded_versions == 64);
    MIRA_CHECK(limits.max_parameters == 64);

    // End-to-end smoke over the public surface only.
    const std::vector<ExposedToolSpec> view = make_view();
    const WorkflowDefinition alpha = make_alpha_definition("closure summary");
    const WorkflowToolRefManifest refs = extract_or_abort(alpha, view);
    const SkillDescriptor descriptor =
        make_descriptor_or_abort(alpha, refs, view, "fixture.closure", {1, 0, 0});
    const WorkflowVersionHistory history =
        single_version_history(kWorkflowAlpha, alpha, WorkflowValidationResult::DryRunPassed);
    SkillPublicationRegistry registry;
    MIRA_CHECK(registry.publish_skill(descriptor, alpha, refs, view, history).has_value());
    const auto index =
        must(project_skill_procedure_index(registry.publications()), "closure index");
    MIRA_CHECK(index.entries.size() == 1);
    MIRA_CHECK(must(skill_procedure_entry_from_statement(index.entries.front().statement),
                    "closure rebuild") == index.entries.front());
    const auto decoded =
        must(skill_descriptor_from_json(skill_descriptor_to_json(descriptor)), "closure decode");
    MIRA_CHECK(decoded == descriptor);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report [path]: canonical report of the fixed scenarios for
    // cross-process byte comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const std::string report = build_skill_report();
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
        {"G1 parameter type and constraint matrix", g1_parameter_type_and_constraint_matrix},
        {"G1 side effects derivation", g1_side_effects_derivation},
        {"G1 summary bounds", g1_summary_bounds},
        {"G1 binding fail closed", g1_binding_fail_closed},
        {"G1 descriptor identity and charset", g1_descriptor_identity_and_charset},
        {"G1 descriptor json round trip", g1_descriptor_json_round_trip},
        {"G2 publish positive", g2_publish_positive},
        {"G2 publish negative matrix", g2_publish_negative_matrix},
        {"G2 publish duplicates and capacity", g2_publish_duplicates_and_capacity},
        {"G2 upgrade matrix", g2_upgrade_matrix},
        {"G2 revoke matrix", g2_revoke_matrix},
        {"G2 seal and close windows", g2_seal_and_close_windows},
        {"G3 event closed set and redaction", g3_event_closed_set_and_redaction},
        {"G3 sink failure and nil sink isolation", g3_sink_failure_and_nil_sink_isolation},
        {"G3 report deterministic in-process", g3_report_deterministic_in_process},
        {"G4 projection sorted deterministic and digest",
         g4_projection_sorted_deterministic_and_digest},
        {"G4 statement shape and no description", g4_statement_shape_and_no_description},
        {"G4 rebuild round trip and replay", g4_rebuild_round_trip_and_replay},
        {"G4 projection evolution and unpublished assets",
         g4_projection_evolution_and_unpublished_assets},
        {"G4 projection capacity guard", g4_projection_capacity_guard},
        {"G5 registry exposure and execution unchanged",
         g5_registry_exposure_and_execution_unchanged},
        {"G5 combination chain tr0 tr1", g5_combination_chain_tr0_tr1},
        {"G6 public surface closure", g6_public_surface_closure},
    };

    std::cout << "M7 TR1 skill publication verification (" << sizeof(gates) / sizeof(gates[0])
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
