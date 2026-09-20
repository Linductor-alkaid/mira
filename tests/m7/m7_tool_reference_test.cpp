// M7 TR0 stable tool reference verification: gates M7-TR0-G1..G6 (frozen in
// docs/plans/m7-tools-evaluation-platform-v1.md section 5.5; contract source
// docs/design/tool_reference_and_skill_design.md sections 4-9).
//
// Deterministic by construction: every value that reaches an assertion or the
// --report output is content-derived. Workflow/step/tool identities are parsed
// from fixed hex strings and view spec digests are fixed sha256 values, so
// manifests, projections and decisions reproduce byte-for-byte across
// processes and build trees (no clock, no randomness in asserted data).

#include "../support/test.hpp"

#include <mira/json.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>
#include <mira/tool_executor.hpp>
#include <mira/tool_reference.hpp>

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

// Fixed workflow identities.
const WorkflowId kWorkflowAlpha = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a1");
    return id.has_value() ? *id : WorkflowId{};
}();
const WorkflowId kWorkflowBeta = [] {
    const auto id = WorkflowId::parse("000000000000000000000000000000a2");
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
const char *kStepTransform = "000000000000000000000000000000b3";

ToolId fixed_tool(const char *hex) {
    const auto id = ToolId::parse(hex);
    return id.has_value() ? *id : ToolId{};
}

// Fixed content-derived view digests.
const Hash kDigestOne = digest_string("m7-tr0/fixture/epsilon.tool/v1");
const Hash kDigestTwo = digest_string("m7-tr0/fixture/epsilon.tool/v2");
const Hash kDigestPinned = digest_string("m7-tr0/fixture/golden-pinned-spec");

template <typename T> T must(const Result<T> &result, const char *what) {
    if (!result.has_value()) {
        fixture_failed(std::string(what) + ": " + result.error().safe_message);
    }
    return result.value();
}

bool failed_in_reference_domain(const Error &error, std::int32_t domain_code) {
    return error.domain == "mira.tool_reference" && error.domain_code == domain_code &&
           error.code == ErrorCode::InvalidArgument && !error.safe_message.empty();
}

// A hand-built exposure view entry: spec digest is supplied by the caller so
// evolution scenarios are purely content-driven.
ExposedToolSpec make_spec(const std::string &wire, std::string_view schema_text,
                          const Hash &spec_digest, const ToolId &tool_id,
                          const SemanticVersion &version = SemanticVersion{1, 0, 0},
                          const std::string &description = "fixture tool",
                          bool side_effects = false) {
    ExposedToolSpec spec;
    spec.tool_id = tool_id;
    spec.version = version;
    spec.wire_name = wire;
    spec.description = description;
    spec.parameters_schema = JsonSchema{json_or_abort(schema_text)};
    spec.spec_digest = spec_digest;
    spec.has_side_effects = side_effects;
    return spec;
}

WorkflowStep toolcall_step(const char *step_hex, const JsonValue &arguments) {
    WorkflowStep step;
    step.id = fixed_step(step_hex);
    step.name = "step-" + std::string(step_hex).substr(28);
    step.kind = WorkflowStepKind::ToolCall;
    step.arguments = arguments;
    return step;
}

// A Verify step is the minimal non-ToolCall step that passes IR validation.
WorkflowStep verify_step(const char *step_hex) {
    WorkflowStep step;
    step.id = fixed_step(step_hex);
    step.name = "verify";
    step.kind = WorkflowStepKind::Verify;
    step.verification =
        WorkflowPredicate{"step_result:done", WorkflowPredicateOp::Exists, JsonValue{}};
    return step;
}

WorkflowDefinition base_definition(const WorkflowId &workflow_id, const std::string &name,
                                   std::vector<WorkflowStep> steps) {
    WorkflowDefinition definition;
    definition.schema_version = SchemaVersion{1, 0};
    definition.workflow_id = workflow_id;
    definition.name = name;
    definition.summary = "TR0 gate fixture";
    definition.steps = std::move(steps);
    definition.default_policy = WorkflowPolicy::Strict;
    definition.allowed_policies = {WorkflowPolicy::Strict};
    return definition;
}

WorkflowToolRefManifest extract_or_abort(const WorkflowDefinition &definition,
                                         std::span<const ExposedToolSpec> view,
                                         const ToolRefExtractionOptions &options = {}) {
    return must(extract_workflow_tool_references(definition, view, options), "extraction fixture");
}

struct MatrixResult {
    ToolRefCompatEntry entry;
    WorkflowToolCompatState state;
    Hash digest;
};

// The one-tool pinned scenario used across the resolution matrix: extract
// pinned against the publish view (digest one), project against an arbitrary
// current view (null = the tool vanished).
MatrixResult project_single(const JsonValue &arguments_v1, std::string_view schema_v1,
                            const ExposedToolSpec *current) {
    const std::vector<ExposedToolSpec> publish_view{make_spec(
        "epsilon.tool", schema_v1, kDigestOne, fixed_tool("000000000000000000000000000000d1"))};
    WorkflowDefinition definition =
        base_definition(kWorkflowAlpha, "matrix", {toolcall_step(kStepRender, arguments_v1)});
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);
    std::vector<ExposedToolSpec> current_view;
    if (current != nullptr) {
        current_view.push_back(*current);
    }
    const WorkflowToolCompatProjection projection =
        must(project_workflow_tool_compatibility(definition, manifest, current_view),
             "projection fixture");
    if (projection.entries.size() != 1) {
        fixture_failed("matrix projection must carry exactly one entry");
    }
    return MatrixResult{projection.entries.front(), projection.state, projection.digest};
}

// Same scenario shape with a follow-latest extraction.
MatrixResult project_single_follow(const ExposedToolSpec &publish, const ExposedToolSpec *current) {
    const std::vector<ExposedToolSpec> publish_view{publish};
    WorkflowDefinition definition = base_definition(
        kWorkflowAlpha, "matrix-follow",
        {toolcall_step(kStepRender, json_or_abort(R"json({"tool":"epsilon.tool"})json"))});
    ToolRefExtractionOptions follow;
    follow.default_mode = ToolReferenceMode::FollowLatest;
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view, follow);
    std::vector<ExposedToolSpec> current_view;
    if (current != nullptr) {
        current_view.push_back(*current);
    }
    const WorkflowToolCompatProjection projection =
        must(project_workflow_tool_compatibility(definition, manifest, current_view),
             "follow projection fixture");
    if (projection.entries.size() != 1) {
        fixture_failed("follow projection must carry exactly one entry");
    }
    return MatrixResult{projection.entries.front(), projection.state, projection.digest};
}

// ---------------------------------------------------------------------------
// Registry fixture: a real BuiltinToolRegistry drives the extraction view.
// Tool ids are fixed so the registry-derived spec digests are deterministic.
// ---------------------------------------------------------------------------

struct RegistryTool {
    std::string wire;
    std::string description;
    std::string schema_text;
    bool side_effects = false;
    const char *tool_id_hex;
    SemanticVersion version{1, 0, 0};
};

void fill_registry(BuiltinToolRegistry &registry, const std::vector<RegistryTool> &tools) {
    for (const auto &tool : tools) {
        BuiltinToolSpec spec;
        spec.tool_id = fixed_tool(tool.tool_id_hex);
        spec.version = tool.version;
        spec.wire_name = tool.wire;
        spec.description = tool.description;
        spec.parameters_schema = JsonSchema{json_or_abort(tool.schema_text)};
        spec.has_side_effects = tool.side_effects;
        BuiltinToolHandler handler = [](const JsonValue &,
                                        const OperationContext &) -> Result<JsonValue> {
            return JsonValue{JsonValue::Object{{"echo", JsonValue{std::int64_t{1}}}}};
        };
        if (!registry.register_tool(std::move(spec), std::move(handler)).has_value()) {
            fixture_failed("registry fixture registration failed for '" + tool.wire + "'");
        }
    }
}

std::vector<RegistryTool> gate_registry_tools() {
    return {
        {"delta.lookup", "fixture read-only lookup", R"json({
            "type": "object",
            "properties": {"key": {"type": "string"}},
            "required": ["key"],
            "additionalProperties": false
        })json",
         false, "000000000000000000000000000000e1", SemanticVersion{1, 4, 0}},
        {"delta.render", "fixture render tool", R"json({
            "type": "object",
            "properties": {"scene": {"type": "string"}},
            "required": ["scene"],
            "additionalProperties": false
        })json",
         true, "000000000000000000000000000000e2", SemanticVersion{1, 0, 0}},
        {"echo.transform", "fixture transform tool", R"json({"type":"object"})json", true,
         "000000000000000000000000000000e3", SemanticVersion{2, 0, 0}},
    };
}

WorkflowDefinition make_gate_definition() {
    return base_definition(kWorkflowAlpha, "fixture.alpha", {
        // Construction order deliberately differs from step_id order.
        toolcall_step(kStepTransform, json_or_abort(R"json({"tool":"echo.transform"})json")),
        toolcall_step(kStepRender,
                      json_or_abort(R"json({"tool":"delta.render","scene":{"$param":"scene"}})json")),
        toolcall_step(kStepLookup, json_or_abort(R"json({"tool":"delta.lookup","key":"k"})json")),
    });
}

// ---------------------------------------------------------------------------
// --report scenario (pure content-derived values only)
// ---------------------------------------------------------------------------

