#include "../support/test.hpp"

#include <mira/tool_module.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// A valid manifest exercising every optional field: host_provided trust
// fields, module + infra + member capabilities, descriptions, data access and
// conflict declarations.
constexpr const char *kValidManifestJson = R"({
    "schema": "mira.tool_module.manifest.v1",
    "schema_version": "1.0",
    "module_id": "acme.tools.clipboard",
    "version": {"major": 1, "minor": 2, "patch": 3},
    "origin": {"isolation": "host_provided", "signer": "acme-host"},
    "min_mira_module_abi": 1,
    "required_capabilities": ["env.screen.capture", "env.ui.tree"],
    "infra_capabilities": ["host.bridge.rpc"],
    "tools": [
        {
            "name": "read_clipboard",
            "version": {"major": 0, "minor": 1, "patch": 0},
            "description": "Reads clipboard text.",
            "arguments_schema": {"type": "object"},
            "result_schema": {"type": "object",
                              "properties": {"text": {"type": "string"}},
                              "required": ["text"]},
            "required_capabilities": ["env.screen.capture"],
            "side_effect": "read_only",
            "data_access": {"reads": ["clipboard"], "writes": []}
        },
        {
            "name": "write_clipboard",
            "version": {"major": 0, "minor": 1, "patch": 1},
            "arguments_schema": {"type": "object"},
            "result_schema": {"type": "object"},
            "side_effect": "user_visible",
            "data_access": {"writes": ["clipboard"]}
        }
    ],
    "conflicts_with": ["acme.tools.other"],
    "resources": {"max_total_concurrent_invocations": 4,
                  "max_total_result_bytes": 1048576}
})";

// Same manifest content as kValidManifestJson with every object's members in
// reverse order and different whitespace: the digest must not care.
constexpr const char *kValidManifestJsonReordered = R"({
      "resources": {"max_total_result_bytes": 1048576,
                    "max_total_concurrent_invocations": 4},
      "conflicts_with": ["acme.tools.other"],
      "tools": [
        { "data_access": {"writes": [], "reads": ["clipboard"]},
          "side_effect": "read_only",
          "required_capabilities": ["env.screen.capture"],
          "result_schema": {"required": ["text"],
                            "properties": {"text": {"type": "string"}},
                            "type": "object"},
          "arguments_schema": {"type": "object"},
          "description": "Reads clipboard text.",
          "version": {"patch": 0, "minor": 1, "major": 0},
          "name": "read_clipboard" },
        { "data_access": {"writes": ["clipboard"], "reads": []},
          "side_effect": "user_visible",
          "result_schema": {"type": "object"},
          "arguments_schema": {"type": "object"},
          "version": {"patch": 1, "minor": 1, "major": 0},
          "name": "write_clipboard" }
      ],
      "infra_capabilities": ["host.bridge.rpc"],
      "required_capabilities": ["env.screen.capture", "env.ui.tree"],
      "min_mira_module_abi": 1,
      "origin": {"signer": "acme-host", "isolation": "host_provided"},
      "version": {"patch": 3, "minor": 2, "major": 1},
      "module_id": "acme.tools.clipboard",
      "schema_version": "1.0",
      "schema": "mira.tool_module.manifest.v1"
})";

JsonValue version_json(std::int64_t major, std::int64_t minor, std::int64_t patch) {
    JsonValue::Object object;
    object.emplace_back("major", JsonValue{major});
    object.emplace_back("minor", JsonValue{minor});
    object.emplace_back("patch", JsonValue{patch});
    return JsonValue{std::move(object)};
}

