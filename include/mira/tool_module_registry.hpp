#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/tool_module.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// ---------------------------------------------------------------------------
// Tool module registry lifecycle (M7 TM1; DEC-009, DEC-042).
//
// TM1 owns everything between "a manifest parsed" and "an Active snapshot
// exists": origin trust verification (fail closed into Quarantined), the
// module state machine, immutable registry snapshots, the revoke tombstone,
// versioned lifecycle events, and the negotiation trigger hooks that turn
// session establishment, environment changes and module state changes into
// new negotiation generations (tool module design §5.3/§6/§7.1/§10/§13).
//
// Ownership and threading: the registry and the negotiation coordinator are
// serial-control-plane components (DEC-001). Every method must be called
// from the serial control plane or under external synchronization; neither
// component spawns threads, performs I/O or reads the clock outside the
// event timestamps stamped by the caller-supplied store. The deploy-time
// verification path is the one Executor-routed piece: submit_module_
// verification() runs the bounded pure trust check as a submit_auto() task
// whose future must be consumed.
//
// Explicitly not in TM1: the LLM exposure projection and ToolId assignment
// (TM2), real cryptographic primitives (the OutOfProcess verifier is
// host-injected; Core ships a deterministic reference binding check), and
// persistence of registry state across processes (tombstones and trust
// material are deploy-time inputs; events remain the audit record).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Origin trust (tool module design §5.3 rule 1, §12; DEC-009 v1 freeze)
// ---------------------------------------------------------------------------

// Host-injected signature verification for OutOfProcess modules. Implementations
// authenticate `signature` under `algorithm` for `signer` over the unsigned
// canonical manifest digest and must be deterministic and side-effect free.
// When registration verification also runs on the Executor, implementations
// must be safe for concurrent use. Core deliberately ships no cryptography:
// the reference implementation used by tests is a deterministic binding check,
// real signature verification is a deploy/host responsibility.
class IModuleSignatureVerifier {
  public:
    virtual ~IModuleSignatureVerifier() = default;

    [[nodiscard]] virtual bool verify(std::string_view signer, std::string_view algorithm,
                                      std::string_view signature, const Hash &signed_digest) = 0;
};

// Deploy-time trust material. Frozen v1 semantics (DEC-009, upgraded from the
// tentative default at M7-TM1-02): BuiltIn modules trust the build-pinned
// digest set; HostProvided modules trust explicit host injection recorded in
// the module_id allowlist; OutOfProcess packages trust a declared signer from
// the trusted set plus a successful verifier decision over the unsigned
// manifest digest. Admission is not authorization: capability grants, policy
// and confirmation semantics are unchanged.
struct ModuleTrustConfig final {
    // Build-pinned digests for BuiltIn modules (full manifest digests).
    std::vector<Hash> builtin_digests;
    // module_ids the host deployment pipeline explicitly injected.
    std::vector<std::string> host_provided_allowlist;
    // Trusted OutOfProcess signers; signatures are checked by `verifier`.
    std::vector<std::string> trusted_out_of_process_signers;
    // Required for any OutOfProcess module; absent verifier fails closed.
    IModuleSignatureVerifier *verifier = nullptr;
    // Tombstones seeded from a previous registry lifetime (revoked digests);
    // runtime revocations append to this set.
    std::vector<Hash> revoked_digests;

    [[nodiscard]] Result<void> validate() const;
};

// Outcome of origin trust verification for one manifest.
struct ModuleTrustReport final {
    std::string module_id;
    SemanticVersion version{};
    Hash manifest_digest{};
    ToolModuleOrigin origin = ToolModuleOrigin::BuiltIn;
    bool trusted = false;
    std::string reason; // bounded, sanitized; always set when !trusted.
};

// Pure origin trust verification: no I/O, no clock, no registry state. The
// same input always produces the same verdict and reason. Fail-closed matrix
// (design §5.3/§11/§12): BuiltIn requires the manifest digest in the pinned
// set; HostProvided requires the module_id in the allowlist; OutOfProcess
// requires a non-empty signer from the trusted set, a non-empty algorithm and
// signature, an available verifier, and an affirmative verifier decision over
// the unsigned manifest digest. Any failure returns a report with
// trusted=false and a machine-readable reason. Verifier implementations must
// not throw; a throwing verifier is a host defect whose exception propagates
// so every consumption site converts it to an explicit fail-closed result.
[[nodiscard]] ModuleTrustReport verify_module_trust(const ToolModuleManifest &manifest,
                                                    const ModuleTrustConfig &config);