std::string build_reference_report() {
    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.tool_reference.report.v1"));

    // Golden reference forms and their canonical round trips.
    const std::string pinned_text = "toolref:epsilon.tool@" + kDigestPinned.to_string();
    JsonValue::Array golden;
    {
        const auto parsed = must(parse_tool_reference("toolref:delta.render"), "report follow");
        JsonValue::Object entry;
        entry.emplace_back("input", std::string("toolref:delta.render"));
        entry.emplace_back("canonical", tool_reference_to_string(parsed));
        entry.emplace_back("mode", std::string(tool_reference_mode_name(parsed.mode)));
        entry.emplace_back("wire_name", parsed.wire_name);
        golden.emplace_back(JsonValue{std::move(entry)});
    }
    {
        const auto parsed = must(parse_tool_reference(pinned_text), "report pinned");
        JsonValue::Object entry;
        entry.emplace_back("input", pinned_text);
        entry.emplace_back("canonical", tool_reference_to_string(parsed));
        entry.emplace_back("mode", std::string(tool_reference_mode_name(parsed.mode)));
        entry.emplace_back("wire_name", parsed.wire_name);
        entry.emplace_back("pinned_spec_digest", JsonValue{parsed.pinned_spec_digest->to_string()});
        golden.emplace_back(JsonValue{std::move(entry)});
    }
    report.emplace_back("golden", JsonValue{std::move(golden)});

    // Registry-backed extraction anchors (fixed tool ids keep the
    // registry-derived spec digests stable across processes and trees).
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view);
    report.emplace_back("manifest_digest", JsonValue{manifest.digest.to_string()});
    report.emplace_back("manifest_json",
                        canonical_json_string(workflow_tool_refs_to_json(manifest)));
    ToolRefExtractionOptions follow_all;
    follow_all.default_mode = ToolReferenceMode::FollowLatest;
    const WorkflowToolRefManifest following = extract_or_abort(definition, view, follow_all);
    report.emplace_back("manifest_follow_digest", JsonValue{following.digest.to_string()});

    // Resolution matrix anchors over the frozen epsilon.tool scenario.
    const ToolId tool_one = fixed_tool("000000000000000000000000000000d1");
    const ToolId tool_two = fixed_tool("000000000000000000000000000000d2");
    const ExposedToolSpec evolved_bindable = make_spec(
        "epsilon.tool",
        R"json({
            "type": "object",
            "properties": {"scene": {"type": "integer"}},
            "required": ["scene"],
            "additionalProperties": false
        })json",
        kDigestTwo, tool_two, SemanticVersion{2, 0, 0});
    const ExposedToolSpec evolved_broken = make_spec(
        "epsilon.tool",
        R"json({
            "type": "object",
            "properties": {"scene": {"type": "integer"}, "extra": {"type": "string"}},
            "required": ["scene", "extra"],
            "additionalProperties": false
        })json",
        kDigestTwo, tool_two);
    const ExposedToolSpec evolved_plain = make_spec(
        "epsilon.tool",
        R"json({
            "type": "object",
            "properties": {"scene": {"type": "string"}},
            "required": ["scene"],
            "additionalProperties": false
        })json",
        kDigestTwo, tool_two, SemanticVersion{2, 0, 0});
    const ExposedToolSpec unchanged = make_spec("epsilon.tool", R"json({"type":"object"})json",
                                                kDigestOne, tool_one);

    JsonValue::Array matrix;
    const auto record_matrix = [&](const char *name, const MatrixResult &result) {
        JsonValue::Object entry;
        entry.emplace_back("case", std::string(name));
        entry.emplace_back("mode", std::string(tool_reference_mode_name(result.entry.mode)));
        entry.emplace_back("status", std::string(tool_reference_compat_name(result.entry.status)));
        entry.emplace_back("state", std::string(workflow_tool_compat_state_name(result.state)));
        entry.emplace_back("detail", result.entry.detail);
        entry.emplace_back("projection_digest", JsonValue{result.digest.to_string()});
        matrix.emplace_back(JsonValue{std::move(entry)});
    };
    record_matrix("pinned_resolved",
                  project_single(json_or_abort(R"json({"tool":"epsilon.tool"})json"),
                                 R"json({"type":"object"})json", &unchanged));
    record_matrix("pinned_evolved_compatible",
                  project_single(json_or_abort(R"json({"tool":"epsilon.tool","scene":{"$param":"scene"}})json"),
                                 R"json({
                                     "type": "object",
                                     "properties": {"scene": {"type": "string"}},
                                     "required": ["scene"],
                                     "additionalProperties": false
                                 })json",
                                 &evolved_bindable));
    record_matrix("pinned_evolved_incompatible",
                  project_single(json_or_abort(R"json({"tool":"epsilon.tool","scene":"s"})json"),
                                 R"json({
                                     "type": "object",
                                     "properties": {"scene": {"type": "string"}},
                                     "required": ["scene"],
                                     "additionalProperties": false
                                 })json",
                                 &evolved_broken));
    record_matrix("pinned_unresolved",
                  project_single(json_or_abort(R"json({"tool":"epsilon.tool"})json"),
                                 R"json({"type":"object"})json", nullptr));
    record_matrix("follow_resolved", project_single_follow(evolved_plain, &evolved_plain));
    record_matrix("follow_unresolved", project_single_follow(unchanged, nullptr));
    report.emplace_back("matrix", JsonValue{std::move(matrix)});

    // Degraded audit artifact anchor plus its admission decision.
    {
        const std::vector<ExposedToolSpec> publish_view{make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "string"}},
                "required": ["scene"],
                "additionalProperties": false
            })json",
            kDigestOne, tool_one)};
        WorkflowDefinition degraded_definition = base_definition(
            kWorkflowAlpha, "report-degraded",
            {toolcall_step(kStepRender,
                           json_or_abort(R"json({"tool":"epsilon.tool","scene":{"$param":"scene"}})json"))});
        const WorkflowToolRefManifest degraded_manifest =
            extract_or_abort(degraded_definition, publish_view);
        const std::vector<ExposedToolSpec> current_view{evolved_bindable};
        const WorkflowToolCompatProjection degraded = must(
            project_workflow_tool_compatibility(degraded_definition, degraded_manifest,
                                                current_view),
            "report degraded projection");
        report.emplace_back("degraded_audit_json",
                            canonical_json_string(workflow_tool_compat_to_json(degraded)));
        const WorkflowToolCompatDecision degraded_decision =
            admit_workflow_run_by_tool_compat(degraded);
        report.emplace_back("degraded_admitted", JsonValue{degraded_decision.admitted});
        report.emplace_back("degraded_reason", degraded_decision.reason);
    }

    // Decision anchors for the three aggregate states.
    JsonValue::Array decisions;
    const auto record_decision = [&](const char *name, const WorkflowToolCompatDecision &decision) {
        JsonValue::Object entry;
        entry.emplace_back("case", std::string(name));
        entry.emplace_back("state", std::string(workflow_tool_compat_state_name(decision.state)));
        entry.emplace_back("admitted", decision.admitted);
        entry.emplace_back("reason", decision.reason);
        decisions.emplace_back(JsonValue{std::move(entry)});
    };
    {
        const MatrixResult resolved =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool"})json"),
                           R"json({"type":"object"})json", &unchanged);
        WorkflowToolCompatDecision decision;
        decision.admitted = true;
        decision.state = resolved.state;
        record_decision("runnable", decision);
    }
    {
        const std::vector<ExposedToolSpec> publish_view{make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "string"}},
                "required": ["scene"],
                "additionalProperties": false
            })json",
            kDigestOne, tool_one)};
        WorkflowDefinition degraded_definition = base_definition(
            kWorkflowAlpha, "report-degraded",
            {toolcall_step(kStepRender,
                           json_or_abort(R"json({"tool":"epsilon.tool","scene":{"$param":"scene"}})json"))});
        const WorkflowToolRefManifest degraded_manifest =
            extract_or_abort(degraded_definition, publish_view);
        const std::vector<ExposedToolSpec> current_view{evolved_bindable};
        const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(
            must(project_workflow_tool_compatibility(degraded_definition, degraded_manifest,
                                                     current_view),
                 "report degraded decision"));
        record_decision("degraded", decision);
    }
    {
        const MatrixResult unresolved =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool"})json"),
                           R"json({"type":"object"})json", nullptr);
        WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(
            WorkflowToolCompatProjection{kWorkflowAlpha, Sha256Digest{},
                                         WorkflowToolCompatState::Invalid,
                                         {unresolved.entry}, unresolved.digest});
        record_decision("invalid", decision);
    }
    report.emplace_back("decisions", JsonValue{std::move(decisions)});

    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G1: reference syntax and parse matrix (M7-TR0-G1)
// ---------------------------------------------------------------------------

int g1_follow_golden_round_trip() {
    // Golden follow form: scheme + governed wire name, no digest part.
    const auto reference = must(parse_tool_reference("toolref:delta.render"), "parse follow");
    MIRA_CHECK(reference.mode == ToolReferenceMode::FollowLatest);
    MIRA_CHECK(reference.wire_name == "delta.render");
    MIRA_CHECK(!reference.pinned_spec_digest.has_value());
    MIRA_CHECK(tool_reference_to_string(reference) == "toolref:delta.render");

    // Round trip is lossless and canonical: parse(to_string(r)) == r.
    const auto reparsed = parse_tool_reference(tool_reference_to_string(reference));
    MIRA_CHECK(reparsed.has_value() && reparsed.value() == reference);
    MIRA_CHECK(tool_reference_to_string(reparsed.value()) == "toolref:delta.render");

    // Charset breadth: digits, '_' and '-' inside segments, multi-segment.
    for (const char *golden :
         {"toolref:delta.render", "toolref:a1_b2-c3.v2", "toolref:z9",
          "toolref:alpha_beta.gamma-1.delta"}) {
        const auto parsed = parse_tool_reference(golden);
        MIRA_CHECK(parsed.has_value());
        MIRA_CHECK(tool_reference_to_string(parsed.value()) == golden);
        MIRA_CHECK(parsed.value().mode == ToolReferenceMode::FollowLatest);
    }
    return 0;
}

