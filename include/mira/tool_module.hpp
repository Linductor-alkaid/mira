#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/security.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Tool module contracts (M7 TM0; DEC-009, DEC-039, DEC-042).
//
// A ToolModule is the only unit through which tools enter the Runtime: a
// manifest aggregates member tool specs, capability requirements, conflict
// declarations and aggregate resource limits under one versioned identity.
// This header freezes the TM0 stage: the governed capability catalog, the
// pure `EnvironmentCapabilities -> env.*` derivation, manifest parsing with
// fail-closed validation, and the deterministic availability negotiation.
// Everything here is pure computation over caller-supplied data: no I/O, no
// clock, no randomness, so negotiation results and digests are reproducible
// byte-for-byte across processes.
//
// Explicitly not in TM0: signature/allowlist verification and the registry
// state machine (TM1), the LLM exposure projection and ToolId assignment
// (TM2). Manifest signature fields are carried as opaque strings until TM1;
// module member identity in TM0 is the module-unique tool name that joins
// the wire namespace, and cross-module name uniqueness is the negotiation
// gate (ToolId-level uniqueness lands with TM2 identity assignment).
// ---------------------------------------------------------------------------

// Kind of a governed capability. Boolean capabilities are either provided or
// missing; Counted capabilities are satisfied when the environment provides
// at least one unit (the v1 vocabulary declares minimums, never ranges).
enum class CapabilityKind : std::uint8_t { Boolean, Counted };

struct CapabilityDescriptor final {
    CapabilityId id; // lowercase dot-separated vocabulary, e.g. "env.screen.capture".
    CapabilityKind kind = CapabilityKind::Boolean;
    std::string summary; // single line, redaction-safe.
};

// Governed capability vocabulary. Manifests may only reference capabilities
// present in a catalog; anything else fails closed at validation time. The
// Core-shipped vocabulary is core_capability_catalog(); tests and hosts may
// build explicit catalogs through make(), which rejects duplicates and
// malformed ids so a catalog is always a valid governance artifact.
class CapabilityCatalog final {
  public:
    CapabilityCatalog() = default;

    // Fails closed on duplicate ids or ids outside the governed charset.
    [[nodiscard]] static Result<CapabilityCatalog> make(std::vector<CapabilityDescriptor> entries);

    // The vocabulary shipped with Core: the env.* derivation targets from the
    // tool module design plus the documented host.*/tool.* infrastructure
    // capabilities. Extension requires design review; modules can never
    // invent vocabulary.
    [[nodiscard]] static const CapabilityCatalog &core();

    [[nodiscard]] const CapabilityDescriptor *find(std::string_view id) const;
    [[nodiscard]] bool contains(std::string_view id) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<CapabilityDescriptor> &entries() const noexcept {
        return entries_;
    }

    // Canonical digest over the sorted entry set; stable across processes.
    [[nodiscard]] Hash digest() const noexcept { return digest_; }

  private:
    CapabilityCatalog(std::vector<CapabilityDescriptor> entries, Hash digest) noexcept
        : entries_(std::move(entries)), digest_(digest) {}

    std::vector<CapabilityDescriptor> entries_; // sorted by id, unique.
    Hash digest_{};
};

// Derives the environment-side capability set from an honest capability
// declaration. Pure function; the mapping is fixed by the tool module design
// and quality-only fields (e.g. max_component_skew) never become vocabulary.
[[nodiscard]] std::vector<CapabilityId>
derive_environment_capabilities(const EnvironmentCapabilities &capabilities);

// ---------------------------------------------------------------------------
// SemanticVersion JSON helpers (object form {major, minor, patch}, matching
// the model contract wire format).
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue semantic_version_to_json(const SemanticVersion &version);
[[nodiscard]] Result<SemanticVersion> semantic_version_from_json(const JsonValue &value);

// ---------------------------------------------------------------------------
// ToolModule manifest ("mira.tool_module.manifest.v1")
// ---------------------------------------------------------------------------

inline constexpr std::string_view kToolModuleManifestSchema = "mira.tool_module.manifest.v1";
// Additive-evolution version of the manifest schema (DEC-002); readers fail
// closed on anything other than the frozen major.
inline constexpr std::string_view kToolModuleManifestSchemaVersion = "1.0";

// Runtime-side tool module ABI. Manifests declaring a higher
// min_mira_module_abi are rejected; the ABI only moves with a reviewed
// contract change.
inline constexpr std::uint32_t kCurrentToolModuleAbi = 1;

enum class ToolModuleOrigin : std::uint8_t { BuiltIn, HostProvided, OutOfProcess };

[[nodiscard]] std::string_view tool_module_origin_name(ToolModuleOrigin origin);
[[nodiscard]] std::optional<ToolModuleOrigin> tool_module_origin_from_name(std::string_view name);

