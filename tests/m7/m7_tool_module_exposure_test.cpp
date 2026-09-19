// M7 TM2 verification: tool module LLM exposure projection (gates
// M7-TM2-G1..G6, frozen in docs/plans/m7-tools-evaluation-platform-v1.md
// section 5.3).
//
// Deterministic by construction: no clock and no randomness reach any asserted
// value or the --report output (ModelRequestId::generate() in the G6 consumer
// fixture only feeds resolve_tool_calls bookkeeping and never enters an
// assertion or the report). The --report mode prints a canonical JSON report of
// a fixed exposure scenario for cross-process byte comparison.

#include "../support/test.hpp"

#include <mira/model_tool.hpp>
#include <mira/tool_module_exposure.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Fixtures (built through the real TM0 parser; a fixture failure is fatal)
// ---------------------------------------------------------------------------

JsonValue version_json(std::int64_t major, std::int64_t minor, std::int64_t patch) {
    JsonValue::Object object;
    object.emplace_back("major", JsonValue{major});
    object.emplace_back("minor", JsonValue{minor});
    object.emplace_back("patch", JsonValue{patch});
    return JsonValue{std::move(object)};
}

struct FixtureMember final {
    std::string name;
    std::string side_effect = "read_only"; // manifest JSON spelling
    std::string description = {};          // empty = omit the field
    std::vector<CapabilityId> required = {};
};

ToolModuleManifest fixture_manifest(const std::string &module_id,
                                    const std::vector<FixtureMember> &members,
                                    const std::vector<std::string> &conflicts_with = {},
                                    const std::vector<CapabilityId> &module_required = {}) {
    JsonValue::Array tools;
    for (const auto &member : members) {
        JsonValue::Object tool;
        tool.emplace_back("name", member.name);
        tool.emplace_back("version", version_json(0, 1, 0));
        if (!member.description.empty()) {
            tool.emplace_back("description", member.description);
        }
        tool.emplace_back("arguments_schema",
                          JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
        tool.emplace_back("result_schema",
                          JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
        if (!member.required.empty()) {
            JsonValue::Array capabilities;
            for (const auto &id : member.required) {
                capabilities.emplace_back(JsonValue{id});
            }
            tool.emplace_back("required_capabilities", JsonValue{std::move(capabilities)});
        }
        tool.emplace_back("side_effect", member.side_effect);
        tools.emplace_back(JsonValue{std::move(tool)});
    }

    JsonValue::Object origin;
    origin.emplace_back("isolation",
                        std::string(tool_module_origin_name(ToolModuleOrigin::BuiltIn)));

    JsonValue::Object resources;
    resources.emplace_back("max_total_concurrent_invocations", JsonValue{std::int64_t{2}});
    resources.emplace_back("max_total_result_bytes", JsonValue{std::int64_t{4096}});

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolModuleManifestSchema));
    root.emplace_back("schema_version", std::string(kToolModuleManifestSchemaVersion));
    root.emplace_back("module_id", module_id);
    root.emplace_back("version", version_json(1, 0, 0));
    root.emplace_back("origin", JsonValue{std::move(origin)});
    root.emplace_back("tools", JsonValue{std::move(tools)});
    root.emplace_back("resources", JsonValue{std::move(resources)});
    if (!module_required.empty()) {
        JsonValue::Array capabilities;
        for (const auto &id : module_required) {
            capabilities.emplace_back(JsonValue{id});
        }
        root.emplace_back("required_capabilities", JsonValue{std::move(capabilities)});
    }
    if (!conflicts_with.empty()) {
        JsonValue::Array conflicts;
        for (const auto &id : conflicts_with) {
            conflicts.emplace_back(JsonValue{id});
        }
        root.emplace_back("conflicts_with", JsonValue{std::move(conflicts)});
    }

    auto parsed = parse_tool_module_manifest(JsonValue{std::move(root)}, CapabilityCatalog::core());
    if (!parsed.has_value()) {
        std::cerr << "fixture manifest '" << module_id
                  << "' failed to parse: " << parsed.error().safe_message << '\n';
        std::abort();
    }
    return parsed.value();
}

EnvironmentCapabilities full_environment() {
    EnvironmentCapabilities capabilities;
    capabilities.screen_capture = true;
    capabilities.ui_tree = true;
    capabilities.foreground_app = true;
    capabilities.device_state = true;
    capabilities.perception_sources = 2;
    capabilities.atomic_observation = true;
    capabilities.discrete_input = true;
    capabilities.input_release = true;
    capabilities.epoch_invalidation = true;
    return capabilities;
}

// Manual all-`status` negotiation for the paths the pure TM0 negotiation can
// never emit (Revoked) or that must bypass the TM0 gates to reach the TM2
// defensive checks (duplicate wire names reaching the projection).
ModuleNegotiationResult manual_negotiation(std::span<const ModuleSnapshot> snapshots,
                                           ModuleStatus status) {
    ModuleNegotiationResult result;
    for (const auto &snapshot : snapshots) {
        ModuleAvailability availability;
        availability.module_id = snapshot.module_id;
        availability.version = snapshot.version;
        availability.module_digest = snapshot.module_digest;
        availability.status = status;
        result.modules.push_back(std::move(availability));
    }
    return result;
}

template <typename T> bool failed_with(const Result<T> &result, ErrorCode code) {
    return !result.has_value() && result.error().code == code;
}

template <typename T> bool failed_in_tool_module_domain(const Result<T> &result) {
    return !result.has_value() && result.error().domain == "mira.tool_module" &&
           !result.error().safe_message.empty();
}

const ToolExclusion *find_exclusion(const ToolExposure &exposure, ToolExclusionLevel level,
                                    ToolExclusionReason reason, std::string_view module_id) {
    for (const auto &exclusion : exposure.exclusions) {
        if (exclusion.level == level && exclusion.reason == reason &&
            exclusion.module_id == module_id) {
            return &exclusion;
        }
    }
    return nullptr;
}

std::size_t count_reason(const ToolExposure &exposure, ToolExclusionReason reason) {
    std::size_t total = 0;
    for (const auto &exclusion : exposure.exclusions) {
        if (exclusion.reason == reason) {
            ++total;
        }
    }
    return total;
}

const ExposedToolSpec *find_tool(const ToolExposure &exposure, std::string_view wire_name) {
    for (const auto &tool : exposure.tools) {
        if (tool.wire_name == wire_name) {
            return &tool;
        }
    }
    return nullptr;
}

std::string_view side_effect_name(ActionRisk risk) {
    switch (risk) {
    case ActionRisk::R0ReadOnly:
        return "read_only";
    case ActionRisk::R1ReversibleLow:
        return "reversible_low";
    case ActionRisk::R2UserVisible:
        return "user_visible";
    case ActionRisk::R3Sensitive:
        return "sensitive";
    case ActionRisk::R4Critical:
        return "critical";
    }
    return "read_only";
}