// ---------------------------------------------------------------------------
// Module state machine (tool module design §6)
// ---------------------------------------------------------------------------

enum class ModuleState : std::uint8_t {
    Discovered, // accepted for verification; transient inside register_module
    Verified,   // structure and origin verified; no run entitlement
    Staged,     // usable by harness/shadow validation; not in production views
    Active,     // participates in negotiation; members may enter views
    Deprecated, // no new bindings; in-flight invocations settle normally
    Revoked,    // terminal; digest tombstoned, re-admission requires a new digest
    Quarantined // terminal; verification failed, no partial state escaped
};

[[nodiscard]] std::string_view module_state_name(ModuleState state);
[[nodiscard]] Result<ModuleState> parse_module_state(std::string_view name);

// Read-only view of one registered module (identity, digest, state, last
// transition reason). Owned by the registry; pointers stay stable.
struct ModuleRecord final {
    ToolModuleManifest manifest;
    Hash manifest_digest{};
    ModuleState state = ModuleState::Discovered;
    std::string last_reason; // bounded, sanitized
};

// Immutable value the registry publishes: the Active module set at one
// generation. Copies are independent; state changes only affect the next
// active_snapshot() read, so in-flight consumers keep settling against the
// generation they pinned (design §6, §7.1).
struct RegistrySnapshot final {
    std::uint64_t generation = 0;        // 0 until the first module activates
    std::vector<ModuleSnapshot> modules; // Active modules, sorted by module_id
    Hash digest{};                       // canonical digest over generation + module digests
};

struct ModuleRegistryStats final {
    std::uint64_t discovered = 0;
    std::uint64_t registered = 0;  // reached Verified
    std::uint64_t quarantined = 0; // verification failures
    std::uint64_t activated = 0;
    std::uint64_t deprecated = 0;
    std::uint64_t revoked = 0;
    std::uint64_t rejected_transitions = 0; // illegal transition or unknown id
    std::uint64_t rejected_closed = 0;      // any mutation after close()
    std::uint64_t tombstone_hits = 0;       // re-registration of a tombstoned digest
    std::uint64_t events_emitted = 0;
    std::uint64_t event_sink_failures = 0;
    std::size_t tombstones = 0;
};

struct ModuleRegistryCloseReport final {
    std::uint64_t snapshot_generation = 0;
    std::size_t active_modules = 0;
    std::size_t tombstones = 0;
    std::uint64_t events_emitted = 0;
    std::uint64_t event_sink_failures = 0;
};

// Lifecycle event payload schema (additive-evolution, DEC-002): one versioned
// JSON object per state transition or admission rejection. `kind` is one of
// "discovered", "verified", "staged", "activated", "deprecated", "revoked",
// "quarantined", "registration_rejected", "activation_rejected". Payloads
// carry module identity, version, digest and origin but never the signature
// text or any secret.
inline constexpr std::string_view kModuleLifecycleEventSchema = "mira.tool_module.lifecycle.v1";

// Registry admission gate and state machine. Registration happens only in
// the init/deploy window (before seal()); afterwards only downgrade
// transitions are accepted; after close() every mutation is rejected while
// active_snapshot() stays readable for in-flight settlement. A module_id can
// be registered at most once per registry lifetime; version upgrades are a
// new deploy cycle. Revoked digests enter the tombstone and block later
// registration of any manifest with the same digest.
class ModuleRegistry final {
  public:
    // The catalog and trust config are copied; the event sink (optional) and
    // ids stamp lifecycle events. The sink is never owned and must outlive
    // the registry; nil ids or a null sink disable event emission.
    ModuleRegistry(CapabilityCatalog catalog, ModuleTrustConfig trust = {},
                   IEventStore *event_sink = nullptr, RuntimeId runtime_id = RuntimeId{},
                   SessionId session_id = SessionId{});
    ~ModuleRegistry();

