#include <mira/tool_module_registry.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace mira {

namespace {

// Shared error domain for the registry, trust and negotiation layers.
Error make_module_error(ErrorCode code, std::string safe_message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_module";
    error.safe_message = std::move(safe_message);
    return error;
}

// Diagnostic reasons travel into events and records; keep them bounded so a
// hostile or sloppy caller cannot balloon event payloads (RULE-08). Reasons
// are ASCII diagnostics in practice; byte truncation is acceptable here.
constexpr std::size_t kMaxReasonBytes = 256;

std::string bounded_reason(std::string reason) {
    if (reason.size() > kMaxReasonBytes) {
        reason.resize(kMaxReasonBytes);
    }
    return reason;
}

// Validation bounds for deploy-supplied trust material (RULE-08).
constexpr std::size_t kMaxTrustDigests = 4096;
constexpr std::size_t kMaxTrustEntries = 1024;
constexpr std::size_t kMaxSignerBytes = 256;

bool is_valid_vocabulary_id(std::string_view id) {
    if (id.empty() || id.front() == '.' || id.back() == '.') {
        return false;
    }
    bool segment_nonempty = false;
    for (const char character : id) {
        if (character == '.') {
            if (!segment_nonempty) {
                return false;
            }
            segment_nonempty = false;
            continue;
        }
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '_' ||
                             character == '-';
        if (!allowed) {
            return false;
        }
        segment_nonempty = true;
    }
    return segment_nonempty;
}

// Sink failures never propagate to the control plane: they are counted and
// visible in stats (the same isolation the Context/Memory supervisor applies
// to its diagnostic events).
bool append_module_event(IEventStore *sink, RuntimeId runtime_id, SessionId session_id,
                         const std::string &type, EventClass classification, JsonValue payload) {
    if (sink == nullptr || runtime_id.is_nil() || session_id.is_nil()) {
        return false;
    }
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = runtime_id;
    request.session_id = session_id;
    request.payload.type = type;
    request.payload.data = to_json_string(payload);
    request.payload.classification = classification;
    return sink->append(request).has_value();
}

bool same_environment(const EnvironmentCapabilities &left, const EnvironmentCapabilities &right) {
    return left.screen_capture == right.screen_capture && left.ui_tree == right.ui_tree &&
           left.foreground_app == right.foreground_app && left.device_state == right.device_state &&
           left.perception_sources == right.perception_sources &&
           left.atomic_observation == right.atomic_observation &&
           left.max_component_skew == right.max_component_skew &&
           left.discrete_input == right.discrete_input &&
           left.input_release == right.input_release &&
           left.epoch_invalidation == right.epoch_invalidation;
}

[[nodiscard]] JsonValue registry_snapshot_digest_json(const RegistrySnapshot &snapshot) {
    JsonValue::Object root;
    root.emplace_back("generation", static_cast<std::int64_t>(snapshot.generation));
    JsonValue::Array modules;
    modules.reserve(snapshot.modules.size());
    for (const auto &module : snapshot.modules) {
        JsonValue::Object entry;
        entry.emplace_back("module_id", module.module_id);
        entry.emplace_back("digest", module.module_digest.to_string());
        modules.emplace_back(JsonValue{std::move(entry)});
    }
    root.emplace_back("modules", JsonValue{std::move(modules)});
    return JsonValue{std::move(root)};
}

} // namespace

// ---------------------------------------------------------------------------
// Origin trust
// ---------------------------------------------------------------------------

Result<void> ModuleTrustConfig::validate() const {
    if (builtin_digests.size() > kMaxTrustDigests) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "built-in digest set exceeds the trust bound");
    }
    if (host_provided_allowlist.size() > kMaxTrustEntries) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "host-provided allowlist exceeds the trust bound");
    }
    if (trusted_out_of_process_signers.size() > kMaxTrustEntries) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "out-of-process signer set exceeds the trust bound");
    }
    if (revoked_digests.size() > kMaxTrustDigests) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "tombstone seed exceeds the trust bound");
    }
    for (const auto &id : host_provided_allowlist) {
        if (!is_valid_vocabulary_id(id)) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "host-provided allowlist entry is not a valid id: '" + id +
                                         "'");
        }
    }
    for (const auto &signer : trusted_out_of_process_signers) {
        if (signer.empty() || signer.size() > kMaxSignerBytes) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "out-of-process signer entry is empty or oversized");
        }
    }
    if (!trusted_out_of_process_signers.empty() && verifier == nullptr) {
        return make_module_error(
            ErrorCode::InvalidArgument,
            "out-of-process signers are configured without a signature verifier");
    }
    return Result<void>{};
}

