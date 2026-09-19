// M7 TM1 verification: tool module registry lifecycle (gates M7-TM1-G1..G6,
// frozen in docs/plans/m7-tools-evaluation-platform-v1.md section 5.2).
//
// Deterministic by construction: no clock and no randomness reach any asserted
// value (event ids and store timestamps exist but never enter an assertion or
// the --report output). The --report mode prints a canonical JSON report of a
// fixed registry/negotiation scenario for cross-process byte comparison.

#include "../support/test.hpp"

#include <mira/tool_module.hpp>
#include <mira/tool_module_registry.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

JsonValue version_json(std::int64_t major, std::int64_t minor, std::int64_t patch) {
    JsonValue::Object object;
    object.emplace_back("major", JsonValue{major});
    object.emplace_back("minor", JsonValue{minor});
    object.emplace_back("patch", JsonValue{patch});
    return JsonValue{std::move(object)};
}

// Known-good manifest built through the real parser (TM0 fixture style):
// one member tool, explicit resources, optional trust fields and requirements.
// A fixture failure is fatal: these inputs are test constants, not subjects.
ToolModuleManifest fixture_manifest(const std::string &module_id, const std::string &tool_name,
                                    ToolModuleOrigin origin, const std::string &signer = {},
                                    const std::string &algorithm = {},
                                    const std::string &signature = {},
                                    const std::vector<CapabilityId> &required = {}) {
    JsonValue::Object origin_object;
    origin_object.emplace_back("isolation", std::string(tool_module_origin_name(origin)));
    if (!signer.empty()) {
        origin_object.emplace_back("signer", signer);
    }
    if (!algorithm.empty()) {
        origin_object.emplace_back("signature_algorithm", algorithm);
    }
    if (!signature.empty()) {
        origin_object.emplace_back("signature", signature);
    }

    JsonValue::Object tool;
    tool.emplace_back("name", tool_name);
    tool.emplace_back("version", version_json(0, 1, 0));
    tool.emplace_back("arguments_schema",
                      JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    tool.emplace_back("result_schema",
                      JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    tool.emplace_back("side_effect", std::string("read_only"));
    JsonValue::Array tools;
    tools.emplace_back(JsonValue{std::move(tool)});

    JsonValue::Object resources;
    resources.emplace_back("max_total_concurrent_invocations", JsonValue{std::int64_t{2}});
    resources.emplace_back("max_total_result_bytes", JsonValue{std::int64_t{4096}});

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolModuleManifestSchema));
    root.emplace_back("schema_version", std::string(kToolModuleManifestSchemaVersion));
    root.emplace_back("module_id", module_id);
    root.emplace_back("version", version_json(1, 0, 0));
    root.emplace_back("origin", JsonValue{std::move(origin_object)});
    root.emplace_back("tools", JsonValue{std::move(tools)});
    root.emplace_back("resources", JsonValue{std::move(resources)});
    if (!required.empty()) {
        JsonValue::Array capabilities;
        for (const auto &id : required) {
            capabilities.emplace_back(JsonValue{id});
        }
        root.emplace_back("required_capabilities", JsonValue{std::move(capabilities)});
    }

    auto parsed = parse_tool_module_manifest(JsonValue{std::move(root)}, CapabilityCatalog::core());
    if (!parsed.has_value()) {
        std::cerr << "fixture manifest '" << module_id
                  << "' failed to parse: " << parsed.error().safe_message << '\n';
        std::abort();
    }
    return parsed.value();
}

ModuleTrustConfig builtin_trust_for(const std::vector<ToolModuleManifest> &manifests) {
    ModuleTrustConfig config;
    for (const auto &manifest : manifests) {
        config.builtin_digests.push_back(tool_module_manifest_digest(manifest));
    }
    return config;
}

ModuleTrustConfig host_trust_for(std::vector<std::string> allowlist) {
    ModuleTrustConfig config;
    config.host_provided_allowlist = std::move(allowlist);
    return config;
}

ModuleTrustConfig oop_trust_for(IModuleSignatureVerifier *verifier,
                                std::vector<std::string> signers) {
    ModuleTrustConfig config;
    config.trusted_out_of_process_signers = std::move(signers);
    config.verifier = verifier;
    return config;
}

// Deterministic ids so event emission is exercised without randomness leaking
// into anything asserted.
RuntimeId test_runtime_id() {
    const auto id = RuntimeId::parse("0102030405060708090a0b0c0d0e0f10");
    return id.has_value() ? *id : RuntimeId{};
}

SessionId test_session_id() {
    const auto id = SessionId::parse("1112131415161718191a1b1c1d1e1f20");
    return id.has_value() ? *id : SessionId{};
}

// Deterministic reference verifier: counts calls, remembers the exact binding
// tuple, and either accepts or rejects. Callers read `last_*` only after a
// synchronizing event (pure call or future.get()).
class ProbeVerifier final : public IModuleSignatureVerifier {
  public:
    explicit ProbeVerifier(bool accept) : accept_(accept) {}

    bool verify(std::string_view signer, std::string_view algorithm, std::string_view signature,
                const Hash &signed_digest) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        last_signer = std::string(signer);
        last_algorithm = std::string(algorithm);
        last_signature = std::string(signature);
        last_digest = signed_digest;
        return accept_;
    }

    std::atomic<int> calls_{0};
    std::string last_signer;
    std::string last_algorithm;
    std::string last_signature;
    Hash last_digest{};

  private:
    bool accept_;
};

class ThrowingVerifier final : public IModuleSignatureVerifier {
  public:
    bool verify(std::string_view, std::string_view, std::string_view, const Hash &) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("host verifier defect");
    }

    std::atomic<int> calls_{0};
};

Error store_error(std::string message) {
    Error error;
    error.code = ErrorCode::Unavailable;
    error.domain = "mira.test";
    error.safe_message = std::move(message);
    return error;
}

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

template <typename T> bool failed_with(const Result<T> &result, ErrorCode code) {
    return !result.has_value() && result.error().code == code;
}

bool state_is(const ModuleRegistry &registry, std::string_view module_id, ModuleState expected) {
    const ModuleRecord *record = registry.find(module_id);
    return record != nullptr && record->state == expected;
}

Result<std::vector<EventEnvelope>> query_events(const MemoryEventStore &store) {
    EventQuery query;
    query.session_id = test_session_id();
    query.limit = 8192;
    auto page = store.read(query);
    if (!page.has_value()) {
        return page.error();
    }
    return page.value().events;
}

std::size_t count_events(const std::vector<EventEnvelope> &events, std::string_view type) {
    std::size_t total = 0;
    for (const auto &envelope : events) {
        if (envelope.payload.type == type) {
            ++total;
        }
    }
    return total;
}

std::vector<std::string> lifecycle_kinds(const std::vector<EventEnvelope> &events) {
    std::vector<std::string> kinds;
    for (const auto &envelope : events) {
        if (envelope.payload.type != kModuleLifecycleEventSchema) {
            continue;
        }
        auto data = parse_json(envelope.payload.data);
        if (!data.has_value()) {
            kinds.emplace_back("<unparseable>");
            continue;
        }
        const auto *kind = data.value().find("kind");
        if (kind == nullptr || kind->as_string() == nullptr) {
            kinds.emplace_back("<missing>");
            continue;
        }
        kinds.push_back(*kind->as_string());
    }
    return kinds;
}