JsonValue minimal_tool_json(const std::string &name) {
    JsonValue::Object object;
    object.emplace_back("name", JsonValue{name});
    object.emplace_back("version", version_json(0, 1, 0));
    object.emplace_back("arguments_schema",
                        JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    object.emplace_back("result_schema",
                        JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    object.emplace_back("side_effect", JsonValue{std::string("read_only")});
    return JsonValue{std::move(object)};
}

JsonValue &member_of(JsonValue &object, const char *key) { return *object.find(key); }

void erase_member(JsonValue &object, const char *key) {
    JsonValue::Object kept;
    for (const auto &entry : *object.as_object()) {
        if (entry.first != key) {
            kept.push_back(entry);
        }
    }
    object = JsonValue{std::move(kept)};
}

ModuleToolSpec make_tool_spec(const std::string &name) {
    ModuleToolSpec spec;
    spec.name = name;
    return spec;
}

ModuleSnapshot make_snapshot(const std::string &id, char digest_hex,
                             std::vector<std::string> tool_names,
                             std::vector<CapabilityId> required,
                             std::vector<std::string> conflicts) {
    ModuleSnapshot snapshot;
    snapshot.module_id = id;
    snapshot.version = SemanticVersion{};
    snapshot.module_digest = digest_from_hex(std::string(64, digest_hex)).value();
    for (const auto &name : tool_names) {
        snapshot.tools.push_back(make_tool_spec(name));
    }
    snapshot.required = std::move(required);
    snapshot.conflicts_with = std::move(conflicts);
    return snapshot;
}

// The fixed negotiation dataset for G4/G5 (G3 reuses parts of it). The input
// order is deliberately not sorted by module_id; the negotiation output must
// be. Environment: screen capture only.
//  - acme.alpha: declares conflict with acme.beta and shares the wire name
//    "alpha_read" with acme.gamma -> Conflict, smallest partner "acme.beta".
//  - acme.beta: conflicted via acme.alpha's declaration -> Conflict.
//  - acme.delta: requires device state + perception in a screen-only
//    environment -> Unavailable with both missing.
//  - acme.echo: requirements satisfied -> Available.
//  - acme.gamma: shares "alpha_read" with acme.alpha -> Conflict.
std::vector<ModuleSnapshot> golden_dataset() {
    std::vector<ModuleSnapshot> active;
    active.push_back(
        make_snapshot("acme.alpha", 'a', {"alpha_read"}, {"env.screen.capture"}, {"acme.beta"}));
    active.push_back(make_snapshot("acme.beta", 'b', {"beta_read"}, {"env.screen.capture"}, {}));
    active.push_back(make_snapshot("acme.delta", 'c', {"delta_read"},
                                   {"env.device.state", "env.perception.sources"}, {}));
    active.push_back(make_snapshot("acme.gamma", 'd', {"alpha_read"}, {}, {}));
    active.push_back(make_snapshot("acme.echo", 'e', {"echo_read"}, {"env.screen.capture"}, {}));
    return active;
}

EnvironmentCapabilities golden_environment() {
    EnvironmentCapabilities environment;
    environment.screen_capture = true;
    environment.ui_tree = true;
    return environment;
}

ModuleNegotiationResult golden_negotiation() {
    const auto active = golden_dataset();
    return negotiate_modules(active, golden_environment(), CapabilityCatalog::core());
}

// Byte-exact canonical projection of golden_negotiation(); frozen from one
// --report run of this binary and asserted cross-process by G4 and by the
// --report mode in main().
constexpr const char *kGoldenNegotiationReport =
    R"({"modules":[{"conflicting_with":"acme.beta","missing":[],"module_digest":)"
    R"("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
    R"("module_id":"acme.alpha","status":"conflict","version":{"major":1,"minor":0,"patch":0}},)"
    R"({"conflicting_with":"acme.alpha","missing":[],"module_digest":)"
    R"("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
    R"("module_id":"acme.beta","status":"conflict","version":{"major":1,"minor":0,"patch":0}},)"
    R"({"conflicting_with":"","missing":["env.device.state","env.perception.sources"],)"
    R"("module_digest":)"
    R"("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",)"
    R"("module_id":"acme.delta","status":"unavailable","version":{"major":1,"minor":0,"patch":0}},)"
    R"({"conflicting_with":"","missing":[],"module_digest":)"
    R"("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",)"
    R"("module_id":"acme.echo","status":"available","version":{"major":1,"minor":0,"patch":0}},)"
    R"({"conflicting_with":"acme.alpha","missing":[],"module_digest":)"
    R"("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",)"
    R"("module_id":"acme.gamma","status":"conflict","version":{"major":1,"minor":0,"patch":0}}]})";

// ---------------------------------------------------------------------------
// G1: capability catalog and environment derivation
// ---------------------------------------------------------------------------

int core_catalog_matches_the_frozen_vocabulary() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();
    struct ExpectedEntry {
        const char *id;
        CapabilityKind kind;
    };
    const std::array<ExpectedEntry, 13> expected{{
        {"env.device.state", CapabilityKind::Boolean},
        {"env.epoch.invalidation", CapabilityKind::Boolean},
        {"env.foreground.app", CapabilityKind::Boolean},
        {"env.input.discrete", CapabilityKind::Boolean},
        {"env.input.release", CapabilityKind::Boolean},
        {"env.observation.atomic", CapabilityKind::Boolean},
        {"env.perception.sources", CapabilityKind::Counted},
        {"env.screen.capture", CapabilityKind::Boolean},
        {"env.ui.tree", CapabilityKind::Boolean},
        {"host.bridge.rpc", CapabilityKind::Boolean},
        {"host.process.supervision", CapabilityKind::Boolean},
        {"tool.fs.root", CapabilityKind::Boolean},
        {"tool.net.egress", CapabilityKind::Boolean},
    }};
    MIRA_CHECK(catalog.size() == expected.size());
    const auto &entries = catalog.entries();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        MIRA_CHECK(entries[index].id == expected[index].id);
        MIRA_CHECK(entries[index].kind == expected[index].kind);
    }
    // Strictly sorted by id: the catalog is a governance artifact.
    for (std::size_t index = 1; index < entries.size(); ++index) {
        MIRA_CHECK(entries[index - 1].id < entries[index].id);
    }

    // Digest is stable across calls and independent of the insertion order
    // that produced the catalog.
    MIRA_CHECK(catalog.digest() == CapabilityCatalog::core().digest());
    const std::vector<CapabilityDescriptor> reversed(entries.rbegin(), entries.rend());
    auto rebuilt = CapabilityCatalog::make(reversed);
    MIRA_CHECK(rebuilt.has_value());
    MIRA_CHECK(rebuilt.value().size() == catalog.size());
    MIRA_CHECK(rebuilt.value().digest() == catalog.digest());

    // Lookup: known entry (with the only Counted kind), unknown ids fail.
    const auto *counted = catalog.find("env.perception.sources");
    MIRA_CHECK(counted != nullptr);
    MIRA_CHECK(counted->kind == CapabilityKind::Counted);
    MIRA_CHECK(catalog.contains("tool.net.egress"));
    MIRA_CHECK(catalog.find("env.unknown.capability") == nullptr);
    MIRA_CHECK(!catalog.contains("ENV.SCREEN.CAPTURE"));
    return 0;
}

int catalog_make_fails_closed() {
    const auto duplicated = CapabilityCatalog::make(std::vector<CapabilityDescriptor>{
        {"env.screen.capture", CapabilityKind::Boolean, "first"},
        {"env.screen.capture", CapabilityKind::Boolean, "second"}});
    MIRA_CHECK(!duplicated.has_value());
    MIRA_CHECK(duplicated.error().domain == "mira.tool_module");

    const std::array<const char *, 6> invalid_ids{
        {"", ".leading.dot", "trailing.dot.", "dou..ble.dot", "has space", "UpperCase"}};
    for (const auto *invalid : invalid_ids) {
        const auto rejected = CapabilityCatalog::make(
            std::vector<CapabilityDescriptor>{{invalid, CapabilityKind::Boolean, "invalid"}});
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().domain == "mira.tool_module");
    }

    // An empty catalog is a valid (if useless) governance artifact.
    auto empty = CapabilityCatalog::make(std::vector<CapabilityDescriptor>{});
    MIRA_CHECK(empty.has_value());
    MIRA_CHECK(empty.value().size() == 0);
    return 0;
}