int g1_pinned_golden_round_trip() {
    // Golden pinned form with a real spec digest (content-derived fixture).
    const std::string pinned_text = "toolref:epsilon.tool@" + kDigestPinned.to_string();
    const auto reference = must(parse_tool_reference(pinned_text), "parse pinned");
    MIRA_CHECK(reference.mode == ToolReferenceMode::PinnedDigest);
    MIRA_CHECK(reference.wire_name == "epsilon.tool");
    MIRA_CHECK(reference.pinned_spec_digest.has_value());
    MIRA_CHECK(reference.pinned_spec_digest.value() == kDigestPinned);
    MIRA_CHECK(tool_reference_to_string(reference) == pinned_text);

    const auto reparsed = parse_tool_reference(tool_reference_to_string(reference));
    MIRA_CHECK(reparsed.has_value() && reparsed.value() == reference);

    // All-zero digest is a valid 64-lowercase-hex pin.
    const std::string zeros = "toolref:w@" + std::string(64, '0');
    const auto zero_parsed = parse_tool_reference(zeros);
    MIRA_CHECK(zero_parsed.has_value() &&
               zero_parsed.value().pinned_spec_digest == Hash{});
    MIRA_CHECK(tool_reference_to_string(zero_parsed.value()) == zeros);
    return 0;
}

int g1_negative_matrix() {
    // Every entry is a near-miss of a valid form; each must fail closed with
    // the mira.tool_reference domain and the parse-failure domain code 1.
    const std::string hex64 = kDigestPinned.to_string();
    const std::vector<std::string> rejected = {
        // Scheme problems.
        "",
        "delta.render",
        "toolref",
        "toolref:",
        "toolrefs:delta.render",
        "Toolref:delta.render",
        "TOOLREF:delta.render",
        " toolref:delta.render",
        "toolref :delta.render",
        // Wire name charset violations.
        "toolref:delta render",
        "toolref:delta\trender",
        "toolref:delta\nrender",
        "toolref:Delta.render",
        "toolref:delta.render!",
        "toolref:delta/render",
        "toolref:delta..render",
        "toolref:.delta.render",
        "toolref:delta.render.",
        // Digest part violations.
        "toolref:delta.render@",
        "toolref:delta.render@" + hex64 + "x",
        "toolref:delta.render@" + hex64.substr(0, 63),
        "toolref:delta.render@" + hex64 + "00",
        "toolref:delta.render@A" + hex64.substr(1),
        "toolref:delta.render@F" + hex64.substr(1),
        "toolref:delta.render@DEADBEEF" + hex64.substr(8),
        "toolref:delta.render@g" + hex64.substr(1),
        "toolref:delta.render@" + hex64.substr(0, 32) + " " + hex64.substr(32, 31),
        "toolref:delta.render@" + hex64 + " ",
        "toolref:delta.render@aa@bb",
        "toolref:delta.render@" + hex64 + "@tail",
        // Length limits (reference > 256 bytes; wire > 128 bytes).
        "toolref:" + std::string(300, 'a'),
        "toolref:" + std::string(129, 'a'),
        "toolref:" + std::string(128, 'a') + "@" + hex64.substr(0, 63) + "zz",
    };
    for (const std::string &text : rejected) {
        const auto parsed = parse_tool_reference(text);
        MIRA_CHECK(!parsed.has_value());
        MIRA_CHECK(failed_in_reference_domain(parsed.error(), 1));
    }

    // Boundary controls: the exact limits are still accepted.
    MIRA_CHECK(parse_tool_reference("toolref:" + std::string(128, 'a')).has_value());
    MIRA_CHECK(parse_tool_reference("toolref:w@" + hex64).has_value());
    MIRA_CHECK(parse_tool_reference("toolref:" + std::string(128, 'a') + "@" + hex64).has_value());
    return 0;
}

int g1_mode_closed_set() {
    // The mode name space is a closed two-value set on the wire.
    const auto pinned = parse_tool_reference_mode("pinned_digest");
    const auto follow = parse_tool_reference_mode("follow_latest");
    MIRA_CHECK(pinned.has_value() && pinned.value() == ToolReferenceMode::PinnedDigest);
    MIRA_CHECK(follow.has_value() && follow.value() == ToolReferenceMode::FollowLatest);
    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::PinnedDigest) == "pinned_digest");
    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::FollowLatest) == "follow_latest");
    for (const char *unknown :
         {"PinnedDigest", "FollowLatest", "pinned", "follow", "latest", "", "pinned_digest "}) {
        const auto parsed = parse_tool_reference_mode(unknown);
        MIRA_CHECK(!parsed.has_value());
        MIRA_CHECK(failed_in_reference_domain(parsed.error(), 1));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G2: extraction and pinned observation (M7-TR0-G2)
// ---------------------------------------------------------------------------

int g2_registry_extraction_positive() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();
    MIRA_CHECK(view.size() == 3);
    const WorkflowDefinition definition = make_gate_definition();
    MIRA_CHECK(validate_workflow_definition(definition).has_value());

    // Default mode = pinned digest: every entry records the observed view digest.
    const WorkflowToolRefManifest pinned = extract_or_abort(definition, view);
    MIRA_CHECK(pinned.workflow_id == kWorkflowAlpha);
    MIRA_CHECK(pinned.definition_digest == workflow_definition_digest(definition));
    MIRA_CHECK(!pinned.empty());
    MIRA_CHECK(pinned.digest != Hash{});
    MIRA_CHECK(pinned.entries.size() == 3);
    // Output is sorted by step_id regardless of construction order.
    MIRA_CHECK(pinned.entries[0].step_id == fixed_step(kStepRender).to_string());
    MIRA_CHECK(pinned.entries[1].step_id == fixed_step(kStepLookup).to_string());
    MIRA_CHECK(pinned.entries[2].step_id == fixed_step(kStepTransform).to_string());
    for (const auto &entry : pinned.entries) {
        MIRA_CHECK(entry.mode == ToolReferenceMode::PinnedDigest);
        MIRA_CHECK(entry.pinned_spec_digest.has_value());
        bool digest_observed = false;
        for (const auto &spec : view) {
            if (spec.wire_name == entry.wire_name) {
                digest_observed = entry.pinned_spec_digest.value() == spec.spec_digest;
            }
        }
        MIRA_CHECK(digest_observed);
    }

    // Per-wire override wins over the default; follow entries carry no digest.
    ToolRefExtractionOptions overridden;
    overridden.default_mode = ToolReferenceMode::PinnedDigest;
    overridden.per_tool_modes = {{"delta.lookup", ToolReferenceMode::FollowLatest}};
    const WorkflowToolRefManifest mixed = extract_or_abort(definition, view, overridden);
    MIRA_CHECK(mixed.entries[0].mode == ToolReferenceMode::PinnedDigest); // delta.render
    MIRA_CHECK(mixed.entries[0].pinned_spec_digest.has_value());
    MIRA_CHECK(mixed.entries[1].mode == ToolReferenceMode::FollowLatest); // delta.lookup
    MIRA_CHECK(!mixed.entries[1].pinned_spec_digest.has_value());
    MIRA_CHECK(mixed.entries[2].mode == ToolReferenceMode::PinnedDigest);

    // Default mode = follow: no entry pins anything.
    ToolRefExtractionOptions follow_defaults;
    follow_defaults.default_mode = ToolReferenceMode::FollowLatest;
    const WorkflowToolRefManifest following = extract_or_abort(definition, view, follow_defaults);
    for (const auto &entry : following.entries) {
        MIRA_CHECK(entry.mode == ToolReferenceMode::FollowLatest);
        MIRA_CHECK(!entry.pinned_spec_digest.has_value());
    }
    MIRA_CHECK(following.digest != pinned.digest);

    // Replay binding: the manifest belongs to exactly this identity/content.
    MIRA_CHECK(
        verify_workflow_tool_refs(pinned, kWorkflowAlpha, workflow_definition_digest(definition))
            .has_value());
    return 0;
}