    ModuleRegistry(const ModuleRegistry &) = delete;
    ModuleRegistry &operator=(const ModuleRegistry &) = delete;

    // Admission (init/deploy window only): verifies origin trust against the
    // configured material and moves the module Discovered -> Verified, or
    // Discovered -> Quarantined when verification fails (record retained for
    // diagnostics, no partial state). Rejected with InvalidState once sealed
    // or closed, with AlreadyExists for a known module_id, and with
    // InvalidState when the manifest digest is tombstoned.
    [[nodiscard]] Result<void> register_module(ToolModuleManifest manifest);

    // Executor-precomputed variant: accepts a trust report produced by
    // submit_module_verification() for this exact manifest. Fails closed on
    // any binding mismatch (module_id, version, digest, origin) or an
    // untrusted report; a mismatch rejects the registration outright
    // (InvalidArgument, no record) because it is a caller error, not a module
    // defect.
    [[nodiscard]] Result<void> register_module(ToolModuleManifest manifest,
                                               ModuleTrustReport precomputed);

    [[nodiscard]] Result<void> stage_module(std::string_view module_id);
    // Staged -> Active. Fails closed (module stays Staged) when any member
    // tool name collides with a member of an already-Active module: the later
    // activation is rejected and the incumbent is untouched (design §5.3
    // rule 3, §11).
    [[nodiscard]] Result<void> activate_module(std::string_view module_id);
    // Downgrade transitions, valid until close(): Active -> Deprecated and
    // {Active, Staged, Deprecated} -> Revoked (digest enters the tombstone).
    [[nodiscard]] Result<void> deprecate_module(std::string_view module_id,
                                                std::string reason = {});
    [[nodiscard]] Result<void> revoke_module(std::string_view module_id, std::string reason = {});

    // Ends the init/deploy window: no further registrations or promotions,
    // downgrade transitions remain available until close().
    [[nodiscard]] Result<void> seal();

    // Terminal close (design §10 step 5): rejects every mutation afterwards;
    // active_snapshot() stays readable so in-flight generations can settle.
    ModuleRegistryCloseReport close();

    [[nodiscard]] bool sealed() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] RegistrySnapshot active_snapshot() const;
    // Pointer stays stable for the registry lifetime; null when unknown.
    [[nodiscard]] const ModuleRecord *find(std::string_view module_id) const;
    [[nodiscard]] bool is_tombstoned(const Hash &digest) const;
    [[nodiscard]] ModuleRegistryStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Negotiation trigger hooks (tool module design §7.1)
// ---------------------------------------------------------------------------

enum class NegotiationTrigger : std::uint8_t {
    SessionEstablished,
    EnvironmentChanged,
    ModuleStatesChanged
};

[[nodiscard]] std::string_view negotiation_trigger_name(NegotiationTrigger trigger);

// One negotiation generation: the trigger that produced it, the environment
// epoch it negotiated against, the pure negotiation result and a canonical
// view digest. Value semantics: consumers pin a view and settle against it
// regardless of later generations; new consumers read current().
struct NegotiationView final {
    std::uint64_t generation = 0; // monotonic, starts at 1
    EnvironmentEpoch epoch = 0;
    NegotiationTrigger trigger = NegotiationTrigger::SessionEstablished;
    ModuleNegotiationResult negotiation;
    Hash view_digest{}; // canonical over generation, epoch, trigger, result
};

// Negotiation event payload schema: {"schema": ..., "trigger": ..., "epoch":
// ..., "generation": ..., "view_digest": ..., "registry_generation": ...,
// "negotiation": <module_negotiation_to_json projection>}.
inline constexpr std::string_view kModuleNegotiationEventSchema = "mira.tool_module.negotiation.v1";