int derivation_covers_every_boolean_combination() {
    // Independent hand-written mapping from the frozen design table
    // (tool_module_design.md section 4.2), pushed in sorted id order so the
    // expectation is sorted by construction and shares no code with the
    // implementation under test.
    const std::array<CapabilityId, 9> kExpectedIds{
        {"env.device.state", "env.epoch.invalidation", "env.foreground.app", "env.input.discrete",
         "env.input.release", "env.observation.atomic", "env.perception.sources",
         "env.screen.capture", "env.ui.tree"}};

    constexpr std::array<bool EnvironmentCapabilities::*, 8> kBoolFields{
        &EnvironmentCapabilities::screen_capture,     &EnvironmentCapabilities::ui_tree,
        &EnvironmentCapabilities::foreground_app,     &EnvironmentCapabilities::device_state,
        &EnvironmentCapabilities::atomic_observation, &EnvironmentCapabilities::discrete_input,
        &EnvironmentCapabilities::input_release,      &EnvironmentCapabilities::epoch_invalidation,
    };

    const std::array<std::size_t, 3> kPerceptionValues{{0, 1, 3}};
    for (std::size_t mask = 0; mask < 256; ++mask) {
        for (const auto perception : kPerceptionValues) {
            EnvironmentCapabilities capabilities;
            for (std::size_t bit = 0; bit < kBoolFields.size(); ++bit) {
                capabilities.*kBoolFields[bit] = (mask & (std::size_t{1} << bit)) != 0;
            }
            capabilities.perception_sources = perception;

            std::vector<CapabilityId> expected;
            if (capabilities.device_state) {
                expected.push_back(kExpectedIds[0]);
            }
            if (capabilities.epoch_invalidation) {
                expected.push_back(kExpectedIds[1]);
            }
            if (capabilities.foreground_app) {
                expected.push_back(kExpectedIds[2]);
            }
            if (capabilities.discrete_input) {
                expected.push_back(kExpectedIds[3]);
            }
            if (capabilities.input_release) {
                expected.push_back(kExpectedIds[4]);
            }
            if (capabilities.atomic_observation) {
                expected.push_back(kExpectedIds[5]);
            }
            if (capabilities.perception_sources >= 1) {
                expected.push_back(kExpectedIds[6]);
            }
            if (capabilities.screen_capture) {
                expected.push_back(kExpectedIds[7]);
            }
            if (capabilities.ui_tree) {
                expected.push_back(kExpectedIds[8]);
            }

            const auto derived = derive_environment_capabilities(capabilities);
            MIRA_CHECK(derived == expected);
            // Strictly sorted output, and every derived id is governed
            // vocabulary.
            for (std::size_t index = 1; index < derived.size(); ++index) {
                MIRA_CHECK(derived[index - 1] < derived[index]);
            }
            for (const auto &id : derived) {
                MIRA_CHECK(CapabilityCatalog::core().contains(id));
            }
        }
    }

    // The all-false/zero declaration derives the empty set.
    const auto none = derive_environment_capabilities(EnvironmentCapabilities{});
    MIRA_CHECK(none.empty());

    // Quality-only fields never become vocabulary.
    EnvironmentCapabilities skewed;
    skewed.screen_capture = true;
    skewed.max_component_skew = std::chrono::nanoseconds(120);
    const auto with_skew = derive_environment_capabilities(skewed);
    MIRA_CHECK(with_skew == std::vector<CapabilityId>{"env.screen.capture"});
    return 0;
}

// ---------------------------------------------------------------------------
// G2: manifest parsing fail-closed matrix
// ---------------------------------------------------------------------------

int valid_manifest_parses_and_round_trips() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();
    auto base = parse_json(kValidManifestJson);
    MIRA_CHECK(base.has_value());
    auto parsed = parse_tool_module_manifest(base.value(), catalog);
    MIRA_CHECK(parsed.has_value());
    const ToolModuleManifest &manifest = parsed.value();
    MIRA_CHECK(manifest.module_id == "acme.tools.clipboard");
    MIRA_CHECK((manifest.version == SemanticVersion{1, 2, 3}));
    MIRA_CHECK(manifest.origin == ToolModuleOrigin::HostProvided);
    MIRA_CHECK(manifest.signer == "acme-host");
    MIRA_CHECK(manifest.signature_algorithm.empty());
    MIRA_CHECK(manifest.signature.empty());
    MIRA_CHECK(manifest.min_mira_module_abi == 1);
    MIRA_CHECK(manifest.required_capabilities ==
               (std::vector<CapabilityId>{"env.screen.capture", "env.ui.tree"}));
    MIRA_CHECK(manifest.infra_capabilities == std::vector<CapabilityId>{"host.bridge.rpc"});
    MIRA_CHECK(manifest.tools.size() == 2);
    MIRA_CHECK(manifest.tools[0].name == "read_clipboard");
    MIRA_CHECK((manifest.tools[0].version == SemanticVersion{0, 1, 0}));
    MIRA_CHECK(manifest.tools[0].description == "Reads clipboard text.");
    MIRA_CHECK(manifest.tools[0].side_effect == ActionRisk::R0ReadOnly);
    MIRA_CHECK(manifest.tools[0].required_capabilities ==
               std::vector<CapabilityId>{"env.screen.capture"});
    MIRA_CHECK(manifest.tools[0].data_access.reads == std::vector<std::string>{"clipboard"});
    MIRA_CHECK(manifest.tools[0].data_access.writes.empty());
    const auto *type = manifest.tools[0].arguments_schema.root.find("type");
    MIRA_CHECK(type != nullptr && type->as_string() != nullptr && *type->as_string() == "object");
    MIRA_CHECK(manifest.tools[1].name == "write_clipboard");
    MIRA_CHECK(manifest.tools[1].side_effect == ActionRisk::R2UserVisible);
    MIRA_CHECK(manifest.tools[1].data_access.writes == std::vector<std::string>{"clipboard"});
    MIRA_CHECK(manifest.conflicts_with == std::vector<std::string>{"acme.tools.other"});
    MIRA_CHECK(manifest.resources.max_total_concurrent_invocations == 4);
    MIRA_CHECK(manifest.resources.max_total_result_bytes == 1048576ULL);

    // Origin spellings round-trip through the names.
    MIRA_CHECK(tool_module_origin_name(ToolModuleOrigin::BuiltIn) == "built_in");
    MIRA_CHECK(tool_module_origin_name(ToolModuleOrigin::HostProvided) == "host_provided");
    MIRA_CHECK(tool_module_origin_name(ToolModuleOrigin::OutOfProcess) == "out_of_process");
    MIRA_CHECK(tool_module_origin_from_name("host_provided") == ToolModuleOrigin::HostProvided);
    MIRA_CHECK(tool_module_origin_from_name("in_process") == std::nullopt);

    // Version helpers: object form, boundary values, out-of-range rejection.
    const auto version = version_json(65535, 0, 65535);
    auto back = semantic_version_from_json(version);
    MIRA_CHECK(back.has_value());
    MIRA_CHECK((back.value() == SemanticVersion{65535, 0, 65535}));
    MIRA_CHECK(semantic_version_to_json(back.value()) == version);
    MIRA_CHECK(!semantic_version_from_json(version_json(65536, 0, 0)).has_value());
    MIRA_CHECK(!semantic_version_from_json(JsonValue{std::string("1.0.0")}).has_value());

    // Snapshot union: module level, infra and every member requirement,
    // sorted and deduplicated; the snapshot digest is the manifest digest.
    const ModuleSnapshot snapshot = module_snapshot_from_manifest(manifest);
    MIRA_CHECK(snapshot.module_id == manifest.module_id);
    MIRA_CHECK(snapshot.required ==
               (std::vector<CapabilityId>{"env.screen.capture", "env.ui.tree", "host.bridge.rpc"}));
    MIRA_CHECK(snapshot.module_digest == tool_module_manifest_digest(manifest));
    MIRA_CHECK(snapshot.tools.size() == manifest.tools.size());
    MIRA_CHECK(snapshot.conflicts_with == manifest.conflicts_with);
    return 0;
}