// Independent recomputation of the member spec digest from the documented
// canonical field set (module identity + member contract). Key order is
// irrelevant because canonical_json_string sorts.
Hash recompute_member_spec_digest(const std::string &module_id, const ModuleToolSpec &member) {
    JsonValue::Object data_access;
    JsonValue::Array reads;
    for (const auto &entry : member.data_access.reads) {
        reads.emplace_back(JsonValue{entry});
    }
    JsonValue::Array writes;
    for (const auto &entry : member.data_access.writes) {
        writes.emplace_back(JsonValue{entry});
    }
    data_access.emplace_back("reads", JsonValue{std::move(reads)});
    data_access.emplace_back("writes", JsonValue{std::move(writes)});

    JsonValue::Object root;
    root.emplace_back("module_id", module_id);
    root.emplace_back("name", member.name);
    root.emplace_back(
        "version", version_json(member.version.major, member.version.minor, member.version.patch));
    root.emplace_back("description", member.description);
    root.emplace_back("arguments_schema", member.arguments_schema.root);
    root.emplace_back("result_schema", member.result_schema.root);
    JsonValue::Array capabilities;
    for (const auto &id : member.required_capabilities) {
        capabilities.emplace_back(JsonValue{id});
    }
    root.emplace_back("required_capabilities", JsonValue{std::move(capabilities)});
    root.emplace_back("side_effect", std::string(side_effect_name(member.side_effect)));
    root.emplace_back("data_access", JsonValue{std::move(data_access)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

// Independent recomputation of the composite snapshot digest from the
// documented binding structure: schema + generation + included module digest
// set (module_id order) + member (tool_id, spec_digest, wire_name) triples
// (wire_name order). Exclusions are not part of the digest input.
Hash recompute_snapshot_digest(const ToolExposure &exposure) {
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolExposureSchema));
    root.emplace_back("negotiation_generation",
                      static_cast<std::int64_t>(exposure.negotiation_generation));
    JsonValue::Array modules;
    for (const auto &binding : exposure.modules) {
        JsonValue::Object entry;
        entry.emplace_back("module_id", binding.module_id);
        entry.emplace_back("module_digest", binding.module_digest.to_string());
        modules.emplace_back(JsonValue{std::move(entry)});
    }
    JsonValue::Array tools;
    for (const auto &tool : exposure.tools) { // already sorted by wire_name
        JsonValue::Object entry;
        entry.emplace_back("tool_id", tool.tool_id.to_string());
        entry.emplace_back("spec_digest", tool.spec_digest.to_string());
        entry.emplace_back("wire_name", tool.wire_name);
        tools.emplace_back(JsonValue{std::move(entry)});
    }
    root.emplace_back("modules", JsonValue{std::move(modules)});
    root.emplace_back("tools", JsonValue{std::move(tools)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

// ---------------------------------------------------------------------------
// --report scenario: simulator reference module (available) + an Unavailable
// module + a TaskPolicy exclusion + a TaskBudget exclusion, under one fixed
// budget. Pure content-derived values only.
// ---------------------------------------------------------------------------

std::string build_exposure_report() {
    const auto must = [](const auto &result) {
        if (!result.has_value()) {
            std::abort();
        }
        return result.value();
    };

    auto reference = must(make_simulator_reference_module());
    const ToolModuleManifest unavail = fixture_manifest(
        "acme.rep.unavail", {FixtureMember{"rep_unavail_probe"}}, {}, {"env.screen.capture"});
    const ToolModuleManifest policy =
        fixture_manifest("acme.rep.policy", {FixtureMember{"rep_policy_probe"}});
    // Equal-length short member names on purpose (SSO range): the report
    // path must not depend on the long-name workaround either.
    const ToolModuleManifest big = fixture_manifest(
        "acme.rep.big", {FixtureMember{"rep_big_one"}, FixtureMember{"rep_big_two"},
                         FixtureMember{"rep_big_three"}});

    const std::vector<ModuleSnapshot> active{
        module_snapshot_from_manifest(reference),
        module_snapshot_from_manifest(unavail),
        module_snapshot_from_manifest(policy),
        module_snapshot_from_manifest(big),
    };

    // discrete input only: the reference module is Available, `unavail` misses
    // env.screen.capture (its complete missing list must appear).
    EnvironmentCapabilities environment;
    environment.discrete_input = true;

    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, environment, CapabilityCatalog::core());

    ToolExposureRequest request;
    request.excluded_modules.emplace_back("acme.rep.policy",
                                          std::string("caller excluded for this task"));
    request.max_exposed_tools = 2; // `big` (3 members) cannot fit; reference (2) can.

    const ToolExposure exposure = must(project_tool_exposure(7, active, negotiation, request));
    const ToolExposure empty = must(
        project_tool_exposure(3, std::span<const ModuleSnapshot>{}, ModuleNegotiationResult{}));

    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.tool_module.exposure.report.v1"));
    report.emplace_back("reference_manifest_digest",
                        tool_module_manifest_digest(reference).to_string());
    report.emplace_back("negotiation_digest", negotiation.digest.to_string());
    report.emplace_back("exposure", tool_exposure_to_json(exposure));
    report.emplace_back("exposure_snapshot_digest", exposure.snapshot_digest.to_string());
    report.emplace_back("empty_snapshot_digest", empty.snapshot_digest.to_string());
    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G1: projection and identity assignment (M7-TM2-G1)
// ---------------------------------------------------------------------------

int g1_available_members_projected_with_derived_identity() {
    // Members deliberately declared out of wire-name order; the module also
    // mixes side-effect tiers so the mapping is asserted per member.
    // Member names here and in the report scenario's big module are
    // deliberately EQUAL-LENGTH SHORT names (SSO range, e.g. "zz_probe" vs
    // "aa_probe"). parse_tool_module_manifest's duplicate-name gate once kept
    // std::string_views into a loop-local string (dangling after the move),
    // which misread such pairs as duplicates; the gate now stores copies
    // (src/tool/tool_module.cpp, regression gate in
    // manifest_short_member_names_parse_and_duplicates_rejected). Keeping the
    // short pairs here proves the TM2 projection path no longer depends on
    // long-name workarounds. Other multi-member fixtures keep long names for
    // wire-sort variety, which is orthogonal.
    const ToolModuleManifest alpha =
        fixture_manifest("acme.exp.alpha", {FixtureMember{"zz_probe", "read_only", "alpha zz"},
                                            FixtureMember{"aa_probe", "user_visible", "alpha aa"}});
    const ModuleSnapshot alpha_snapshot = module_snapshot_from_manifest(alpha);
    const std::vector<ModuleSnapshot> active{alpha_snapshot};
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, full_environment(), CapabilityCatalog::core());

    auto projected = project_tool_exposure(11, active, negotiation);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.negotiation_generation == 11);

    // tools sorted by wire_name across the whole view.
    MIRA_CHECK(exposure.tools.size() == 2);
    MIRA_CHECK(exposure.tools[0].wire_name == "aa_probe");
    MIRA_CHECK(exposure.tools[1].wire_name == "zz_probe");

    const std::vector<ModuleToolSpec> &members = alpha_snapshot.tools;
    for (const auto &tool : exposure.tools) {
        const auto member =
            std::find_if(members.begin(), members.end(),
                         [&](const ModuleToolSpec &spec) { return spec.name == tool.wire_name; });
        MIRA_CHECK(member != members.end());
        // wire_name is the member name verbatim; no rename, no rewriting.
        MIRA_CHECK(tool.wire_name == member->name);
        // ToolId is the deterministic derivation over (module_id, digest, name)
        // and matches an independent second call to the derivation.
        const ToolId derived =
            derive_module_tool_id(alpha.module_id, alpha_snapshot.module_digest, member->name);
        MIRA_CHECK(tool.tool_id == derived);
        MIRA_CHECK(derive_module_tool_id(alpha.module_id, alpha_snapshot.module_digest,
                                         member->name) == derived);
        MIRA_CHECK(!tool.tool_id.is_nil());
        // spec_digest is canonical and matches both the public helper and an
        // independent recomputation from the documented field set.
        MIRA_CHECK(tool.spec_digest == module_tool_spec_digest(alpha.module_id, *member));
        MIRA_CHECK(tool.spec_digest == recompute_member_spec_digest(alpha.module_id, *member));
        MIRA_CHECK(tool.version == member->version);
        MIRA_CHECK(tool.description == member->description);
        MIRA_CHECK(to_json_string(tool.parameters_schema.root) ==
                   to_json_string(member->arguments_schema.root));
        // side-effect mapping: read_only -> false, user_visible -> true.
        MIRA_CHECK(tool.has_side_effects == (member->side_effect != ActionRisk::R0ReadOnly));
    }
    MIRA_CHECK(find_tool(exposure, "aa_probe")->has_side_effects);
    MIRA_CHECK(!find_tool(exposure, "zz_probe")->has_side_effects);

    // modules records the included binding in module_id order.
    MIRA_CHECK(exposure.modules.size() == 1);
    MIRA_CHECK(exposure.modules[0].module_id == "acme.exp.alpha");
    MIRA_CHECK(exposure.modules[0].version == alpha.version);
    MIRA_CHECK(exposure.modules[0].module_digest == alpha_snapshot.module_digest);
    MIRA_CHECK(exposure.modules[0].module_digest == tool_module_manifest_digest(alpha));

    // Exclusions empty for a fully available view.
    MIRA_CHECK(exposure.exclusions.empty());

    // Same inputs, second independent projection: identical digest and view.
    auto repeat = project_tool_exposure(11, active, negotiation);
    MIRA_CHECK(repeat.has_value());
    MIRA_CHECK(repeat.value().snapshot_digest == exposure.snapshot_digest);
    MIRA_CHECK(repeat.value().tools.size() == exposure.tools.size());
    MIRA_CHECK(repeat.value().tools[0].tool_id == exposure.tools[0].tool_id);
    MIRA_CHECK(repeat.value().tools[1].tool_id == exposure.tools[1].tool_id);

    // The digest matches the documented composite recomputation.
    MIRA_CHECK(exposure.snapshot_digest == recompute_snapshot_digest(exposure));
    MIRA_CHECK(!(exposure.snapshot_digest == Hash{}));
    return 0;
}

int g1_identity_invariants_across_modules_and_inputs() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.exp.alpha", {FixtureMember{"shared_member_name"}});
    const ToolModuleManifest beta =
        fixture_manifest("acme.exp.beta", {FixtureMember{"shared_member_name"}});
    const ModuleSnapshot alpha_snapshot = module_snapshot_from_manifest(alpha);
    const ModuleSnapshot beta_snapshot = module_snapshot_from_manifest(beta);

    const ToolId alpha_id =
        derive_module_tool_id(alpha.module_id, alpha_snapshot.module_digest, "shared_member_name");
    const ToolId beta_id =
        derive_module_tool_id(beta.module_id, beta_snapshot.module_digest, "shared_member_name");
    // Different module identity -> different ToolId even for the same member
    // name (the module digest participates in the derivation).
    MIRA_CHECK(alpha_id != beta_id);

    // Same module content under a different module_id yields a different
    // digest and therefore a different ToolId.
    const ToolModuleManifest renamed =
        fixture_manifest("acme.exp.renamed", {FixtureMember{"shared_member_name"}});
    MIRA_CHECK(tool_module_manifest_digest(renamed) != tool_module_manifest_digest(alpha));
    MIRA_CHECK(derive_module_tool_id(renamed.module_id,
                                     module_snapshot_from_manifest(renamed).module_digest,
                                     "shared_member_name") != alpha_id);

    // Member name participates: different names in one module differ.
    const ToolModuleManifest dual =
        fixture_manifest("acme.exp.dual", {FixtureMember{"fixture.dual.member_a"},
                                           FixtureMember{"fixture.dual.member_b"}});
    const ModuleSnapshot dual_snapshot = module_snapshot_from_manifest(dual);
    MIRA_CHECK(derive_module_tool_id(dual.module_id, dual_snapshot.module_digest,
                                     "fixture.dual.member_a") !=
               derive_module_tool_id(dual.module_id, dual_snapshot.module_digest,
                                     "fixture.dual.member_b"));

    // spec_digest binds module identity: identical member declarations under
    // different module ids digest differently; content changes move it.
    MIRA_CHECK(module_tool_spec_digest(alpha.module_id, alpha_snapshot.tools[0]) !=
               module_tool_spec_digest(beta.module_id, beta_snapshot.tools[0]));
    const ToolModuleManifest altered = fixture_manifest(
        "acme.exp.alpha", {FixtureMember{"shared_member_name", "read_only", "changed text"}});
    MIRA_CHECK(
        module_tool_spec_digest(alpha.module_id, alpha_snapshot.tools[0]) !=
        module_tool_spec_digest(alpha.module_id, module_snapshot_from_manifest(altered).tools[0]));
    MIRA_CHECK(module_tool_spec_digest(alpha.module_id, alpha_snapshot.tools[0]) ==
               module_tool_spec_digest(alpha.module_id, alpha_snapshot.tools[0]));

    // Available modules merge into one wire-sorted view with a merged module
    // list in module_id order (registration order deliberately not sorted).
    // Member names must stay distinct here: a shared name is a TM0 wire
    // conflict by design (covered in G2), not an Available merge.
    const ToolModuleManifest beta_distinct =
        fixture_manifest("acme.exp.beta", {FixtureMember{"beta_member_probe"}});
    const ModuleSnapshot beta_distinct_snapshot = module_snapshot_from_manifest(beta_distinct);
    const ToolId beta_distinct_id = derive_module_tool_id(
        beta_distinct.module_id, beta_distinct_snapshot.module_digest, "beta_member_probe");

    const std::vector<ModuleSnapshot> active{beta_distinct_snapshot, alpha_snapshot, dual_snapshot};
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, full_environment(), CapabilityCatalog::core());
    auto projected = project_tool_exposure(4, active, negotiation);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.tools.size() == 4);
    MIRA_CHECK(exposure.tools[0].wire_name == "beta_member_probe");     // beta
    MIRA_CHECK(exposure.tools[1].wire_name == "fixture.dual.member_a"); // dual
    MIRA_CHECK(exposure.tools[2].wire_name == "fixture.dual.member_b"); // dual
    MIRA_CHECK(exposure.tools[3].wire_name == "shared_member_name");    // alpha
    MIRA_CHECK(exposure.tools[0].tool_id == beta_distinct_id);
    MIRA_CHECK(exposure.tools[3].tool_id == alpha_id);
    MIRA_CHECK(exposure.modules.size() == 3);
    MIRA_CHECK(exposure.modules[0].module_id == "acme.exp.alpha");
    MIRA_CHECK(exposure.modules[1].module_id == "acme.exp.beta");
    MIRA_CHECK(exposure.modules[2].module_id == "acme.exp.dual");
    MIRA_CHECK(exposure.snapshot_digest == recompute_snapshot_digest(exposure));
    MIRA_CHECK(!exposure.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// G2: two-level exclusion matrix (M7-TM2-G2)
// ---------------------------------------------------------------------------

int g2_module_level_negotiation_exclusions() {
    // Conflict pair: two modules declaring each other.
    const ToolModuleManifest conflict_x = fixture_manifest(
        "acme.g2.conflictx", {FixtureMember{"conflictx_probe"}}, {"acme.g2.conflicty"});
    const ToolModuleManifest conflict_y = fixture_manifest(
        "acme.g2.conflicty", {FixtureMember{"conflicty_probe"}}, {"acme.g2.conflictx"});
    // Unavailable module: two missing capabilities, both must be listed.
    const ToolModuleManifest unavail =
        fixture_manifest("acme.g2.unavail", {FixtureMember{"unavail_probe"}}, {},
                         {"env.screen.capture", "env.ui.tree"});
    const ToolModuleManifest okay = fixture_manifest("acme.g2.okay", {FixtureMember{"okay_probe"}});

    const std::vector<ModuleSnapshot> active{
        module_snapshot_from_manifest(okay),
        module_snapshot_from_manifest(unavail),
        module_snapshot_from_manifest(conflict_y),
        module_snapshot_from_manifest(conflict_x),
    };

    // Environment with neither screen capture nor ui tree: `unavail` misses
    // both; everyone else has no requirements.
    EnvironmentCapabilities environment;
    environment.discrete_input = true;
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, environment, CapabilityCatalog::core());

    // Sanity: the real negotiation produced the expected verdicts.
    MIRA_CHECK(negotiation.modules.size() == 4);
    for (const auto &availability : negotiation.modules) {
        if (availability.module_id == "acme.g2.unavail") {
            MIRA_CHECK(availability.status == ModuleStatus::Unavailable);
            MIRA_CHECK(availability.missing ==
                       (std::vector<CapabilityId>{"env.screen.capture", "env.ui.tree"}));
        } else if (availability.module_id == "acme.g2.okay") {
            MIRA_CHECK(availability.status == ModuleStatus::Available);
        } else {
            MIRA_CHECK(availability.status == ModuleStatus::Conflict);
            MIRA_CHECK(!availability.conflicting_with.empty());
        }
    }

    auto projected = project_tool_exposure(12, active, negotiation);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();

    // Module atomicity: only `okay` contributes members.
    MIRA_CHECK(exposure.tools.size() == 1);
    MIRA_CHECK(exposure.tools[0].wire_name == "okay_probe");
    MIRA_CHECK(exposure.modules.size() == 1);
    MIRA_CHECK(exposure.modules[0].module_id == "acme.g2.okay");

    // Three module-level exclusions sorted by module_id; each carries
    // level/reason/module_id and its reason-specific payload.
    MIRA_CHECK(exposure.exclusions.size() == 3);
    MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.conflictx");
    MIRA_CHECK(exposure.exclusions[1].module_id == "acme.g2.conflicty");
    MIRA_CHECK(exposure.exclusions[2].module_id == "acme.g2.unavail");
    for (const auto &exclusion : exposure.exclusions) {
        MIRA_CHECK(exclusion.level == ToolExclusionLevel::Module);
        MIRA_CHECK(!exclusion.detail.empty());
    }

    const ToolExclusion *const x =
        find_exclusion(exposure, ToolExclusionLevel::Module, ToolExclusionReason::ModuleConflict,
                       "acme.g2.conflictx");
    MIRA_CHECK(x != nullptr);
    MIRA_CHECK(x->conflicting_with == "acme.g2.conflicty");
    const ToolExclusion *const y =
        find_exclusion(exposure, ToolExclusionLevel::Module, ToolExclusionReason::ModuleConflict,
                       "acme.g2.conflicty");
    MIRA_CHECK(y != nullptr);
    MIRA_CHECK(y->conflicting_with == "acme.g2.conflictx");

    const ToolExclusion *const missing_entry =
        find_exclusion(exposure, ToolExclusionLevel::Module, ToolExclusionReason::ModuleUnavailable,
                       "acme.g2.unavail");
    MIRA_CHECK(missing_entry != nullptr);
    MIRA_CHECK(missing_entry->missing ==
               (std::vector<CapabilityId>{"env.screen.capture", "env.ui.tree"}));
    MIRA_CHECK(missing_entry->conflicting_with.empty());

    MIRA_CHECK(exposure.snapshot_digest == recompute_snapshot_digest(exposure));
    return 0;
}