// Full field-level lifecycle payload check: schema, kind, identity, version,
// digest, origin, classification and (optionally) a reason substring.
bool lifecycle_event_matches(const EventEnvelope &envelope, std::string_view kind,
                             const ToolModuleManifest &manifest, EventClass classification,
                             const std::string &reason_substring = std::string{}) {
    if (envelope.payload.type != kModuleLifecycleEventSchema) {
        return false;
    }
    if (envelope.payload.classification != classification) {
        return false;
    }
    auto data = parse_json(envelope.payload.data);
    if (!data.has_value()) {
        return false;
    }
    const JsonValue &root = data.value();
    const auto *schema = root.find("schema");
    if (schema == nullptr || schema->as_string() == nullptr ||
        *schema->as_string() != kModuleLifecycleEventSchema) {
        return false;
    }
    const auto *kind_member = root.find("kind");
    if (kind_member == nullptr || kind_member->as_string() == nullptr ||
        *kind_member->as_string() != kind) {
        return false;
    }
    const auto *module_id = root.find("module_id");
    if (module_id == nullptr || module_id->as_string() == nullptr ||
        *module_id->as_string() != manifest.module_id) {
        return false;
    }
    const auto *version = root.find("version");
    if (version == nullptr || !version->is_object()) {
        return false;
    }
    const auto *major = version->find("major");
    const auto *minor = version->find("minor");
    const auto *patch = version->find("patch");
    if (major == nullptr || !major->is_integer() || major->as_integer() != manifest.version.major ||
        minor == nullptr || !minor->is_integer() || minor->as_integer() != manifest.version.minor ||
        patch == nullptr || !patch->is_integer() || patch->as_integer() != manifest.version.patch) {
        return false;
    }
    const auto *digest = root.find("digest");
    if (digest == nullptr || digest->as_string() == nullptr ||
        *digest->as_string() != tool_module_manifest_digest(manifest).to_string()) {
        return false;
    }
    const auto *origin = root.find("origin");
    if (origin == nullptr || origin->as_string() == nullptr ||
        *origin->as_string() != tool_module_origin_name(manifest.origin)) {
        return false;
    }
    if (!reason_substring.empty()) {
        const auto *reason = root.find("reason");
        if (reason == nullptr || reason->as_string() == nullptr ||
            reason->as_string()->find(reason_substring) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// Independent recomputation of the snapshot digest from the documented
// canonical structure {generation, modules: [{module_id, digest hex}]}.
Hash recompute_snapshot_digest(const RegistrySnapshot &snapshot) {
    JsonValue::Object root;
    root.emplace_back("generation", static_cast<std::int64_t>(snapshot.generation));
    JsonValue::Array modules;
    for (const auto &module : snapshot.modules) {
        JsonValue::Object entry;
        entry.emplace_back("module_id", module.module_id);
        entry.emplace_back("digest", module.module_digest.to_string());
        modules.emplace_back(JsonValue{std::move(entry)});
    }
    root.emplace_back("modules", JsonValue{std::move(modules)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

// Independent recomputation of the view digest from the view fields.
Hash recompute_view_digest(const NegotiationView &view) {
    JsonValue::Object root;
    root.emplace_back("generation", static_cast<std::int64_t>(view.generation));
    root.emplace_back("epoch", static_cast<std::int64_t>(view.epoch));
    root.emplace_back("trigger", std::string(negotiation_trigger_name(view.trigger)));
    root.emplace_back("negotiation_digest", view.negotiation.digest.to_string());
    return canonical_json_digest(JsonValue{std::move(root)});
}

// The fixed report scenario dataset, shared by --report and the determinism
// gate: three pinned built-in modules with distinct member tool names.
struct ReportScenario {
    ToolModuleManifest alpha;
    ToolModuleManifest beta;
    ToolModuleManifest gamma;
    ModuleTrustConfig trust;

    ReportScenario()
        : alpha(fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn, {}, {},
                                 {}, {"env.screen.capture"})),
          beta(fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn)),
          gamma(fixture_manifest("acme.reg.gamma", "gamma_read", ToolModuleOrigin::BuiltIn)) {
        trust = builtin_trust_for({alpha, beta, gamma});
    }
};

EnvironmentCapabilities report_environment() {
    EnvironmentCapabilities capabilities;
    capabilities.screen_capture = true;
    return capabilities;
}

// Builds the canonical report string: registry snapshot digests at every
// generation and every negotiation view digest of the fixed scenario. Pure
// content-derived values only.
std::string build_registry_report() {
    const ReportScenario scenario;
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), scenario.trust, &store, test_runtime_id(),
                            test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};

    JsonValue::Array snapshots;
    auto record_snapshot = [&snapshots](const RegistrySnapshot &snapshot) {
        JsonValue::Object entry;
        entry.emplace_back("generation", static_cast<std::int64_t>(snapshot.generation));
        JsonValue::Array modules;
        for (const auto &module : snapshot.modules) {
            JsonValue::Object member;
            member.emplace_back("module_id", module.module_id);
            member.emplace_back("digest", module.module_digest.to_string());
            modules.emplace_back(JsonValue{std::move(member)});
        }
        entry.emplace_back("modules", JsonValue{std::move(modules)});
        entry.emplace_back("digest", snapshot.digest.to_string());
        snapshots.emplace_back(JsonValue{std::move(entry)});
    };

    JsonValue::Array views;
    auto record_view = [&views](const NegotiationView &view) {
        JsonValue::Object entry;
        entry.emplace_back("generation", static_cast<std::int64_t>(view.generation));
        entry.emplace_back("epoch", static_cast<std::int64_t>(view.epoch));
        entry.emplace_back("trigger", std::string(negotiation_trigger_name(view.trigger)));
        entry.emplace_back("negotiation_digest", view.negotiation.digest.to_string());
        entry.emplace_back("view_digest", view.view_digest.to_string());
        views.emplace_back(JsonValue{std::move(entry)});
    };

    // Scenario steps must all succeed; aborting keeps the report deterministic
    // instead of emitting a partial trail.
    const auto must = [](const Result<void> &result) {
        if (!result.has_value()) {
            std::abort();
        }
    };

    record_snapshot(registry.active_snapshot()); // generation 0, empty

    must(registry.register_module(scenario.alpha));
    must(registry.stage_module("acme.reg.alpha"));
    must(registry.activate_module("acme.reg.alpha"));
    record_snapshot(registry.active_snapshot()); // generation 1: [alpha]

    must(registry.register_module(scenario.beta));
    must(registry.stage_module("acme.reg.beta"));
    must(registry.activate_module("acme.reg.beta"));
    record_snapshot(registry.active_snapshot()); // generation 2: [alpha, beta]

    auto first = coordinator.on_session_established(7, report_environment(), registry);
    if (!first.has_value()) {
        std::abort();
    }
    record_view(first.value()); // negotiation view generation 1

    must(registry.register_module(scenario.gamma));
    must(registry.stage_module("acme.reg.gamma"));
    must(registry.activate_module("acme.reg.gamma"));
    record_snapshot(registry.active_snapshot()); // generation 3: [alpha, beta, gamma]

    auto modules_changed = coordinator.on_modules_changed(registry);
    if (!modules_changed.has_value()) {
        std::abort();
    }
    record_view(modules_changed.value()); // negotiation view generation 2

    must(registry.deprecate_module("acme.reg.beta", "retired"));
    record_snapshot(registry.active_snapshot()); // generation 4: [alpha, gamma]

    auto modules_changed_again = coordinator.on_modules_changed(registry);
    if (!modules_changed_again.has_value()) {
        std::abort();
    }
    record_view(modules_changed_again.value()); // negotiation view generation 3

    EnvironmentCapabilities richer = report_environment();
    richer.ui_tree = true;
    auto environment_changed = coordinator.on_environment_changed(8, richer, registry);
    if (!environment_changed.has_value()) {
        std::abort();
    }
    record_view(environment_changed.value()); // negotiation view generation 4

    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.tool_module.registry.report.v1"));
    report.emplace_back("snapshots", JsonValue{std::move(snapshots)});
    report.emplace_back("negotiation_views", JsonValue{std::move(views)});
    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G1: state machine and downgrade-only transitions (M7-TM1-G1)
// ---------------------------------------------------------------------------

int g1_state_names_round_trip() {
    const ModuleState kStates[]{ModuleState::Discovered, ModuleState::Verified,
                                ModuleState::Staged,     ModuleState::Active,
                                ModuleState::Deprecated, ModuleState::Revoked,
                                ModuleState::Quarantined};
    const std::string_view kNames[]{"discovered", "verified", "staged",     "active",
                                    "deprecated", "revoked",  "quarantined"};
    for (std::size_t index = 0; index < sizeof(kStates) / sizeof(kStates[0]); ++index) {
        MIRA_CHECK(module_state_name(kStates[index]) == kNames[index]);
        const auto parsed = parse_module_state(kNames[index]);
        MIRA_CHECK(parsed.has_value());
        MIRA_CHECK(parsed.value() == kStates[index]);
    }
    MIRA_CHECK(module_state_name(static_cast<ModuleState>(200)) == "unknown");
    MIRA_CHECK(!parse_module_state("unknown").has_value());
    MIRA_CHECK(!parse_module_state("").has_value());
    MIRA_CHECK(failed_with(parse_module_state("active-ish"), ErrorCode::InvalidArgument));
    return 0;
}

int g1_lifecycle_happy_path_and_events() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha}), &store,
                            test_runtime_id(), test_session_id()};

    // register: Discovered -> Verified.
    auto registered = registry.register_module(alpha);
    MIRA_CHECK(registered.has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Verified));

    // stage: Verified -> Staged (no generation bump).
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Staged));
    MIRA_CHECK(registry.active_snapshot().generation == 0);

    // activate: Staged -> Active (generation 1, snapshot rebuilt).
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Active));
    const RegistrySnapshot active = registry.active_snapshot();
    MIRA_CHECK(active.generation == 1);
    MIRA_CHECK(active.modules.size() == 1);
    MIRA_CHECK(active.modules[0].module_id == "acme.reg.alpha");
    MIRA_CHECK(active.modules[0].module_digest == tool_module_manifest_digest(alpha));
    MIRA_CHECK(active.digest == recompute_snapshot_digest(active));

    // deprecate: Active -> Deprecated (generation 2, module leaves the view).
    MIRA_CHECK(registry.deprecate_module("acme.reg.alpha", "retiring").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Deprecated));
    const RegistrySnapshot deprecated = registry.active_snapshot();
    MIRA_CHECK(deprecated.generation == 2);
    MIRA_CHECK(deprecated.modules.empty());
    MIRA_CHECK(deprecated.digest == recompute_snapshot_digest(deprecated));

    // revoke: Deprecated -> Revoked (terminal; no generation bump from a
    // non-Active state; digest enters the tombstone).
    const Hash alpha_digest = tool_module_manifest_digest(alpha);
    MIRA_CHECK(registry.revoke_module("acme.reg.alpha", "unsafe").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));
    MIRA_CHECK(registry.active_snapshot().generation == 2);
    MIRA_CHECK(registry.is_tombstoned(alpha_digest));

    // Event trail: kinds, order and full payload fields.
    auto events = query_events(store);
    MIRA_CHECK(events.has_value());
    const std::vector<std::string> kinds = lifecycle_kinds(events.value());
    MIRA_CHECK(kinds == (std::vector<std::string>{"discovered", "verified", "staged", "activated",
                                                  "deprecated", "revoked"}));
    MIRA_CHECK(events.value().size() == 6);
    MIRA_CHECK(lifecycle_event_matches(events.value()[0], "discovered", alpha, EventClass::State));
    MIRA_CHECK(
        lifecycle_event_matches(events.value()[1], "verified", alpha, EventClass::State, "pinned"));
    MIRA_CHECK(lifecycle_event_matches(events.value()[2], "staged", alpha, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[3], "activated", alpha, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[4], "deprecated", alpha, EventClass::State,
                                       "retiring"));
    // Revoked and Quarantined are Critical (design section 6).
    MIRA_CHECK(lifecycle_event_matches(events.value()[5], "revoked", alpha, EventClass::Critical,
                                       "unsafe"));
    // Reason-free payloads must not carry a reason key at all.
    {
        auto data = parse_json(events.value()[0].payload.data);
        MIRA_CHECK(data.has_value());
        MIRA_CHECK(data.value().find("reason") == nullptr);
    }

    // find() stays stable across the whole lifecycle; the record keeps its
    // digest and last reason.
    const ModuleRecord *record = registry.find("acme.reg.alpha");
    MIRA_CHECK(record != nullptr);
    MIRA_CHECK(record->manifest_digest == alpha_digest);
    MIRA_CHECK(record->last_reason == "unsafe");

    // Stats: one full lifecycle, zero rejections, one tombstone.
    const ModuleRegistryStats stats = registry.stats();
    MIRA_CHECK(stats.discovered == 1);
    MIRA_CHECK(stats.registered == 1);
    MIRA_CHECK(stats.quarantined == 0);
    MIRA_CHECK(stats.activated == 1);
    MIRA_CHECK(stats.deprecated == 1);
    MIRA_CHECK(stats.revoked == 1);
    MIRA_CHECK(stats.rejected_transitions == 0);
    MIRA_CHECK(stats.rejected_closed == 0);
    MIRA_CHECK(stats.tombstone_hits == 0);
    MIRA_CHECK(stats.events_emitted == 6);
    MIRA_CHECK(stats.event_sink_failures == 0);
    MIRA_CHECK(stats.tombstones == 1);
    return 0;
}

