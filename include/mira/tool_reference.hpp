#pragma once

#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/workflow_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Tool stable references and compatibility projection (M7 TR0; DEC-040).
//
// A Workflow can outlive the tools it calls: schemas evolve, versions retire,
// MCP servers go away. This header freezes the stable logical reference a
// workflow asset stores (design §4), the extraction of the per-version
// reference manifest (§5), the deterministic resolution matrix and the
// Runnable/Degraded/Invalid compatibility projection (§6/§7), and the
// admission decision with its Degraded audit artifact (§8). Everything here
// is pure computation over caller-supplied data: no I/O, no clock, no
// randomness, so manifests, projections and digests reproduce byte-for-byte
// across processes.
//
// The reference space is the flat wire namespace shared by every tool origin
// (BuiltIn registry specs and module members both surface as ExposedToolSpec,
// so the caller folds whichever view it owns into one span). Pinning is
// content-addressed (the member spec digest); follow-latest resolves to
// whatever is current when the projection runs.
//
// Explicitly not here: any change to the DEC-019 IR v1 schema or to the
// execution path. The projection is an admission-time pre-check; DEC-015
// execution-time identity verification still gates every dispatch, and a
// workflow admitted as Degraded gets no exemption (design §9). Skill publish
// lifecycle and the Procedure index projection are TR1 (design §15).
// ---------------------------------------------------------------------------

// Reference string scheme (design §4.1): "toolref:<wire-name>" follows the
// latest available version; "toolref:<wire-name>@<64 lowercase hex>" pins the
// member spec digest. Additive evolution only: future pin forms must keep
// these two shapes valid and let older readers fail closed on the rest.
inline constexpr std::string_view kToolReferenceScheme = "toolref:";

// Versioned schemas (DEC-002 additive evolution).
inline constexpr std::string_view kWorkflowToolRefsSchema = "mira.workflow.tool_refs.v1";
inline constexpr std::string_view kWorkflowToolCompatSchema = "mira.workflow.tool_compat.v1";

enum class ToolReferenceMode : std::uint8_t { PinnedDigest, FollowLatest };

[[nodiscard]] std::string_view tool_reference_mode_name(ToolReferenceMode mode);
[[nodiscard]] Result<ToolReferenceMode> parse_tool_reference_mode(std::string_view name);

// One stable reference. `wire_name` uses the governed vocabulary charset (the
// module member charset), so BuiltIn, HostProvided and OutOfProcess tools
// share one namespace. `pinned_spec_digest` is set exactly when the mode is
// PinnedDigest.
struct ToolReference final {
    std::string wire_name;
    ToolReferenceMode mode = ToolReferenceMode::PinnedDigest;
    std::optional<Hash> pinned_spec_digest;

    friend bool operator==(const ToolReference &, const ToolReference &) = default;
};

// Strict parser (design §4.2): wrong or missing scheme, vocabulary charset
// violations, a digest part that is not exactly 64 lowercase hex characters,
// embedded whitespace, trailing characters and over-length input all fail
// closed with the mira.tool_reference domain.
[[nodiscard]] Result<ToolReference> parse_tool_reference(std::string_view text);

// The unique canonical string form (pinned digests lowercase hex); the
// round trip parse(to_string(r)) == r always holds for a parsed reference.
[[nodiscard]] std::string tool_reference_to_string(const ToolReference &reference);

// RULE-08 bounds: extraction, manifests and projections are capped so hostile
// or sloppy inputs are rejected, not absorbed. Defaults align with the IR
// step budget and the manifest name bound.
struct ToolReferenceLimits final {
    std::size_t max_reference_bytes = 256;
    std::size_t max_wire_name_bytes = 128;
    std::size_t max_entries = 256;      // references per workflow (IR max_steps).
    std::size_t max_detail_bytes = 256; // per-entry compat detail.
};

inline constexpr ToolReferenceLimits kDefaultToolReferenceLimits{};

// Extraction policy (design §5.2): the default mode applies to every
// referenced tool unless overridden per wire name. Duplicate or empty
// override names fail closed.
struct ToolRefExtractionOptions final {
    ToolReferenceMode default_mode = ToolReferenceMode::PinnedDigest;
    std::vector<std::pair<std::string, ToolReferenceMode>> per_tool_modes;
};

// One extracted reference, keyed by the ToolCall step that carries it. For
// pinned entries `pinned_spec_digest` records the digest observed in the
// extraction view — the publish-time snapshot the workflow is pinned to.
struct WorkflowToolRefEntry final {
    std::string step_id;
    std::string wire_name;
    ToolReferenceMode mode = ToolReferenceMode::PinnedDigest;
    std::optional<Hash> pinned_spec_digest; // set iff mode == PinnedDigest.

    friend bool operator==(const WorkflowToolRefEntry &, const WorkflowToolRefEntry &) = default;
};

// The per-version reference manifest (schema kWorkflowToolRefsSchema): the
// workflow identity plus the definition digest it was extracted from, the
// sorted entries and the canonical manifest digest. A projection and the
// stored artifact are projections (RULE-07); how the manifest is stored with
// the version record is the consumer's concern (TR1 wiring).
struct WorkflowToolRefManifest final {
    WorkflowId workflow_id;
    Sha256Digest definition_digest{};
    std::vector<WorkflowToolRefEntry> entries; // sorted by step_id.
    Hash digest{};

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
};