int g2_revoked_and_reserved_wire_names() {
    // Revoked: TM0 negotiation never emits it; construct the verdict directly
    // (the TM1 tombstone projection shape).
    {
        const ToolModuleManifest revoked =
            fixture_manifest("acme.g2.revoked", {FixtureMember{"revoked_probe"}});
        const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(revoked)};
        const ModuleNegotiationResult negotiation =
            manual_negotiation(active, ModuleStatus::Revoked);
        auto projected = project_tool_exposure(15, active, negotiation);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.empty());
        MIRA_CHECK(exposure.modules.empty());
        MIRA_CHECK(exposure.exclusions.size() == 1);
        MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Module);
        MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::ModuleRevoked);
        MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.revoked");
        MIRA_CHECK(!exposure.exclusions[0].detail.empty());
        MIRA_CHECK(exposure.empty());
        MIRA_CHECK(!(exposure.snapshot_digest == Hash{}));
    }

    // Reserved wire names: "bash" passes the manifest charset (the parser
    // accepts it), a lone module negotiates Available, and the projection must
    // exclude the whole module — no rename, no partial member exposure.
    {
        const ToolModuleManifest reserved = fixture_manifest(
            "acme.g2.reserved", {FixtureMember{"safe_probe"}, FixtureMember{"bash"}});
        const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(reserved)};
        const ModuleNegotiationResult negotiation =
            negotiate_modules(active, full_environment(), CapabilityCatalog::core());
        MIRA_CHECK(negotiation.modules.size() == 1);
        MIRA_CHECK(negotiation.modules[0].status == ModuleStatus::Available);

        auto projected = project_tool_exposure(16, active, negotiation);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.empty());
        MIRA_CHECK(exposure.modules.empty());
        MIRA_CHECK(exposure.exclusions.size() == 1);
        MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Module);
        MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::ReservedWireName);
        MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.reserved");
        MIRA_CHECK(!exposure.exclusions[0].detail.empty());
        // Atomicity: the co-declared safe member never leaks into the view.
        MIRA_CHECK(find_tool(exposure, "safe_probe") == nullptr);
    }

    // Every hosted name triggers the same whole-module exclusion; a module
    // without hosted names stays included.
    {
        const ToolModuleManifest shell =
            fixture_manifest("acme.g2.shellhost", {FixtureMember{"shell"}});
        const ToolModuleManifest clean =
            fixture_manifest("acme.g2.clean", {FixtureMember{"clean_probe"}});
        const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(shell),
                                                 module_snapshot_from_manifest(clean)};
        const ModuleNegotiationResult negotiation =
            negotiate_modules(active, full_environment(), CapabilityCatalog::core());
        auto projected = project_tool_exposure(17, active, negotiation);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.size() == 1);
        MIRA_CHECK(exposure.tools[0].wire_name == "clean_probe");
        MIRA_CHECK(exposure.exclusions.size() == 1);
        MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::ReservedWireName);
        MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.shellhost");
    }
    return 0;
}