int g1_illegal_transitions_rejected_and_unchanged() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest gamma =
        fixture_manifest("acme.reg.gamma", "gamma_read", ToolModuleOrigin::BuiltIn);

    // Build one module in each interesting state, plus one quarantined module.
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};
    MIRA_CHECK(registry.register_module(alpha).has_value());

    // Upgrade attempts from Verified.
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.revoke_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Verified));

    // Self-transitions and downgrades from Staged.
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Staged));

    // Revocation from Staged is legal (digest tombstoned, no generation bump).
    const RegistrySnapshot before_staged_revoke = registry.active_snapshot();
    MIRA_CHECK(registry.revoke_module("acme.reg.alpha").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));
    MIRA_CHECK(registry.active_snapshot().generation == before_staged_revoke.generation);

    // Terminal revival from Revoked: every promotion and downgrade rejected.
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.revoke_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));

    // Deprecated: only revoke remains; upgrading back to Active is rejected.
    MIRA_CHECK(registry.register_module(beta).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.deprecate_module("acme.reg.beta").has_value());
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.beta", ModuleState::Deprecated));

    // Quarantined has no exits.
    ModuleRegistry untrusted_registry{CapabilityCatalog::core(), ModuleTrustConfig{}, &store,
                                      test_runtime_id(), test_session_id()};
    auto quarantined = untrusted_registry.register_module(gamma);
    MIRA_CHECK(failed_with(quarantined, ErrorCode::SafetyRejected));
    MIRA_CHECK(state_is(untrusted_registry, "acme.reg.gamma", ModuleState::Quarantined));
    MIRA_CHECK(
        failed_with(untrusted_registry.stage_module("acme.reg.gamma"), ErrorCode::InvalidState));
    MIRA_CHECK(
        failed_with(untrusted_registry.activate_module("acme.reg.gamma"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(untrusted_registry.deprecate_module("acme.reg.gamma"),
                           ErrorCode::InvalidState));
    MIRA_CHECK(
        failed_with(untrusted_registry.revoke_module("acme.reg.gamma"), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(untrusted_registry, "acme.reg.gamma", ModuleState::Quarantined));
    // The quarantined record exists for diagnostics but never enters a view.
    MIRA_CHECK(untrusted_registry.active_snapshot().modules.empty());
    // Re-registering the same id in any historical state is AlreadyExists.
    MIRA_CHECK(failed_with(untrusted_registry.register_module(gamma), ErrorCode::AlreadyExists));
    MIRA_CHECK(failed_with(registry.register_module(beta), ErrorCode::AlreadyExists));

    // Unknown ids are rejected, not silently created.
    MIRA_CHECK(failed_with(registry.stage_module("no.such.module"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.activate_module("no.such.module"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("no.such.module"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.revoke_module("no.such.module"), ErrorCode::InvalidState));
    MIRA_CHECK(registry.find("no.such.module") == nullptr);

    // Every illegal attempt is counted and leaves states untouched:
    // 3 from Verified, 2 from Staged, 4 from Revoked, 3 from Deprecated,
    // 4 unknown ids and 1 duplicate registration.
    const ModuleRegistryStats stats = registry.stats();
    MIRA_CHECK(stats.rejected_transitions == 17);
    MIRA_CHECK(stats.rejected_closed == 0);
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));
    MIRA_CHECK(state_is(registry, "acme.reg.beta", ModuleState::Deprecated));
    return 0;
}

int g1_seal_and_close_semantics() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};

    MIRA_CHECK(registry.register_module(alpha).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());

    // seal(): ends the registration window, keeps downgrades available.
    MIRA_CHECK(registry.seal().has_value());
    MIRA_CHECK(registry.sealed());
    MIRA_CHECK(!registry.closed());
    MIRA_CHECK(failed_with(registry.register_module(beta), ErrorCode::InvalidState));
    MIRA_CHECK(registry.find("acme.reg.beta") == nullptr);
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    // Repeated seal is an error.
    MIRA_CHECK(failed_with(registry.seal(), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Active));
    // Downgrades stay available after seal.
    MIRA_CHECK(registry.deprecate_module("acme.reg.alpha", "post-seal").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Deprecated));
    MIRA_CHECK(registry.revoke_module("acme.reg.alpha", "post-seal revoke").has_value());
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));

    // close(): terminal; every mutation is rejected, snapshot stays readable.
    const RegistrySnapshot last = registry.active_snapshot();
    const ModuleRegistryCloseReport close_report = registry.close();
    MIRA_CHECK(registry.closed());
    MIRA_CHECK(close_report.snapshot_generation == last.generation);
    MIRA_CHECK(close_report.active_modules == last.modules.size());
    MIRA_CHECK(close_report.tombstones == registry.stats().tombstones);

    MIRA_CHECK(failed_with(registry.register_module(beta), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.revoke_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.seal(), ErrorCode::InvalidState));
    MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));

    const RegistrySnapshot after_close = registry.active_snapshot();
    MIRA_CHECK(after_close.generation == last.generation);
    MIRA_CHECK(after_close.digest == last.digest);
    MIRA_CHECK(after_close.modules.size() == last.modules.size());

    // close() is idempotent and keeps returning a report.
    const ModuleRegistryCloseReport second = registry.close();
    MIRA_CHECK(second.snapshot_generation == close_report.snapshot_generation);
    MIRA_CHECK(second.active_modules == close_report.active_modules);
    MIRA_CHECK(second.tombstones == close_report.tombstones);

    // Closed rejections are visible in stats and separated from transition
    // rejections: 6 post-close mutations above were all rejected_closed.
    const ModuleRegistryStats stats = registry.stats();
    MIRA_CHECK(stats.rejected_closed == 6);
    MIRA_CHECK(stats.rejected_transitions >= 4);
    return 0;
}

int g1_event_emission_gating_and_sink_failures() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);

    // No sink: nothing emitted, nothing failed.
    {
        ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha})};
        MIRA_CHECK(registry.register_module(alpha).has_value());
        MIRA_CHECK(registry.stats().events_emitted == 0);
        MIRA_CHECK(registry.stats().event_sink_failures == 0);
    }
    // Sink without ids: emission disabled, not failed.
    {
        MemoryEventStore store;
        ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha}), &store};
        MIRA_CHECK(registry.register_module(alpha).has_value());
        MIRA_CHECK(registry.stats().events_emitted == 0);
        MIRA_CHECK(registry.stats().event_sink_failures == 0);
        MIRA_CHECK(store.size() == 0);
    }
    // Runtime id but nil session: still disabled.
    {
        MemoryEventStore store;
        ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha}), &store,
                                test_runtime_id(), SessionId{}};
        MIRA_CHECK(registry.register_module(alpha).has_value());
        MIRA_CHECK(registry.stats().events_emitted == 0);
        MIRA_CHECK(store.size() == 0);
    }
    // Failing sink: counted, never thrown, never blocking the mutation.
    {
        FailingEventStore store;
        ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha}), &store,
                                test_runtime_id(), test_session_id()};
        MIRA_CHECK(registry.register_module(alpha).has_value());
        MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
        MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());
        MIRA_CHECK(registry.deprecate_module("acme.reg.alpha").has_value());
        MIRA_CHECK(registry.revoke_module("acme.reg.alpha").has_value());
        const ModuleRegistryStats stats = registry.stats();
        MIRA_CHECK(stats.event_sink_failures == 6); // discovered, verified, staged, activated,
                                                    // deprecated, revoked
        MIRA_CHECK(stats.events_emitted == 0);
        MIRA_CHECK(state_is(registry, "acme.reg.alpha", ModuleState::Revoked));
        MIRA_CHECK(store.failures == 6);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G2: origin trust fail-closed (M7-TM1-G2)
// ---------------------------------------------------------------------------