ModuleTrustReport verify_module_trust(const ToolModuleManifest &manifest,
                                      const ModuleTrustConfig &config) {
    ModuleTrustReport report;
    report.module_id = manifest.module_id;
    report.version = manifest.version;
    report.manifest_digest = tool_module_manifest_digest(manifest);
    report.origin = manifest.origin;

    const auto fail = [&report](std::string reason) {
        report.trusted = false;
        report.reason = std::move(reason);
        return report;
    };

    switch (manifest.origin) {
    case ToolModuleOrigin::BuiltIn: {
        const auto pinned = std::find(config.builtin_digests.begin(), config.builtin_digests.end(),
                                      report.manifest_digest);
        if (pinned == config.builtin_digests.end()) {
            return fail("built-in module digest is not pinned by the build");
        }
        report.trusted = true;
        report.reason = "built-in digest pinned by the build";
        return report;
    }
    case ToolModuleOrigin::HostProvided: {
        const auto allowed = std::find(config.host_provided_allowlist.begin(),
                                       config.host_provided_allowlist.end(), manifest.module_id);
        if (allowed == config.host_provided_allowlist.end()) {
            return fail("host-provided module id is not in the injection allowlist");
        }
        report.trusted = true;
        report.reason = "host-provided module explicitly injected (allowlist)";
        return report;
    }
    case ToolModuleOrigin::OutOfProcess: {
        if (manifest.signer.empty()) {
            return fail("out-of-process module declares no signer");
        }
        if (manifest.signature_algorithm.empty() || manifest.signature.empty()) {
            return fail("out-of-process module declares no signature");
        }
        const auto trusted =
            std::find(config.trusted_out_of_process_signers.begin(),
                      config.trusted_out_of_process_signers.end(), manifest.signer);
        if (trusted == config.trusted_out_of_process_signers.end()) {
            return fail("out-of-process signer is not trusted");
        }
        if (config.verifier == nullptr) {
            return fail("no signature verifier is configured");
        }
        const Hash signed_digest = tool_module_unsigned_manifest_digest(manifest);
        if (!config.verifier->verify(manifest.signer, manifest.signature_algorithm,
                                     manifest.signature, signed_digest)) {
            return fail("signature verification rejected the unsigned manifest digest");
        }
        report.trusted = true;
        report.reason = "out-of-process signature verified over the unsigned digest";
        return report;
    }
    }
    return fail("unknown origin");
}

// ---------------------------------------------------------------------------
// Module state machine
// ---------------------------------------------------------------------------

std::string_view module_state_name(ModuleState state) {
    switch (state) {
    case ModuleState::Discovered:
        return "discovered";
    case ModuleState::Verified:
        return "verified";
    case ModuleState::Staged:
        return "staged";
    case ModuleState::Active:
        return "active";
    case ModuleState::Deprecated:
        return "deprecated";
    case ModuleState::Revoked:
        return "revoked";
    case ModuleState::Quarantined:
        return "quarantined";
    }
    return "unknown";
}

Result<ModuleState> parse_module_state(std::string_view name) {
    if (name == "discovered")
        return ModuleState::Discovered;
    if (name == "verified")
        return ModuleState::Verified;
    if (name == "staged")
        return ModuleState::Staged;
    if (name == "active")
        return ModuleState::Active;
    if (name == "deprecated")
        return ModuleState::Deprecated;
    if (name == "revoked")
        return ModuleState::Revoked;
    if (name == "quarantined")
        return ModuleState::Quarantined;
    return make_module_error(ErrorCode::InvalidArgument,
                             "unknown module state name: '" + std::string(name) + "'");
}