int g2_task_policy_exclusions() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.g2.alpha", {FixtureMember{"alpha_probe"}});
    const ToolModuleManifest beta =
        fixture_manifest("acme.g2.beta", {FixtureMember{"fixture.beta_probe_one"},
                                          FixtureMember{"fixture.beta_probe_two"}});
    const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(alpha),
                                             module_snapshot_from_manifest(beta)};
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, full_environment(), CapabilityCatalog::core());

    ToolExposureRequest request;
    request.excluded_modules.emplace_back("acme.g2.beta",
                                          std::string("caller excluded beta for this task"));
    auto projected = project_tool_exposure(21, active, negotiation, request);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();

    // Module-grained policy exclusion: alpha fully exposed, beta contributes
    // nothing, one task-level exclusion carrying the caller reason verbatim.
    MIRA_CHECK(exposure.tools.size() == 1);
    MIRA_CHECK(exposure.tools[0].wire_name == "alpha_probe");
    MIRA_CHECK(exposure.modules.size() == 1);
    MIRA_CHECK(exposure.modules[0].module_id == "acme.g2.alpha");
    MIRA_CHECK(exposure.exclusions.size() == 1);
    MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Task);
    MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::TaskPolicy);
    MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.beta");
    MIRA_CHECK(exposure.exclusions[0].detail == "caller excluded beta for this task");
    MIRA_CHECK(exposure.exclusions[0].missing.empty());
    MIRA_CHECK(exposure.exclusions[0].conflicting_with.empty());

    // Reason at exactly the limit is accepted; one byte over is not.
    {
        ToolExposureRequest bounded;
        bounded.excluded_modules.emplace_back(
            "acme.g2.alpha", std::string(kDefaultToolExposureLimits.max_reason_bytes, 'r'));
        auto at_limit = project_tool_exposure(22, active, negotiation, bounded);
        MIRA_CHECK(at_limit.has_value());
        // Only alpha is policy-excluded; beta still contributes both members.
        MIRA_CHECK(at_limit.value().tools.size() == 2);
        MIRA_CHECK(at_limit.value().exclusions.size() == 1);
        MIRA_CHECK(at_limit.value().exclusions[0].reason == ToolExclusionReason::TaskPolicy);
        MIRA_CHECK(at_limit.value().exclusions[0].module_id == "acme.g2.alpha");
        MIRA_CHECK(at_limit.value().exclusions[0].detail.size() ==
                   kDefaultToolExposureLimits.max_reason_bytes);

        bounded.excluded_modules.clear();
        bounded.excluded_modules.emplace_back(
            "acme.g2.alpha", std::string(kDefaultToolExposureLimits.max_reason_bytes + 1, 'r'));
        auto over_limit = project_tool_exposure(22, active, negotiation, bounded);
        MIRA_CHECK(failed_with(over_limit, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(over_limit));
    }
    return 0;
}

int g2_task_budget_exclusions() {
    const ToolModuleManifest big = fixture_manifest(
        "acme.g2.big", {FixtureMember{"fixture.big_one_a"}, FixtureMember{"fixture.big_one_b"},
                        FixtureMember{"fixture.big_one_c"}}); // sorts before "acme.g2.small"
    const ToolModuleManifest small =
        fixture_manifest("acme.g2.small", {FixtureMember{"small_probe"}});
    const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(small),
                                             module_snapshot_from_manifest(big)};
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, full_environment(), CapabilityCatalog::core());

    // Exactly fits: budget equals the member count, everything included.
    {
        ToolExposureRequest request;
        request.max_exposed_tools = 4;
        auto projected = project_tool_exposure(31, active, negotiation, request);
        MIRA_CHECK(projected.has_value());
        MIRA_CHECK(projected.value().tools.size() == 4);
        MIRA_CHECK(projected.value().exclusions.empty());
        MIRA_CHECK(projected.value().modules.size() == 2);
    }

    // The larger module is evaluated first in module_id order and does not
    // fit; the later smaller module still fits in the remaining budget.
    {
        ToolExposureRequest request;
        request.max_exposed_tools = 2;
        auto projected = project_tool_exposure(32, active, negotiation, request);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.size() == 1);
        MIRA_CHECK(exposure.tools[0].wire_name == "small_probe");
        MIRA_CHECK(exposure.modules.size() == 1);
        MIRA_CHECK(exposure.modules[0].module_id == "acme.g2.small");
        MIRA_CHECK(exposure.exclusions.size() == 1);
        MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Task);
        MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::TaskBudget);
        MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.big");
        MIRA_CHECK(!exposure.exclusions[0].detail.empty());
    }

    // Zero budget is legal and excludes every module through the budget path.
    {
        ToolExposureRequest request;
        request.max_exposed_tools = 0;
        auto projected = project_tool_exposure(33, active, negotiation, request);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.empty());
        MIRA_CHECK(exposure.modules.empty());
        MIRA_CHECK(exposure.exclusions.size() == 2);
        MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.big");
        MIRA_CHECK(exposure.exclusions[1].module_id == "acme.g2.small");
        for (const auto &exclusion : exposure.exclusions) {
            MIRA_CHECK(exclusion.level == ToolExclusionLevel::Task);
            MIRA_CHECK(exclusion.reason == ToolExclusionReason::TaskBudget);
        }
        MIRA_CHECK(exposure.empty());
        MIRA_CHECK(!(exposure.snapshot_digest == Hash{}));
        MIRA_CHECK(exposure.snapshot_digest == recompute_snapshot_digest(exposure));
    }

    // Partial fits are forbidden: a module never contributes some of its
    // members (budget 2 against a 3-member module leaves zero members).
    {
        const std::vector<ModuleSnapshot> lone{module_snapshot_from_manifest(big)};
        const ModuleNegotiationResult lone_negotiation =
            negotiate_modules(lone, full_environment(), CapabilityCatalog::core());
        ToolExposureRequest request;
        request.max_exposed_tools = 2;
        auto projected = project_tool_exposure(34, lone, lone_negotiation, request);
        MIRA_CHECK(projected.has_value());
        MIRA_CHECK(projected.value().tools.empty());
        MIRA_CHECK(projected.value().exclusions.size() == 1);
        MIRA_CHECK(projected.value().exclusions[0].reason == ToolExclusionReason::TaskBudget);
    }
    return 0;
}