int g2_trust_config_validation_bounds() {
    // Empty trust material is a valid (deny-everything) configuration.
    MIRA_CHECK(ModuleTrustConfig{}.validate().has_value());

    // Digest sets: 4096 entries pass, 4097 fail.
    {
        ModuleTrustConfig config;
        config.builtin_digests.assign(4096, Hash{});
        MIRA_CHECK(config.validate().has_value());
        config.builtin_digests.push_back(Hash{});
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    {
        ModuleTrustConfig config;
        config.revoked_digests.assign(4097, Hash{});
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    // Entry lists: 1024 pass, 1025 fail.
    {
        ModuleTrustConfig config;
        config.host_provided_allowlist.assign(1024, "acme.entry");
        MIRA_CHECK(config.validate().has_value());
        config.host_provided_allowlist.push_back("acme.overflow");
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    {
        ModuleTrustConfig config;
        config.trusted_out_of_process_signers.assign(1025, "signer");
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    // Allowlist entries must be valid lowercase dotted ids.
    for (const char *bad : {"Bad.ID", "", ".leading", "trailing.", "dou..ble", "has space"}) {
        ModuleTrustConfig config;
        config.host_provided_allowlist.emplace_back(bad);
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    // Signers must be non-empty and bounded.
    {
        ModuleTrustConfig config;
        config.trusted_out_of_process_signers.push_back("");
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    {
        ModuleTrustConfig config;
        config.trusted_out_of_process_signers.push_back(std::string(257, 's'));
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
    }
    // Out-of-process signers without a verifier fail closed at validation.
    {
        ModuleTrustConfig config;
        config.trusted_out_of_process_signers.push_back("acme-signer");
        MIRA_CHECK(failed_with(config.validate(), ErrorCode::InvalidArgument));
        ProbeVerifier verifier(true);
        config.verifier = &verifier;
        MIRA_CHECK(config.validate().has_value());
    }
    // An invalid trust config aborts registry construction.
    {
        ModuleTrustConfig config;
        config.trusted_out_of_process_signers.push_back("acme-signer");
        bool threw = false;
        try {
            ModuleRegistry registry{CapabilityCatalog::core(), config};
            (void)registry;
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        MIRA_CHECK(threw);
    }
    return 0;
}

int g2_unsigned_digest_properties() {
    ToolModuleManifest signed_one =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::OutOfProcess,
                         "acme-signer", "ed25519-ref", "signature-payload-one");
    ToolModuleManifest signed_two = signed_one;
    signed_two.signature = "signature-payload-two";

    // Only the signature differs: full digests differ, unsigned digests match.
    MIRA_CHECK(tool_module_manifest_digest(signed_one) != tool_module_manifest_digest(signed_two));
    MIRA_CHECK(tool_module_unsigned_manifest_digest(signed_one) ==
               tool_module_unsigned_manifest_digest(signed_two));

    // The unsigned digest is exactly the binding payload: everything except
    // the signature text is covered (id and signer changes both move it).
    ToolModuleManifest renamed = signed_one;
    renamed.module_id = "acme.reg.beta";
    MIRA_CHECK(tool_module_unsigned_manifest_digest(renamed) !=
               tool_module_unsigned_manifest_digest(signed_one));
    ToolModuleManifest resigned = signed_one;
    resigned.signer = "other-signer";
    MIRA_CHECK(tool_module_unsigned_manifest_digest(resigned) !=
               tool_module_unsigned_manifest_digest(signed_one));

    // An unsigned manifest digests identically through both functions.
    const ToolModuleManifest plain =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    MIRA_CHECK(tool_module_manifest_digest(plain) == tool_module_unsigned_manifest_digest(plain));

    // Digesting is stable across repeated calls.
    MIRA_CHECK(tool_module_unsigned_manifest_digest(signed_one) ==
               tool_module_unsigned_manifest_digest(signed_one));
    return 0;
}

int g2_verify_module_trust_matrix() {
    const ToolModuleManifest builtin =
        fixture_manifest("acme.reg.builtin", "builtin_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest hosted = fixture_manifest(
        "acme.reg.hosted", "hosted_read", ToolModuleOrigin::HostProvided, "host-pipeline");
    const ToolModuleManifest oop =
        fixture_manifest("acme.reg.oop", "oop_read", ToolModuleOrigin::OutOfProcess, "acme-signer",
                         "ed25519-ref", "dGVzdC1zaWduYXR1cmUtbWF0ZXJpYWw");

    // BuiltIn: pinned digest passes, anything else fails.
    {
        const ModuleTrustConfig config = builtin_trust_for({builtin});
        const auto pinned = verify_module_trust(builtin, config);
        MIRA_CHECK(pinned.trusted);
        MIRA_CHECK(pinned.origin == ToolModuleOrigin::BuiltIn);
        MIRA_CHECK(pinned.module_id == builtin.module_id);
        MIRA_CHECK(pinned.version == builtin.version);
        MIRA_CHECK(pinned.manifest_digest == tool_module_manifest_digest(builtin));
        MIRA_CHECK(!pinned.reason.empty());

        // Same content with a different signature has a different digest and
        // therefore loses its pinned status.
        ToolModuleManifest tampered = builtin;
        tampered.signature = "extra-signature";
        const auto unpinned = verify_module_trust(tampered, config);
        MIRA_CHECK(!unpinned.trusted);
        MIRA_CHECK(!unpinned.reason.empty());

        // The pure function is deterministic: same input, same verdict.
        const auto repeat = verify_module_trust(builtin, config);
        MIRA_CHECK(repeat.trusted == pinned.trusted);
        MIRA_CHECK(repeat.reason == pinned.reason);
        MIRA_CHECK(repeat.manifest_digest == pinned.manifest_digest);
    }
    // BuiltIn against empty trust: fail closed with a reason.
    {
        const auto rejected = verify_module_trust(builtin, ModuleTrustConfig{});
        MIRA_CHECK(!rejected.trusted);
        MIRA_CHECK(!rejected.reason.empty());
    }

    // HostProvided: allowlist membership decides.
    {
        const ModuleTrustConfig config = host_trust_for({"acme.reg.hosted"});
        MIRA_CHECK(verify_module_trust(hosted, config).trusted);
        const auto outside = verify_module_trust(hosted, host_trust_for({"other.module"}));
        MIRA_CHECK(!outside.trusted);
        MIRA_CHECK(!outside.reason.empty());
    }

    // OutOfProcess matrix.
    {
        ProbeVerifier acceptor(true);
        const ModuleTrustConfig config = oop_trust_for(&acceptor, {"acme-signer"});

        // Trusted signer + verifier acceptance over the unsigned digest.
        const auto verified = verify_module_trust(oop, config);
        MIRA_CHECK(verified.trusted);
        MIRA_CHECK(!verified.reason.empty());
        MIRA_CHECK(acceptor.calls_.load() == 1);
        MIRA_CHECK(acceptor.last_signer == "acme-signer");
        MIRA_CHECK(acceptor.last_algorithm == "ed25519-ref");
        MIRA_CHECK(acceptor.last_signature == "dGVzdC1zaWduYXR1cmUtbWF0ZXJpYWw");
        MIRA_CHECK(acceptor.last_digest == tool_module_unsigned_manifest_digest(oop));
        MIRA_CHECK(acceptor.last_digest != tool_module_manifest_digest(oop));

        // Untrusted signer: rejected before the verifier runs.
        ProbeVerifier counter(true);
        ToolModuleManifest stranger = oop;
        stranger.signer = "stranger-signer";
        const auto untrusted =
            verify_module_trust(stranger, oop_trust_for(&counter, {"acme-signer"}));
        MIRA_CHECK(!untrusted.trusted);
        MIRA_CHECK(!untrusted.reason.empty());
        MIRA_CHECK(counter.calls_.load() == 0);

        // Missing signer / algorithm / signature: rejected, verifier untouched.
        ToolModuleManifest no_signer = oop;
        no_signer.signer = "";
        MIRA_CHECK(!verify_module_trust(no_signer, config).trusted);
        ToolModuleManifest no_algorithm = oop;
        no_algorithm.signature_algorithm = "";
        MIRA_CHECK(!verify_module_trust(no_algorithm, config).trusted);
        ToolModuleManifest no_signature = oop;
        no_signature.signature = "";
        MIRA_CHECK(!verify_module_trust(no_signature, config).trusted);
        MIRA_CHECK(acceptor.calls_.load() == 1); // unchanged

        // Verifier rejection: fail closed with a reason.
        ProbeVerifier rejector(false);
        const auto denied = verify_module_trust(oop, oop_trust_for(&rejector, {"acme-signer"}));
        MIRA_CHECK(!denied.trusted);
        MIRA_CHECK(!denied.reason.empty());
        MIRA_CHECK(rejector.calls_.load() == 1);

        // Verifier unavailable (trusted signers configured, no verifier).
        ModuleTrustConfig missing_verifier;
        missing_verifier.trusted_out_of_process_signers.push_back("acme-signer");
        const auto unavailable = verify_module_trust(oop, missing_verifier);
        MIRA_CHECK(!unavailable.trusted);
        MIRA_CHECK(!unavailable.reason.empty());
    }

    // A throwing verifier is a host defect: verify_module_trust propagates it
    // so every consumption site converts it explicitly.
    {
        ThrowingVerifier thrower;
        bool propagated = false;
        try {
            (void)verify_module_trust(oop, oop_trust_for(&thrower, {"acme-signer"}));
        } catch (const std::runtime_error &) {
            propagated = true;
        }
        MIRA_CHECK(propagated);
        MIRA_CHECK(thrower.calls_.load() == 1);
    }
    return 0;
}

int g2_registry_fail_closed_and_quarantine() {
    const ToolModuleManifest builtin =
        fixture_manifest("acme.reg.builtin", "builtin_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest hosted = fixture_manifest(
        "acme.reg.hosted", "hosted_read", ToolModuleOrigin::HostProvided, "host-pipeline");
    const ToolModuleManifest oop =
        fixture_manifest("acme.reg.oop", "oop_read", ToolModuleOrigin::OutOfProcess, "acme-signer",
                         "ed25519-ref", "dGVzdC1zaWduYXR1cmUtbWF0ZXJpYWw");

    // Each origin with failing trust material: whole-group rejection into
    // Quarantined, record retained, no partial state, Critical event.
    const ToolModuleManifest *kFailures[] = {&builtin, &hosted, &oop};
    for (const ToolModuleManifest *manifest : kFailures) {
        MemoryEventStore store;
        ProbeVerifier rejector(false);
        ModuleTrustConfig trust;
        trust.trusted_out_of_process_signers.push_back("acme-signer");
        trust.verifier = &rejector;
        ModuleRegistry registry{CapabilityCatalog::core(), trust, &store, test_runtime_id(),
                                test_session_id()};

        const auto admitted = registry.register_module(*manifest);
        MIRA_CHECK(failed_with(admitted, ErrorCode::SafetyRejected));
        MIRA_CHECK(admitted.error().domain == "mira.tool_module");
        MIRA_CHECK(!admitted.error().safe_message.empty());
        const ModuleRecord *record = registry.find(manifest->module_id);
        MIRA_CHECK(record != nullptr);
        MIRA_CHECK(record->state == ModuleState::Quarantined);
        MIRA_CHECK(!record->last_reason.empty());
        // No partial state: not staged, not in any view, counted as quarantined.
        MIRA_CHECK(registry.active_snapshot().modules.empty());
        MIRA_CHECK(registry.stats().quarantined == 1);
        MIRA_CHECK(registry.stats().registered == 0);
        MIRA_CHECK(registry.stats().discovered == 1);
        // Events: discovered (State) then quarantined (Critical) with reason.
        auto events = query_events(store);
        MIRA_CHECK(events.has_value());
        const std::vector<std::string> kinds = lifecycle_kinds(events.value());
        MIRA_CHECK(kinds == (std::vector<std::string>{"discovered", "quarantined"}));
        MIRA_CHECK(
            lifecycle_event_matches(events.value()[0], "discovered", *manifest, EventClass::State));
        MIRA_CHECK(lifecycle_event_matches(events.value()[1], "quarantined", *manifest,
                                           EventClass::Critical));
        // The quarantine reason is present in the event payload as well.
        {
            auto data = parse_json(events.value()[1].payload.data);
            MIRA_CHECK(data.has_value());
            const auto *reason = data.value().find("reason");
            MIRA_CHECK(reason != nullptr && reason->as_string() != nullptr &&
                       !reason->as_string()->empty());
        }
    }

    // OutOfProcess accepted by the verifier reaches Verified through the same
    // inline path, and the verifier saw the unsigned digest.
    {
        MemoryEventStore store;
        ProbeVerifier acceptor(true);
        ModuleRegistry registry{CapabilityCatalog::core(),
                                oop_trust_for(&acceptor, {"acme-signer"}), &store,
                                test_runtime_id(), test_session_id()};
        MIRA_CHECK(registry.register_module(oop).has_value());
        MIRA_CHECK(state_is(registry, oop.module_id, ModuleState::Verified));
        MIRA_CHECK(registry.stats().registered == 1);
        MIRA_CHECK(registry.stats().quarantined == 0);
        MIRA_CHECK(acceptor.calls_.load() == 1);
        MIRA_CHECK(acceptor.last_digest == tool_module_unsigned_manifest_digest(oop));
    }

    // A throwing verifier during inline registration is quarantined with the
    // explicit "signature verifier threw" reason instead of escaping.
    {
        MemoryEventStore store;
        ThrowingVerifier thrower;
        ModuleRegistry registry{CapabilityCatalog::core(), oop_trust_for(&thrower, {"acme-signer"}),
                                &store, test_runtime_id(), test_session_id()};
        const auto admitted = registry.register_module(oop);
        MIRA_CHECK(failed_with(admitted, ErrorCode::SafetyRejected));
        const ModuleRecord *record = registry.find(oop.module_id);
        MIRA_CHECK(record != nullptr);
        MIRA_CHECK(record->state == ModuleState::Quarantined);
        MIRA_CHECK(record->last_reason == "signature verifier threw");
        MIRA_CHECK(registry.stats().quarantined == 1);
        auto events = query_events(store);
        MIRA_CHECK(events.has_value());
        MIRA_CHECK(lifecycle_event_matches(events.value()[1], "quarantined", oop,
                                           EventClass::Critical, "verifier threw"));
    }
    return 0;
}

int g2_precomputed_report_binding() {
    const ToolModuleManifest builtin =
        fixture_manifest("acme.reg.builtin", "builtin_read", ToolModuleOrigin::BuiltIn);
    const ModuleTrustConfig trust = builtin_trust_for({builtin});
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), trust, &store, test_runtime_id(),
                            test_session_id()};

    // A report computed for this exact manifest is accepted.
    const ModuleTrustReport good = verify_module_trust(builtin, trust);
    MIRA_CHECK(good.trusted);
    MIRA_CHECK(registry.register_module(builtin, good).has_value());
    MIRA_CHECK(state_is(registry, builtin.module_id, ModuleState::Verified));

    // Every binding mismatch rejects outright with no record and an event:
    // one case per binding field (module_id, version, digest, origin).
    for (std::size_t index = 0; index < 4; ++index) {
        ToolModuleManifest other = fixture_manifest("acme.reg.other" + std::to_string(index),
                                                    "other_read", ToolModuleOrigin::BuiltIn);
        ModuleTrustReport report = verify_module_trust(other, trust);
        MIRA_CHECK(!report.trusted); // 'other' is not pinned
        switch (index) {
        case 0:
            report.module_id = "acme.reg.different";
            break;
        case 1:
            report.version = SemanticVersion{9, 9, 9};
            break;
        case 2:
            report.manifest_digest = Hash{}; // all-zero digest never matches
            break;
        case 3:
            report.origin = ToolModuleOrigin::HostProvided;
            break;
        default:
            break;
        }
        std::size_t events_before = 0;
        {
            auto events = query_events(store);
            MIRA_CHECK(events.has_value());
            events_before = events.value().size();
        }
        const auto rejected = registry.register_module(other, report);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(registry.find(other.module_id) == nullptr); // no record survives
        auto events = query_events(store);
        MIRA_CHECK(events.has_value());
        // discovered + registration_rejected, nothing else.
        MIRA_CHECK(events.value().size() == events_before + 2);
        const EventEnvelope &last_event = events.value().back();
        MIRA_CHECK(lifecycle_event_matches(last_event, "registration_rejected", other,
                                           EventClass::State, "bind"));
    }

    // A bound but untrusted report quarantines (module defect, record kept).
    {
        const ToolModuleManifest stranger =
            fixture_manifest("acme.reg.stranger", "stranger_read", ToolModuleOrigin::BuiltIn);
        const ModuleTrustReport untrusted = verify_module_trust(stranger, trust);
        MIRA_CHECK(!untrusted.trusted);
        const auto rejected = registry.register_module(stranger, untrusted);
        MIRA_CHECK(failed_with(rejected, ErrorCode::SafetyRejected));
        MIRA_CHECK(state_is(registry, stranger.module_id, ModuleState::Quarantined));
    }
    return 0;
}

int g2_tombstone_blocks_reregistration() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    const Hash alpha_digest = tool_module_manifest_digest(alpha);

    // Runtime revocation tombstones the digest; re-registering the identical
    // manifest hits the duplicate-id gate first (AlreadyExists), and a fresh
    // registry seeded with the digest rejects the same content outright.
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};
    MIRA_CHECK(registry.register_module(alpha).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.revoke_module("acme.reg.alpha", "compromised").has_value());
    MIRA_CHECK(registry.is_tombstoned(alpha_digest));

    MIRA_CHECK(failed_with(registry.register_module(alpha), ErrorCode::AlreadyExists));
    MIRA_CHECK(registry.stats().tombstone_hits == 0);

    // Fresh lifetime seeded from the revocation: the tombstone blocks
    // admission of any manifest with that digest even under a new module id
    // namespace (a digest is content, not identity).
    MemoryEventStore seeded_store;
    ModuleTrustConfig seeded_trust = builtin_trust_for({alpha, beta});
    seeded_trust.revoked_digests.push_back(alpha_digest);
    ModuleRegistry seeded{CapabilityCatalog::core(), seeded_trust, &seeded_store, test_runtime_id(),
                          test_session_id()};
    MIRA_CHECK(seeded.stats().tombstones == 1);
    MIRA_CHECK(seeded.is_tombstoned(alpha_digest));

    ToolModuleManifest exact_copy = alpha; // same id, same digest
    const auto blocked = seeded.register_module(exact_copy);
    MIRA_CHECK(failed_with(blocked, ErrorCode::InvalidState));
    MIRA_CHECK(seeded.stats().tombstone_hits == 1);
    MIRA_CHECK(seeded.stats().discovered == 0); // rejected before discovery
    MIRA_CHECK(seeded.find(alpha.module_id) == nullptr);

    // The tombstone rejection is a Critical registration_rejected event.
    auto events = query_events(seeded_store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(events.value().size() == 1);
    MIRA_CHECK(lifecycle_event_matches(events.value()[0], "registration_rejected", exact_copy,
                                       EventClass::Critical, "tombstoned"));

    // Untombstoned content still registers normally in the seeded registry.
    MIRA_CHECK(seeded.register_module(beta).has_value());
    MIRA_CHECK(state_is(seeded, beta.module_id, ModuleState::Verified));

    // Runtime revocation also appends to the seeded set.
    MIRA_CHECK(seeded.stage_module(beta.module_id).has_value());
    MIRA_CHECK(seeded.revoke_module(beta.module_id).has_value());
    MIRA_CHECK(seeded.stats().tombstones == 2);
    MIRA_CHECK(seeded.is_tombstoned(tool_module_manifest_digest(beta)));
    return 0;
}

// ---------------------------------------------------------------------------
// G3: negotiation triggers and generations (M7-TM1-G3)
// ---------------------------------------------------------------------------

int g3_trigger_names_and_pre_session_errors() {
    MIRA_CHECK(negotiation_trigger_name(NegotiationTrigger::SessionEstablished) ==
               "session_established");
    MIRA_CHECK(negotiation_trigger_name(NegotiationTrigger::EnvironmentChanged) ==
               "environment_changed");
    MIRA_CHECK(negotiation_trigger_name(NegotiationTrigger::ModuleStatesChanged) ==
               "module_states_changed");

    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha})};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core()};

    MIRA_CHECK(coordinator.current() == nullptr);
    EnvironmentCapabilities capabilities;
    capabilities.screen_capture = true;

    // Triggers before a session fail closed.
    MIRA_CHECK(failed_with(coordinator.on_environment_changed(1, capabilities, registry),
                           ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(coordinator.on_modules_changed(registry), ErrorCode::InvalidState));
    MIRA_CHECK(coordinator.current() == nullptr);

    // Zero epochs are invalid even for the first trigger.
    MIRA_CHECK(failed_with(coordinator.on_session_established(0, capabilities, registry),
                           ErrorCode::InvalidArgument));
    return 0;
}