struct ModuleRegistry::Impl final {
    Impl(CapabilityCatalog catalog_in, ModuleTrustConfig trust_in, IEventStore *sink,
         RuntimeId runtime, SessionId session)
        : catalog(std::move(catalog_in)), trust(std::move(trust_in)), event_sink(sink),
          runtime_id(runtime), session_id(session) {}

    [[nodiscard]] bool eventable() const {
        return event_sink != nullptr && !runtime_id.is_nil() && !session_id.is_nil();
    }

    void emit(std::string kind, const std::string &module_id, const SemanticVersion &version,
              const Hash &digest, ToolModuleOrigin origin, const std::string &reason,
              EventClass classification) {
        if (!eventable()) {
            return;
        }
        JsonValue::Object root;
        root.emplace_back("schema", std::string(kModuleLifecycleEventSchema));
        root.emplace_back("kind", std::move(kind));
        root.emplace_back("module_id", module_id);
        root.emplace_back("version", semantic_version_to_json(version));
        root.emplace_back("digest", digest.to_string());
        root.emplace_back("origin", std::string(tool_module_origin_name(origin)));
        if (!reason.empty()) {
            root.emplace_back("reason", reason);
        }
        const bool ok = append_module_event(event_sink, runtime_id, session_id,
                                            std::string(kModuleLifecycleEventSchema),
                                            classification, JsonValue{std::move(root)});
        if (ok) {
            ++stats.events_emitted;
        } else {
            ++stats.event_sink_failures;
        }
    }

    void rebuild_snapshot() {
        RegistrySnapshot snapshot;
        snapshot.generation = generation;
        for (const auto &[id, record] : modules) {
            if (record.state == ModuleState::Active) {
                snapshot.modules.push_back(module_snapshot_from_manifest(record.manifest));
            }
        }
        std::sort(snapshot.modules.begin(), snapshot.modules.end(),
                  [](const ModuleSnapshot &left, const ModuleSnapshot &right) {
                      return left.module_id < right.module_id;
                  });
        snapshot.digest = canonical_json_digest(registry_snapshot_digest_json(snapshot));
        active = std::move(snapshot);
    }

    CapabilityCatalog catalog;
    ModuleTrustConfig trust;
    IEventStore *event_sink = nullptr;
    RuntimeId runtime_id;
    SessionId session_id;

    // Sha256Digest only defines equality; tombstones order by raw bytes.
    struct DigestLess final {
        bool operator()(const Hash &left, const Hash &right) const noexcept {
            return left.bytes < right.bytes;
        }
    };

    std::map<std::string, ModuleRecord, std::less<>> modules;
    std::set<Hash, DigestLess> tombstones;
    std::uint64_t generation = 0;
    bool sealed_flag = false;
    bool closed_flag = false;
    RegistrySnapshot active;
    ModuleRegistryStats stats;
};

ModuleRegistry::ModuleRegistry(CapabilityCatalog catalog, ModuleTrustConfig trust,
                               IEventStore *event_sink, RuntimeId runtime_id,
                               SessionId session_id) {
    const auto valid = trust.validate();
    if (!valid) {
        throw std::invalid_argument(valid.error().safe_message);
    }
    impl_ = std::make_unique<Impl>(std::move(catalog), std::move(trust), event_sink, runtime_id,
                                   session_id);
    for (const auto &digest : impl_->trust.revoked_digests) {
        impl_->tombstones.insert(digest);
    }
    impl_->stats.tombstones = impl_->tombstones.size();
}

ModuleRegistry::~ModuleRegistry() = default;

Result<void> ModuleRegistry::register_module(ToolModuleManifest manifest) {
    ModuleTrustReport report;
    try {
        report = verify_module_trust(manifest, impl_->trust);
    } catch (const std::exception &) {
        // A throwing verifier is a host defect; admission fails closed into
        // Quarantined instead of escaping into the control plane.
        report = ModuleTrustReport{};
        report.module_id = manifest.module_id;
        report.version = manifest.version;
        report.manifest_digest = tool_module_manifest_digest(manifest);
        report.origin = manifest.origin;
        report.trusted = false;
        report.reason = "signature verifier threw";
    } catch (...) {
        report = ModuleTrustReport{};
        report.module_id = manifest.module_id;
        report.version = manifest.version;
        report.manifest_digest = tool_module_manifest_digest(manifest);
        report.origin = manifest.origin;
        report.trusted = false;
        report.reason = "signature verifier threw";
    }
    return register_module(std::move(manifest), std::move(report));
}

