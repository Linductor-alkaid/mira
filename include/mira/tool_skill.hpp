#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/tool_reference.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_versioning.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Skill publication lifecycle and the Procedure index projection (M7 TR1;
// DEC-040, tool reference & skill design §17).
//
// A Skill is "a Workflow exposed as a Tool" (DEC-040 §3.2): the descriptor
// pins the source WorkflowId plus the ir_digest of the exact validated
// version, and the Tool-channel surface (description, parameter schema, side
// effects) is derived deterministically from the source definition, the TR0
// reference manifest and the exposure view of the publish moment. Publication
// is a host-explicit action — compiled trajectories never become Skills on
// their own — and upgrades re-pin explicitly with a strictly increasing skill
// version, leaving a superseded-version trail. Revocation is downgrade-only.
//
// The Procedure index projection (design §17.3) turns publications into the
// canonical-JSON statements a memory wiring would store as
// MemoryRecord(kind=Procedure) — bounded by the host's explicit publish act,
// never an automatic index over the whole workflow library (DEC-029 rejected
// exactly that write face). The projection is timestamp-free: wall clocks
// belong to the consumer wiring, not to a pure recomputation.
//
// Ownership and threading: the registry is a serial-control-plane component
// (DEC-001); callers serialize access. It spawns no threads, performs no I/O
// and reads no clock; the optional event sink is never owned and sink
// failures only show up in stats.
//
// Explicitly not in TR1: any execution face. Skills never enter
// BuiltinToolRegistry, exposure or negotiation here, and the sub-workflow
// call adapter, the create_run admission consumption of the TR0 projection,
// the Degraded event emission and the IR reference-expression evolution are
// TR2 (design §15).
// ---------------------------------------------------------------------------

inline constexpr std::string_view kSkillDescriptorSchema = "mira.skill.descriptor.v1";
inline constexpr std::string_view kSkillPublicationEventSchema = "mira.skill.publication.v1";
inline constexpr std::string_view kSkillProcedureIndexSchema = "mira.skill.procedure_index.v1";

// RULE-08 bounds for descriptors, publications and the Procedure index.
struct SkillLimits final {
    std::size_t max_name_bytes = 128;
    std::size_t max_description_bytes = 2 * 1024; // summary, per property summary too.
    std::size_t max_reason_bytes = 256;           // revoke reason.
    std::size_t max_skills = 256;
    std::size_t max_superseded_versions = 64;
    std::size_t max_parameters = 64; // aligns with the IR parameter budget.
};

inline constexpr SkillLimits kDefaultSkillLimits{};

// The Tool-channel surface derived from the source workflow. `description`
// is the definition summary; `parameters_schema` maps the declared workflow
// parameters (constraints carried over, defaults excluded — binding applies
// them at execution); `has_side_effects` is true when any tool referenced by
// the pinned reference manifest has side effects.
struct SkillSurface final {
    std::string description;
    JsonSchema parameters_schema;
    bool has_side_effects = false;

    // JsonSchema has no defaulted equality; the schema root value does.
    friend bool operator==(const SkillSurface &lhs, const SkillSurface &rhs) {
        return lhs.description == rhs.description &&
               lhs.parameters_schema.root == rhs.parameters_schema.root &&
               lhs.has_side_effects == rhs.has_side_effects;
    }
};

struct SkillDescriptor final {
    std::string name; // wire identity, governed vocabulary charset.
    SemanticVersion version;
    WorkflowId source_workflow_id;
    Sha256Digest source_ir_digest{}; // pinned version identity.
    SkillSurface surface;
    Hash digest{}; // canonical digest over the descriptor fields.

    friend bool operator==(const SkillDescriptor &, const SkillDescriptor &) = default;
};

// Canonical descriptor digest; independent of input key order (DEC-002).
[[nodiscard]] Hash skill_descriptor_digest(const SkillDescriptor &descriptor);