int g2_extraction_negative_matrix() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();

    // arguments["tool"] missing / non-string / charset violations / ghost wire.
    const JsonValue missing_tool = json_or_abort(R"json({"scene":"x"})json");
    const JsonValue integer_tool = json_or_abort(R"json({"tool":7})json");
    const JsonValue empty_tool = json_or_abort(R"json({"tool":""})json");
    const JsonValue uppercase_tool = json_or_abort(R"json({"tool":"delta.Render"})json");
    const JsonValue spaced_tool = json_or_abort(R"json({"tool":"delta lookup"})json");
    const JsonValue ghost_tool = json_or_abort(R"json({"tool":"delta.ghost"})json");
    for (const JsonValue &arguments :
         {missing_tool, integer_tool, empty_tool, uppercase_tool, spaced_tool, ghost_tool}) {
        const WorkflowDefinition definition =
            base_definition(kWorkflowAlpha, "negative", {toolcall_step(kStepRender, arguments)});
        const auto extracted = extract_workflow_tool_references(definition, view);
        MIRA_CHECK(!extracted.has_value());
        MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));
    }

    // A definition that fails IR structural validation (Verify step without a
    // verification predicate) rejects the whole group.
    {
        WorkflowDefinition broken = base_definition(
            kWorkflowAlpha, "broken",
            {toolcall_step(kStepRender, json_or_abort(R"json({"tool":"delta.lookup"})json")),
             verify_step(kStepLookup)});
        broken.steps[1].verification.reset();
        const auto extracted = extract_workflow_tool_references(broken, view);
        MIRA_CHECK(!extracted.has_value());
    }

    // Duplicate and empty per-wire override names fail closed.
    const WorkflowDefinition definition = make_gate_definition();
    const ToolRefExtractionOptions duplicate_override{
        ToolReferenceMode::PinnedDigest,
        {{"delta.lookup", ToolReferenceMode::FollowLatest},
         {"delta.lookup", ToolReferenceMode::PinnedDigest}}};
    auto extracted = extract_workflow_tool_references(definition, view, duplicate_override);
    MIRA_CHECK(!extracted.has_value());
    MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));

    const ToolRefExtractionOptions empty_override{ToolReferenceMode::PinnedDigest,
                                                  {{"", ToolReferenceMode::FollowLatest}}};
    extracted = extract_workflow_tool_references(definition, view, empty_override);
    MIRA_CHECK(!extracted.has_value());
    MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));

    // Duplicate wire names in the view reject defensively (no partial manifest).
    std::vector<ExposedToolSpec> duplicated_view = view;
    duplicated_view.push_back(make_spec("delta.lookup", R"json({"type":"object"})json",
                                        digest_string("m7-tr0/fixture/shadow"),
                                        fixed_tool("000000000000000000000000000000e9")));
    extracted = extract_workflow_tool_references(definition, duplicated_view);
    MIRA_CHECK(!extracted.has_value());
    MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));

    // Tightened limits are honored (RULE-08): entry cap and wire-name cap.
    ToolReferenceLimits tight_entries;
    tight_entries.max_entries = 1;
    extracted = extract_workflow_tool_references(definition, view, {}, tight_entries);
    MIRA_CHECK(!extracted.has_value());
    MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));

    ToolReferenceLimits tight_wire;
    tight_wire.max_wire_name_bytes = 4;
    extracted = extract_workflow_tool_references(definition, view, {}, tight_wire);
    MIRA_CHECK(!extracted.has_value());
    MIRA_CHECK(failed_in_reference_domain(extracted.error(), 2));
    return 0;
}

int g2_binding_and_empty_manifest() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();

    // Wrong workflow id and wrong definition digest are explicit failures.
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view);
    const auto wrong_id =
        verify_workflow_tool_refs(manifest, kWorkflowBeta, manifest.definition_digest);
    MIRA_CHECK(!wrong_id.has_value());
    MIRA_CHECK(failed_in_reference_domain(wrong_id.error(), 3));
    Sha256Digest tampered = manifest.definition_digest;
    tampered.bytes[0] = static_cast<std::uint8_t>(tampered.bytes[0] ^ 0xff);
    const auto wrong_digest = verify_workflow_tool_refs(manifest, kWorkflowAlpha, tampered);
    MIRA_CHECK(!wrong_digest.has_value());
    MIRA_CHECK(failed_in_reference_domain(wrong_digest.error(), 3));
    MIRA_CHECK(
        verify_workflow_tool_refs(manifest, kWorkflowAlpha, manifest.definition_digest).has_value());

    // A definition without ToolCall steps yields a defined empty manifest.
    const WorkflowDefinition no_tools =
        base_definition(kWorkflowAlpha, "fixture.no_tools", {verify_step(kStepLookup)});
    const WorkflowToolRefManifest empty_one = extract_or_abort(no_tools, view);
    const WorkflowToolRefManifest empty_two = extract_or_abort(no_tools, view);
    MIRA_CHECK(empty_one.empty());
    MIRA_CHECK(empty_one.entries.empty());
    MIRA_CHECK(empty_one.digest != Hash{});
    MIRA_CHECK(empty_one.digest == empty_two.digest);
    MIRA_CHECK(empty_one.workflow_id == empty_two.workflow_id);
    MIRA_CHECK(empty_one.definition_digest == empty_two.definition_digest);
    MIRA_CHECK(empty_one.entries == empty_two.entries);
    MIRA_CHECK(empty_one.definition_digest == workflow_definition_digest(no_tools));
    MIRA_CHECK(verify_workflow_tool_refs(empty_one, kWorkflowAlpha, empty_one.definition_digest)
                   .has_value());
    // The empty manifest serializes and survives its strict decode.
    const JsonValue empty_json = workflow_tool_refs_to_json(empty_one);
    const auto empty_decoded = must(workflow_tool_refs_from_json(empty_json), "empty decode");
    MIRA_CHECK(empty_decoded.entries == empty_one.entries);
    MIRA_CHECK(empty_decoded.digest == empty_one.digest);
    MIRA_CHECK(empty_decoded.definition_digest == empty_one.definition_digest);
    return 0;
}

// Re-serializes a manifest JSON with a consistent stored digest, so mutations
// reach the semantic checks instead of tripping the digest binding first.
JsonValue resign(const JsonValue &content) {
    JsonValue::Object members;
    for (const auto &member : *content.as_object()) {
        if (member.first != "digest") {
            members.emplace_back(member);
        }
    }
    members.emplace_back("digest",
                         JsonValue{canonical_json_digest(JsonValue{members}).to_string()});
    return JsonValue{std::move(members)};
}

// Rebuilds the manifest JSON with the entries array replaced.
JsonValue with_entries(const JsonValue &source, JsonValue::Array entries) {
    JsonValue::Object members;
    for (const auto &member : *source.as_object()) {
        if (member.first == "entries") {
            members.emplace_back("entries", JsonValue{std::move(entries)});
        } else {
            members.emplace_back(member);
        }
    }
    return JsonValue{std::move(members)};
}