Result<void> ModuleRegistry::register_module(ToolModuleManifest manifest,
                                             ModuleTrustReport precomputed) {
    const std::string module_id = manifest.module_id;
    const Hash digest = tool_module_manifest_digest(manifest);
    const ToolModuleOrigin origin = manifest.origin;
    const SemanticVersion version = manifest.version;

    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    if (impl_->sealed_flag) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "registration window is sealed: no new modules");
    }
    if (impl_->modules.find(module_id) != impl_->modules.end()) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::AlreadyExists,
                                 "module id is already registered: '" + module_id + "'");
    }
    if (impl_->tombstones.contains(digest)) {
        ++impl_->stats.tombstone_hits;
        impl_->emit("registration_rejected", module_id, version, digest, origin,
                    "manifest digest is tombstoned by a prior revocation", EventClass::Critical);
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "manifest digest is tombstoned by a prior revocation");
    }

    ++impl_->stats.discovered;
    impl_->emit("discovered", module_id, version, digest, origin, std::string{}, EventClass::State);

    const bool bound = precomputed.module_id == module_id && precomputed.version == version &&
                       precomputed.manifest_digest == digest && precomputed.origin == origin;
    if (!bound) {
        impl_->emit("registration_rejected", module_id, version, digest, origin,
                    "trust report does not bind to this manifest", EventClass::State);
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidArgument,
                                 "trust report does not bind to this manifest");
    }

    ModuleRecord record;
    record.manifest = std::move(manifest);
    record.manifest_digest = digest;
    if (!precomputed.trusted) {
        record.state = ModuleState::Quarantined;
        record.last_reason = bounded_reason(precomputed.reason);
        const std::string reason = record.last_reason;
        impl_->modules.emplace(module_id, std::move(record));
        ++impl_->stats.quarantined;
        impl_->emit("quarantined", module_id, version, digest, origin, reason,
                    EventClass::Critical);
        return make_module_error(ErrorCode::SafetyRejected,
                                 "origin trust verification failed: " + reason);
    }

    record.state = ModuleState::Verified;
    record.last_reason = bounded_reason(precomputed.reason);
    impl_->modules.emplace(module_id, std::move(record));
    ++impl_->stats.registered;
    impl_->emit("verified", module_id, version, digest, origin,
                impl_->modules.at(module_id).last_reason, EventClass::State);
    return Result<void>{};
}

Result<void> ModuleRegistry::stage_module(std::string_view module_id) {
    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    if (impl_->sealed_flag) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "registration window is sealed: no further promotions");
    }
    const auto found = impl_->modules.find(module_id);
    if (found == impl_->modules.end() || found->second.state != ModuleState::Verified) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "stage requires state verified for module: '" +
                                     std::string(module_id) + "'");
    }
    found->second.state = ModuleState::Staged;
    impl_->emit("staged", found->second.manifest.module_id, found->second.manifest.version,
                found->second.manifest_digest, found->second.manifest.origin, std::string{},
                EventClass::State);
    return Result<void>{};
}