void mutate_wrong_schema(JsonValue &manifest) {
    manifest.set("schema", JsonValue{std::string("mira.tool_module.manifest.v2")});
}

void mutate_wrong_schema_version(JsonValue &manifest) {
    manifest.set("schema_version", JsonValue{std::string("1.1")});
}

void mutate_uppercase_module_id(JsonValue &manifest) {
    manifest.set("module_id", JsonValue{std::string("Acme.Tools.Clipboard")});
}

void mutate_empty_module_id(JsonValue &manifest) {
    manifest.set("module_id", JsonValue{std::string()});
}

void mutate_leading_dot_module_id(JsonValue &manifest) {
    manifest.set("module_id", JsonValue{std::string(".acme.tools.clipboard")});
}

void mutate_oversize_module_id(JsonValue &manifest) {
    manifest.set("module_id", std::string(129, 'a'));
}

void mutate_missing_version(JsonValue &manifest) { erase_member(manifest, "version"); }

void mutate_version_missing_patch(JsonValue &manifest) {
    JsonValue version = *manifest.find("version");
    erase_member(version, "patch");
    manifest.set("version", std::move(version));
}

void mutate_version_out_of_range(JsonValue &manifest) {
    manifest.set("version", version_json(1, 2, 65536));
}

void mutate_unknown_isolation(JsonValue &manifest) {
    member_of(manifest, "origin").set("isolation", JsonValue{std::string("in_process")});
}

void mutate_out_of_process_missing_signature(JsonValue &manifest) {
    JsonValue &origin = member_of(manifest, "origin");
    origin.set("isolation", JsonValue{std::string("out_of_process")});
    origin.set("signature_algorithm", JsonValue{std::string("ed25519")});
    // signer present, signature absent: the trust triple is incomplete.
}

void mutate_out_of_process_missing_algorithm(JsonValue &manifest) {
    JsonValue &origin = member_of(manifest, "origin");
    origin.set("isolation", JsonValue{std::string("out_of_process")});
    origin.set("signature", JsonValue{std::string("c2lnbmF0dXJl")});
    // signer present, signature_algorithm absent.
}

void mutate_host_provided_missing_signer(JsonValue &manifest) {
    erase_member(member_of(manifest, "origin"), "signer");
}

void mutate_required_outside_catalog(JsonValue &manifest) {
    manifest.set("required_capabilities",
                 JsonValue{JsonValue::Array{JsonValue{std::string("env.nonexistent.capability")}}});
}

void mutate_infra_outside_catalog(JsonValue &manifest) {
    manifest.set("infra_capabilities",
                 JsonValue{JsonValue::Array{JsonValue{std::string("host.secret.vault")}}});
}