// Resource labels a member tool reads or writes; policy/display metadata
// only, never a substitute for per-call data access checks.
struct ToolDataAccess final {
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

// One member tool as declared by the manifest. `name` is unique within the
// module and joins the cross-module wire namespace. `side_effect` reuses the
// governed ActionRisk tiers; JSON spellings are "read_only", "reversible_low",
// "user_visible", "sensitive" and "critical".
struct ModuleToolSpec final {
    std::string name;
    SemanticVersion version;
    std::string description;
    JsonSchema arguments_schema;
    JsonSchema result_schema;
    std::vector<CapabilityId> required_capabilities;
    ActionRisk side_effect = ActionRisk::R0ReadOnly;
    ToolDataAccess data_access;
};

// Aggregate module budgets, enforced on top of per-tool limits so one module
// cannot exhaust the global quota. Zero is invalid: manifests must declare
// explicit, bounded resources.
struct ModuleResourceLimits final {
    std::uint32_t max_total_concurrent_invocations = 0;
    std::uint64_t max_total_result_bytes = 0;
};

struct ToolModuleManifest final {
    std::string module_id;
    SemanticVersion version;
    ToolModuleOrigin origin = ToolModuleOrigin::BuiltIn;
    std::string signer;              // opaque until TM1 trust verification.
    std::string signature_algorithm; // opaque until TM1 trust verification.
    std::string signature;           // opaque until TM1 trust verification.
    std::uint32_t min_mira_module_abi = 1;
    std::vector<CapabilityId> required_capabilities;
    std::vector<CapabilityId> infra_capabilities;
    std::vector<ModuleToolSpec> tools;
    std::vector<std::string> conflicts_with;
    ModuleResourceLimits resources;
};

// Validation bounds (RULE-08): every list, string and budget in a manifest is
// capped so a hostile or sloppy manifest is rejected, not absorbed. These are
// validation constants, not runtime execution budgets.
struct ToolModuleLimits final {
    std::size_t max_manifest_bytes = 256 * 1024;   // canonical serialization size.
    std::size_t max_module_id_bytes = 128;
    std::size_t max_name_bytes = 128;
    std::size_t max_description_bytes = 4 * 1024;
    std::size_t max_signer_bytes = 256;
    std::size_t max_signature_bytes = 4 * 1024;
    std::size_t max_label_bytes = 256;             // data access entries, conflict ids.
    std::size_t max_tools = 256;
    std::size_t max_capabilities = 64;             // per capability list.
    std::size_t max_conflicts = 64;
    std::size_t max_data_access_entries = 64;      // per direction.
    std::uint32_t max_concurrent_invocations = 1024;
    std::uint64_t max_result_bytes = 64ull * 1024 * 1024;
};

inline constexpr ToolModuleLimits kDefaultToolModuleLimits{};

// Parses and validates one manifest. Fail-closed: any violation (schema
// version, id charset, duplicate member names, unknown capabilities, member
// schemas outside the shared JSON Schema subset, missing or out-of-range
// resources, ABI above the runtime ABI, origin trust fields) rejects the
// whole manifest with a single error; no partial state escapes. Signature
// values are structurally bounded but only verified from TM1 on.
[[nodiscard]] Result<ToolModuleManifest>
parse_tool_module_manifest(const JsonValue &json, const CapabilityCatalog &catalog,
                           const ToolModuleLimits &limits = kDefaultToolModuleLimits);

// Canonical digest over the validated manifest structure (not the raw input
// bytes), so semantically identical manifests digest identically regardless
// of input key order or whitespace. The digest covers the signature fields as
// opaque strings; it is the module identity replay and events refer to.
[[nodiscard]] Hash tool_module_manifest_digest(const ToolModuleManifest &manifest);

// ---------------------------------------------------------------------------
// Availability negotiation (pure function; tool module design §7)
// ---------------------------------------------------------------------------

// The negotiation input for one module: the registry's Active snapshot.
// `required` is the union of module-level, infrastructure and all member
// capability requirements.
struct ModuleSnapshot final {
    std::string module_id;
    SemanticVersion version;
    Hash module_digest{};
    std::vector<ModuleToolSpec> tools;
    std::vector<CapabilityId> required;
    std::vector<std::string> conflicts_with;
};

// Builds the negotiation snapshot from a validated manifest (required =
// module ∪ infra ∪ member capabilities).
[[nodiscard]] ModuleSnapshot module_snapshot_from_manifest(const ToolModuleManifest &manifest);

// Negotiation outcome for one module. Revoked is reserved for the TM1
// registry tombstone projection; TM0 negotiation over Active snapshots never
// emits it.
enum class ModuleStatus : std::uint8_t { Available, Unavailable, Conflict, Revoked };

[[nodiscard]] std::string_view module_status_name(ModuleStatus status);

struct ModuleAvailability final {
    std::string module_id;
    SemanticVersion version;
    Hash module_digest{};
    ModuleStatus status = ModuleStatus::Unavailable;
    std::vector<CapabilityId> missing; // sorted; set for Unavailable.
    std::string conflicting_with;      // set for Conflict.
};

struct ModuleNegotiationResult final {
    std::vector<ModuleAvailability> modules; // sorted by module_id.
    Hash digest{};                           // canonical digest of the projection below.
};

// Deterministic negotiation: Active snapshots × environment × catalog.
// Fail-closed invariants (tool module design §7.3): a module with any
// missing or catalog-unknown capability is Unavailable with the complete
// sorted missing list; two active modules that declare each other in
// conflicts_with, or that expose the same member tool name into the wire
// namespace, are both Conflict (no partial member availability, no
// arbitration). Unknown capabilities are missing by definition. The output
// is sorted by module_id and independent of input order; the same inputs
// always produce the same result and digest. No I/O, no clock, no
// randomness.
[[nodiscard]] ModuleNegotiationResult
negotiate_modules(std::span<const ModuleSnapshot> active,
                  const EnvironmentCapabilities &environment, const CapabilityCatalog &catalog);

// Canonical JSON projection of a negotiation result (module digest/version,
// status, missing list, conflict partner); the digest input for reproducible
// tests, events and replay.
[[nodiscard]] JsonValue module_negotiation_to_json(const ModuleNegotiationResult &result);

} // namespace mira