Result<void> ModuleRegistry::activate_module(std::string_view module_id) {
    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    if (impl_->sealed_flag) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "registration window is sealed: no further promotions");
    }
    const auto found = impl_->modules.find(module_id);
    if (found == impl_->modules.end() || found->second.state != ModuleState::Staged) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "activate requires state staged for module: '" +
                                     std::string(module_id) + "'");
    }

    // Cross-module member-name gate (design §5.3 rule 3): the later
    // activation is rejected, the incumbent is untouched.
    for (const auto &[id, record] : impl_->modules) {
        if (record.state != ModuleState::Active) {
            continue;
        }
        for (const auto &candidate : found->second.manifest.tools) {
            for (const auto &member : record.manifest.tools) {
                if (candidate.name == member.name) {
                    ++impl_->stats.rejected_transitions;
                    const std::string reason = "member tool name '" + candidate.name +
                                               "' collides with active module '" + id + "'";
                    impl_->emit("activation_rejected", found->second.manifest.module_id,
                                found->second.manifest.version, found->second.manifest_digest,
                                found->second.manifest.origin, reason, EventClass::State);
                    found->second.last_reason = bounded_reason(reason);
                    return make_module_error(ErrorCode::InvalidState, reason);
                }
            }
        }
    }

    found->second.state = ModuleState::Active;
    ++impl_->generation;
    impl_->rebuild_snapshot();
    ++impl_->stats.activated;
    impl_->emit("activated", found->second.manifest.module_id, found->second.manifest.version,
                found->second.manifest_digest, found->second.manifest.origin, std::string{},
                EventClass::State);
    return Result<void>{};
}

Result<void> ModuleRegistry::deprecate_module(std::string_view module_id, std::string reason) {
    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    const auto found = impl_->modules.find(module_id);
    if (found == impl_->modules.end() || found->second.state != ModuleState::Active) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "deprecate requires state active for module: '" +
                                     std::string(module_id) + "'");
    }
    found->second.state = ModuleState::Deprecated;
    found->second.last_reason = bounded_reason(std::move(reason));
    ++impl_->generation;
    impl_->rebuild_snapshot();
    ++impl_->stats.deprecated;
    impl_->emit("deprecated", found->second.manifest.module_id, found->second.manifest.version,
                found->second.manifest_digest, found->second.manifest.origin,
                found->second.last_reason, EventClass::State);
    return Result<void>{};
}

Result<void> ModuleRegistry::revoke_module(std::string_view module_id, std::string reason) {
    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    const auto found = impl_->modules.find(module_id);
    if (found == impl_->modules.end() ||
        (found->second.state != ModuleState::Active && found->second.state != ModuleState::Staged &&
         found->second.state != ModuleState::Deprecated)) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState,
                                 "revoke requires an undegraded state for module: '" +
                                     std::string(module_id) + "'");
    }
    const bool was_active = found->second.state == ModuleState::Active;
    found->second.state = ModuleState::Revoked;
    found->second.last_reason = bounded_reason(std::move(reason));
    impl_->tombstones.insert(found->second.manifest_digest);
    impl_->stats.tombstones = impl_->tombstones.size();
    if (was_active) {
        ++impl_->generation;
        impl_->rebuild_snapshot();
    }
    ++impl_->stats.revoked;
    impl_->emit("revoked", found->second.manifest.module_id, found->second.manifest.version,
                found->second.manifest_digest, found->second.manifest.origin,
                found->second.last_reason, EventClass::Critical);
    return Result<void>{};
}

Result<void> ModuleRegistry::seal() {
    if (impl_->closed_flag) {
        ++impl_->stats.rejected_closed;
        return make_module_error(ErrorCode::InvalidState, "registry is closed");
    }
    if (impl_->sealed_flag) {
        ++impl_->stats.rejected_transitions;
        return make_module_error(ErrorCode::InvalidState, "registration window already sealed");
    }
    impl_->sealed_flag = true;
    return Result<void>{};
}

ModuleRegistryCloseReport ModuleRegistry::close() {
    impl_->closed_flag = true;
    ModuleRegistryCloseReport report;
    report.snapshot_generation = impl_->active.generation;
    report.active_modules = impl_->active.modules.size();
    report.tombstones = impl_->tombstones.size();
    report.events_emitted = impl_->stats.events_emitted;
    report.event_sink_failures = impl_->stats.event_sink_failures;
    return report;
}

bool ModuleRegistry::sealed() const { return impl_->sealed_flag; }
bool ModuleRegistry::closed() const { return impl_->closed_flag; }

RegistrySnapshot ModuleRegistry::active_snapshot() const { return impl_->active; }

const ModuleRecord *ModuleRegistry::find(std::string_view module_id) const {
    const auto found = impl_->modules.find(module_id);
    return found == impl_->modules.end() ? nullptr : &found->second;
}