int g2_exclusion_priority_and_ordering() {
    const ToolModuleManifest unavail = fixture_manifest(
        "acme.g2.prio_unavail", {FixtureMember{"prio_unavail_probe"}}, {}, {"env.screen.capture"});
    const ToolModuleManifest reserved =
        fixture_manifest("acme.g2.prio_reserved", {FixtureMember{"mcp"}});
    const ToolModuleManifest plain =
        fixture_manifest("acme.g2.prio_plain", {FixtureMember{"prio_plain_probe"}});

    const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(unavail),
                                             module_snapshot_from_manifest(reserved),
                                             module_snapshot_from_manifest(plain)};
    EnvironmentCapabilities no_screen;
    no_screen.discrete_input = true;
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, no_screen, CapabilityCatalog::core());

    // All three task-level triggers at once, plus module-level facts: policy
    // excludes everything, budget is zero, unavail is Unavailable and reserved
    // carries a hosted name. Each module must produce exactly one exclusion
    // with the documented priority:
    //   module-level (Unavailable) > task policy > reserved name > budget.
    ToolExposureRequest request;
    request.max_exposed_tools = 0;
    request.excluded_modules.emplace_back("acme.g2.prio_reserved", std::string("policy first"));
    request.excluded_modules.emplace_back("acme.g2.prio_plain", std::string("policy plain"));
    auto projected = project_tool_exposure(41, active, negotiation, request);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.tools.empty());
    MIRA_CHECK(exposure.modules.empty());
    MIRA_CHECK(exposure.exclusions.size() == 3);
    // Sorted by (level, module_id): module level first.
    MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Module);
    MIRA_CHECK(exposure.exclusions[0].module_id == "acme.g2.prio_unavail");
    MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::ModuleUnavailable);
    MIRA_CHECK(exposure.exclusions[1].level == ToolExclusionLevel::Task);
    MIRA_CHECK(exposure.exclusions[1].module_id == "acme.g2.prio_plain");
    MIRA_CHECK(exposure.exclusions[1].reason == ToolExclusionReason::TaskPolicy); // policy > budget
    MIRA_CHECK(exposure.exclusions[1].detail == "policy plain");
    MIRA_CHECK(exposure.exclusions[2].level == ToolExclusionLevel::Task);
    MIRA_CHECK(exposure.exclusions[2].module_id == "acme.g2.prio_reserved");
    MIRA_CHECK(exposure.exclusions[2].reason ==
               ToolExclusionReason::TaskPolicy); // policy > reserved name
    MIRA_CHECK(count_reason(exposure, ToolExclusionReason::ReservedWireName) == 0);
    MIRA_CHECK(count_reason(exposure, ToolExclusionReason::TaskBudget) == 0);

    // Policy-excluded modules do not consume budget: budget 1 with alpha
    // policy-excluded (1 member) leaves room for beta (1 member).
    {
        const ToolModuleManifest alpha =
            fixture_manifest("acme.g2.prio_a", {FixtureMember{"prio_a_probe"}});
        const ToolModuleManifest beta =
            fixture_manifest("acme.g2.prio_b", {FixtureMember{"prio_b_probe"}});
        const std::vector<ModuleSnapshot> pair{module_snapshot_from_manifest(alpha),
                                               module_snapshot_from_manifest(beta)};
        const ModuleNegotiationResult pair_negotiation =
            negotiate_modules(pair, full_environment(), CapabilityCatalog::core());
        ToolExposureRequest pair_request;
        pair_request.max_exposed_tools = 1;
        pair_request.excluded_modules.emplace_back("acme.g2.prio_a", std::string("skip a"));
        auto pair_projected = project_tool_exposure(42, pair, pair_negotiation, pair_request);
        MIRA_CHECK(pair_projected.has_value());
        MIRA_CHECK(pair_projected.value().tools.size() == 1);
        MIRA_CHECK(pair_projected.value().tools[0].wire_name == "prio_b_probe");
        MIRA_CHECK(pair_projected.value().exclusions.size() == 1);
        MIRA_CHECK(pair_projected.value().exclusions[0].reason == ToolExclusionReason::TaskPolicy);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G3: consistency fail-closed (M7-TM2-G3)
// ---------------------------------------------------------------------------

int g3_consistency_fail_closed_matrix() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.fc.alpha", {FixtureMember{"fc_alpha_probe"}});
    const ToolModuleManifest beta =
        fixture_manifest("acme.fc.beta", {FixtureMember{"fc_beta_probe"}});
    const ModuleSnapshot alpha_snapshot = module_snapshot_from_manifest(alpha);
    const ModuleSnapshot beta_snapshot = module_snapshot_from_manifest(beta);
    const std::vector<ModuleSnapshot> pair{alpha_snapshot, beta_snapshot};
    const ModuleNegotiationResult good_negotiation =
        negotiate_modules(pair, full_environment(), CapabilityCatalog::core());

    // Sanity: the well-formed pairing succeeds.
    MIRA_CHECK(project_tool_exposure(50, pair, good_negotiation).has_value());

    // Empty module_id in the active set.
    {
        std::vector<ModuleSnapshot> broken{alpha_snapshot};
        broken.push_back(ModuleSnapshot{}); // empty module_id
        auto rejected =
            project_tool_exposure(51, broken, manual_negotiation(broken, ModuleStatus::Available));
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Duplicate module_id in the active set.
    {
        const std::vector<ModuleSnapshot> duplicated{alpha_snapshot, alpha_snapshot};
        auto rejected = project_tool_exposure(
            52, duplicated, manual_negotiation(duplicated, ModuleStatus::Available));
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Verdict references a module outside the active set.
    {
        ModuleNegotiationResult stray = good_negotiation;
        ModuleAvailability ghost;
        ghost.module_id = "acme.fc.ghost";
        ghost.version = beta.version;
        ghost.module_digest = beta_snapshot.module_digest;
        ghost.status = ModuleStatus::Available;
        stray.modules.push_back(std::move(ghost));
        auto rejected = project_tool_exposure(53, pair, stray);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Duplicate verdicts for one module in the negotiation.
    {
        ModuleNegotiationResult duplicated = good_negotiation;
        duplicated.modules.push_back(duplicated.modules[0]);
        auto rejected = project_tool_exposure(54, pair, duplicated);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Verdict module_digest does not match the snapshot.
    {
        ModuleNegotiationResult tampered = good_negotiation;
        tampered.modules[0].module_digest = beta_snapshot.module_digest;
        auto rejected = project_tool_exposure(55, pair, tampered);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Verdict version does not match the snapshot.
    {
        ModuleNegotiationResult tampered = good_negotiation;
        tampered.modules[1].version = SemanticVersion{9, 9, 9};
        auto rejected = project_tool_exposure(56, pair, tampered);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Active module without a verdict.
    {
        ModuleNegotiationResult incomplete = good_negotiation;
        incomplete.modules.pop_back();
        auto rejected = project_tool_exposure(57, pair, incomplete);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Task policy request validation: empty id, duplicate ids, unknown module.
    {
        ToolExposureRequest request;
        request.excluded_modules.emplace_back("", std::string("no id"));
        auto rejected = project_tool_exposure(58, pair, good_negotiation, request);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));

        request.excluded_modules.clear();
        request.excluded_modules.emplace_back("acme.fc.alpha", std::string("a"));
        request.excluded_modules.emplace_back("acme.fc.alpha", std::string("a again"));
        rejected = project_tool_exposure(59, pair, good_negotiation, request);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));

        request.excluded_modules.clear();
        request.excluded_modules.emplace_back("acme.fc.unknown", std::string("ghost"));
        rejected = project_tool_exposure(60, pair, good_negotiation, request);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
    }
    // Defensive duplicate wire names reaching the projection (the TM0/TM1
    // gates would have marked these Conflict; here they arrive Available).
    {
        const ToolModuleManifest twin_a =
            fixture_manifest("acme.fc.twin_a", {FixtureMember{"fc_shared_name"}});
        const ToolModuleManifest twin_b =
            fixture_manifest("acme.fc.twin_b", {FixtureMember{"fc_shared_name"}});
        const std::vector<ModuleSnapshot> twins{module_snapshot_from_manifest(twin_a),
                                                module_snapshot_from_manifest(twin_b)};
        auto rejected =
            project_tool_exposure(61, twins, manual_negotiation(twins, ModuleStatus::Available));
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // Exposure limits: too many tools and too many modules are rejected, not
    // absorbed.
    {
        const ToolModuleManifest triple =
            fixture_manifest("acme.fc.triple", {FixtureMember{"fixture.triple_one"},
                                                FixtureMember{"fixture.triple_two"},
                                                FixtureMember{"fixture.triple_three"}});
        const std::vector<ModuleSnapshot> lone{module_snapshot_from_manifest(triple)};
        const ModuleNegotiationResult lone_negotiation =
            negotiate_modules(lone, full_environment(), CapabilityCatalog::core());

        ToolExposureLimits tool_limits;
        tool_limits.max_tools = 2;
        auto rejected = project_tool_exposure(62, lone, lone_negotiation, {}, tool_limits);
        MIRA_CHECK(failed_with(rejected, ErrorCode::ResourceExhausted));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));

        ToolExposureLimits module_limits;
        module_limits.max_modules = 1;
        rejected = project_tool_exposure(63, pair, good_negotiation, {}, module_limits);
        MIRA_CHECK(failed_with(rejected, ErrorCode::ResourceExhausted));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    return 0;
}

int g3_empty_projection_is_defined() {
    // Empty active set + empty negotiation: a legal empty view with a defined
    // digest (the M3 empty-allowlist path; design section 14).
    auto projected =
        project_tool_exposure(70, std::span<const ModuleSnapshot>{}, ModuleNegotiationResult{});
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.tools.empty());
    MIRA_CHECK(exposure.modules.empty());
    MIRA_CHECK(exposure.exclusions.empty());
    MIRA_CHECK(exposure.empty());
    MIRA_CHECK(!(exposure.snapshot_digest == Hash{}));
    MIRA_CHECK(exposure.negotiation_generation == 70);

    auto repeat =
        project_tool_exposure(70, std::span<const ModuleSnapshot>{}, ModuleNegotiationResult{});
    MIRA_CHECK(repeat.has_value());
    MIRA_CHECK(repeat.value().snapshot_digest == exposure.snapshot_digest);

    // The generation is bound even for the empty view.
    auto other_generation =
        project_tool_exposure(71, std::span<const ModuleSnapshot>{}, ModuleNegotiationResult{});
    MIRA_CHECK(other_generation.has_value());
    MIRA_CHECK(other_generation.value().snapshot_digest != exposure.snapshot_digest);
    return 0;
}

// ---------------------------------------------------------------------------
// G4: digest binding and determinism (M7-TM2-G4)
// ---------------------------------------------------------------------------

int g4_snapshot_digest_binding() {
    const ToolModuleManifest alpha_v1 = fixture_manifest(
        "acme.dig.alpha", {FixtureMember{"dig_probe", "read_only", "first revision"}});
    const ToolModuleManifest alpha_v2 = fixture_manifest(
        "acme.dig.alpha", {FixtureMember{"dig_probe", "read_only", "second revision"}});
    const ToolModuleManifest beta = fixture_manifest(
        "acme.dig.beta", {FixtureMember{"beta_probe", "user_visible", "beta member"}});

    const std::vector<ModuleSnapshot> v1_with_beta{module_snapshot_from_manifest(alpha_v1),
                                                   module_snapshot_from_manifest(beta)};
    const std::vector<ModuleSnapshot> v2_with_beta{module_snapshot_from_manifest(alpha_v2),
                                                   module_snapshot_from_manifest(beta)};
    const ModuleNegotiationResult negotiation_v1 =
        negotiate_modules(v1_with_beta, full_environment(), CapabilityCatalog::core());
    const ModuleNegotiationResult negotiation_v2 =
        negotiate_modules(v2_with_beta, full_environment(), CapabilityCatalog::core());

    auto first = project_tool_exposure(80, v1_with_beta, negotiation_v1);
    MIRA_CHECK(first.has_value());
    auto second = project_tool_exposure(80, v2_with_beta, negotiation_v2);
    MIRA_CHECK(second.has_value());

    // Included module digest change (member content) moves the digest.
    MIRA_CHECK(first.value().snapshot_digest != second.value().snapshot_digest);
    MIRA_CHECK(find_tool(first.value(), "dig_probe")->spec_digest !=
               find_tool(second.value(), "dig_probe")->spec_digest);

    // Generation is bound: same inputs, different generation, different digest.
    auto bumped = project_tool_exposure(81, v1_with_beta, negotiation_v1);
    MIRA_CHECK(bumped.has_value());
    MIRA_CHECK(bumped.value().snapshot_digest != first.value().snapshot_digest);
    MIRA_CHECK(bumped.value().snapshot_digest == recompute_snapshot_digest(bumped.value()));

    // Exclusion-only changes leave the digest untouched: swapping the task
    // policy reason text does not move the bound content.
    {
        ToolExposureRequest reason_a;
        reason_a.excluded_modules.emplace_back("acme.dig.beta", std::string("reason a"));
        ToolExposureRequest reason_b;
        reason_b.excluded_modules.emplace_back("acme.dig.beta", std::string("reason b"));

        auto with_a = project_tool_exposure(82, v1_with_beta, negotiation_v1, reason_a);
        MIRA_CHECK(with_a.has_value());
        auto with_b = project_tool_exposure(82, v1_with_beta, negotiation_v1, reason_b);
        MIRA_CHECK(with_b.has_value());
        MIRA_CHECK(with_a.value().snapshot_digest == with_b.value().snapshot_digest);
        MIRA_CHECK(with_a.value().exclusions.size() == 1);
        MIRA_CHECK(with_a.value().exclusions[0].detail == "reason a");
        MIRA_CHECK(with_b.value().exclusions[0].detail == "reason b");
        // Both digests recompute from the documented structure.
        MIRA_CHECK(with_a.value().snapshot_digest == recompute_snapshot_digest(with_a.value()));
        MIRA_CHECK(with_b.value().snapshot_digest == recompute_snapshot_digest(with_b.value()));
    }

    // Dropping an included module changes the digest (module set is bound).
    {
        const std::vector<ModuleSnapshot> lone{module_snapshot_from_manifest(alpha_v1)};
        const ModuleNegotiationResult lone_negotiation =
            negotiate_modules(lone, full_environment(), CapabilityCatalog::core());
        auto solo = project_tool_exposure(80, lone, lone_negotiation);
        MIRA_CHECK(solo.has_value());
        MIRA_CHECK(solo.value().snapshot_digest != first.value().snapshot_digest);
    }
    return 0;
}

int g4_exposure_json_projection() {
    // Reason name mapping is complete and stable.
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::ModuleUnavailable) ==
               "module_unavailable");
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::ModuleConflict) ==
               "module_conflict");
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::ModuleRevoked) == "module_revoked");
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::ReservedWireName) ==
               "reserved_wire_name");
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::TaskPolicy) == "task_policy");
    MIRA_CHECK(tool_exclusion_reason_name(ToolExclusionReason::TaskBudget) == "task_budget");

    // A view with every exclusion flavor: unavailable + conflict module-level,
    // task policy and task budget task-level, plus included tools of both
    // side-effect tiers.
    const ToolModuleManifest unavail = fixture_manifest(
        "acme.json.unavail", {FixtureMember{"json_unavail_probe"}}, {}, {"env.screen.capture"});
    const ToolModuleManifest conflict_x = fixture_manifest(
        "acme.json.conflictx", {FixtureMember{"json_conflictx_probe"}}, {"acme.json.conflicty"});
    const ToolModuleManifest conflict_y = fixture_manifest(
        "acme.json.conflicty", {FixtureMember{"json_conflicty_probe"}}, {"acme.json.conflictx"});
    const ToolModuleManifest big =
        fixture_manifest("acme.json.big", {FixtureMember{"fixture.json_big_one"},
                                           FixtureMember{"fixture.json_big_two"},
                                           FixtureMember{"fixture.json_big_three"}});
    const ToolModuleManifest mixed = fixture_manifest(
        "acme.json.mixed", {FixtureMember{"json_mixed_read", "read_only", "read member"},
                            FixtureMember{"json_mixed_write", "user_visible", "write member"}});

    const std::vector<ModuleSnapshot> active{
        module_snapshot_from_manifest(mixed),      module_snapshot_from_manifest(unavail),
        module_snapshot_from_manifest(big),        module_snapshot_from_manifest(conflict_x),
        module_snapshot_from_manifest(conflict_y),
    };
    EnvironmentCapabilities environment;
    environment.discrete_input = true;
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, environment, CapabilityCatalog::core());

    ToolExposureRequest request;
    request.max_exposed_tools = 2; // big (3) cannot fit after nothing exposed
    request.excluded_modules.emplace_back("acme.json.big", std::string("also policy excluded"));
    auto projected = project_tool_exposure(90, active, negotiation, request);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.tools.size() == 2);
    MIRA_CHECK(exposure.exclusions.size() == 4);

    const JsonValue json = tool_exposure_to_json(exposure);
    MIRA_CHECK(json.is_object());

    // Versioned schema, generation and digest fields.
    const auto *schema = json.find("schema");
    MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
               *schema->as_string() == kToolExposureSchema);
    const auto *generation = json.find("negotiation_generation");
    MIRA_CHECK(generation != nullptr && generation->is_integer() &&
               *generation->as_integer() == 90);
    const auto *digest = json.find("snapshot_digest");
    MIRA_CHECK(digest != nullptr && digest->as_string() != nullptr &&
               *digest->as_string() == exposure.snapshot_digest.to_string());

    // Modules: identity bindings only.
    const auto *modules = json.find("modules");
    MIRA_CHECK(modules != nullptr && modules->is_array() && modules->as_array()->size() == 1);
    {
        const JsonValue &entry = (*modules->as_array())[0];
        const auto *module_id = entry.find("module_id");
        MIRA_CHECK(module_id != nullptr && module_id->as_string() != nullptr &&
                   *module_id->as_string() == "acme.json.mixed");
        const auto *version = entry.find("version");
        MIRA_CHECK(version != nullptr && version->is_object());
        const auto *major = version->find("major");
        MIRA_CHECK(major != nullptr && major->is_integer() && *major->as_integer() == 1);
        const auto *module_digest = entry.find("module_digest");
        MIRA_CHECK(module_digest != nullptr && module_digest->as_string() != nullptr &&
                   *module_digest->as_string() == exposure.modules[0].module_digest.to_string());
    }

    // Tools: identity + digest + side effect, no schema bodies or descriptions.
    const auto *tools = json.find("tools");
    MIRA_CHECK(tools != nullptr && tools->is_array() && tools->as_array()->size() == 2);
    {
        const JsonValue &read_entry = (*tools->as_array())[0];
        const auto *wire_name = read_entry.find("wire_name");
        MIRA_CHECK(wire_name != nullptr && wire_name->as_string() != nullptr &&
                   *wire_name->as_string() == "json_mixed_read");
        const auto *tool_id = read_entry.find("tool_id");
        MIRA_CHECK(tool_id != nullptr && tool_id->as_string() != nullptr &&
                   *tool_id->as_string() ==
                       find_tool(exposure, "json_mixed_read")->tool_id.to_string());
        const auto *spec_digest = read_entry.find("spec_digest");
        MIRA_CHECK(spec_digest != nullptr && spec_digest->as_string() != nullptr);
        const auto *side_effects = read_entry.find("has_side_effects");
        MIRA_CHECK(side_effects != nullptr && side_effects->as_boolean().has_value() &&
                   !*side_effects->as_boolean());
        MIRA_CHECK(read_entry.find("description") == nullptr);
        MIRA_CHECK(read_entry.find("parameters_schema") == nullptr);

        const JsonValue &write_entry = (*tools->as_array())[1];
        const auto *write_effects = write_entry.find("has_side_effects");
        MIRA_CHECK(write_effects != nullptr && write_effects->as_boolean().has_value() &&
                   *write_effects->as_boolean());
        const auto *write_version = write_entry.find("version");
        MIRA_CHECK(write_version != nullptr && write_version->is_object());
    }

    // Exclusions: level/reason/module_id plus reason-specific payloads.
    const auto *exclusions = json.find("exclusions");
    MIRA_CHECK(exclusions != nullptr && exclusions->is_array() &&
               exclusions->as_array()->size() == 4);
    {
        const auto &entries = *exclusions->as_array();
        // Sorted (level, module_id): module level first (conflictx, conflicty,
        // unavail), then task level (big).
        const auto *level0 = entries[0].find("level");
        MIRA_CHECK(level0 != nullptr && level0->as_string() != nullptr &&
                   *level0->as_string() == "module");
        const auto *reason0 = entries[0].find("reason");
        MIRA_CHECK(reason0 != nullptr && reason0->as_string() != nullptr &&
                   *reason0->as_string() == "module_conflict");
        const auto *partner0 = entries[0].find("conflicting_with");
        MIRA_CHECK(partner0 != nullptr && partner0->as_string() != nullptr &&
                   *partner0->as_string() == "acme.json.conflicty");

        const auto *unavail_entry = entries[2].find("reason");
        MIRA_CHECK(unavail_entry != nullptr && unavail_entry->as_string() != nullptr &&
                   *unavail_entry->as_string() == "module_unavailable");
        const auto *missing = entries[2].find("missing");
        MIRA_CHECK(missing != nullptr && missing->is_array() && missing->as_array()->size() == 1);
        const auto &missing_first = (*missing->as_array())[0];
        MIRA_CHECK(missing_first.as_string() != nullptr &&
                   *missing_first.as_string() == "env.screen.capture");

        const auto *level3 = entries[3].find("level");
        MIRA_CHECK(level3 != nullptr && level3->as_string() != nullptr &&
                   *level3->as_string() == "task");
        const auto *reason3 = entries[3].find("reason");
        MIRA_CHECK(reason3 != nullptr && reason3->as_string() != nullptr &&
                   *reason3->as_string() == "task_policy");
        const auto *detail3 = entries[3].find("detail");
        MIRA_CHECK(detail3 != nullptr && detail3->as_string() != nullptr &&
                   *detail3->as_string() == "also policy excluded");
    }

    // Round trip: the JSON text parses back (the report path serializes it).
    const std::string text = to_json_string(json);
    auto parsed = parse_json(text);
    MIRA_CHECK(parsed.has_value());
    MIRA_CHECK(parsed.value().find("schema") != nullptr);

    // Deterministic serialization.
    MIRA_CHECK(to_json_string(json) == text);
    return 0;
}