int g2_json_round_trip() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();
    ToolRefExtractionOptions mixed;
    mixed.default_mode = ToolReferenceMode::PinnedDigest;
    mixed.per_tool_modes = {{"delta.lookup", ToolReferenceMode::FollowLatest}};
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view, mixed);

    const JsonValue encoded = workflow_tool_refs_to_json(manifest);
    MIRA_CHECK(encoded.is_object());
    const auto decoded = must(workflow_tool_refs_from_json(encoded), "manifest decode");
    MIRA_CHECK(decoded.entries == manifest.entries);
    MIRA_CHECK(decoded.digest == manifest.digest);
    MIRA_CHECK(decoded.workflow_id == manifest.workflow_id);
    MIRA_CHECK(decoded.definition_digest == manifest.definition_digest);
    MIRA_CHECK(canonical_json_string(workflow_tool_refs_to_json(decoded)) ==
               canonical_json_string(encoded));

    const auto rejects = [](const JsonValue &value) {
        const auto result = workflow_tool_refs_from_json(value);
        return !result.has_value() && failed_in_reference_domain(result.error(), 1);
    };

    // Unknown root field fails closed.
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
                                     ? JsonValue{std::string("mira.workflow.tool_refs.v2")}
                                     : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Stored digest no longer matches content.
    {
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first,
                                 member.first == "digest" ? JsonValue{std::string(64, '0')}
                                                          : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    // Unknown entry field.
    {
        JsonValue::Array entries = *encoded.find("entries")->as_array();
        JsonValue::Object first;
        for (const auto &member : *entries.front().as_object()) {
            first.emplace_back(member);
        }
        first.emplace_back("current_spec_digest", JsonValue{std::string(64, 'a')});
        entries.front() = JsonValue{std::move(first)};
        MIRA_CHECK(rejects(resign(with_entries(encoded, std::move(entries)))));
    }
    // A follow entry must not carry a pinned digest (digest kept consistent so
    // the closed-shape rule is what rejects).
    {
        ToolRefExtractionOptions follow_defaults;
        follow_defaults.default_mode = ToolReferenceMode::FollowLatest;
        const WorkflowToolRefManifest following =
            extract_or_abort(definition, view, follow_defaults);
        const JsonValue following_json = workflow_tool_refs_to_json(following);
        MIRA_CHECK(workflow_tool_refs_from_json(following_json).has_value());
        JsonValue::Array entries = *following_json.find("entries")->as_array();
        JsonValue::Object first;
        for (const auto &member : *entries.front().as_object()) {
            first.emplace_back(member);
        }
        first.emplace_back("pinned_spec_digest", JsonValue{kDigestPinned.to_string()});
        entries.front() = JsonValue{std::move(first)};
        MIRA_CHECK(rejects(resign(with_entries(following_json, std::move(entries)))));
    }
    // A pinned entry without its digest is rejected.
    {
        JsonValue::Array entries = *encoded.find("entries")->as_array();
        JsonValue::Object first;
        for (const auto &member : *entries.front().as_object()) {
            if (member.first != "pinned_spec_digest") {
                first.emplace_back(member);
            }
        }
        entries.front() = JsonValue{std::move(first)};
        MIRA_CHECK(rejects(resign(with_entries(encoded, std::move(entries)))));
    }
    // Entries must be an array; the workflow id must parse as an id.
    {
        JsonValue::Object entries_not_array;
        for (const auto &member : *encoded.as_object()) {
            if (member.first == "entries") {
                entries_not_array.emplace_back("entries", JsonValue{JsonValue::Object{}});
            } else {
                entries_not_array.emplace_back(member);
            }
        }
        MIRA_CHECK(rejects(resign(JsonValue{std::move(entries_not_array)})));
        JsonValue::Object mutated;
        for (const auto &member : *encoded.as_object()) {
            mutated.emplace_back(member.first,
                                 member.first == "workflow_id"
                                     ? JsonValue{std::string("not-an-id")}
                                     : member.second);
        }
        MIRA_CHECK(rejects(JsonValue{std::move(mutated)}));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G3: resolution matrix and state aggregation (M7-TR0-G3)
// ---------------------------------------------------------------------------

int g3_resolution_matrix() {
    const ToolId tool_one = fixed_tool("000000000000000000000000000000d1");
    const ToolId tool_two = fixed_tool("000000000000000000000000000000d2");

    // Pinned x present x same digest -> Resolved.
    {
        const ExposedToolSpec current =
            make_spec("epsilon.tool", R"json({"type":"object"})json", kDigestOne, tool_one);
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool"})json"), R"json({"type":"object"})json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::Resolved);
        MIRA_CHECK(result.entry.pinned_spec_digest == kDigestOne);
        MIRA_CHECK(result.entry.current_spec_digest == kDigestOne);
        MIRA_CHECK(result.entry.detail.empty());
        MIRA_CHECK(result.state == WorkflowToolCompatState::Runnable);
    }
    // Pinned x present x evolved digest x skeleton still binds ->
    // EvolvedCompatible.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "integer"}},
                "required": ["scene"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two, SemanticVersion{2, 0, 0});
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool","scene":{"$param":"scene"}})json"),
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "string"}},
                "required": ["scene"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(result.entry.pinned_spec_digest == kDigestOne);
        MIRA_CHECK(result.entry.current_spec_digest == kDigestTwo);
        MIRA_CHECK(result.entry.detail.empty());
        MIRA_CHECK(result.state == WorkflowToolCompatState::Degraded);
    }
    // Pinned x present x evolved digest x skeleton no longer binds ->
    // EvolvedIncompatible with a bounded detail.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "integer"}, "extra": {"type": "string"}},
                "required": ["scene", "extra"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool","scene":"s"})json"),
            R"json({
                "type": "object",
                "properties": {"scene": {"type": "string"}},
                "required": ["scene"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
        MIRA_CHECK(!result.entry.detail.empty());
        MIRA_CHECK(result.entry.detail.size() <=
                   kDefaultToolReferenceLimits.max_detail_bytes + 3);
        MIRA_CHECK(result.state == WorkflowToolCompatState::Invalid);
    }
    // Pinned x absent -> Unresolved (no current digest echoed).
    {
        const MatrixResult result =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool"})json"),
                           R"json({"type":"object"})json", nullptr);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::Unresolved);
        MIRA_CHECK(!result.entry.current_spec_digest.has_value());
        MIRA_CHECK(result.state == WorkflowToolCompatState::Invalid);
    }
    // Follow x present (any digest) -> Resolved to whatever is current.
    {
        const ExposedToolSpec publish =
            make_spec("epsilon.tool", R"json({"type":"object"})json", kDigestOne, tool_one);
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({"type":"object","properties":{"scene":{"type":"string"}}})json", kDigestTwo,
            tool_two, SemanticVersion{2, 0, 0});
        const MatrixResult result = project_single_follow(publish, &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::Resolved);
        MIRA_CHECK(result.entry.mode == ToolReferenceMode::FollowLatest);
        MIRA_CHECK(!result.entry.pinned_spec_digest.has_value());
        MIRA_CHECK(result.entry.current_spec_digest == kDigestTwo);
        MIRA_CHECK(result.state == WorkflowToolCompatState::Runnable);
    }
    // Follow x absent -> Unresolved.
    {
        const ExposedToolSpec publish =
            make_spec("epsilon.tool", R"json({"type":"object"})json", kDigestOne, tool_one);
        const MatrixResult result = project_single_follow(publish, nullptr);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::Unresolved);
        MIRA_CHECK(result.state == WorkflowToolCompatState::Invalid);
    }
    return 0;
}

int g3_state_aggregation() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> publish_view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);

    // All resolved -> Runnable.
    const auto projection =
        must(project_workflow_tool_compatibility(definition, manifest, publish_view), "project");
    MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
    MIRA_CHECK(projection.digest != Hash{});
    MIRA_CHECK(projection.entries.size() == 3);
    for (const auto &entry : projection.entries) {
        MIRA_CHECK(entry.status == ToolReferenceCompat::Resolved);
    }
    return 0;
}

int g3_aggregation_branches() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> publish_view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);

    // Degraded: one compatible evolution (echo.transform changes content so
    // its digest moves while the recorded skeleton still binds).
    std::vector<ExposedToolSpec> degraded_view = publish_view;
    for (auto &spec : degraded_view) {
        if (spec.wire_name == "echo.transform") {
            spec.spec_digest = digest_string("m7-tr0/fixture/echo.transform/v3");
            spec.description = "fixture transform tool (evolved, bindable)";
        }
    }
    {
        const auto projection = must(
            project_workflow_tool_compatibility(definition, manifest, degraded_view), "degraded");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);
        std::size_t compatible = 0;
        std::size_t resolved = 0;
        for (const auto &entry : projection.entries) {
            if (entry.status == ToolReferenceCompat::EvolvedCompatible) {
                ++compatible;
                MIRA_CHECK(entry.detail.empty());
            } else if (entry.status == ToolReferenceCompat::Resolved) {
                ++resolved;
            }
        }
        MIRA_CHECK(compatible == 1);
        MIRA_CHECK(resolved == 2);
    }

    // Invalid via one unresolved entry even alongside a compatible evolution
    // (Invalid dominates Degraded).
    {
        std::vector<ExposedToolSpec> invalid_view;
        for (const auto &spec : degraded_view) {
            if (spec.wire_name != "delta.lookup") {
                invalid_view.push_back(spec);
            }
        }
        const auto projection = must(
            project_workflow_tool_compatibility(definition, manifest, invalid_view), "invalid");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Invalid);
        bool saw_unresolved = false;
        bool saw_compatible = false;
        for (const auto &entry : projection.entries) {
            saw_unresolved |= entry.status == ToolReferenceCompat::Unresolved;
            saw_compatible |= entry.status == ToolReferenceCompat::EvolvedCompatible;
        }
        MIRA_CHECK(saw_unresolved);
        MIRA_CHECK(saw_compatible);
    }

    // Invalid via one evolved-incompatible entry (required payload appears).
    {
        std::vector<ExposedToolSpec> incompatible_view = publish_view;
        for (auto &spec : incompatible_view) {
            if (spec.wire_name == "echo.transform") {
                spec.spec_digest = digest_string("m7-tr0/fixture/echo.transform/v4");
                spec.parameters_schema = JsonSchema{json_or_abort(R"json({
                    "type": "object",
                    "properties": {"payload": {"type": "string"}},
                    "required": ["payload"],
                    "additionalProperties": false
                })json")};
            }
        }
        const auto projection =
            must(project_workflow_tool_compatibility(definition, manifest, incompatible_view),
                 "incompatible");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Invalid);
        for (const auto &entry : projection.entries) {
            if (entry.wire_name == "echo.transform") {
                MIRA_CHECK(entry.status == ToolReferenceCompat::EvolvedIncompatible);
                MIRA_CHECK(entry.detail.find("required") != std::string::npos);
            }
        }
    }

    // Empty manifest -> Runnable with a defined digest.
    {
        const WorkflowDefinition no_tools =
            base_definition(kWorkflowAlpha, "fixture.no_tools", {verify_step(kStepLookup)});
        const WorkflowToolRefManifest empty = extract_or_abort(no_tools, publish_view);
        const auto projection =
            must(project_workflow_tool_compatibility(no_tools, empty, publish_view),
                 "empty project");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
        MIRA_CHECK(projection.empty());
        MIRA_CHECK(projection.digest != Hash{});
        MIRA_CHECK(admit_workflow_run_by_tool_compat(projection).admitted);
    }
    return 0;
}

// Builds a manifest whose stored digest matches its (forged) content, so the
// projection binding check is the gate that catches the step mismatch.
WorkflowToolRefManifest forge_step_id(const WorkflowToolRefManifest &manifest,
                                      std::size_t entry_index, const std::string &new_step_id) {
    JsonValue encoded = workflow_tool_refs_to_json(manifest);
    JsonValue::Array entries = *encoded.find("entries")->as_array();
    entries.at(entry_index).set("step_id", JsonValue{new_step_id});
    return must(workflow_tool_refs_from_json(resign(with_entries(encoded, std::move(entries)))),
                "forge manifest");
}

// An empty-entries manifest bound to an arbitrary workflow identity and
// definition digest (exercising the projection-side structural gate).
WorkflowToolRefManifest forge_bound_empty(const WorkflowId &workflow_id,
                                          const Sha256Digest &definition_digest) {
    JsonValue::Object content;
    content.emplace_back("schema", JsonValue{std::string(kWorkflowToolRefsSchema)});
    content.emplace_back("workflow_id", JsonValue{workflow_id.to_string()});
    content.emplace_back("definition_digest", JsonValue{definition_digest.to_string()});
    content.emplace_back("entries", JsonValue{JsonValue::Array{}});
    content.emplace_back("digest",
                         JsonValue{canonical_json_digest(JsonValue{content}).to_string()});
    return must(workflow_tool_refs_from_json(JsonValue{std::move(content)}),
                "forge bound empty manifest");
}

