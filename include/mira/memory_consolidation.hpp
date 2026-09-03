#pragma once

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
    Applied,             // passed policy, applied through IMemory
    PendingApproval,     // high-risk write; waits for explicit human approval
    RejectedForbidden,   // secret markers or retention-forbidden content
    RejectedInjection,   // untrusted instruction-shaped text
    RejectedInvalid,     // failed record/mutation validation
    RejectedConflict,    // store rejected (e.g. version conflict); re-plan
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
    // High-risk kinds requiring explicit approval before IMemory.apply().
    std::vector<MemoryKind> approval_required_kinds = {MemoryKind::Preference};
    // Content markers that must never persist into long-term memory.
    std::vector<std::string> forbidden_markers = {
        "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};
    // Instruction-shaped markers for untrusted/model text.
    std::vector<std::string> injection_markers = {
        "ignore previous", "disregard previous", "you are now", "system:",
        "new instructions:", "override policy"};
    std::size_t max_candidates_per_run = 64;

    [[nodiscard]] Result<void> validate() const;
};

struct ConsolidationEntry final {
    CandidateDisposition disposition = CandidateDisposition::Applied;
    MutationId mutation;
    std::string key; // deterministic candidate key (kind + statement)
    std::string reason_code;
    std::uint64_t resulting_version = 0;
    bool idempotent_replay = false;
};

struct ConsolidationReport final {
    std::size_t candidates_examined = 0;
    std::vector<ConsolidationEntry> entries;
    // Mutations held for human approval; apply them via apply_pending().
    std::vector<MemoryMutation> pending_approval;
    [[nodiscard]] std::size_t count_of(CandidateDisposition disposition) const;
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
    [[nodiscard]] Result<ConsolidationReport>
    consolidate(IMemory &memory, std::span<const EventEnvelope> events,
                const MemoryScope &scope, const Timestamp &now) const;

    // Applies a previously pending mutation after explicit approval. The
    // mutation is unchanged from the report; approvals never rewrite content.
    [[nodiscard]] static Result<MemoryMutationResult>
    apply_pending(IMemory &memory, const MemoryMutation &mutation);

    [[nodiscard]] const ConsolidationPolicy &policy() const noexcept { return policy_; }

  private:
    [[nodiscard]] std::vector<MemoryCandidate>
    extract_deterministic(std::span<const EventEnvelope> events, const MemoryScope &scope) const;

    ConsolidationPolicy policy_;
    IConsolidationModel *model_ = nullptr;
};

} // namespace mira