bool ModuleRegistry::is_tombstoned(const Hash &digest) const {
    return impl_->tombstones.contains(digest);
}

ModuleRegistryStats ModuleRegistry::stats() const { return impl_->stats; }

// ---------------------------------------------------------------------------
// Negotiation trigger hooks
// ---------------------------------------------------------------------------

std::string_view negotiation_trigger_name(NegotiationTrigger trigger) {
    switch (trigger) {
    case NegotiationTrigger::SessionEstablished:
        return "session_established";
    case NegotiationTrigger::EnvironmentChanged:
        return "environment_changed";
    case NegotiationTrigger::ModuleStatesChanged:
        return "module_states_changed";
    }
    return "unknown";
}

ModuleNegotiationCoordinator::ModuleNegotiationCoordinator(CapabilityCatalog catalog,
                                                           IEventStore *event_sink,
                                                           RuntimeId runtime_id,
                                                           SessionId session_id)
    : catalog_(std::move(catalog)), event_sink_(event_sink), runtime_id_(runtime_id),
      session_id_(session_id) {}

NegotiationView ModuleNegotiationCoordinator::build_view_(NegotiationTrigger trigger,
                                                          const ModuleRegistry &registry) {
    const RegistrySnapshot snapshot = registry.active_snapshot();

    NegotiationView view;
    view.generation = next_generation_++;
    view.epoch = epoch_;
    view.trigger = trigger;
    view.negotiation = negotiate_modules(snapshot.modules, *capabilities_, catalog_);

    JsonValue::Object digest_root;
    digest_root.emplace_back("generation", static_cast<std::int64_t>(view.generation));
    digest_root.emplace_back("epoch", static_cast<std::int64_t>(view.epoch));
    digest_root.emplace_back("trigger", std::string(negotiation_trigger_name(trigger)));
    digest_root.emplace_back("negotiation_digest", view.negotiation.digest.to_string());
    view.view_digest = canonical_json_digest(JsonValue{std::move(digest_root)});

    current_ = view;

    if (event_sink_ != nullptr && !runtime_id_.is_nil() && !session_id_.is_nil()) {
        JsonValue::Object root;
        root.emplace_back("schema", std::string(kModuleNegotiationEventSchema));
        root.emplace_back("trigger", std::string(negotiation_trigger_name(trigger)));
        root.emplace_back("epoch", static_cast<std::int64_t>(view.epoch));
        root.emplace_back("generation", static_cast<std::int64_t>(view.generation));
        root.emplace_back("view_digest", view.view_digest.to_string());
        root.emplace_back("registry_generation", static_cast<std::int64_t>(snapshot.generation));
        root.emplace_back("negotiation", module_negotiation_to_json(view.negotiation));
        (void)append_module_event(event_sink_, runtime_id_, session_id_,
                                  std::string(kModuleNegotiationEventSchema), EventClass::State,
                                  JsonValue{std::move(root)});
    }
    return view;
}

Result<NegotiationView>
ModuleNegotiationCoordinator::on_session_established(EnvironmentEpoch epoch,
                                                     const EnvironmentCapabilities &capabilities,
                                                     const ModuleRegistry &registry) {
    if (registry.closed()) {
        return make_module_error(ErrorCode::InvalidState,
                                 "registry is closed: negotiation trigger rejected");
    }
    if (current_.has_value()) {
        return make_module_error(ErrorCode::InvalidState, "session already established");
    }
    if (epoch == 0) {
        return make_module_error(ErrorCode::InvalidArgument, "session epoch must be non-zero");
    }
    capabilities_ = capabilities;
    epoch_ = epoch;
    return build_view_(NegotiationTrigger::SessionEstablished, registry);
}