int g4_report_is_deterministic() {
    // In-process determinism: the fixed scenario report is byte-identical when
    // rebuilt. Cross-process determinism is asserted by running --report twice
    // and comparing bytes (see the verification commands).
    const std::string first = build_exposure_report();
    const std::string second = build_exposure_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());

    auto parsed = parse_json(first);
    MIRA_CHECK(parsed.has_value());
    const auto *schema = parsed.value().find("schema");
    MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
               *schema->as_string() == "mira.tool_module.exposure.report.v1");
    const auto *exposure = parsed.value().find("exposure");
    MIRA_CHECK(exposure != nullptr && exposure->is_object());
    const auto *tools = exposure->find("tools");
    MIRA_CHECK(tools != nullptr && tools->is_array() && tools->as_array()->size() == 2);
    const auto *exclusions = exposure->find("exclusions");
    MIRA_CHECK(exclusions != nullptr && exclusions->is_array() &&
               exclusions->as_array()->size() == 3);
    return 0;
}

// ---------------------------------------------------------------------------
// G5: simulator reference module and replay binding (M7-TM2-G5)
// ---------------------------------------------------------------------------

int g5_simulator_reference_module_golden() {
    auto reference = make_simulator_reference_module();
    MIRA_CHECK(reference.has_value());
    const ToolModuleManifest &manifest = reference.value();

    // Declared fixture shape (tool module design section 14).
    MIRA_CHECK(manifest.module_id == "builtin.simulator.env");
    MIRA_CHECK(manifest.version == (SemanticVersion{1, 0, 0}));
    MIRA_CHECK(manifest.origin == ToolModuleOrigin::BuiltIn);
    MIRA_CHECK(manifest.resources.max_total_concurrent_invocations == 2);
    MIRA_CHECK(manifest.resources.max_total_result_bytes == 4096);
    MIRA_CHECK(manifest.tools.size() == 2);

    const auto describe =
        std::find_if(manifest.tools.begin(), manifest.tools.end(), [](const ModuleToolSpec &tool) {
            return tool.name == "simulator.describe_display";
        });
    MIRA_CHECK(describe != manifest.tools.end());
    MIRA_CHECK(describe->side_effect == ActionRisk::R0ReadOnly);
    MIRA_CHECK(describe->required_capabilities.empty());

    const auto inject =
        std::find_if(manifest.tools.begin(), manifest.tools.end(), [](const ModuleToolSpec &tool) {
            return tool.name == "simulator.inject_tap";
        });
    MIRA_CHECK(inject != manifest.tools.end());
    MIRA_CHECK(inject->side_effect == ActionRisk::R2UserVisible);
    MIRA_CHECK(inject->required_capabilities == (std::vector<CapabilityId>{"env.input.discrete"}));

    // Golden digest constant: record the first value and compare every later
    // rebuild (two fresh make() calls plus a repeated digest call). The value
    // itself is content-derived and appears in --report for cross-process
    // comparison.
    const Hash golden = tool_module_manifest_digest(manifest);
    MIRA_CHECK(!(golden == Hash{}));
    MIRA_CHECK(tool_module_manifest_digest(manifest) == golden);
    auto rebuilt = make_simulator_reference_module();
    MIRA_CHECK(rebuilt.has_value());
    MIRA_CHECK(tool_module_manifest_digest(rebuilt.value()) == golden);
    return 0;
}