int g3_trigger_sequence_and_epoch_rules() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn, {}, {}, {},
                         {"env.screen.capture"});
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};

    MIRA_CHECK(registry.register_module(alpha).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());

    EnvironmentCapabilities capabilities;
    capabilities.screen_capture = true;

    // SessionEstablished: generation 1, epoch bound, negotiation over the
    // Active snapshot, canonical view digest, event with all fields.
    auto first = coordinator.on_session_established(7, capabilities, registry);
    MIRA_CHECK(first.has_value());
    const NegotiationView view_one = first.value();
    MIRA_CHECK(view_one.generation == 1);
    MIRA_CHECK(view_one.epoch == 7);
    MIRA_CHECK(view_one.trigger == NegotiationTrigger::SessionEstablished);
    const ModuleNegotiationResult expected = negotiate_modules(
        registry.active_snapshot().modules, capabilities, CapabilityCatalog::core());
    MIRA_CHECK(view_one.negotiation.digest == expected.digest);
    MIRA_CHECK(view_one.negotiation.modules.size() == expected.modules.size());
    MIRA_CHECK(view_one.view_digest == recompute_view_digest(view_one));
    MIRA_CHECK(coordinator.current() != nullptr);
    MIRA_CHECK(coordinator.current()->generation == 1);

    auto events = query_events(store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(count_events(events.value(), kModuleNegotiationEventSchema) == 1);
    {
        const EventEnvelope &envelope = events.value().back();
        MIRA_CHECK(envelope.payload.type == kModuleNegotiationEventSchema);
        MIRA_CHECK(envelope.payload.classification == EventClass::State);
        auto data = parse_json(envelope.payload.data);
        MIRA_CHECK(data.has_value());
        const auto *schema = data.value().find("schema");
        MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
                   *schema->as_string() == kModuleNegotiationEventSchema);
        const auto *trigger = data.value().find("trigger");
        MIRA_CHECK(trigger != nullptr && trigger->as_string() != nullptr &&
                   *trigger->as_string() == "session_established");
        const auto *epoch = data.value().find("epoch");
        MIRA_CHECK(epoch != nullptr && epoch->is_integer() && *epoch->as_integer() == 7);
        const auto *generation = data.value().find("generation");
        MIRA_CHECK(generation != nullptr && generation->is_integer() &&
                   *generation->as_integer() == 1);
        const auto *view_digest = data.value().find("view_digest");
        MIRA_CHECK(view_digest != nullptr && view_digest->as_string() != nullptr &&
                   *view_digest->as_string() == view_one.view_digest.to_string());
        const auto *registry_generation = data.value().find("registry_generation");
        MIRA_CHECK(registry_generation != nullptr && registry_generation->is_integer() &&
                   *registry_generation->as_integer() == 1);
        const auto *negotiation = data.value().find("negotiation");
        MIRA_CHECK(negotiation != nullptr && negotiation->is_object());
    }

    // A second session establishment is rejected; current() is untouched.
    MIRA_CHECK(failed_with(coordinator.on_session_established(8, capabilities, registry),
                           ErrorCode::InvalidState));
    MIRA_CHECK(coordinator.current()->generation == 1);

    // Same epoch, same capabilities: idempotent no-op (no event, no bump).
    auto again = coordinator.on_environment_changed(7, capabilities, registry);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value().generation == 1);
    MIRA_CHECK(again.value().view_digest == view_one.view_digest);
    events = query_events(store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(count_events(events.value(), kModuleNegotiationEventSchema) == 1);

    // Same epoch, changed capabilities: fail closed.
    EnvironmentCapabilities changed = capabilities;
    changed.ui_tree = true;
    MIRA_CHECK(failed_with(coordinator.on_environment_changed(7, changed, registry),
                           ErrorCode::InvalidArgument));
    MIRA_CHECK(coordinator.current()->generation == 1);

    // Older epoch: stale.
    MIRA_CHECK(failed_with(coordinator.on_environment_changed(3, capabilities, registry),
                           ErrorCode::StaleObservation));
    MIRA_CHECK(failed_with(coordinator.on_environment_changed(0, changed, registry),
                           ErrorCode::StaleObservation));

    // Newer epoch: renegotiate as EnvironmentChanged.
    auto second = coordinator.on_environment_changed(9, changed, registry);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().generation == 2);
    MIRA_CHECK(second.value().epoch == 9);
    MIRA_CHECK(second.value().trigger == NegotiationTrigger::EnvironmentChanged);
    MIRA_CHECK(second.value().view_digest == recompute_view_digest(second.value()));
    MIRA_CHECK(second.value().view_digest != view_one.view_digest);
    events = query_events(store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(count_events(events.value(), kModuleNegotiationEventSchema) == 2);

    // ModuleStatesChanged: renegotiation against the pinned environment.
    MIRA_CHECK(registry.register_module(beta).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.beta").has_value());
    auto third = coordinator.on_modules_changed(registry);
    MIRA_CHECK(third.has_value());
    MIRA_CHECK(third.value().generation == 3);
    MIRA_CHECK(third.value().epoch == 9);
    MIRA_CHECK(third.value().trigger == NegotiationTrigger::ModuleStatesChanged);
    MIRA_CHECK(third.value().negotiation.modules.size() == 2);
    MIRA_CHECK(third.value().negotiation.modules[0].status == ModuleStatus::Available);
    MIRA_CHECK(third.value().negotiation.modules[1].status == ModuleStatus::Available);
    MIRA_CHECK(coordinator.current()->generation == 3);
    events = query_events(store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(count_events(events.value(), kModuleNegotiationEventSchema) == 3);

    // Negotiation events without a sink are simply not recorded.
    {
        ModuleNegotiationCoordinator silent{CapabilityCatalog::core()};
        ModuleRegistry silent_registry{CapabilityCatalog::core(), builtin_trust_for({alpha})};
        MIRA_CHECK(silent_registry.register_module(alpha).has_value());
        MIRA_CHECK(silent_registry.stage_module("acme.reg.alpha").has_value());
        MIRA_CHECK(silent_registry.activate_module("acme.reg.alpha").has_value());
        auto view = silent.on_session_established(1, capabilities, silent_registry);
        MIRA_CHECK(view.has_value());
        MIRA_CHECK(view.value().generation == 1);
        MIRA_CHECK(view.value().view_digest == recompute_view_digest(view.value()));
    }
    return 0;
}

int g3_report_is_deterministic() {
    // In-process determinism: the fixed scenario report is byte-identical when
    // rebuilt, and no randomness reaches it. Cross-process determinism is
    // asserted by running --report twice and comparing bytes.
    const std::string first = build_registry_report();
    const std::string second = build_registry_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());
    // It is canonical JSON with the two documented sections.
    auto parsed = parse_json(first);
    MIRA_CHECK(parsed.has_value());
    const auto *schema = parsed.value().find("schema");
    MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
               *schema->as_string() == "mira.tool_module.registry.report.v1");
    const auto *snapshots = parsed.value().find("snapshots");
    MIRA_CHECK(snapshots != nullptr && snapshots->is_array() && snapshots->as_array()->size() == 5);
    const auto *views = parsed.value().find("negotiation_views");
    MIRA_CHECK(views != nullptr && views->is_array() && views->as_array()->size() == 4);
    // Generation coverage: snapshots 0..4, views 1..4.
    const auto &snapshot_array = *snapshots->as_array();
    for (std::size_t index = 0; index < snapshot_array.size(); ++index) {
        const auto *generation = snapshot_array[index].find("generation");
        MIRA_CHECK(generation != nullptr && generation->is_integer() &&
                   *generation->as_integer() == static_cast<std::int64_t>(index));
    }
    const auto &view_array = *views->as_array();
    for (std::size_t index = 0; index < view_array.size(); ++index) {
        const auto *generation = view_array[index].find("generation");
        MIRA_CHECK(generation != nullptr && generation->is_integer() &&
                   *generation->as_integer() == static_cast<std::int64_t>(index + 1));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G4: immutable snapshots and in-flight settlement (M7-TM1-G4)
// ---------------------------------------------------------------------------

int g4_snapshot_immutability_and_generation_rules() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest gamma =
        fixture_manifest("acme.reg.gamma", "gamma_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta, gamma}),
                            &store, test_runtime_id(), test_session_id()};

    MIRA_CHECK(registry.register_module(beta).has_value());  // registration order is
    MIRA_CHECK(registry.register_module(alpha).has_value()); // deliberately not sorted

    // Stage does not bump the generation.
    MIRA_CHECK(registry.active_snapshot().generation == 0);
    MIRA_CHECK(registry.stage_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.active_snapshot().generation == 0);

    // Activations bump and rebuild; the published set is sorted by module_id
    // regardless of activation order.
    MIRA_CHECK(registry.activate_module("acme.reg.beta").has_value());
    const RegistrySnapshot pinned_one = registry.active_snapshot();
    MIRA_CHECK(pinned_one.generation == 1);
    MIRA_CHECK(pinned_one.modules.size() == 1);
    MIRA_CHECK(pinned_one.modules[0].module_id == "acme.reg.beta");

    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());
    const RegistrySnapshot pinned_two = registry.active_snapshot();
    MIRA_CHECK(pinned_two.generation == 2);
    MIRA_CHECK(pinned_two.modules.size() == 2);
    MIRA_CHECK(pinned_two.modules[0].module_id == "acme.reg.alpha");
    MIRA_CHECK(pinned_two.modules[1].module_id == "acme.reg.beta");
    MIRA_CHECK(pinned_two.digest == recompute_snapshot_digest(pinned_two));

    // The earlier snapshot copy is immutable: same modules, digest, generation.
    MIRA_CHECK(pinned_one.generation == 1);
    MIRA_CHECK(pinned_one.modules.size() == 1);
    MIRA_CHECK(pinned_one.modules[0].module_id == "acme.reg.beta");
    MIRA_CHECK(pinned_one.digest == recompute_snapshot_digest(pinned_one));
    MIRA_CHECK(pinned_one.digest != pinned_two.digest);

    // Downgrades bump again and remove members; revoked non-Active does not.
    MIRA_CHECK(registry.deprecate_module("acme.reg.alpha").has_value());
    const RegistrySnapshot pinned_three = registry.active_snapshot();
    MIRA_CHECK(pinned_three.generation == 3);
    MIRA_CHECK(pinned_three.modules.size() == 1);
    MIRA_CHECK(pinned_two.digest == recompute_snapshot_digest(pinned_two)); // still unchanged
    MIRA_CHECK(pinned_two.generation == 2);

    // Revoking the Staged gamma never bumps (it was never Active).
    MIRA_CHECK(registry.register_module(gamma).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.gamma").has_value());
    MIRA_CHECK(registry.revoke_module("acme.reg.gamma").has_value());
    MIRA_CHECK(registry.active_snapshot().generation == 3);

    // Revoking an Active member bumps.
    MIRA_CHECK(registry.revoke_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.active_snapshot().generation == 4);
    MIRA_CHECK(registry.active_snapshot().modules.empty());

    // Generation is monotonic across the whole history.
    MIRA_CHECK(pinned_one.generation < pinned_two.generation);
    MIRA_CHECK(pinned_two.generation < pinned_three.generation);
    MIRA_CHECK(pinned_three.generation < registry.active_snapshot().generation);
    return 0;
}

int g4_inflight_views_and_wire_name_conflict() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};
    EnvironmentCapabilities capabilities;
    capabilities.screen_capture = true;

    MIRA_CHECK(registry.register_module(alpha).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());

    auto first = coordinator.on_session_established(5, capabilities, registry);
    MIRA_CHECK(first.has_value());
    const NegotiationView pinned = first.value(); // in-flight consumer pins view 1
    const RegistrySnapshot pinned_snapshot = registry.active_snapshot();

    // The registry and coordinator move on; the pinned values never change.
    MIRA_CHECK(registry.register_module(beta).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.beta").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.beta").has_value());
    auto second = coordinator.on_modules_changed(registry);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().generation == 2);

    MIRA_CHECK(pinned.generation == 1);
    MIRA_CHECK(
        pinned.negotiation.digest ==
        negotiate_modules(pinned_snapshot.modules, capabilities, CapabilityCatalog::core()).digest);
    MIRA_CHECK(pinned.view_digest == recompute_view_digest(pinned));
    MIRA_CHECK(pinned.negotiation.modules.size() == 1);
    MIRA_CHECK(second.value().negotiation.modules.size() == 2);
    MIRA_CHECK(second.value().view_digest != pinned.view_digest);
    // New consumers read current(), and it is the new generation.
    MIRA_CHECK(coordinator.current() != nullptr);
    MIRA_CHECK(coordinator.current()->generation == 2);
    MIRA_CHECK(coordinator.current()->view_digest == second.value().view_digest);

    // Cross-module wire-name collision: the later activation is rejected, the
    // incumbent keeps serving and the challenger stays Staged.
    const ToolModuleManifest incumbent =
        fixture_manifest("acme.reg.incumbent", "shared_probe", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest challenger =
        fixture_manifest("acme.reg.challenger", "shared_probe", ToolModuleOrigin::BuiltIn);
    MemoryEventStore conflict_store;
    ModuleRegistry conflict_registry{CapabilityCatalog::core(),
                                     builtin_trust_for({incumbent, challenger}), &conflict_store,
                                     test_runtime_id(), test_session_id()};
    MIRA_CHECK(conflict_registry.register_module(incumbent).has_value());
    MIRA_CHECK(conflict_registry.stage_module("acme.reg.incumbent").has_value());
    MIRA_CHECK(conflict_registry.activate_module("acme.reg.incumbent").has_value());
    const RegistrySnapshot before_attempt = conflict_registry.active_snapshot();

    MIRA_CHECK(conflict_registry.register_module(challenger).has_value());
    MIRA_CHECK(conflict_registry.stage_module("acme.reg.challenger").has_value());
    const auto attempt = conflict_registry.activate_module("acme.reg.challenger");
    MIRA_CHECK(failed_with(attempt, ErrorCode::InvalidState));
    MIRA_CHECK(attempt.error().safe_message.find("shared_probe") != std::string::npos);
    MIRA_CHECK(state_is(conflict_registry, "acme.reg.challenger", ModuleState::Staged));
    MIRA_CHECK(state_is(conflict_registry, "acme.reg.incumbent", ModuleState::Active));

    // No generation movement, the incumbent is untouched and still the only
    // Active module; the rejection is visible as an activation_rejected event.
    const RegistrySnapshot after_attempt = conflict_registry.active_snapshot();
    MIRA_CHECK(after_attempt.generation == before_attempt.generation);
    MIRA_CHECK(after_attempt.digest == before_attempt.digest);
    MIRA_CHECK(after_attempt.modules.size() == 1);
    MIRA_CHECK(after_attempt.modules[0].module_id == "acme.reg.incumbent");
    MIRA_CHECK(conflict_registry.stats().activated == 1);
    auto events = query_events(conflict_store);
    MIRA_CHECK(events.has_value());
    MIRA_CHECK(lifecycle_event_matches(events.value().back(), "activation_rejected", challenger,
                                       EventClass::State, "collides"));

    // The incumbent still downgrades normally, and once it leaves Active the
    // challenger may activate (fail-closed, not permanent poisoning).
    MIRA_CHECK(conflict_registry.deprecate_module("acme.reg.incumbent").has_value());
    MIRA_CHECK(conflict_registry.activate_module("acme.reg.challenger").has_value());
    MIRA_CHECK(state_is(conflict_registry, "acme.reg.challenger", ModuleState::Active));
    MIRA_CHECK(conflict_registry.active_snapshot().modules.size() == 1);
    MIRA_CHECK(conflict_registry.active_snapshot().modules[0].module_id == "acme.reg.challenger");
    return 0;
}