int g3_input_mismatch_rejected() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view);

    // Same workflow id, different definition content -> digest mismatch.
    WorkflowDefinition renamed = definition;
    renamed.name = "fixture.alpha.renamed";
    MIRA_CHECK(workflow_definition_digest(renamed) != manifest.definition_digest);
    auto rejected = project_workflow_tool_compatibility(renamed, manifest, view);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(failed_in_reference_domain(rejected.error(), 3));

    // Different workflow id -> binding mismatch.
    WorkflowDefinition other_id = definition;
    other_id.workflow_id = kWorkflowBeta;
    const WorkflowToolRefManifest other_manifest = extract_or_abort(other_id, view);
    rejected = project_workflow_tool_compatibility(definition, other_manifest, view);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(failed_in_reference_domain(rejected.error(), 3));

    // Manifest entry pointing at a step the definition does not contain. The
    // forged id keeps the entry's sorted position so the stored digest stays
    // consistent; the projection only walks steps for digest mismatches, so
    // the forged entry is projected against an evolved echo.transform view to
    // reach the skeleton path that catches the fault.
    std::vector<ExposedToolSpec> evolved_view = view;
    for (auto &spec : evolved_view) {
        if (spec.wire_name == "echo.transform") {
            spec.spec_digest = digest_string("m7-tr0/fixture/echo.transform/v3");
        }
    }
    const WorkflowToolRefManifest forged =
        forge_step_id(manifest, 2, "000000000000000000000000000000c1");
    rejected = project_workflow_tool_compatibility(definition, forged, evolved_view);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(failed_in_reference_domain(rejected.error(), 3));

    // A definition that fails structural validation is rejected at projection
    // time as well (the forged manifest carries the matching binding).
    {
        WorkflowDefinition broken =
            base_definition(kWorkflowAlpha, "broken", {verify_step(kStepRender)});
        broken.steps.front().verification.reset();
        const WorkflowToolRefManifest forged_binding =
            forge_bound_empty(kWorkflowAlpha, workflow_definition_digest(broken));
        rejected = project_workflow_tool_compatibility(broken, forged_binding, view);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_reference_domain(rejected.error(), 3));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G4: skeleton bindability and determinism (M7-TR0-G4)
// ---------------------------------------------------------------------------

int g4_placeholder_instantiation_compatible() {
    const ToolId tool_two = fixed_tool("000000000000000000000000000000d2");

    // $param position type change: string -> integer stays compatible because
    // the placeholder materializes from the CURRENT schema at its position.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"q": {"type": "integer"}},
                "required": ["q"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool","q":{"$param":"v"}})json"),
            R"json({
                "type": "object",
                "properties": {"q": {"type": "string"}},
                "required": ["q"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(result.entry.detail.empty());
    }
    // Nested object and array recursion: placeholders deep in the tree
    // materialize per position.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {
                    "cfg": {
                        "type": "object",
                        "properties": {"name": {"type": "integer"}},
                        "required": ["name"],
                        "additionalProperties": false
                    },
                    "tags": {"type": "array", "items": {"type": "integer"}, "minItems": 1}
                },
                "required": ["cfg", "tags"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result = project_single(
            json_or_abort(
                R"json({"tool":"epsilon.tool","cfg":{"name":{"$param":"n"}},"tags":[{"$param":"t"}]})json"),
            R"json({
                "type": "object",
                "properties": {
                    "cfg": {
                        "type": "object",
                        "properties": {"name": {"type": "string"}},
                        "required": ["name"],
                        "additionalProperties": false
                    },
                    "tags": {"type": "array", "items": {"type": "string"}, "minItems": 1}
                },
                "required": ["cfg", "tags"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(result.entry.detail.empty());
    }
    // An array placeholder materializes from the current schema including a
    // raised minItems (bounded growth), so it still binds.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"tags": {"type": "array", "items": {"type": "integer"}, "minItems": 2}},
                "required": ["tags"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool","tags":{"$param":"list"}})json"),
            R"json({
                "type": "object",
                "properties": {"tags": {"type": "array", "items": {"type": "string"}}},
                "required": ["tags"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(result.entry.detail.empty());
    }
    // An enum placeholder picks the first member of the CURRENT enum, so a
    // narrowing enum alone keeps the skeleton bindable.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"mode": {"enum": ["turbo"]}},
                "required": ["mode"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result = project_single(
            json_or_abort(R"json({"tool":"epsilon.tool","mode":{"$param":"m"}})json"),
            R"json({
                "type": "object",
                "properties": {"mode": {"enum": ["fast", "slow"]}},
                "required": ["mode"],
                "additionalProperties": false
            })json",
            &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedCompatible);
        MIRA_CHECK(result.entry.detail.empty());
    }
    return 0;
}

int g4_skeleton_incompatible_matrix() {
    const ToolId tool_two = fixed_tool("000000000000000000000000000000d2");

    // New required property the recorded arguments never carry.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"q": {"type": "string"}, "extra": {"type": "integer"}},
                "required": ["q", "extra"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool","q":"x"})json"),
                           R"json({
                               "type": "object",
                               "properties": {"q": {"type": "string"}},
                               "required": ["q"],
                               "additionalProperties": false
                           })json",
                           &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
        MIRA_CHECK(result.entry.detail.find("required") != std::string::npos);
        MIRA_CHECK(result.state == WorkflowToolCompatState::Invalid);
    }
    // Concrete value type tightened: recorded integer no longer binds. The
    // detail equals the first violation of the shared strict validator.
    {
        const std::string evolved_schema = R"json({
            "type": "object",
            "properties": {"q": {"type": "string"}},
            "required": ["q"],
            "additionalProperties": false
        })json";
        const ExposedToolSpec current =
            make_spec("epsilon.tool", evolved_schema, kDigestTwo, tool_two);
        const std::string published_schema = R"json({
            "type": "object",
            "properties": {"q": {"type": "integer"}},
            "required": ["q"],
            "additionalProperties": false
        })json";
        const MatrixResult result =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool","q":5})json"),
                           published_schema, &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
        const auto violations = validate_instance_against_schema(
            json_or_abort(R"json({"q":5})json"), JsonSchema{json_or_abort(evolved_schema)});
        MIRA_CHECK(!violations.empty());
        const std::string expected =
            "path '" + violations.front().path + "' keyword '" + violations.front().keyword + "'";
        MIRA_CHECK(result.entry.detail == expected);
        MIRA_CHECK(violations.front().keyword == "type");
    }
    // Enum narrowed so the recorded concrete value is no longer a member.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {"mode": {"enum": ["turbo"]}},
                "required": ["mode"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool","mode":"fast"})json"),
                           R"json({
                               "type": "object",
                               "properties": {"mode": {"enum": ["fast", "slow"]}},
                               "required": ["mode"],
                               "additionalProperties": false
                           })json",
                           &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
        MIRA_CHECK(result.entry.detail.find("enum") != std::string::npos);
        MIRA_CHECK(result.entry.detail.find("mode") != std::string::npos);
    }
    // Nested concrete value type change deep in the object tree.
    {
        const ExposedToolSpec current = make_spec(
            "epsilon.tool",
            R"json({
                "type": "object",
                "properties": {
                    "cfg": {
                        "type": "object",
                        "properties": {"name": {"type": "integer"}},
                        "required": ["name"],
                        "additionalProperties": false
                    }
                },
                "required": ["cfg"],
                "additionalProperties": false
            })json",
            kDigestTwo, tool_two);
        const MatrixResult result =
            project_single(json_or_abort(R"json({"tool":"epsilon.tool","cfg":{"name":"x"}})json"),
                           R"json({
                               "type": "object",
                               "properties": {
                                   "cfg": {
                                       "type": "object",
                                       "properties": {"name": {"type": "string"}},
                                       "required": ["name"],
                                       "additionalProperties": false
                                   }
                               },
                               "required": ["cfg"],
                               "additionalProperties": false
                           })json",
                           &current);
        MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
        MIRA_CHECK(result.entry.detail.find("cfg.name") != std::string::npos);
        MIRA_CHECK(result.entry.detail.find("type") != std::string::npos);
    }
    return 0;
}

int g4_detail_bounded_and_first() {
    // A violation path longer than the detail bound truncates with an ellipsis
    // marker and never carries unbounded schema content.
    const std::string long_property(300, 'p');
    const std::string evolved_schema = R"json({
        "type": "object",
        "properties": {")json" + long_property + R"json(": {"type": "string"}},
        "required": [")json" + long_property + R"json("],
        "additionalProperties": false
    })json";
    const ToolId tool_two = fixed_tool("000000000000000000000000000000d2");
    const ExposedToolSpec current =
        make_spec("epsilon.tool", evolved_schema, kDigestTwo, tool_two);
    const MatrixResult result =
        project_single(json_or_abort(R"json({"tool":"epsilon.tool","p":1})json"),
                       R"json({
                           "type": "object",
                           "properties": {"p": {"type": "integer"}},
                           "required": ["p"],
                           "additionalProperties": false
                       })json",
                       &current);
    // "p" is not in the evolved properties and additionalProperties is false,
    // so the skeleton violates regardless of the long required property.
    MIRA_CHECK(result.entry.status == ToolReferenceCompat::EvolvedIncompatible);
    MIRA_CHECK(result.entry.detail.size() <= kDefaultToolReferenceLimits.max_detail_bytes + 3);
    MIRA_CHECK(result.entry.detail.find("path") != std::string::npos);
    MIRA_CHECK(result.entry.detail.find("keyword") != std::string::npos);
    return 0;
}