// Derives the exposed surface from the source definition, the TR0 reference
// manifest bound to it and the exposure view of the publish moment. Fail
// closed: the definition must pass structural validation, the manifest must
// bind this definition (workflow_id + definition digest), every referenced
// wire name must resolve in the view (duplicates rejected), the summary and
// per-parameter summaries must be non-empty and within bounds, and the
// derived schema must pass gate_schema_subset.
[[nodiscard]] Result<SkillSurface>
derive_skill_surface(const WorkflowDefinition &definition, const WorkflowToolRefManifest &refs,
                     std::span<const ExposedToolSpec> view,
                     const SkillLimits &limits = kDefaultSkillLimits);

// Assembles and digests a descriptor: name charset and hosted-reserved-name
// checks, surface derivation (same fail-closed matrix), source identity from
// the definition and the manifest binding.
[[nodiscard]] Result<SkillDescriptor>
make_skill_descriptor(const WorkflowDefinition &definition, const WorkflowToolRefManifest &refs,
                      std::span<const ExposedToolSpec> view, std::string_view name,
                      SemanticVersion version, const SkillLimits &limits = kDefaultSkillLimits);

[[nodiscard]] JsonValue skill_descriptor_to_json(const SkillDescriptor &descriptor);
[[nodiscard]] Result<SkillDescriptor> skill_descriptor_from_json(const JsonValue &json);

// ---------------------------------------------------------------------------
// Publication lifecycle
// ---------------------------------------------------------------------------

enum class SkillPublicationStatus : std::uint8_t { Published, Revoked };

[[nodiscard]] std::string_view skill_publication_status_name(SkillPublicationStatus status);

// One skill's lifecycle state. `superseded_versions` is the ascending trail
// of versions replaced by explicit upgrades; the descriptor always carries
// the current version and pin.
struct SkillPublicationRecord final {
    SkillDescriptor descriptor;
    SkillPublicationStatus status = SkillPublicationStatus::Published;
    std::vector<SemanticVersion> superseded_versions;

    friend bool operator==(const SkillPublicationRecord &,
                           const SkillPublicationRecord &) = default;
};

// Versioned lifecycle event payload (schema kSkillPublicationEventSchema):
// identities, digests and a bounded revoke reason only — never the
// description or schema bodies.
struct SkillPublicationEvent final {
    std::string kind; // "published" | "upgraded" | "revoked".
    std::string name;
    SemanticVersion version;
    WorkflowId source_workflow_id;
    Sha256Digest source_ir_digest{};
    Hash descriptor_digest{};
    std::string reason; // revoked only; bounded.
};

[[nodiscard]] JsonValue skill_publication_event_to_json(const SkillPublicationEvent &event);

struct SkillPublicationStats final {
    std::uint64_t published = 0;
    std::uint64_t upgrades = 0;
    std::uint64_t revocations = 0;
    std::uint64_t idempotent_noops = 0;
    std::uint64_t rejected_sealed = 0; // publish/upgrade after seal().
    std::uint64_t rejected_closed = 0; // any mutation after close().
    std::uint64_t rejected_other = 0;
    std::uint64_t events_emitted = 0;
    std::uint64_t event_sink_failures = 0;
};

// Serial-control-plane registry of host-explicit skill publications.
// publish/upgrade require the full fail-closed chain: the source version
// record must resolve in the library history by the pinned digest, be
// runnable (DryRunPassed/Validated — the DEC-025 publish gate), match the
// definition content and identity, and re-derive the exact descriptor
// surface. Same name + same descriptor digest publishes as an idempotent
// NoOp; anything else on a taken name fails closed. Upgrades re-pin with a
// strictly increasing version (same digest is a NoOp) and append to the
// superseded trail. Revocation is downgrade-only and idempotent. seal()
// ends the publish/upgrade window (revoke stays available); close() rejects
// every mutation while reads keep working.
class SkillPublicationRegistry final {
  public:
    // The event sink is optional and never owned; nil ids or a null sink
    // disable event emission (stats still count attempts and failures).
    SkillPublicationRegistry(SkillLimits limits = kDefaultSkillLimits,
                             IEventStore *event_sink = nullptr, RuntimeId runtime_id = RuntimeId{},
                             SessionId session_id = SessionId{});
    ~SkillPublicationRegistry() = default;
    SkillPublicationRegistry(const SkillPublicationRegistry &) = delete;
    SkillPublicationRegistry &operator=(const SkillPublicationRegistry &) = delete;