int g5_reference_negotiation_and_projection() {
    auto reference = make_simulator_reference_module();
    MIRA_CHECK(reference.has_value());
    const ModuleSnapshot snapshot = module_snapshot_from_manifest(reference.value());
    MIRA_CHECK(snapshot.required == (std::vector<CapabilityId>{"env.input.discrete"}));

    // discrete_input=true: Available, both members exposed with the correct
    // side-effect mapping and derived identity.
    {
        EnvironmentCapabilities environment;
        environment.discrete_input = true;
        const std::vector<ModuleSnapshot> active{snapshot};
        const ModuleNegotiationResult negotiation =
            negotiate_modules(active, environment, CapabilityCatalog::core());
        MIRA_CHECK(negotiation.modules.size() == 1);
        MIRA_CHECK(negotiation.modules[0].status == ModuleStatus::Available);
        MIRA_CHECK(negotiation.modules[0].missing.empty());

        auto projected = project_tool_exposure(95, active, negotiation);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.size() == 2);
        MIRA_CHECK(exposure.tools[0].wire_name == "simulator.describe_display");
        MIRA_CHECK(exposure.tools[1].wire_name == "simulator.inject_tap");
        MIRA_CHECK(!exposure.tools[0].has_side_effects);
        MIRA_CHECK(exposure.tools[1].has_side_effects);
        MIRA_CHECK(exposure.tools[0].tool_id ==
                   derive_module_tool_id(snapshot.module_id, snapshot.module_digest,
                                         "simulator.describe_display"));
        MIRA_CHECK(exposure.tools[1].tool_id == derive_module_tool_id(snapshot.module_id,
                                                                      snapshot.module_digest,
                                                                      "simulator.inject_tap"));
        MIRA_CHECK(exposure.modules.size() == 1);
        MIRA_CHECK(exposure.modules[0].module_digest == snapshot.module_digest);
        MIRA_CHECK(exposure.modules[0].module_digest ==
                   tool_module_manifest_digest(reference.value()));
        MIRA_CHECK(exposure.exclusions.empty());
        MIRA_CHECK(exposure.snapshot_digest == recompute_snapshot_digest(exposure));
    }

    // discrete_input=false: module-level Unavailable with missing exactly
    // [env.input.discrete]; the projection excludes the whole module.
    {
        EnvironmentCapabilities environment; // no discrete input
        const std::vector<ModuleSnapshot> active{snapshot};
        const ModuleNegotiationResult negotiation =
            negotiate_modules(active, environment, CapabilityCatalog::core());
        MIRA_CHECK(negotiation.modules.size() == 1);
        MIRA_CHECK(negotiation.modules[0].status == ModuleStatus::Unavailable);
        MIRA_CHECK(negotiation.modules[0].missing ==
                   (std::vector<CapabilityId>{"env.input.discrete"}));

        auto projected = project_tool_exposure(96, active, negotiation);
        MIRA_CHECK(projected.has_value());
        const ToolExposure &exposure = projected.value();
        MIRA_CHECK(exposure.tools.empty());
        MIRA_CHECK(exposure.modules.empty());
        MIRA_CHECK(exposure.exclusions.size() == 1);
        MIRA_CHECK(exposure.exclusions[0].level == ToolExclusionLevel::Module);
        MIRA_CHECK(exposure.exclusions[0].reason == ToolExclusionReason::ModuleUnavailable);
        MIRA_CHECK(exposure.exclusions[0].module_id == "builtin.simulator.env");
        MIRA_CHECK(exposure.exclusions[0].missing ==
                   (std::vector<CapabilityId>{"env.input.discrete"}));
        MIRA_CHECK(!(exposure.snapshot_digest == Hash{}));
    }
    return 0;
}