int g4_projection_determinism_and_guards() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> publish_view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);

    // Degraded view (one compatible evolution) projected twice: identical
    // projections and digests.
    std::vector<ExposedToolSpec> degraded_view = publish_view;
    for (auto &spec : degraded_view) {
        if (spec.wire_name == "echo.transform") {
            spec.spec_digest = digest_string("m7-tr0/fixture/echo.transform/v3");
        }
    }
    const auto first =
        must(project_workflow_tool_compatibility(definition, manifest, degraded_view), "first");
    const auto second =
        must(project_workflow_tool_compatibility(definition, manifest, degraded_view), "second");
    MIRA_CHECK(first.digest == second.digest);
    MIRA_CHECK(first.state == second.state);
    MIRA_CHECK(first.entries == second.entries);
    MIRA_CHECK(workflow_tool_compat_to_json(first) == workflow_tool_compat_to_json(second));

    // Duplicate wire names in the projection view reject the whole group.
    std::vector<ExposedToolSpec> duplicated = publish_view;
    duplicated.push_back(make_spec("delta.render", R"json({"type":"object"})json",
                                   digest_string("m7-tr0/fixture/shadow"),
                                   fixed_tool("000000000000000000000000000000e8")));
    const auto rejected = project_workflow_tool_compatibility(definition, manifest, duplicated);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(failed_in_reference_domain(rejected.error(), 3));

    // Tightened entry bound rejects an over-limit manifest.
    ToolReferenceLimits tight;
    tight.max_entries = 1;
    const auto over =
        project_workflow_tool_compatibility(definition, manifest, publish_view, tight);
    MIRA_CHECK(!over.has_value());
    MIRA_CHECK(failed_in_reference_domain(over.error(), 3));
    return 0;
}

int g4_report_is_deterministic() {
    const std::string first = build_reference_report();
    const std::string second = build_reference_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// G5: admission decisions, audit redaction, DEC-015 combination (M7-TR0-G5)
// ---------------------------------------------------------------------------

int g5_admission_decisions() {
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> publish_view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);

    // Runnable -> admitted, no reason.
    {
        const auto projection =
            must(project_workflow_tool_compatibility(definition, manifest, publish_view),
                 "runnable");
        const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(projection);
        MIRA_CHECK(decision.admitted);
        MIRA_CHECK(decision.state == WorkflowToolCompatState::Runnable);
        MIRA_CHECK(decision.reason.empty());
    }
    // Degraded -> admitted, no rejection reason.
    {
        std::vector<ExposedToolSpec> degraded_view = publish_view;
        for (auto &spec : degraded_view) {
            if (spec.wire_name == "echo.transform") {
                spec.spec_digest = digest_string("m7-tr0/fixture/echo.transform/v3");
            }
        }
        const auto projection =
            must(project_workflow_tool_compatibility(definition, manifest, degraded_view),
                 "degraded");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);
        const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(projection);
        MIRA_CHECK(decision.admitted);
        MIRA_CHECK(decision.state == WorkflowToolCompatState::Degraded);
        MIRA_CHECK(decision.reason.empty());
    }
    // Invalid -> rejected with a deterministic reason naming the FIRST
    // offending entry in step_id order.
    {
        // Step b1 (render) -> delta.render vanishes; step b2 (lookup) ->
        // delta.lookup evolves incompatibly; step b3 stays resolved.
        std::vector<ExposedToolSpec> invalid_view;
        for (const auto &spec : publish_view) {
            if (spec.wire_name == "delta.render") {
                continue;
            }
            if (spec.wire_name == "delta.lookup") {
                ExposedToolSpec evolved = spec;
                evolved.spec_digest = digest_string("m7-tr0/fixture/delta.lookup/v9");
                evolved.parameters_schema = JsonSchema{json_or_abort(R"json({
                    "type": "object",
                    "properties": {"key": {"type": "integer"}},
                    "required": ["key"],
                    "additionalProperties": false
                })json")};
                invalid_view.push_back(evolved);
                continue;
            }
            invalid_view.push_back(spec);
        }
        const auto projection =
            must(project_workflow_tool_compatibility(definition, manifest, invalid_view),
                 "invalid");
        MIRA_CHECK(projection.state == WorkflowToolCompatState::Invalid);
        const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(projection);
        MIRA_CHECK(!decision.admitted);
        MIRA_CHECK(decision.state == WorkflowToolCompatState::Invalid);
        // First offender in step order is b1 (render, unresolved).
        const std::string first_step = fixed_step(kStepRender).to_string();
        const std::string second_step = fixed_step(kStepLookup).to_string();
        MIRA_CHECK(decision.reason.find(first_step) != std::string::npos);
        MIRA_CHECK(decision.reason.find("unresolved") != std::string::npos);
        MIRA_CHECK(decision.reason.find(second_step) == std::string::npos);
        MIRA_CHECK(decision.reason.size() <= kDefaultToolReferenceLimits.max_detail_bytes + 3);
    }
    return 0;
}

int g5_audit_projection_redaction() {
    // Registry-backed degraded scenario carrying redaction markers in the
    // fields the audit artifact must never carry.
    BuiltinToolRegistry registry;
    fill_registry(registry, std::vector<RegistryTool>{
        {{"gamma.render", "confidential-description-marker super-secret-token", R"json({
              "type": "object",
              "properties": {"scene": {"type": "string"}},
              "required": ["scene"],
              "additionalProperties": false,
              "title": "alpha_marker_secret_prop"
          })json",
          true, "000000000000000000000000000000f1"}}});
    const std::vector<ExposedToolSpec> publish_view = registry.exposed_tools();
    const WorkflowDefinition definition = base_definition(
        kWorkflowAlpha, "fixture.render",
        {toolcall_step(kStepRender, json_or_abort(
                                        R"json({"tool":"gamma.render","scene":{"$param":"scene"}})json"))});
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, publish_view);

    std::vector<ExposedToolSpec> degraded_view = publish_view;
    degraded_view.front().spec_digest = digest_string("m7-tr0/fixture/gamma.render/v2");
    degraded_view.front().version = SemanticVersion{2, 0, 0};
    // scene stays bindable (string -> integer placeholder materializes to 0).
    degraded_view.front().parameters_schema = JsonSchema{json_or_abort(R"json({
        "type": "object",
        "properties": {"scene": {"type": "integer"}},
        "required": ["scene"],
        "additionalProperties": false
    })json")};
    const auto projection =
        must(project_workflow_tool_compatibility(definition, manifest, degraded_view),
             "degraded");
    MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);

    const JsonValue audit = workflow_tool_compat_to_json(projection);
    MIRA_CHECK(audit.is_object());
    const std::string serialized = canonical_json_string(audit);
    // Schema identity and digest binding.
    const auto *schema = audit.find("schema");
    MIRA_CHECK(schema != nullptr && schema->is_string() &&
               *schema->as_string() == std::string(kWorkflowToolCompatSchema));
    const auto *digest = audit.find("digest");
    MIRA_CHECK(digest != nullptr && digest->is_string() &&
               *digest->as_string() == projection.digest.to_string());
    // Redaction: no description text, no schema body, no secrets.
    for (const char *marker : {"confidential-description-marker", "super-secret-token",
                               "alpha_marker_secret_prop", "description",
                               "parameters_schema", "title"}) {
        MIRA_CHECK(serialized.find(marker) == std::string::npos);
    }
    // Closed root field set: identities, digests, state, entries, digest only.
    for (const auto &member : *audit.as_object()) {
        const bool known = member.first == "schema" || member.first == "workflow_id" ||
                           member.first == "definition_digest" || member.first == "state" ||
                           member.first == "entries" || member.first == "digest";
        MIRA_CHECK(known);
    }
    // Entries carry only identity/status/detail fields.
    const auto *entries = audit.find("entries");
    MIRA_CHECK(entries != nullptr && entries->is_array() && !entries->as_array()->empty());
    for (const auto &entry : *entries->as_array()) {
        for (const auto &member : *entry.as_object()) {
            const bool known = member.first == "step_id" || member.first == "wire_name" ||
                               member.first == "mode" || member.first == "status" ||
                               member.first == "pinned_spec_digest" ||
                               member.first == "current_spec_digest" || member.first == "detail";
            MIRA_CHECK(known);
        }
    }
    // The incompatible variant carries a bounded detail and never any marker.
    std::vector<ExposedToolSpec> invalid_view = degraded_view;
    invalid_view.front().parameters_schema = JsonSchema{json_or_abort(R"json({
        "type": "object",
        "properties": {"scene": {"type": "integer"}, "extra": {"type": "integer"}},
        "required": ["scene", "extra"],
        "additionalProperties": false
    })json")};
    const auto invalid_projection =
        must(project_workflow_tool_compatibility(definition, manifest, invalid_view), "invalid");
    MIRA_CHECK(invalid_projection.state == WorkflowToolCompatState::Invalid);
    const std::string invalid_serialized =
        canonical_json_string(workflow_tool_compat_to_json(invalid_projection));
    MIRA_CHECK(invalid_serialized.find("\"detail\"") != std::string::npos);
    for (const auto &entry : invalid_projection.entries) {
        MIRA_CHECK(entry.detail.size() <= kDefaultToolReferenceLimits.max_detail_bytes + 3);
    }
    MIRA_CHECK(invalid_serialized.find("confidential-description-marker") == std::string::npos);
    MIRA_CHECK(invalid_serialized.find("alpha_marker_secret_prop") == std::string::npos);
    return 0;
}