// ---------------------------------------------------------------------------
// G5: Executor routing and close (M7-TM1-G5)
// ---------------------------------------------------------------------------

int g5_submit_verification_settlement_matrix() {
    const ToolModuleManifest builtin =
        fixture_manifest("acme.reg.builtin", "builtin_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest oop =
        fixture_manifest("acme.reg.oop", "oop_read", ToolModuleOrigin::OutOfProcess, "acme-signer",
                         "ed25519-ref", "dGVzdC1zaWduYXR1cmUtbWF0ZXJpYWw");
    const Hash builtin_digest = tool_module_manifest_digest(builtin);
    const ModuleTrustConfig builtin_trust = builtin_trust_for({builtin});

    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    config.queue_capacity = 64;
    MIRA_CHECK(executor.initialize(config));

    // Normal completion: consume_module_verification returns the trusted
    // report (full round trip: identity, digest, verdict, reason).
    auto normal = submit_module_verification(executor, builtin, builtin_trust);
    MIRA_CHECK(normal.has_value());
    auto normal_result = consume_module_verification(std::move(normal.value()));
    MIRA_CHECK(normal_result.has_value());
    MIRA_CHECK(normal_result.value().trusted);
    MIRA_CHECK(normal_result.value().manifest_digest == builtin_digest);
    MIRA_CHECK(normal_result.value().module_id == builtin.module_id);
    MIRA_CHECK(normal_result.value().version == builtin.version);
    MIRA_CHECK(normal_result.value().origin == ToolModuleOrigin::BuiltIn);
    MIRA_CHECK(!normal_result.value().reason.empty());

    // Cancellation pre-set: consume returns Cancelled and the verifier is
    // never called.
    {
        ProbeVerifier verifier(true);
        const ModuleTrustConfig oop_trust = oop_trust_for(&verifier, {"acme-signer"});
        auto flag = std::make_shared<std::atomic<bool>>(true);
        ModuleDeployToken token(flag);
        MIRA_CHECK(token.stop_requested());
        auto cancelled = submit_module_verification(executor, oop, oop_trust, token);
        MIRA_CHECK(cancelled.has_value());
        auto cancelled_result = consume_module_verification(std::move(cancelled.value()));
        MIRA_CHECK(failed_with(cancelled_result, ErrorCode::Cancelled));
        MIRA_CHECK(verifier.calls_.load() == 0);
    }
    // A default-constructed token never reports stop.
    {
        ModuleDeployToken token;
        MIRA_CHECK(!token.stop_requested());
        ProbeVerifier verifier(true);
        auto plain = submit_module_verification(executor, oop,
                                                oop_trust_for(&verifier, {"acme-signer"}), token);
        MIRA_CHECK(plain.has_value());
        auto plain_result = consume_module_verification(std::move(plain.value()));
        MIRA_CHECK(plain_result.has_value());
        MIRA_CHECK(plain_result.value().trusted);
        MIRA_CHECK(verifier.calls_.load() == 1);
    }

    // Task exception (throwing verifier): consume returns an Internal error
    // instead of swallowing or propagating the exception.
    {
        ThrowingVerifier thrower;
        auto thrown =
            submit_module_verification(executor, oop, oop_trust_for(&thrower, {"acme-signer"}));
        MIRA_CHECK(thrown.has_value());
        auto thrown_result = consume_module_verification(std::move(thrown.value()));
        MIRA_CHECK(failed_with(thrown_result, ErrorCode::Internal));
        MIRA_CHECK(thrower.calls_.load() == 1);
    }

    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g5_submission_rejection_converts_to_error_result() {
    const ToolModuleManifest builtin =
        fixture_manifest("acme.reg.builtin", "builtin_read", ToolModuleOrigin::BuiltIn);
    const ModuleTrustConfig trust = builtin_trust_for({builtin});

    // Synchronous rejection surface, executor stopped: the facade reports a
    // stopped or uninitialized submission path as a plain runtime_error, which
    // submit_module_verification must convert to Unavailable with no future.
    {
        executor::Executor stopped;
        executor::ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        MIRA_CHECK(stopped.initialize(config));
        MIRA_CHECK(stopped.shutdown(true) == executor::ShutdownResult::Completed);

        auto rejected = submit_module_verification(stopped, builtin, trust);
        MIRA_CHECK(!rejected.has_value()); // no future escapes
        MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable);
        MIRA_CHECK(rejected.error().domain == "mira.tool_module");
        MIRA_CHECK(!rejected.error().safe_message.empty());
    }

    // Admission rejection surface, capacity exhausted: this facade delivers
    // the rejection as a ready-with-exception future; the submission returns a
    // future and consume_module_verification() folds it into an explicit
    // ResourceExhausted error result — never an escaping exception. A facade
    // that instead rejects synchronously must use the same error code.
    {
        executor::Executor bounded;
        executor::ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.queue_capacity = 4;
        config.max_in_flight_tasks = 1;
        MIRA_CHECK(bounded.initialize(config));

        std::promise<void> gate;
        auto gate_future = gate.get_future().share();
        auto blocker = bounded.submit([gate_future] { gate_future.wait(); });

        auto rejected = submit_module_verification(bounded, builtin, trust);
        if (rejected.has_value()) {
            auto folded = consume_module_verification(std::move(rejected.value()));
            MIRA_CHECK(failed_with(folded, ErrorCode::ResourceExhausted));
            MIRA_CHECK(folded.error().domain == "mira.tool_module");
            MIRA_CHECK(!folded.error().safe_message.empty());
        } else {
            MIRA_CHECK(rejected.error().code == ErrorCode::ResourceExhausted);
        }

        // The same admission rejection folds identically for a second
        // submission while the blocker still holds the admission slot.
        auto second = submit_module_verification(bounded, builtin, trust);
        MIRA_CHECK(second.has_value());
        auto folded_second = consume_module_verification(std::move(second.value()));
        MIRA_CHECK(failed_with(folded_second, ErrorCode::ResourceExhausted));

        gate.set_value();
        blocker.get(); // consume the blocker future before shutdown
        MIRA_CHECK(bounded.shutdown(true) == executor::ShutdownResult::Completed);
    }
    return 0;
}

int g5_close_rejects_everything_with_visible_stats() {
    const ToolModuleManifest alpha =
        fixture_manifest("acme.reg.alpha", "alpha_read", ToolModuleOrigin::BuiltIn);
    const ToolModuleManifest beta =
        fixture_manifest("acme.reg.beta", "beta_read", ToolModuleOrigin::BuiltIn);
    MemoryEventStore store;
    ModuleRegistry registry{CapabilityCatalog::core(), builtin_trust_for({alpha, beta}), &store,
                            test_runtime_id(), test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};
    EnvironmentCapabilities capabilities;

    MIRA_CHECK(registry.register_module(alpha).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.alpha").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.alpha").has_value());
    auto session = coordinator.on_session_established(2, capabilities, registry);
    MIRA_CHECK(session.has_value());

    const RegistrySnapshot last = registry.active_snapshot();
    const auto close_report = registry.close();
    MIRA_CHECK(close_report.snapshot_generation == last.generation);
    MIRA_CHECK(close_report.active_modules == 1);

    // Every registry mutation is rejected after close.
    MIRA_CHECK(failed_with(registry.register_module(beta), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.stage_module("acme.reg.beta"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.activate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.revoke_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(failed_with(registry.seal(), ErrorCode::InvalidState));
    MIRA_CHECK(registry.stats().rejected_closed == 6);

    // The snapshot stays readable for in-flight settlement and unchanged.
    const RegistrySnapshot readable = registry.active_snapshot();
    MIRA_CHECK(readable.generation == last.generation);
    MIRA_CHECK(readable.digest == last.digest);
    MIRA_CHECK(readable.modules.size() == 1);

    // The coordinator still exposes the pinned current view (it never owns
    // registry state; the registry is the mutation authority that closed).
    MIRA_CHECK(coordinator.current() != nullptr);
    MIRA_CHECK(coordinator.current()->generation == 1);

    // Frozen gate G5: after close() every negotiation trigger is rejected
    // (InvalidState), no new generation is produced and current() stays
    // readable. The environment re-declaration below would otherwise be an
    // idempotent no-op and the module trigger a new generation, so these
    // assertions discriminate the closed check, not the general epoch rules.
    MIRA_CHECK(failed_with(coordinator.on_environment_changed(2, capabilities, registry),
                           ErrorCode::InvalidState));
    MIRA_CHECK(coordinator.on_environment_changed(2, capabilities, registry)
                   .error()
                   .safe_message.find("registry is closed") != std::string::npos);
    MIRA_CHECK(failed_with(coordinator.on_modules_changed(registry), ErrorCode::InvalidState));
    // A fresh coordinator cannot even establish a session against a closed
    // registry (distinguishes the closed check from the duplicate-session
    // rejection the established coordinator would hit).
    ModuleNegotiationCoordinator fresh{CapabilityCatalog::core(), &store, test_runtime_id(),
                                       test_session_id()};
    MIRA_CHECK(fresh.current() == nullptr);
    MIRA_CHECK(failed_with(fresh.on_session_established(3, capabilities, registry),
                           ErrorCode::InvalidState));
    MIRA_CHECK(fresh.current() == nullptr);

    // No trigger moved the generation; the pinned view is intact and still
    // settles, and the registry-side rejection remains the enforcement point.
    MIRA_CHECK(coordinator.current() != nullptr);
    MIRA_CHECK(coordinator.current()->generation == 1);
    MIRA_CHECK(coordinator.current()->view_digest == session.value().view_digest);
    MIRA_CHECK(failed_with(registry.deprecate_module("acme.reg.alpha"), ErrorCode::InvalidState));
    MIRA_CHECK(registry.stats().rejected_closed == 7);
    MIRA_CHECK(registry.active_snapshot().digest == last.digest);
    return 0;
}

// ---------------------------------------------------------------------------
// G6: event payload schema and redaction (M7-TM1-G6)
// ---------------------------------------------------------------------------

int g6_event_payloads_versioned_and_redacted() {
    const std::string secret_signature = "zz-oop-secret-signature-material-zz";
    const ToolModuleManifest oop =
        fixture_manifest("acme.reg.oop", "oop_read", ToolModuleOrigin::OutOfProcess, "acme-signer",
                         "ed25519-ref", secret_signature);
    // Same secret material but a signer outside the trusted set: quarantined.
    const ToolModuleManifest outsider =
        fixture_manifest("acme.reg.outsider", "outsider_read", ToolModuleOrigin::OutOfProcess,
                         "rogue-signer", "ed25519-ref", secret_signature);

    MemoryEventStore store;
    ProbeVerifier acceptor(true);
    ModuleTrustConfig trust = oop_trust_for(&acceptor, {"acme-signer"});
    ModuleRegistry registry{CapabilityCatalog::core(), trust, &store, test_runtime_id(),
                            test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};

    MIRA_CHECK(registry.register_module(oop).has_value());
    MIRA_CHECK(registry.stage_module("acme.reg.oop").has_value());
    MIRA_CHECK(registry.activate_module("acme.reg.oop").has_value());
    auto quarantined = registry.register_module(outsider);
    MIRA_CHECK(failed_with(quarantined, ErrorCode::SafetyRejected));

    EnvironmentCapabilities capabilities;
    auto view = coordinator.on_session_established(3, capabilities, registry);
    MIRA_CHECK(view.has_value());

    auto events = query_events(store);
    MIRA_CHECK(events.has_value());
    // oop: discovered, verified, staged, activated; outsider: discovered,
    // quarantined; then one negotiation event.
    MIRA_CHECK(events.value().size() == 7);
    MIRA_CHECK(count_events(events.value(), kModuleLifecycleEventSchema) == 6);
    MIRA_CHECK(count_events(events.value(), kModuleNegotiationEventSchema) == 1);

    for (const auto &envelope : events.value()) {
        // Versioned JSON payload with the exact schema type.
        MIRA_CHECK(envelope.payload.type == kModuleLifecycleEventSchema ||
                   envelope.payload.type == kModuleNegotiationEventSchema);
        auto data = parse_json(envelope.payload.data);
        MIRA_CHECK(data.has_value());
        const auto *schema = data.value().find("schema");
        MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr);
        MIRA_CHECK(*schema->as_string() == envelope.payload.type);
        // Redaction: no signature material, no signer identity, no signature
        // key at all, in either event family.
        MIRA_CHECK(envelope.payload.data.find(secret_signature) == std::string::npos);
        MIRA_CHECK(envelope.payload.data.find("acme-signer") == std::string::npos);
        MIRA_CHECK(envelope.payload.data.find("rogue-signer") == std::string::npos);
        MIRA_CHECK(data.value().find("signature") == nullptr);
        MIRA_CHECK(data.value().find("signer") == nullptr);
        MIRA_CHECK(data.value().find("signature_algorithm") == nullptr);
    }

    // The lifecycle payloads carry the documented public identity fields.
    MIRA_CHECK(lifecycle_event_matches(events.value()[0], "discovered", oop, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[1], "verified", oop, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[2], "staged", oop, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[3], "activated", oop, EventClass::State));
    MIRA_CHECK(
        lifecycle_event_matches(events.value()[4], "discovered", outsider, EventClass::State));
    MIRA_CHECK(lifecycle_event_matches(events.value()[5], "quarantined", outsider,
                                       EventClass::Critical, "signer"));
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report: canonical report of the fixed scenario for cross-process byte
    // comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        std::cout << build_registry_report() << '\n';
        return 0;
    }

    struct Gate {
        const char *name;
        int (*run)();
    };
    const Gate gates[] = {
        {"G1 state names round trip", g1_state_names_round_trip},
        {"G1 lifecycle happy path and events", g1_lifecycle_happy_path_and_events},
        {"G1 illegal transitions rejected and unchanged",
         g1_illegal_transitions_rejected_and_unchanged},
        {"G1 seal and close semantics", g1_seal_and_close_semantics},
        {"G1 event emission gating and sink failures", g1_event_emission_gating_and_sink_failures},
        {"G2 trust config validation bounds", g2_trust_config_validation_bounds},
        {"G2 unsigned digest properties", g2_unsigned_digest_properties},
        {"G2 verify_module_trust matrix", g2_verify_module_trust_matrix},
        {"G2 registry fail closed and quarantine", g2_registry_fail_closed_and_quarantine},
        {"G2 precomputed report binding", g2_precomputed_report_binding},
        {"G2 tombstone blocks re-registration", g2_tombstone_blocks_reregistration},
        {"G3 trigger names and pre-session errors", g3_trigger_names_and_pre_session_errors},
        {"G3 trigger sequence and epoch rules", g3_trigger_sequence_and_epoch_rules},
        {"G3 report determinism", g3_report_is_deterministic},
        {"G4 snapshot immutability and generation rules",
         g4_snapshot_immutability_and_generation_rules},
        {"G4 in-flight views and wire-name conflict", g4_inflight_views_and_wire_name_conflict},
        {"G5 submit verification settlement matrix", g5_submit_verification_settlement_matrix},
        {"G5 submission rejection converts to error result",
         g5_submission_rejection_converts_to_error_result},
        {"G5 close rejects everything with visible stats",
         g5_close_rejects_everything_with_visible_stats},
        {"G6 event payloads versioned and redacted", g6_event_payloads_versioned_and_redacted},
    };

    std::cout << "M7 TM1 tool module registry verification (" << sizeof(gates) / sizeof(gates[0])
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
