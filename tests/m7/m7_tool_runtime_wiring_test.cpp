// M7 TR2 runtime wiring verification - reference expression and admission
// domain: gates M7-TR2-G1..G3 (frozen in
// docs/plans/m7-tools-evaluation-platform-v1.md sections 4.7/5.7; contract
// source docs/design/tool_reference_and_skill_design.md section 18).
//
// Covers the WorkflowRuntime consumption of the TR0 reference layer: the IR
// v1.1 reference expression and its extraction, the mount table, the
// publish-gate auto extraction and the create_run tool-compatibility
// admission gate with its Degraded audit event. The skill execution and
// Procedure wiring domain (gates M7-TR2-G4..G6) lives in
// m7_tool_runtime_skill_test.cpp; shared fixtures live in
// tr2_wiring_support.hpp. Split is a pure move: no assertion, literal or gate
// identity changed.
//
// Deterministic by construction: every value that reaches an assertion or the
// --report output is content-derived, so reports reproduce byte-for-byte
// across processes and build trees.

#include "../support/test.hpp"
#include "tr2_wiring_support.hpp"

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
    };

    std::cout << "M7 TR2 reference and admission wiring verification ("
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
