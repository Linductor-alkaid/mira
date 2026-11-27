#pragma once

#include <mira/content_markers.hpp>

#include <mira/event_store.hpp>
#include <mira/memory_contracts.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Verified Event -> Memory candidate pipeline (M4-12)
// ---------------------------------------------------------------------------

// Stable reason codes describing what the deterministic policy decided; free
// text never reaches the audit surface.
enum class CandidateDisposition : std::uint8_t {
    Applied,           // passed policy, applied through IMemory
    PendingApproval,   // high-risk write; waits for explicit human approval
    RejectedForbidden, // secret markers or retention-forbidden content
    RejectedInjection, // untrusted instruction-shaped text
    RejectedInvalid,   // failed record/mutation validation
    RejectedConflict,  // store rejected (e.g. version conflict); re-plan
};

[[nodiscard]] std::string candidate_disposition_name(CandidateDisposition disposition);

// A deterministic candidate extracted from verified events or proposed by the
// optional model hook; policy validation runs for both sources.
struct MemoryCandidate final {
    MemoryRecord proposed;
    std::vector<EventId> evidence;
    MutationReasonCode reason = MutationReasonCode::VerifiedEvent;
    // True when the IConsolidationModel produced the candidate; model output
    // is always untrusted and cannot claim HumanConfirmed.
    bool model_assisted = false;
    // Host-only SkillHint observation with a durable retry identity. Add is
    // immutable so a replay cannot merge it with another source or replace it.
    std::optional<MutationId> immutable_add_id = std::nullopt;
};

// Optional model-assisted extractor. Implementations call external models on
// Executor-managed workers; the deterministic policy re-validates every
// proposal regardless of source.
class IConsolidationModel {
  public:
    virtual ~IConsolidationModel() = default;
    [[nodiscard]] virtual std::vector<MemoryCandidate>
    propose(std::span<const EventEnvelope> events, const MemoryScope &scope) = 0;
};

struct ConsolidationPolicy final {
    // Kinds requiring explicit approval before IMemory.apply(). DEC-115
    // removed the kind default: user-stated preferences, promotion
    // constraints and agent-recorded preferences write automatically at
    // their source trust level, while the DEC-109 propose/decide API keeps
    // its own mandatory approval. Hosts may re-arm any kind here.
    std::vector<MemoryKind> approval_required_kinds = {};
    // Nonempty ASCII case-insensitive substrings forbidden in long-term memory.
    std::vector<std::string> forbidden_markers = default_forbidden_content_markers();
    // Nonempty instruction-shaped substrings for untrusted/model text only.
    std::vector<std::string> injection_markers = default_injection_content_markers();
    std::size_t max_candidates_per_run = 64;
    // DEC-115 coexistence bound: same-claim-key alternatives preserved per
    // scope beyond this count are rejected with an explicit report (never
    // silently dropped). The incumbent is not counted.
    std::size_t max_alternatives_per_claim = 4;
    // DEC-059 legacy namespaced projections remain compatible. DEC-101
    // memory agents force false: model text cannot mint an import exemption
    // or claim verification above Unverified, even with a namespace tag.
    bool allow_model_import_exemption = true;
    // A proposal-only owner can require approval even for HumanCorrection.
    // Duplicate no-ops remain read-only; the default preserves old callers.
    bool require_explicit_approval = false;
    // DEC-170: admit evidence-backed Unverified EnvironmentFact/ApplicationFact in Session.
    bool allow_unverified_session_facts = false;
    // Read-only duplicate handling during explicit intent review.
    bool allow_duplicate_evidence_updates = true;

    [[nodiscard]] Result<void> validate() const;
};

struct ConsolidationEntry final {
    CandidateDisposition disposition = CandidateDisposition::Applied;
    MutationId mutation;
    std::string key; // deterministic candidate key (kind + statement)
    std::string reason_code;
    std::uint64_t resulting_version = 0;
    bool idempotent_replay = false;
    // DEC-115: the incumbent claim this entry duplicates, appends evidence
    // to, promotes, supersedes or preserves an alternative of.
    std::optional<MemoryId> incumbent;
    MemoryKind kind = MemoryKind::Preference;
};

struct MemoryRecordingCounts final {
    std::size_t applied = 0;
    std::size_t unchanged = 0;
    std::size_t pending_approval = 0;
    std::size_t rejected = 0;
    std::size_t unresolved = 0;
    std::size_t truncated = 0;
    // Subset of unchanged writes and repeated pending approvals.
    std::size_t replayed = 0;
};

struct ConsolidationReport final {
    // DEC-115: the pre-truncation candidate count (candidates that arrived).
    std::size_t candidates_examined = 0;
    // How many candidates were dropped by max_candidates_per_run; zero when
    // everything was examined. Over-cap work is reported, never silent.
    std::size_t candidates_truncated = 0;
    std::vector<ConsolidationEntry> entries;
    // Mutations held for human approval; apply them via apply_pending().
    std::vector<MemoryMutation> pending_approval;
    [[nodiscard]] std::size_t count_of(CandidateDisposition disposition) const;
    [[nodiscard]] MemoryRecordingCounts recording_counts() const;
};

// Deterministic Verified Event -> Memory pipeline: extract candidates, classify
// scope/sensitivity, retrieve conflicts, plan mutations (Add or Supersede with
// the observed version), validate policy and gate high-risk writes behind
// human approval — all before IMemory.apply() (design Context/Memory §13.3).
class MemoryConsolidator final {
  public:
    MemoryConsolidator(ConsolidationPolicy policy = ConsolidationPolicy{},
                       IConsolidationModel *model = nullptr);

    // Pure synchronous pipeline; hosts run it through the Executor supervisor
    // (M4-16). Conflict retrieval and apply() use the given store.
    [[nodiscard]] Result<ConsolidationReport> consolidate(IMemory &memory,
                                                          std::span<const EventEnvelope> events,
                                                          const MemoryScope &scope,
                                                          const Timestamp &now) const;

    // Runs the exact same policy pipeline over pre-extracted candidates — no
    // event extraction and no model hook: candidate provenance and content
    // are the caller's responsibility (the Working Context promotion path,
    // M23). Over-cap input is truncated to the policy bound deterministically,
    // the same rule consolidate() applies to its own extraction.
    [[nodiscard]] Result<ConsolidationReport>
    consolidate_candidates(IMemory &memory, std::vector<MemoryCandidate> candidates,
                           const MemoryScope &scope, const Timestamp &now) const;

    // Applies a previously pending mutation after explicit approval. The
    // mutation is unchanged from the report; approvals never rewrite content.
    [[nodiscard]] static Result<MemoryMutationResult> apply_pending(IMemory &memory,
                                                                    const MemoryMutation &mutation);

    [[nodiscard]] const ConsolidationPolicy &policy() const noexcept { return policy_; }

  private:
    [[nodiscard]] std::vector<MemoryCandidate>
    extract_deterministic(std::span<const EventEnvelope> events, const MemoryScope &scope,
                          const Timestamp &now) const;

    ConsolidationPolicy policy_;
    IConsolidationModel *model_ = nullptr;
};

} // namespace mira