int g5_dec015_execution_gate_combination() {
    // A workflow admitted as Degraded gets no execution-time exemption: the
    // DEC-015 identity checks reject stale proposals even though the
    // compatibility projection admitted the run.
    BuiltinToolRegistry registry_v1;
    fill_registry(registry_v1, std::vector<RegistryTool>{
        {{"gamma.render", "render fixture v1", R"json({
              "type": "object",
              "properties": {"scene": {"type": "string"}},
              "required": ["scene"],
              "additionalProperties": false
          })json",
          true, "000000000000000000000000000000f1"}}});
    const ToolId tool_v1 = fixed_tool("000000000000000000000000000000f1");
    const std::vector<ExposedToolSpec> view_v1 = registry_v1.exposed_tools();

    const WorkflowDefinition definition = base_definition(
        kWorkflowAlpha, "fixture.render",
        {toolcall_step(kStepRender, json_or_abort(
                                        R"json({"tool":"gamma.render","scene":{"$param":"scene"}})json"))});
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view_v1);
    MIRA_CHECK(manifest.entries.size() == 1);
    const Hash pinned_digest = manifest.entries.front().pinned_spec_digest.value();

    // The tool evolves in the exposed view (same wire, new identity, new
    // schema): the skeleton still binds, so admission degrades to Degraded.
    BuiltinToolRegistry registry_v2;
    fill_registry(registry_v2, std::vector<RegistryTool>{
        {{"gamma.render", "render fixture v2", R"json({
              "type": "object",
              "properties": {"scene": {"type": "integer"}},
              "required": ["scene"],
              "additionalProperties": false
          })json",
          true, "000000000000000000000000000000f2", SemanticVersion{2, 0, 0}}}});
    const ToolId tool_v2 = fixed_tool("000000000000000000000000000000f2");
    const std::vector<ExposedToolSpec> view_v2 = registry_v2.exposed_tools();
    MIRA_CHECK(view_v2.front().spec_digest != pinned_digest);
    const auto projection =
        must(project_workflow_tool_compatibility(definition, manifest, view_v2),
             "degraded project");
    MIRA_CHECK(projection.state == WorkflowToolCompatState::Degraded);
    const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(projection);
    MIRA_CHECK(decision.admitted); // admission passes...

    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();

    // ...but the stale proposal (old tool id) is rejected by the registry.
    {
        ToolProposal stale;
        stale.provider_call_id = ProviderToolCallId{"call_stale_id"};
        stale.tool_id = tool_v1;
        stale.wire_name = "gamma.render";
        stale.tool_version = SemanticVersion{1, 0, 0};
        stale.arguments = json_or_abort(R"json({"scene":"s"})json");
        stale.arguments_digest = digest_string(to_json_string(stale.arguments));
        stale.operation_id = OperationId::generate();
        stale.has_side_effects = true;
        const auto outcome = registry_v2.execute(stale, context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::NotFound);
    }
    // A proposal with the new id but the stale exposed version is rejected as
    // an identity mismatch.
    {
        ToolProposal stale_version;
        stale_version.provider_call_id = ProviderToolCallId{"call_stale_version"};
        stale_version.tool_id = tool_v2;
        stale_version.wire_name = "gamma.render";
        stale_version.tool_version = SemanticVersion{1, 0, 0};
        stale_version.arguments = json_or_abort(R"json({"scene":0})json");
        stale_version.arguments_digest = digest_string(to_json_string(stale_version.arguments));
        stale_version.operation_id = OperationId::generate();
        stale_version.has_side_effects = true;
        const auto outcome = registry_v2.execute(stale_version, context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::InvalidState);
    }
    // And if the tool disappears entirely, dispatch fails closed as well.
    {
        BuiltinToolRegistry registry_v3;
        ToolProposal removed;
        removed.provider_call_id = ProviderToolCallId{"call_removed"};
        removed.tool_id = tool_v2;
        removed.wire_name = "gamma.render";
        removed.tool_version = SemanticVersion{2, 0, 0};
        removed.arguments = json_or_abort(R"json({"scene":0})json");
        removed.arguments_digest = digest_string(to_json_string(removed.arguments));
        removed.operation_id = OperationId::generate();
        removed.has_side_effects = true;
        const auto outcome = registry_v3.execute(removed, context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::NotFound);
    }
    // Positive control: the CURRENT identity dispatches fine on registry_v2,
    // so the rejections above are identity-driven, not capability-driven.
    {
        ToolProposal current;
        current.provider_call_id = ProviderToolCallId{"call_current"};
        current.tool_id = tool_v2;
        current.wire_name = "gamma.render";
        current.tool_version = SemanticVersion{2, 0, 0};
        current.arguments = json_or_abort(R"json({"scene":0})json");
        current.arguments_digest = digest_string(to_json_string(current.arguments));
        current.operation_id = OperationId::generate();
        current.has_side_effects = true;
        const auto outcome = registry_v2.execute(current, context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(!outcome.value().failed);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G6: consumer closure surface (M7-TR0-G6; consumer binary run via ctest)
// ---------------------------------------------------------------------------

int g6_public_surface_closure() {
    // The header closes over its own vocabulary: constants, mode/status name
    // closures, default limits and the full extract->project->admit chain are
    // usable from a standalone TU (examples/minimal_consumer.cpp links the
    // same surface; its TR0 closure runs as the mira_minimal_consumer test).
    MIRA_CHECK(kToolReferenceScheme == "toolref:");
    MIRA_CHECK(kWorkflowToolRefsSchema == "mira.workflow.tool_refs.v1");
    MIRA_CHECK(kWorkflowToolCompatSchema == "mira.workflow.tool_compat.v1");

    MIRA_CHECK(tool_reference_compat_name(ToolReferenceCompat::Resolved) == "resolved");
    MIRA_CHECK(tool_reference_compat_name(ToolReferenceCompat::EvolvedCompatible) ==
               "evolved_compatible");
    MIRA_CHECK(tool_reference_compat_name(ToolReferenceCompat::EvolvedIncompatible) ==
               "evolved_incompatible");
    MIRA_CHECK(tool_reference_compat_name(ToolReferenceCompat::Unresolved) == "unresolved");
    MIRA_CHECK(workflow_tool_compat_state_name(WorkflowToolCompatState::Runnable) == "runnable");
    MIRA_CHECK(workflow_tool_compat_state_name(WorkflowToolCompatState::Degraded) == "degraded");
    MIRA_CHECK(workflow_tool_compat_state_name(WorkflowToolCompatState::Invalid) == "invalid");

    ToolReferenceLimits limits{};
    MIRA_CHECK(limits.max_reference_bytes == 256);
    MIRA_CHECK(limits.max_wire_name_bytes == 128);
    MIRA_CHECK(limits.max_entries == 256);
    MIRA_CHECK(limits.max_detail_bytes == 256);
    ToolRefExtractionOptions options{};
    MIRA_CHECK(options.default_mode == ToolReferenceMode::PinnedDigest);
    MIRA_CHECK(options.per_tool_modes.empty());

    // End-to-end smoke over the public surface only.
    BuiltinToolRegistry registry;
    fill_registry(registry, gate_registry_tools());
    const std::vector<ExposedToolSpec> view = registry.exposed_tools();
    const WorkflowDefinition definition = make_gate_definition();
    const WorkflowToolRefManifest manifest = extract_or_abort(definition, view);
    const JsonValue encoded = workflow_tool_refs_to_json(manifest);
    const auto decoded = must(workflow_tool_refs_from_json(encoded), "surface decode");
    MIRA_CHECK(decoded.entries == manifest.entries);
    const auto projection =
        must(project_workflow_tool_compatibility(definition, decoded, view), "surface project");
    const WorkflowToolCompatDecision decision = admit_workflow_run_by_tool_compat(projection);
    MIRA_CHECK(decision.admitted);
    MIRA_CHECK(workflow_tool_compat_to_json(projection).is_object());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report [path]: canonical report of the fixed scenarios for
    // cross-process byte comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const std::string report = build_reference_report();
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
        {"G1 follow golden and round trip", g1_follow_golden_round_trip},
        {"G1 pinned golden and round trip", g1_pinned_golden_round_trip},
        {"G1 negative parse matrix", g1_negative_matrix},
        {"G1 mode closed set", g1_mode_closed_set},
        {"G2 registry extraction positive", g2_registry_extraction_positive},
        {"G2 extraction negative matrix", g2_extraction_negative_matrix},
        {"G2 binding and empty manifest", g2_binding_and_empty_manifest},
        {"G2 manifest json round trip", g2_json_round_trip},
        {"G3 resolution matrix", g3_resolution_matrix},
        {"G3 state aggregation runnable", g3_state_aggregation},
        {"G3 aggregation branches", g3_aggregation_branches},
        {"G3 input mismatch rejected", g3_input_mismatch_rejected},
        {"G4 placeholder instantiation compatible", g4_placeholder_instantiation_compatible},
        {"G4 skeleton incompatible matrix", g4_skeleton_incompatible_matrix},
        {"G4 detail bounded and first violation", g4_detail_bounded_and_first},
        {"G4 projection determinism and guards", g4_projection_determinism_and_guards},
        {"G4 report deterministic in-process", g4_report_is_deterministic},
        {"G5 admission decisions", g5_admission_decisions},
        {"G5 audit projection redaction", g5_audit_projection_redaction},
        {"G5 dec015 execution gate combination", g5_dec015_execution_gate_combination},
        {"G6 public surface closure", g6_public_surface_closure},
    };

    std::cout << "M7 TR0 stable tool reference verification ("
              << sizeof(gates) / sizeof(gates[0]) << " gates)\n";
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
