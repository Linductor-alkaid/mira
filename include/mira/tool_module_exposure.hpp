#pragma once

#include <mira/model_contracts.hpp>
#include <mira/tool_module.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Tool module LLM exposure projection (M7 TM2; DEC-009, DEC-042).
//
// TM2 owns the last hop of the tool module pipeline (tool module design §8):
// a pinned negotiation generation over an Active registry snapshot becomes the
// per-request `ExposedToolSpec` set the model actually sees. The projection
// is the ToolRegistry "view" — a pure function over caller-supplied data, so
// the same (generation, snapshot, negotiation, request-policy) inputs always
// produce the same tools, exclusions and snapshot digest byte-for-byte across
// processes. It never performs I/O, reads the clock or generates randomness.
//
// Identity assignment (deferred from TM0): a member's ToolId is *derived* from
// (module_id, module_digest, member name), never generated randomly, so the
// same manifest always yields the same ToolId in every process and session.
// Cross-module uniqueness is carried by the wire namespace gates — TM0
// negotiation Conflict and TM1 activation rejection — with a defensive
// fail-closed duplicate check here (M7-TM2-02 wire name rules, v1 frozen).
//
// Exclusions are recorded at two levels (design §8.1): module level (why a
// module's members are absent) and task level (why an otherwise available
// module was filtered for this request). The composite snapshot digest binds
// the negotiation generation, the included module digest set and the member
// (tool_id, spec_digest, wire_name) set — the value
// `PromptProvenance.tool_snapshot_digest` carries for module-sourced requests
// (DEC-002 additive evolution; the legacy field and the pure-tool digest
// helper are unchanged, and the empty-registry empty-allowlist path of M3
// stays valid).
//
// Explicitly not in TM2: member execution dispatch and aggregate resource
// enforcement (the existing call protocol, design §9), ModelPackage policy
// bindings (TM4), ContextManager's per-request assembly wiring, and any
// dynamic re-negotiation (the projection consumes an already-produced
// generation; producing generations is TM1's coordinator).
// ---------------------------------------------------------------------------

// Versioned exposure projection schema (additive-evolution, DEC-002): the
// canonical JSON `tool_exposure_to_json` emits and the snapshot digest binds.
inline constexpr std::string_view kToolExposureSchema = "mira.tool_module.exposure.v1";

// Deterministic member identity: the first 16 bytes of the canonical digest
// over (module_id, module_digest, member name). Same manifest -> same ToolId
// in every process; distinct modules can never collide on a ToolId because
// the module digest is part of the derivation.
[[nodiscard]] ToolId derive_module_tool_id(const std::string &module_id, const Hash &module_digest,
                                           std::string_view member_name);

// Canonical digest over one member's exposure projection (module identity +
// name, version, description, argument schema, side-effect tier, data
// access). This is the ToolSpec digest the snapshot digest and replay bind
// to; it is independent of the manifest's other members and key order.
[[nodiscard]] Hash module_tool_spec_digest(const std::string &module_id,
                                           const ModuleToolSpec &member);

// ---------------------------------------------------------------------------
// Exclusions (two levels; tool module design §8.1)
// ---------------------------------------------------------------------------

enum class ToolExclusionLevel : std::uint8_t { Module, Task };

enum class ToolExclusionReason : std::uint8_t {
    ModuleUnavailable, // negotiation verdict Unavailable; `missing` carries the list
    ModuleConflict,    // negotiation verdict Conflict; `conflicting_with` carries the partner
    ModuleRevoked,     // negotiation verdict Revoked
    ReservedWireName,  // a member collides with a hosted provider tool name
    TaskPolicy,        // the caller excluded the module for this request
    TaskBudget,        // the exposure budget was exhausted before the module fit
};

[[nodiscard]] std::string_view tool_exclusion_reason_name(ToolExclusionReason reason);

struct ToolExclusion final {
    ToolExclusionLevel level = ToolExclusionLevel::Module;
    ToolExclusionReason reason = ToolExclusionReason::ModuleUnavailable;
    std::string module_id;
    std::vector<CapabilityId> missing; // set for ModuleUnavailable.
    std::string conflicting_with;      // set for ModuleConflict.
    std::string detail;                // bounded, sanitized; always set.
};