Result<NegotiationView>
ModuleNegotiationCoordinator::on_environment_changed(EnvironmentEpoch epoch,
                                                     const EnvironmentCapabilities &capabilities,
                                                     const ModuleRegistry &registry) {
    if (registry.closed()) {
        return make_module_error(ErrorCode::InvalidState,
                                 "registry is closed: negotiation trigger rejected");
    }
    if (!current_.has_value()) {
        return make_module_error(ErrorCode::InvalidState,
                                 "no session established: environment change rejected");
    }
    if (epoch < epoch_) {
        return make_module_error(ErrorCode::StaleObservation, "environment epoch moved backwards");
    }
    if (epoch == epoch_) {
        if (same_environment(*capabilities_, capabilities)) {
            return *current_; // idempotent re-declaration: no event, no bump
        }
        return make_module_error(ErrorCode::InvalidArgument,
                                 "capabilities changed without an environment epoch bump");
    }
    capabilities_ = capabilities;
    epoch_ = epoch;
    return build_view_(NegotiationTrigger::EnvironmentChanged, registry);
}

Result<NegotiationView>
ModuleNegotiationCoordinator::on_modules_changed(const ModuleRegistry &registry) {
    if (registry.closed()) {
        return make_module_error(ErrorCode::InvalidState,
                                 "registry is closed: negotiation trigger rejected");
    }
    if (!current_.has_value()) {
        return make_module_error(ErrorCode::InvalidState,
                                 "no session established: module change rejected");
    }
    return build_view_(NegotiationTrigger::ModuleStatesChanged, registry);
}

const NegotiationView *ModuleNegotiationCoordinator::current() const noexcept {
    return current_.has_value() ? &*current_ : nullptr;
}

// ---------------------------------------------------------------------------
// Executor-routed deploy verification
// ---------------------------------------------------------------------------

Result<std::future<Result<ModuleTrustReport>>>
submit_module_verification(executor::Executor &executor, ToolModuleManifest manifest,
                           ModuleTrustConfig trust, ModuleDeployToken cancellation) {
    auto task = [manifest = std::move(manifest), trust = std::move(trust),
                 cancellation]() -> Result<ModuleTrustReport> {
        if (cancellation.stop_requested()) {
            return make_module_error(ErrorCode::Cancelled,
                                     "module verification cancelled before verification");
        }
        try {
            ModuleTrustReport report = verify_module_trust(manifest, trust);
            if (cancellation.stop_requested()) {
                return make_module_error(ErrorCode::Cancelled,
                                         "module verification cancelled before completion");
            }
            return report;
        } catch (const std::exception &) {
            return make_module_error(ErrorCode::Internal, "module verification task failed");
        } catch (...) {
            return make_module_error(ErrorCode::Internal, "module verification task failed");
        }
    };

    try {
        return executor.submit_auto(std::move(task));
    } catch (const executor::ExecutorStopping &) {
        return make_module_error(ErrorCode::Unavailable,
                                 "executor is stopping: verification not submitted");
    } catch (const executor::CapacityExhaustedException &) {
        return make_module_error(ErrorCode::ResourceExhausted,
                                 "executor capacity exhausted: verification not submitted");
    } catch (const std::runtime_error &) {
        // This facade reports a stopped or uninitialized submission path as a
        // plain runtime_error ("Async executor not initialized..."); the
        // specific executor exception types were matched above.
        return make_module_error(
            ErrorCode::Unavailable,
            "executor is not accepting submissions (stopped or not initialized)");
    } catch (...) {
        return make_module_error(ErrorCode::Internal, "verification submission failed");
    }
}

Result<ModuleTrustReport>
consume_module_verification(std::future<Result<ModuleTrustReport>> future) {
    try {
        return future.get();
    } catch (const executor::CapacityExhaustedException &) {
        // Admission rejections are delivered as a ready-with-exception future
        // by this facade; fold them into the explicit result surface.
        return make_module_error(ErrorCode::ResourceExhausted,
                                 "executor capacity exhausted: verification not admitted");
    } catch (const executor::ExecutorStopping &) {
        return make_module_error(ErrorCode::Unavailable,
                                 "executor is stopping: verification not admitted");
    } catch (const std::exception &) {
        return make_module_error(ErrorCode::Unavailable,
                                 "executor rejected the verification submission");
    } catch (...) {
        return make_module_error(ErrorCode::Internal, "verification consumption failed");
    }
}

} // namespace mira