    [[nodiscard]] Result<SkillPublicationRecord>
    publish_skill(const SkillDescriptor &descriptor, const WorkflowDefinition &source,
                  const WorkflowToolRefManifest &refs, std::span<const ExposedToolSpec> view,
                  const WorkflowVersionHistory &history);

    [[nodiscard]] Result<SkillPublicationRecord>
    upgrade_skill(std::string_view name, const SkillDescriptor &next,
                  const WorkflowDefinition &source, const WorkflowToolRefManifest &refs,
                  std::span<const ExposedToolSpec> view, const WorkflowVersionHistory &history);

    [[nodiscard]] Result<SkillPublicationRecord> revoke_skill(std::string_view name,
                                                              std::string_view reason);

    void seal();
    void close();
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    [[nodiscard]] std::optional<SkillPublicationRecord> find(std::string_view name) const;
    [[nodiscard]] std::vector<SkillPublicationRecord> publications() const; // sorted by name.
    [[nodiscard]] const SkillPublicationStats &stats() const noexcept { return stats_; }

  private:
    SkillLimits limits_;
    IEventStore *event_sink_ = nullptr;
    RuntimeId runtime_id_;
    SessionId session_id_;
    bool sealed_ = false;
    bool closed_ = false;
    SkillPublicationStats stats_;
    std::vector<SkillPublicationRecord> records_; // sorted by name.

    void emit(const SkillPublicationEvent &event, EventClass classification);
    [[nodiscard]] Result<SkillDescriptor>
    check_publish_inputs(const SkillDescriptor &descriptor, const WorkflowDefinition &source,
                         const WorkflowToolRefManifest &refs, std::span<const ExposedToolSpec> view,
                         const WorkflowVersionHistory &history);
};

// ---------------------------------------------------------------------------
// Procedure index projection (design §17.3)
// ---------------------------------------------------------------------------

// One index entry plus its canonical-JSON statement — the exact string a
// memory wiring would store as MemoryRecord(kind=Procedure).statement.
struct SkillProcedureEntry final {
    std::string name;
    SemanticVersion version;
    WorkflowId source_workflow_id;
    Sha256Digest source_ir_digest{};
    Hash descriptor_digest{};
    bool has_side_effects = false;
    SkillPublicationStatus status = SkillPublicationStatus::Published;
    std::string statement; // canonical JSON, schema kSkillProcedureIndexSchema.

    friend bool operator==(const SkillProcedureEntry &, const SkillProcedureEntry &) = default;
};

struct SkillProcedureIndex final {
    std::vector<SkillProcedureEntry> entries; // sorted by name.
    Hash digest{};                            // canonical digest over the statements.

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
};

// Deterministic projection (RULE-07): same publications always produce the
// same entries, statements and digest. Timestamp-free by design. Fails
// closed over the publication count budget.
[[nodiscard]] Result<SkillProcedureIndex>
project_skill_procedure_index(std::span<const SkillPublicationRecord> publications,
                              const SkillLimits &limits = kDefaultSkillLimits);

// Strict rebuild from one statement (DEC-040 verification matrix: the index
// is rebuildable). Unknown fields, wrong schema or non-canonical content
// fail closed.
[[nodiscard]] Result<SkillProcedureEntry>
skill_procedure_entry_from_statement(std::string_view statement);

} // namespace mira