// Task-level selection input for one request. Exclusions are module-grained:
// a module either contributes all of its members or none (design §7.3 —
// never partial member availability).
struct ToolExposureRequest final {
    // Explicit task policy exclusions: (module_id, bounded reason). Duplicate
    // module_ids or empty ids fail the projection closed.
    std::vector<std::pair<std::string, std::string>> excluded_modules;
    // Upper bound on exposed tools for this request (RULE-08). Modules are
    // considered in module_id order; a module whose member count does not fit
    // the remaining budget is excluded TaskBudget whole. Zero is legal and
    // excludes every module through the budget path.
    std::size_t max_exposed_tools = 64;
};

// Exposure bounds (RULE-08): the projection itself is bounded so a hostile or
// sloppy input set is rejected, not absorbed.
struct ToolExposureLimits final {
    std::size_t max_modules = 256;      // distinct modules in one projection.
    std::size_t max_tools = 512;        // members across included modules.
    std::size_t max_reason_bytes = 512; // task policy reason strings.
};

inline constexpr ToolExposureLimits kDefaultToolExposureLimits{};

// One included module's binding record: what the snapshot digest attests.
struct ExposureModuleBinding final {
    std::string module_id;
    SemanticVersion version;
    Hash module_digest{};
};

struct ToolExposure final {
    // The negotiation generation this projection came from; in-flight
    // requests keep settling against their pinned generation (design §8.4).
    std::uint64_t negotiation_generation = 0;
    std::vector<ExposedToolSpec> tools;         // sorted by wire_name
    std::vector<ExposureModuleBinding> modules; // included modules, sorted by module_id
    std::vector<ToolExclusion> exclusions;      // sorted by level, then module_id
    // Composite digest over generation + included module digests + member
    // (tool_id, spec_digest, wire_name) triples; same inputs -> same digest.
    Hash snapshot_digest{};

    [[nodiscard]] bool empty() const noexcept { return tools.empty(); }
};

// Projects one pinned negotiation generation into the per-request exposure.
//
// Consistency contract (fail closed, whole projection, no partial state):
// every module in `active_modules` must have exactly one verdict in
// `negotiation.modules` with a matching module_id and module_digest, and
// every verdict must belong to `active_modules`; duplicate module_ids, wire
// names or derived ToolIds among would-be-exposed members are rejected
// outright (defense in depth behind the TM0/TM1 gates). An empty Active set
// with an empty negotiation projects to an empty view with a defined digest
// (the M3 empty-allowlist behavior).
//
// Module-level outcomes mirror the negotiation verdicts; a member colliding
// with a hosted provider tool name excludes the whole module
// (ReservedWireName) — never a rename, never partial members. Task-level
// outcomes follow `request` (policy exclusions first, then the budget in
// module_id order).
[[nodiscard]] Result<ToolExposure> project_tool_exposure(
    std::uint64_t negotiation_generation, std::span<const ModuleSnapshot> active_modules,
    const ModuleNegotiationResult &negotiation, const ToolExposureRequest &request = {},
    const ToolExposureLimits &limits = kDefaultToolExposureLimits);

// Canonical JSON projection of an exposure (schema kToolExposureSchema):
// generation, snapshot digest, included module bindings, exposed tools
// (identity + spec digests, no schema bodies) and every exclusion with its
// level, reason and detail. Redaction-safe by construction; the digest input
// for reproducible tests, events and replay.
[[nodiscard]] JsonValue tool_exposure_to_json(const ToolExposure &exposure);

// Replay / audit binding (design §8.3): verifies that a recorded module
// digest set is exactly the set of module digests the exposure includes.
// Recorded sets that are missing, extra or reordered-unequal produce an
// explicit error, never a silent pass; replay never loads real modules.
[[nodiscard]] Result<void> verify_recorded_module_digests(std::span<const Hash> recorded,
                                                          const ToolExposure &exposure);

// ---------------------------------------------------------------------------
// Simulator reference module (tool module design §14)
// ---------------------------------------------------------------------------

// Reference BuiltIn module for the simulator host: the golden fixture for
// exposure and negotiation contract tests. `builtin.simulator.env` declares
// two members — `simulator.describe_display` (read_only, no capability
// requirements) and `simulator.inject_tap` (user_visible, requires
// env.input.discrete) — with bounded explicit resources. Built through the
// real manifest parser, so its canonical digest is a stable golden constant
// and the fixture itself can never drift from the validated contract; a
// parse failure is an explicit error (a Core fixture defect), never a
// partially filled manifest.
[[nodiscard]] Result<ToolModuleManifest> make_simulator_reference_module();

} // namespace mira