int g5_recorded_digest_replay_binding() {
    auto reference = make_simulator_reference_module();
    MIRA_CHECK(reference.has_value());
    const ToolModuleManifest other =
        fixture_manifest("acme.replay.other", {FixtureMember{"replay_probe"}});

    const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(reference.value()),
                                             module_snapshot_from_manifest(other)};
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, full_environment(), CapabilityCatalog::core());
    auto projected = project_tool_exposure(100, active, negotiation);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.modules.size() == 2);

    const Hash &reference_digest = module_snapshot_from_manifest(reference.value()).module_digest;
    const Hash &other_digest = module_snapshot_from_manifest(other).module_digest;
    MIRA_CHECK(reference_digest != other_digest);

    // Exact set match passes, including out-of-order input.
    {
        const std::vector<Hash> recorded{other_digest, reference_digest};
        MIRA_CHECK(verify_recorded_module_digests(recorded, exposure).has_value());
        const std::vector<Hash> ordered{reference_digest, other_digest};
        MIRA_CHECK(verify_recorded_module_digests(ordered, exposure).has_value());
    }
    // One extra digest fails.
    {
        const std::vector<Hash> extra{reference_digest, other_digest, Hash{}};
        const auto rejected = verify_recorded_module_digests(extra, exposure);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
    }
    // One missing digest fails.
    {
        const std::vector<Hash> missing{reference_digest};
        MIRA_CHECK(failed_with(verify_recorded_module_digests(missing, exposure),
                               ErrorCode::InvalidState));
    }
    // A replaced digest fails (same cardinality, wrong member).
    {
        const std::vector<Hash> replaced{reference_digest, digest_string("not-a-module")};
        MIRA_CHECK(failed_with(verify_recorded_module_digests(replaced, exposure),
                               ErrorCode::InvalidState));
    }
    // An empty recorded set matches only an empty exposure.
    {
        auto empty_exposure = project_tool_exposure(101, std::span<const ModuleSnapshot>{},
                                                    ModuleNegotiationResult{});
        MIRA_CHECK(empty_exposure.has_value());
        MIRA_CHECK(verify_recorded_module_digests(std::span<const Hash>{}, empty_exposure.value())
                       .has_value());
        MIRA_CHECK(failed_with(verify_recorded_module_digests(std::span<const Hash>{}, exposure),
                               ErrorCode::InvalidState));
    }
    // Excluded modules are not part of the recorded set: a view that dropped a
    // module through task policy binds only the included digest.
    {
        ToolExposureRequest request;
        request.excluded_modules.emplace_back("acme.replay.other", std::string("dropped"));
        auto reduced = project_tool_exposure(102, active, negotiation, request);
        MIRA_CHECK(reduced.has_value());
        MIRA_CHECK(reduced.value().modules.size() == 1);
        const std::vector<Hash> only_reference{reference_digest};
        MIRA_CHECK(verify_recorded_module_digests(only_reference, reduced.value()).has_value());
        const std::vector<Hash> both{reference_digest, other_digest};
        MIRA_CHECK(failed_with(verify_recorded_module_digests(both, reduced.value()),
                               ErrorCode::InvalidState));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G6: composition into ModelRequest.tools and resolve_tool_calls (M7-TM2-G6)
// ---------------------------------------------------------------------------

ModelRequest exposure_request(const ToolExposure &exposure) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate(); // bookkeeping only; never asserted
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart text;
    text.text = "s";
    system_item.content.emplace_back(std::move(text));
    request.input = {std::move(system_item)};
    request.data_policy.store = false;
    request.tools = exposure.tools;
    return request;
}

ModelResponse call_response(const ModelRequest &request, const std::string &call_id,
                            const std::string &arguments, const std::string &name,
                            const ToolId &tool_id) {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = "m";
    response.status = ModelCompletionStatus::Completed;
    ToolCallOutput call;
    call.provider_call_id = ProviderToolCallId{call_id};
    call.provider_name = name;
    call.tool_id = tool_id;
    call.arguments = parse_json(arguments).value();
    call.arguments_digest = digest_string(to_json_string(call.arguments));
    response.output.emplace_back(std::move(call));
    response.usage.quality = UsageQuality::Missing;
    return response;
}

int g6_projection_feeds_resolve_tool_calls() {
    auto reference = make_simulator_reference_module();
    MIRA_CHECK(reference.has_value());
    const std::vector<ModuleSnapshot> active{module_snapshot_from_manifest(reference.value())};
    EnvironmentCapabilities environment;
    environment.discrete_input = true;
    const ModuleNegotiationResult negotiation =
        negotiate_modules(active, environment, CapabilityCatalog::core());
    auto projected = project_tool_exposure(110, active, negotiation);
    MIRA_CHECK(projected.has_value());
    const ToolExposure &exposure = projected.value();
    MIRA_CHECK(exposure.tools.size() == 2);

    const ModelRequest request = exposure_request(exposure);
    const ExposedToolSpec &describe = *find_tool(exposure, "simulator.describe_display");
    const ExposedToolSpec &inject = *find_tool(exposure, "simulator.inject_tap");

    // The user-visible member resolves with identity, wire name and side
    // effect carried from the exposure.
    {
        const auto response = call_response(request, "call_1", R"({"x":120,"y":40})",
                                            "simulator.inject_tap", inject.tool_id);
        auto batch = resolve_tool_calls(request, response);
        MIRA_CHECK(batch.has_value());
        MIRA_CHECK(batch.value().proposals.size() == 1);
        const ToolProposal &proposal = batch.value().proposals[0];
        MIRA_CHECK(proposal.tool_id == inject.tool_id);
        MIRA_CHECK(proposal.wire_name == "simulator.inject_tap");
        MIRA_CHECK(proposal.has_side_effects);
        MIRA_CHECK(proposal.tool_version == inject.version);
        MIRA_CHECK(!proposal.operation_id.value.is_nil());
    }
    // The read-only member maps has_side_effects=false end to end.
    {
        const auto response = call_response(request, "call_2", R"({"verbose":false})",
                                            "simulator.describe_display", describe.tool_id);
        auto batch = resolve_tool_calls(request, response);
        MIRA_CHECK(batch.has_value());
        MIRA_CHECK(batch.value().proposals.size() == 1);
        MIRA_CHECK(!batch.value().proposals[0].has_side_effects);
        MIRA_CHECK(batch.value().proposals[0].tool_id == describe.tool_id);
    }
    // Fail-closed semantics are unchanged behind module-sourced exposure:
    // an unexposed name and a hosted name are protocol violations.
    {
        const auto unknown =
            call_response(request, "call_3", "{}", "not_exposed_anywhere", describe.tool_id);
        MIRA_CHECK(!resolve_tool_calls(request, unknown).has_value());

        const auto hosted = call_response(request, "call_4", "{}", "bash", describe.tool_id);
        auto rejected = resolve_tool_calls(request, hosted);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
        MIRA_CHECK(rejected.error().safe_message.find("bash") != std::string::npos);
    }
    // Identity mismatch (wire name resolved to a different ToolId) is a
    // violation, and so is a tampered arguments digest.
    {
        const auto mismatch =
            call_response(request, "call_5", "{}", "simulator.inject_tap", describe.tool_id);
        MIRA_CHECK(!resolve_tool_calls(request, mismatch).has_value());

        auto digest_bad =
            call_response(request, "call_6", R"({"x":1})", "simulator.inject_tap", inject.tool_id);
        std::get<ToolCallOutput>(digest_bad.output[0]).arguments_digest = Sha256Digest{};
        MIRA_CHECK(!resolve_tool_calls(request, digest_bad).has_value());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report: canonical report of the fixed scenario for cross-process byte
    // comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        std::cout << build_exposure_report() << '\n';
        return 0;
    }

    struct Gate {
        const char *name;
        int (*run)();
    };
    const Gate gates[] = {
        {"G1 available members projected with derived identity",
         g1_available_members_projected_with_derived_identity},
        {"G1 identity invariants across modules and inputs",
         g1_identity_invariants_across_modules_and_inputs},
        {"G2 module-level negotiation exclusions", g2_module_level_negotiation_exclusions},
        {"G2 revoked and reserved wire names", g2_revoked_and_reserved_wire_names},
        {"G2 task policy exclusions", g2_task_policy_exclusions},
        {"G2 task budget exclusions", g2_task_budget_exclusions},
        {"G2 exclusion priority and ordering", g2_exclusion_priority_and_ordering},
        {"G3 consistency fail-closed matrix", g3_consistency_fail_closed_matrix},
        {"G3 empty projection is defined", g3_empty_projection_is_defined},
        {"G4 snapshot digest binding", g4_snapshot_digest_binding},
        {"G4 exposure json projection", g4_exposure_json_projection},
        {"G4 report is deterministic", g4_report_is_deterministic},
        {"G5 simulator reference module golden", g5_simulator_reference_module_golden},
        {"G5 reference negotiation and projection", g5_reference_negotiation_and_projection},
        {"G5 recorded digest replay binding", g5_recorded_digest_replay_binding},
        {"G6 projection feeds resolve_tool_calls", g6_projection_feeds_resolve_tool_calls},
    };

    std::cout << "M7 TM2 tool module exposure verification (" << sizeof(gates) / sizeof(gates[0])
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