// Extracts the reference manifest from a ToolCall-bearing definition against
// the exposure view of the moment. Fail closed: the definition must pass the
// IR structural validation; every arguments["tool"] must be a non-empty
// vocabulary-charset string; every referenced wire name must resolve in the
// view (publishing a workflow that calls an unknown tool is rejected, never
// absorbed); pinned entries record the observed spec digest; duplicate wire
// names in the view are rejected (defense in depth behind the DEC-009
// gates). A definition without ToolCall steps yields a valid empty manifest
// with a defined digest.
[[nodiscard]] Result<WorkflowToolRefManifest>
extract_workflow_tool_references(const WorkflowDefinition &definition,
                                 std::span<const ExposedToolSpec> view,
                                 const ToolRefExtractionOptions &options = {},
                                 const ToolReferenceLimits &limits = kDefaultToolReferenceLimits);

// Strict round trip (schema kWorkflowToolRefsSchema): unknown fields, wrong
// schema, digest mismatch between content and stored digest fail closed.
[[nodiscard]] JsonValue workflow_tool_refs_to_json(const WorkflowToolRefManifest &manifest);
[[nodiscard]] Result<WorkflowToolRefManifest> workflow_tool_refs_from_json(const JsonValue &json);

// Replay/audit binding: the manifest must belong to this workflow identity
// and this exact definition content. Anything else is an explicit error.
[[nodiscard]] Result<void> verify_workflow_tool_refs(const WorkflowToolRefManifest &manifest,
                                                     const WorkflowId &workflow_id,
                                                     const Sha256Digest &definition_digest);

// ---------------------------------------------------------------------------
// Compatibility projection (design §6/§7)
// ---------------------------------------------------------------------------

enum class ToolReferenceCompat : std::uint8_t {
    Resolved,            // follow-latest resolved, or pinned digest still matches.
    EvolvedCompatible,   // pinned digest differs but the recorded skeleton still binds.
    EvolvedIncompatible, // pinned digest differs and the skeleton no longer binds.
    Unresolved,          // the wire name is absent from the current view.
};

[[nodiscard]] std::string_view tool_reference_compat_name(ToolReferenceCompat status);

enum class WorkflowToolCompatState : std::uint8_t { Runnable, Degraded, Invalid };

[[nodiscard]] std::string_view workflow_tool_compat_state_name(WorkflowToolCompatState state);

struct ToolRefCompatEntry final {
    std::string step_id;
    std::string wire_name;
    ToolReferenceMode mode = ToolReferenceMode::PinnedDigest;
    ToolReferenceCompat status = ToolReferenceCompat::Unresolved;
    std::optional<Hash> pinned_spec_digest;  // echoed for pinned entries.
    std::optional<Hash> current_spec_digest; // set when the wire name resolves.
    std::string detail;                      // bounded, sanitized; set for EvolvedIncompatible.

    friend bool operator==(const ToolRefCompatEntry &, const ToolRefCompatEntry &) = default;
};

// Deterministic recomputation of one workflow's tool compatibility (RULE-07:
// a projection over the manifest and the current view, never a stored fact).
struct WorkflowToolCompatProjection final {
    WorkflowId workflow_id;
    Sha256Digest definition_digest{};
    WorkflowToolCompatState state = WorkflowToolCompatState::Invalid;
    std::vector<ToolRefCompatEntry> entries; // sorted by step_id.
    Hash digest{};

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
};

// Recomputes the compatibility projection (design §6/§7). Fail closed on
// mismatched inputs: the manifest must carry this workflow_id and this
// definition's content digest, the definition must pass structural
// validation and the view must not contain duplicate wire names. Any
// Unresolved or EvolvedIncompatible entry makes the workflow Invalid; a lone
// EvolvedCompatible makes it Degraded; otherwise Runnable. Same inputs always
// produce the same projection and digest (no clock, no randomness).
//
// Skeleton bindability (design §7.2): for a pinned digest mismatch, the
// step's recorded arguments (minus the reserved "tool" member) are validated
// against the current parameters_schema after replacing every
// {"$param": name} node with a minimal instance materialized from the schema
// at its position. The verdict comes from the same strict M3 schema validator
// the dispatch path uses, so admission and execution cannot drift. Positions
// whose schema cannot be deterministically satisfied (e.g. pattern-only
// constraints) materialize a minimal value and fail conservatively.
[[nodiscard]] Result<WorkflowToolCompatProjection> project_workflow_tool_compatibility(
    const WorkflowDefinition &definition, const WorkflowToolRefManifest &refs,
    std::span<const ExposedToolSpec> view,
    const ToolReferenceLimits &limits = kDefaultToolReferenceLimits);

// Admission decision (design §8): a total function over a valid projection.
// Invalid is rejected with a deterministic reason naming the first offending
// entry; Degraded is admitted and the caller is expected to persist
// workflow_tool_compat_to_json(projection) as the audit trail; Runnable is
// admitted without an audit requirement. This function emits nothing and
// decides nothing about execution-time authority.
struct WorkflowToolCompatDecision final {
    bool admitted = false;
    WorkflowToolCompatState state = WorkflowToolCompatState::Invalid;
    std::string reason; // bounded, sanitized; set when rejected.
};

[[nodiscard]] WorkflowToolCompatDecision
admit_workflow_run_by_tool_compat(const WorkflowToolCompatProjection &projection);

// Versioned audit projection (schema kWorkflowToolCompatSchema): identities,
// digests, statuses and bounded detail only — never schema bodies, tool
// descriptions or secrets; safe for events and replay records.
[[nodiscard]] JsonValue
workflow_tool_compat_to_json(const WorkflowToolCompatProjection &projection);

} // namespace mira