// Turns the three design §7.1 triggers into new NegotiationView generations
// over the registry's current Active snapshot, using the TM0 pure
// negotiate_modules(). The coordinator is the serial-plane negotiation
// ledger: sessions establish once, environment epochs move forward only
// (stale epochs rejected, same-epoch re-declarations are idempotent no-ops,
// capability changes require an epoch bump), and module state changes
// renegotiate against the pinned environment. Triggers are rejected while
// the registry is closed (already-produced views keep settling: current()
// stays readable). Every produced view appends a ModuleNegotiationDecided
// event when a sink and ids were provided.
class ModuleNegotiationCoordinator final {
  public:
    ModuleNegotiationCoordinator(CapabilityCatalog catalog, IEventStore *event_sink = nullptr,
                                 RuntimeId runtime_id = RuntimeId{},
                                 SessionId session_id = SessionId{});

    // First trigger: binds the session environment. Fails with InvalidState
    // when a session is already established and InvalidArgument on a zero
    // epoch.
    [[nodiscard]] Result<NegotiationView>
    on_session_established(EnvironmentEpoch epoch, const EnvironmentCapabilities &capabilities,
                           const ModuleRegistry &registry);

    // Environment re-declaration: epoch must be >= the bound epoch. Equal
    // epochs with identical capabilities return the current view unchanged
    // (idempotent no-op, no event); equal epochs with changed capabilities
    // fail closed (InvalidArgument); newer epochs renegotiate.
    [[nodiscard]] Result<NegotiationView>
    on_environment_changed(EnvironmentEpoch epoch, const EnvironmentCapabilities &capabilities,
                           const ModuleRegistry &registry);

    // Module state change: renegotiates against the bound environment. Fails
    // with InvalidState before a session is established.
    [[nodiscard]] Result<NegotiationView> on_modules_changed(const ModuleRegistry &registry);

    // The current generation, or null before the first trigger.
    [[nodiscard]] const NegotiationView *current() const noexcept;

  private:
    NegotiationView build_view_(NegotiationTrigger trigger, const ModuleRegistry &registry);

    CapabilityCatalog catalog_;
    IEventStore *event_sink_ = nullptr;
    RuntimeId runtime_id_;
    SessionId session_id_;
    std::optional<EnvironmentCapabilities> capabilities_;
    EnvironmentEpoch epoch_ = 0;
    std::optional<NegotiationView> current_;
    std::uint64_t next_generation_ = 1;
};

// ---------------------------------------------------------------------------
// Executor-routed deploy verification (tool module design §10)
// ---------------------------------------------------------------------------

// Cooperative cancellation probe for the deploy path: a minimal stand-in for
// std::stop_token (the Android NDK libc++ does not ship <stop_token>; public
// headers must build on every supported platform). A default-constructed
// token never reports stop.
class ModuleDeployToken final {
  public:
    ModuleDeployToken() noexcept = default;
    explicit ModuleDeployToken(std::shared_ptr<const std::atomic<bool>> flag) noexcept
        : flag_(std::move(flag)) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<const std::atomic<bool>> flag_;
};

// Routes manifest trust verification onto the Executor as a bounded finite
// task (submit_auto(); M7 §6, design §10). The manifest and trust config are
// moved into the task; the verifier, if any, must stay alive until the future
// resolves. Two rejection surfaces, both explicit:
//
// - Synchronous submission rejections return an error result instead of a
//   future (Unavailable when the executor is stopped or not accepting work,
//   ResourceExhausted when this facade throws capacity exhaustion upfront).
// - Admission rejections the Executor delivers by making the future ready
//   with an exception (this facade's capacity-exhaustion path) never escape:
//   consume the future through consume_module_verification(), which folds
//   every outcome — report, Cancelled, task exception, admission rejection —
//   into one Result. Futures must be consumed; nothing is silently dropped.
[[nodiscard]] Result<std::future<Result<ModuleTrustReport>>>
submit_module_verification(executor::Executor &executor, ToolModuleManifest manifest,
                           ModuleTrustConfig trust, ModuleDeployToken cancellation = {});

// The single well-defined consumption point for a verification future:
// resolves with the task's Result, or converts a submission-rejection
// exception carried by the future (capacity exhausted, executor stopping or
// not accepting work) into an explicit error result. Never throws, never
// swallows: every failure mode maps to a Result the caller must inspect.
[[nodiscard]] Result<ModuleTrustReport>
consume_module_verification(std::future<Result<ModuleTrustReport>> future);

} // namespace mira