void mutate_member_capability_outside_catalog(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    tools[0].set("required_capabilities",
                 JsonValue{JsonValue::Array{JsonValue{std::string("env.nonexistent.capability")}}});
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_duplicate_capability(JsonValue &manifest) {
    manifest.set("required_capabilities",
                 JsonValue{JsonValue::Array{JsonValue{std::string("env.screen.capture")},
                                            JsonValue{std::string("env.screen.capture")}}});
}

void mutate_duplicate_member_name(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    tools.push_back(tools[0]);
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_member_name_illegal_character(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    tools[0].set("name", JsonValue{std::string("read clipboard")});
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_missing_arguments_schema(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    erase_member(tools[0], "arguments_schema");
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_schema_not_an_object(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    tools[0].set("result_schema", JsonValue{std::string("object")});
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_schema_unsupported_keyword(JsonValue &manifest) {
    JsonValue::Array tools = *manifest.find("tools")->as_array();
    tools[0].set(
        "arguments_schema",
        JsonValue{JsonValue::Object{{"$ref", JsonValue{std::string("#/definitions/clipboard")}}}});
    manifest.set("tools", JsonValue{std::move(tools)});
}

void mutate_missing_resources(JsonValue &manifest) { erase_member(manifest, "resources"); }

void mutate_zero_resources(JsonValue &manifest) {
    member_of(manifest, "resources")
        .set("max_total_concurrent_invocations", JsonValue{static_cast<std::int64_t>(0)});
}

void mutate_oversize_concurrency(JsonValue &manifest) {
    member_of(manifest, "resources")
        .set("max_total_concurrent_invocations", JsonValue{static_cast<std::int64_t>(1025)});
}

void mutate_oversize_result_bytes(JsonValue &manifest) {
    member_of(manifest, "resources")
        .set("max_total_result_bytes", JsonValue{static_cast<std::int64_t>(64) * 1024 * 1024 + 1});
}

void mutate_future_abi(JsonValue &manifest) {
    manifest.set("min_mira_module_abi", JsonValue{static_cast<std::int64_t>(2)});
}

void mutate_empty_tools(JsonValue &manifest) {
    manifest.set("tools", JsonValue{JsonValue::Array{}});
}

int manifest_fail_closed_matrix() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();
    // Expected error domain per rejection: every failure of the manifest
    // parser, including member schema-subset violations, must carry the
    // single "mira.tool_module" domain.
    struct RejectionCase {
        void (*mutate)(JsonValue &);
        const char *expected_domain;
    };
    constexpr char kModuleDomain[] = "mira.tool_module";
    const std::array<RejectionCase, 28> kCases{{
        {mutate_wrong_schema, kModuleDomain},
        {mutate_wrong_schema_version, kModuleDomain},
        {mutate_uppercase_module_id, kModuleDomain},
        {mutate_empty_module_id, kModuleDomain},
        {mutate_leading_dot_module_id, kModuleDomain},
        {mutate_oversize_module_id, kModuleDomain},
        {mutate_missing_version, kModuleDomain},
        {mutate_version_missing_patch, kModuleDomain},
        {mutate_version_out_of_range, kModuleDomain},
        {mutate_unknown_isolation, kModuleDomain},
        {mutate_out_of_process_missing_signature, kModuleDomain},
        {mutate_out_of_process_missing_algorithm, kModuleDomain},
        {mutate_host_provided_missing_signer, kModuleDomain},
        {mutate_required_outside_catalog, kModuleDomain},
        {mutate_infra_outside_catalog, kModuleDomain},
        {mutate_member_capability_outside_catalog, kModuleDomain},
        {mutate_duplicate_capability, kModuleDomain},
        {mutate_duplicate_member_name, kModuleDomain},
        {mutate_member_name_illegal_character, kModuleDomain},
        {mutate_missing_arguments_schema, kModuleDomain},
        {mutate_schema_not_an_object, kModuleDomain},
        {mutate_schema_unsupported_keyword, kModuleDomain},
        {mutate_missing_resources, kModuleDomain},
        {mutate_zero_resources, kModuleDomain},
        {mutate_oversize_concurrency, kModuleDomain},
        {mutate_oversize_result_bytes, kModuleDomain},
        {mutate_future_abi, kModuleDomain},
        {mutate_empty_tools, kModuleDomain},
    }};
    for (const auto &item : kCases) {
        auto base = parse_json(kValidManifestJson);
        MIRA_CHECK(base.has_value());
        item.mutate(base.value());
        auto rejected = parse_tool_module_manifest(base.value(), catalog);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().domain == item.expected_domain);
        // No global state: the untouched manifest still parses after every
        // rejection.
        auto fresh = parse_json(kValidManifestJson);
        MIRA_CHECK(fresh.has_value());
        MIRA_CHECK(parse_tool_module_manifest(fresh.value(), catalog).has_value());
    }

    // A non-object root is rejected as well.
    const JsonValue array_root{
        JsonValue::Array{JsonValue{std::string("mira.tool_module.manifest.v1")}}};
    auto rejected_root = parse_tool_module_manifest(array_root, catalog);
    MIRA_CHECK(!rejected_root.has_value());
    MIRA_CHECK(rejected_root.error().domain == "mira.tool_module");
    return 0;
}

// Regression (2026-09-19, found by TM2 exposure verification): the duplicate
// member-name gate once kept std::string_views into a loop-local string that
// was moved and destroyed each iteration, so two consecutive equal-length
// short (SSO, <=15 chars) member names collided spuriously and a valid
// manifest was rejected as a "duplicate". The gate now stores copies. These
// cases pin both directions with short names specifically.
int manifest_short_member_names_parse_and_duplicates_rejected() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();

    const auto parse_with_tools =
        [&](const std::vector<std::string> &names) -> Result<ToolModuleManifest> {
        auto base = parse_json(kValidManifestJson);
        if (!base.has_value()) {
            std::abort(); // fixture constant, not a subject
        }
        JsonValue::Array tools;
        for (const auto &name : names) {
            tools.push_back(minimal_tool_json(name));
        }
        base.value().set("tools", JsonValue{std::move(tools)});
        return parse_tool_module_manifest(base.value(), catalog);
    };

    // Equal-length distinct short names (the exact shapes the old dangling
    // view misread) parse successfully, and the declared order is preserved.
    {
        const std::vector<std::string> names{"zz_probe", "aa_probe"}; // both 8 chars
        auto parsed = parse_with_tools(names);
        MIRA_CHECK(parsed.has_value());
        MIRA_CHECK(parsed.value().tools.size() == 2);
        MIRA_CHECK(parsed.value().tools[0].name == "zz_probe");
        MIRA_CHECK(parsed.value().tools[1].name == "aa_probe");

        const std::vector<std::string> other{"member_a", "member_b"}; // both 8 chars
        auto second = parse_with_tools(other);
        MIRA_CHECK(second.has_value());
        MIRA_CHECK(second.value().tools.size() == 2);

        // Three members, two of them consecutive equal-length short names.
        const std::vector<std::string> triple{"one_aaa", "two_bbb", "three_ccc"};
        auto third = parse_with_tools(triple);
        MIRA_CHECK(third.has_value());
        MIRA_CHECK(third.value().tools.size() == 3);
        MIRA_CHECK(third.value().tools[0].name == "one_aaa");
        MIRA_CHECK(third.value().tools[1].name == "two_bbb");
        MIRA_CHECK(third.value().tools[2].name == "three_ccc");

        // Same inputs, same digest: the short-name parses are deterministic.
        auto repeat = parse_with_tools(names);
        MIRA_CHECK(repeat.has_value());
        MIRA_CHECK(tool_module_manifest_digest(repeat.value()) ==
                   tool_module_manifest_digest(parsed.value()));
    }

    // True duplicates are still rejected wherever they sit: adjacent, and
    // separated by a name of a different length as well as the same length.
    {
        const auto adjacent = parse_with_tools({"dup_name", "dup_name"});
        MIRA_CHECK(!adjacent.has_value());
        MIRA_CHECK(adjacent.error().domain == "mira.tool_module");
        MIRA_CHECK(adjacent.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(adjacent.error().safe_message.find("dup_name") != std::string::npos);

        const auto separated_short = parse_with_tools({"dup_name", "middle_one", "dup_name"});
        MIRA_CHECK(!separated_short.has_value());
        MIRA_CHECK(separated_short.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(separated_short.error().safe_message.find("dup_name") != std::string::npos);

        const auto separated_equal_length =
            parse_with_tools({"dup_name", "mid_name", "dup_name"}); // all 8 chars
        MIRA_CHECK(!separated_equal_length.has_value());
        MIRA_CHECK(separated_equal_length.error().code == ErrorCode::InvalidArgument);
    }
    return 0;
}

int manifest_digest_is_canonical_and_content_addressed() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();
    auto first = parse_json(kValidManifestJson);
    MIRA_CHECK(first.has_value());
    auto first_manifest = parse_tool_module_manifest(first.value(), catalog);
    MIRA_CHECK(first_manifest.has_value());
    const Hash first_digest = tool_module_manifest_digest(first_manifest.value());

    // Key order and whitespace do not affect the digest.
    auto second = parse_json(kValidManifestJsonReordered);
    MIRA_CHECK(second.has_value());
    auto second_manifest = parse_tool_module_manifest(second.value(), catalog);
    MIRA_CHECK(second_manifest.has_value());
    MIRA_CHECK(tool_module_manifest_digest(second_manifest.value()) == first_digest);
    // ... and the two parses are semantically equal structures.
    MIRA_CHECK(second_manifest.value().tools.size() == first_manifest.value().tools.size());
    MIRA_CHECK(second_manifest.value().tools[0].name == first_manifest.value().tools[0].name);

    // Any structural change changes the digest: identity, risk tier and the
    // opaque signature payload are all covered fields.
    ToolModuleManifest renamed = first_manifest.value();
    renamed.module_id = "acme.tools.clipboard.two";
    MIRA_CHECK(tool_module_manifest_digest(renamed) != first_digest);

    ToolModuleManifest riskier = first_manifest.value();
    riskier.tools[0].side_effect = ActionRisk::R1ReversibleLow;
    MIRA_CHECK(tool_module_manifest_digest(riskier) != first_digest);

    ToolModuleManifest signed_manifest = first_manifest.value();
    signed_manifest.signature = "c2lnbmF0dXJl";
    MIRA_CHECK(tool_module_manifest_digest(signed_manifest) != first_digest);

    ToolModuleManifest rebudgeted = first_manifest.value();
    rebudgeted.resources.max_total_concurrent_invocations = 5;
    MIRA_CHECK(tool_module_manifest_digest(rebudgeted) != first_digest);

    // Repeated digesting is stable.
    MIRA_CHECK(tool_module_manifest_digest(first_manifest.value()) == first_digest);
    return 0;
}

// ---------------------------------------------------------------------------
// G3: negotiation fail-closed behavior
// ---------------------------------------------------------------------------

int negotiation_fails_closed_on_missing_and_conflicts() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();

    // Missing capabilities: Unavailable with the complete sorted missing
    // list; satisfied modules stay Available.
    std::vector<ModuleSnapshot> active;
    active.push_back(make_snapshot("acme.alpha", 'a', {"alpha_read"},
                                   {"env.screen.capture", "env.ui.tree"}, {}));
    active.push_back(make_snapshot("acme.beta", 'b', {"beta_read"},
                                   {"env.screen.capture", "env.perception.sources"}, {}));
    active.push_back(
        make_snapshot("acme.gamma", 'c', {"gamma_read"}, {"env.device.state", "env.ui.tree"}, {}));
    const auto result = negotiate_modules(active, golden_environment(), catalog);
    MIRA_CHECK(result.modules.size() == 3);
    for (std::size_t index = 1; index < result.modules.size(); ++index) {
        MIRA_CHECK(result.modules[index - 1].module_id < result.modules[index].module_id);
    }
    MIRA_CHECK(result.modules[0].module_id == "acme.alpha");
    MIRA_CHECK(result.modules[0].status == ModuleStatus::Available);
    MIRA_CHECK(result.modules[0].missing.empty());
    MIRA_CHECK(result.modules[0].conflicting_with.empty());
    MIRA_CHECK(result.modules[1].module_id == "acme.beta");
    MIRA_CHECK(result.modules[1].status == ModuleStatus::Unavailable);
    MIRA_CHECK(result.modules[1].missing == std::vector<CapabilityId>{"env.perception.sources"});
    MIRA_CHECK(result.modules[2].module_id == "acme.gamma");
    MIRA_CHECK(result.modules[2].status == ModuleStatus::Unavailable);
    MIRA_CHECK(result.modules[2].missing == std::vector<CapabilityId>{"env.device.state"});

    // A one-directional conflicts_with declaration still conflicts both
    // modules.
    std::vector<ModuleSnapshot> declared;
    declared.push_back(make_snapshot("mod.a", 'a', {"tool_a"}, {}, {"mod.z"}));
    declared.push_back(make_snapshot("mod.z", 'b', {"tool_z"}, {}, {}));
    const auto conflict = negotiate_modules(declared, golden_environment(), catalog);
    MIRA_CHECK(conflict.modules[0].module_id == "mod.a");
    MIRA_CHECK(conflict.modules[0].status == ModuleStatus::Conflict);
    MIRA_CHECK(conflict.modules[0].conflicting_with == "mod.z");
    MIRA_CHECK(conflict.modules[1].module_id == "mod.z");
    MIRA_CHECK(conflict.modules[1].status == ModuleStatus::Conflict);
    MIRA_CHECK(conflict.modules[1].conflicting_with == "mod.a");

    // Cross-module wire-namespace collisions on member tool names conflict
    // both modules.
    std::vector<ModuleSnapshot> colliding;
    colliding.push_back(make_snapshot("mod.p", 'c', {"shared_tool"}, {}, {}));
    colliding.push_back(make_snapshot("mod.q", 'd', {"shared_tool"}, {}, {}));
    const auto collision = negotiate_modules(colliding, golden_environment(), catalog);
    MIRA_CHECK(collision.modules[0].module_id == "mod.p");
    MIRA_CHECK(collision.modules[0].status == ModuleStatus::Conflict);
    MIRA_CHECK(collision.modules[0].conflicting_with == "mod.q");
    MIRA_CHECK(collision.modules[1].module_id == "mod.q");
    MIRA_CHECK(collision.modules[1].status == ModuleStatus::Conflict);
    MIRA_CHECK(collision.modules[1].conflicting_with == "mod.p");

    // Catalog-unknown requirements are missing even when the environment
    // honestly claims the derived capability: the governed catalog wins.
    std::vector<CapabilityDescriptor> trimmed;
    for (const auto &entry : CapabilityCatalog::core().entries()) {
        if (entry.id != "env.device.state") {
            trimmed.push_back(entry);
        }
    }
    auto partial = CapabilityCatalog::make(trimmed);
    MIRA_CHECK(partial.has_value());
    EnvironmentCapabilities claiming;
    claiming.device_state = true;
    std::vector<ModuleSnapshot> unknown;
    unknown.push_back(make_snapshot("mod.u", 'e', {"tool_u"}, {"env.device.state"}, {}));
    const auto unknown_result = negotiate_modules(unknown, claiming, partial.value());
    MIRA_CHECK(unknown_result.modules.size() == 1);
    MIRA_CHECK(unknown_result.modules[0].status == ModuleStatus::Unavailable);
    MIRA_CHECK(unknown_result.modules[0].missing == std::vector<CapabilityId>{"env.device.state"});

    // Duplicate module ids in one Active set: every copy conflicts with its
    // twin instead of silently merging.
    std::vector<ModuleSnapshot> twins;
    twins.push_back(make_snapshot("mod.dup", 'a', {"tool_dup"}, {"env.screen.capture"}, {}));
    twins.push_back(make_snapshot("mod.dup", 'b', {"tool_dup"}, {}, {}));
    const auto duplicated = negotiate_modules(twins, golden_environment(), catalog);
    MIRA_CHECK(duplicated.modules.size() == 2);
    MIRA_CHECK(duplicated.modules[0].status == ModuleStatus::Conflict);
    MIRA_CHECK(duplicated.modules[0].conflicting_with == "mod.dup");
    MIRA_CHECK(duplicated.modules[1].status == ModuleStatus::Conflict);
    MIRA_CHECK(duplicated.modules[1].conflicting_with == "mod.dup");

    // TM0 negotiation over Active snapshots never emits Revoked.
    for (const auto &availability : result.modules) {
        MIRA_CHECK(availability.status != ModuleStatus::Revoked);
    }
    for (const auto &availability : conflict.modules) {
        MIRA_CHECK(availability.status != ModuleStatus::Revoked);
    }
    for (const auto &availability : collision.modules) {
        MIRA_CHECK(availability.status != ModuleStatus::Revoked);
    }
    for (const auto &availability : duplicated.modules) {
        MIRA_CHECK(availability.status != ModuleStatus::Revoked);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G4: determinism and the frozen golden report
// ---------------------------------------------------------------------------

int negotiation_is_deterministic_and_matches_the_golden() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();
    const auto first = golden_negotiation();
    const auto second = golden_negotiation();
    MIRA_CHECK(first.digest == second.digest);
    MIRA_CHECK(canonical_json_string(module_negotiation_to_json(first)) ==
               canonical_json_string(module_negotiation_to_json(second)));

    // The result is independent of the input construction order.
    std::vector<ModuleSnapshot> shuffled;
    const auto dataset = golden_dataset();
    shuffled.push_back(dataset[3]);
    shuffled.push_back(dataset[0]);
    shuffled.push_back(dataset[4]);
    shuffled.push_back(dataset[2]);
    shuffled.push_back(dataset[1]);
    const auto reordered = negotiate_modules(shuffled, golden_environment(), catalog);
    MIRA_CHECK(canonical_json_string(module_negotiation_to_json(reordered)) ==
               canonical_json_string(module_negotiation_to_json(first)));
    MIRA_CHECK(reordered.digest == first.digest);

    // A different environment produces a different projection and digest.
    EnvironmentCapabilities richer = golden_environment();
    richer.device_state = true;
    const auto other = negotiate_modules(dataset, richer, catalog);
    MIRA_CHECK(other.digest != first.digest);
    MIRA_CHECK(other.modules[2].module_id == "acme.delta");
    MIRA_CHECK(other.modules[2].missing == std::vector<CapabilityId>{"env.perception.sources"});

    // The frozen golden: byte-exact against the canonical report of the fixed
    // dataset (the same bytes --report prints, asserted across processes).
    const std::string report = canonical_json_string(module_negotiation_to_json(first));
    MIRA_CHECK(report == kGoldenNegotiationReport);

    // The statuses behind the golden, for diagnosable failures.
    MIRA_CHECK(first.modules.size() == 5);
    MIRA_CHECK(first.modules[0].module_id == "acme.alpha");
    MIRA_CHECK(first.modules[0].status == ModuleStatus::Conflict);
    MIRA_CHECK(first.modules[0].conflicting_with == "acme.beta");
    MIRA_CHECK(first.modules[1].module_id == "acme.beta");
    MIRA_CHECK(first.modules[1].status == ModuleStatus::Conflict);
    MIRA_CHECK(first.modules[1].conflicting_with == "acme.alpha");
    MIRA_CHECK(first.modules[2].module_id == "acme.delta");
    MIRA_CHECK(first.modules[2].status == ModuleStatus::Unavailable);
    MIRA_CHECK(first.modules[2].missing ==
               (std::vector<CapabilityId>{"env.device.state", "env.perception.sources"}));
    MIRA_CHECK(first.modules[3].module_id == "acme.echo");
    MIRA_CHECK(first.modules[3].status == ModuleStatus::Available);
    MIRA_CHECK(first.modules[4].module_id == "acme.gamma");
    MIRA_CHECK(first.modules[4].status == ModuleStatus::Conflict);
    MIRA_CHECK(first.modules[4].conflicting_with == "acme.alpha");
    for (const auto &availability : first.modules) {
        MIRA_CHECK(availability.status != ModuleStatus::Revoked);
    }
    // Status spellings are part of the projection contract.
    MIRA_CHECK(module_status_name(ModuleStatus::Available) == "available");
    MIRA_CHECK(module_status_name(ModuleStatus::Unavailable) == "unavailable");
    MIRA_CHECK(module_status_name(ModuleStatus::Conflict) == "conflict");
    MIRA_CHECK(module_status_name(ModuleStatus::Revoked) == "revoked");
    return 0;
}

// ---------------------------------------------------------------------------
// G5: boundedness and degenerate paths
// ---------------------------------------------------------------------------

int boundedness_limits_and_degenerate_paths() {
    const CapabilityCatalog &catalog = CapabilityCatalog::core();

    // 257 member tools exceed the module budget.
    JsonValue::Array tools;
    for (int index = 0; index < 257; ++index) {
        tools.push_back(minimal_tool_json("tool_" + std::to_string(10000 + index)));
    }
    auto many_tools = parse_json(kValidManifestJson);
    MIRA_CHECK(many_tools.has_value());
    many_tools.value().set("tools", JsonValue{std::move(tools)});
    auto rejected_tools = parse_tool_module_manifest(many_tools.value(), catalog);
    MIRA_CHECK(!rejected_tools.has_value());
    MIRA_CHECK(rejected_tools.error().domain == "mira.tool_module");

    // 65 entries in one capability list exceed the list budget.
    JsonValue::Array capabilities(65U, JsonValue{std::string("env.screen.capture")});
    auto many_capabilities = parse_json(kValidManifestJson);
    MIRA_CHECK(many_capabilities.has_value());
    many_capabilities.value().set("required_capabilities", JsonValue{std::move(capabilities)});
    auto rejected_capabilities = parse_tool_module_manifest(many_capabilities.value(), catalog);
    MIRA_CHECK(!rejected_capabilities.has_value());
    MIRA_CHECK(rejected_capabilities.error().code == ErrorCode::ResourceExhausted);

    // 65 conflict declarations exceed the conflict budget.
    JsonValue::Array conflicts(65U, JsonValue{std::string("other.module")});
    auto many_conflicts = parse_json(kValidManifestJson);
    MIRA_CHECK(many_conflicts.has_value());
    many_conflicts.value().set("conflicts_with", JsonValue{std::move(conflicts)});
    auto rejected_conflicts = parse_tool_module_manifest(many_conflicts.value(), catalog);
    MIRA_CHECK(!rejected_conflicts.has_value());
    MIRA_CHECK(rejected_conflicts.error().domain == "mira.tool_module");

    // The manifest byte ceiling is enforced through custom limits.
    ToolModuleLimits tight;
    tight.max_manifest_bytes = 32;
    auto base = parse_json(kValidManifestJson);
    MIRA_CHECK(base.has_value());
    MIRA_CHECK(!parse_tool_module_manifest(base.value(), catalog, tight).has_value());

    // An empty Active set negotiates to an empty result whose digest is
    // still well defined.
    const std::vector<ModuleSnapshot> empty;
    const auto none = negotiate_modules(empty, EnvironmentCapabilities{}, catalog);
    MIRA_CHECK(none.modules.empty());
    const JsonValue empty_report{JsonValue::Object{{"modules", JsonValue{JsonValue::Array{}}}}};
    MIRA_CHECK(none.digest == canonical_json_digest(empty_report));

    // An honest but empty environment makes every module Unavailable.
    std::vector<ModuleSnapshot> active;
    active.push_back(make_snapshot("acme.alpha", 'a', {"alpha_read"}, {"env.screen.capture"}, {}));
    active.push_back(
        make_snapshot("acme.beta", 'b', {"beta_read"}, {"env.perception.sources"}, {}));
    const auto unavailable = negotiate_modules(active, EnvironmentCapabilities{}, catalog);
    MIRA_CHECK(unavailable.modules.size() == 2);
    MIRA_CHECK(unavailable.modules[0].module_id == "acme.alpha");
    MIRA_CHECK(unavailable.modules[0].status == ModuleStatus::Unavailable);
    MIRA_CHECK(unavailable.modules[0].missing == std::vector<CapabilityId>{"env.screen.capture"});
    MIRA_CHECK(unavailable.modules[1].module_id == "acme.beta");
    MIRA_CHECK(unavailable.modules[1].status == ModuleStatus::Unavailable);
    MIRA_CHECK(unavailable.modules[1].missing ==
               std::vector<CapabilityId>{"env.perception.sources"});
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report: print the canonical negotiation report of the fixed dataset
    // and exit; used for cross-process byte comparison of the golden. No
    // other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const auto result = golden_negotiation();
        std::cout << canonical_json_string(module_negotiation_to_json(result)) << '\n';
        return 0;
    }

    // G1: catalog and derivation.
    if (core_catalog_matches_the_frozen_vocabulary() != 0) {
        return 1;
    }
    if (catalog_make_fails_closed() != 0) {
        return 1;
    }
    if (derivation_covers_every_boolean_combination() != 0) {
        return 1;
    }
    // G2: manifest fail-closed matrix.
    if (valid_manifest_parses_and_round_trips() != 0) {
        return 1;
    }
    if (manifest_fail_closed_matrix() != 0) {
        return 1;
    }
    if (manifest_short_member_names_parse_and_duplicates_rejected() != 0) {
        return 1;
    }
    if (manifest_digest_is_canonical_and_content_addressed() != 0) {
        return 1;
    }
    // G3: negotiation fail-closed behavior.
    if (negotiation_fails_closed_on_missing_and_conflicts() != 0) {
        return 1;
    }
    // G4: determinism and golden report.
    if (negotiation_is_deterministic_and_matches_the_golden() != 0) {
        return 1;
    }
    // G5: boundedness and degenerate paths.
    if (boundedness_limits_and_degenerate_paths() != 0) {
        return 1;
    }
    // G6 (consumer closure) lives in examples/minimal_consumer.cpp.
    return 0;
}
