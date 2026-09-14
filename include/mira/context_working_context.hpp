#pragma once

#include <mira/context_contracts.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_consolidation.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Curator Stage W1: the WorkingContextSnapshot (DEC-035, issue #48)
//
// A task/session-oriented view of "what the model needs to know to continue
// this task now", deterministically projected from the committed
// ConversationCheckpoint of the same session (design §4.2/§6). The snapshot
// is a derived projection with five-tuple identity
// `session_id / task_id / task_epoch / environment_epoch / through_event_sequence`
// and per-item provenance back to the checkpoint statements' source events
// (RULE-07): the EventStore and the checkpoint stay the only sources of truth
// and a lost snapshot is rebuilt by re-projecting its source checkpoint.
//
// The snapshot never self-admits into a model request: like the checkpoint
// (M19), it enters only through the Layer 0 candidate conversion below, and
// every item carries UntrustedExternalData authority (RULE-09 — semantic
// components provide candidates, the deterministic Layer 0 keeps admission).
// Stage W1 ships no model and no incremental merge: those arrive with
// `IContextCurator` in Stage W2 behind the same commit pipeline.
// ---------------------------------------------------------------------------

// One projected state statement. Mirrors `ConversationStatement` because in
// W1 the content is the checkpoint's own filtered surface; `IContextCurator`
// (W2) keeps this shape when it starts producing curated state.
struct WorkingContextItem final {
    std::string content;
    std::vector<EventId> source_events;
    SessionSequence source_sequence = 0;
    double confidence = 0.0;
};

using WorkingContextConstraint = WorkingContextItem; // e.g. "never deploy on Fridays"
using WorkingContextDecision = WorkingContextItem;   // e.g. "batch provider, because X failed"
using WorkingContextOpenIssue = WorkingContextItem;  // e.g. "tenant quota still unanswered"

// Deterministic snapshot identity: the same five-tuple always yields the same
// id, so rebuilding the projection for the same conversation prefix re-derives
// the same snapshot instead of minting a new one (RULE-07).
struct WorkingContextSnapshotId final {
    Id128 value{};
    static WorkingContextSnapshotId generate() {
        return WorkingContextSnapshotId{Id128::generate()};
    }
    static std::optional<WorkingContextSnapshotId> parse(std::string_view text) noexcept {
        const auto parsed = Id128::parse(text);
        return parsed ? std::optional<WorkingContextSnapshotId>(
                            WorkingContextSnapshotId{*parsed})
                      : std::nullopt;
    }
    [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }
    [[nodiscard]] std::string to_string() const { return value.to_string(); }
    friend constexpr bool operator==(const WorkingContextSnapshotId &,
                                     const WorkingContextSnapshotId &) noexcept = default;
    friend constexpr auto operator<=>(const WorkingContextSnapshotId &,
                                      const WorkingContextSnapshotId &) noexcept = default;
};

[[nodiscard]] WorkingContextSnapshotId working_context_snapshot_id_from_seed(
    std::string_view seed);

[[nodiscard]] constexpr SchemaVersion working_context_schema_current() noexcept {
    return {1, 0};
}

// Bounds for the deterministic projection (documented defaults, not a frozen
// contract, mirroring `ConsolidationOptions`). Content markers are NOT
// re-filtered here: the input is an already-committed checkpoint whose
// statements passed the consolidation-stage filters (M19), the same trust
// boundary as `context_items_from_checkpoint`.
struct WorkingContextMergeOptions final {
    std::size_t max_items_per_section = 64;
    std::size_t max_item_chars = 512;
    std::size_t max_source_events = 256;

    [[nodiscard]] Result<void> validate() const;
};

// Task-side identity the snapshot is projected for; together with the
// checkpoint's session and watermark it forms the commit-validation tuple.
struct WorkingContextIdentity final {
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
};

struct WorkingContextSnapshot final {
    SchemaVersion schema_version = working_context_schema_current();
    WorkingContextSnapshotId id;
    SessionId session_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    // The committed checkpoint(s) this snapshot was projected from; the
    // recovery path re-projects from these (design §10).
    std::vector<ConversationCheckpointId> source_checkpoints;
    Timestamp created_at;
    std::vector<WorkingContextConstraint> constraints;
    std::vector<WorkingContextDecision> decisions;
    std::vector<WorkingContextOpenIssue> open_issues;

    [[nodiscard]] Result<void> validate() const;
    // Canonical digest over authoritative fields; excludes id and created_at
    // so rebuild time never changes identity or comparisons.
    [[nodiscard]] Hash state_digest() const;
};

[[nodiscard]] JsonValue working_context_to_json(const WorkingContextSnapshot &snapshot);
[[nodiscard]] Result<WorkingContextSnapshot> working_context_from_json(const JsonValue &json);

// ---------------------------------------------------------------------------
// Deterministic projection (design §6, Stage W1)
// ---------------------------------------------------------------------------

// Projects one committed conversation checkpoint into a snapshot candidate —
// all-or-nothing: any bound violation rejects the whole candidate instead of
// producing a partial snapshot. The identity (task/epochs) must be supplied by
// the caller; a checkpoint whose session is nil, a watermark of zero or an
// identity mismatch with the checkpoint's own stamping is rejected. Pure
// function; hosts run it inside an Executor-supervised operation.
[[nodiscard]] Result<WorkingContextSnapshot>
working_context_from_checkpoint(const ConversationCheckpoint &checkpoint,
                                const WorkingContextIdentity &identity,
                                const WorkingContextMergeOptions &options = {});

// ---------------------------------------------------------------------------
// Snapshot store and commit validation (design §5.2/§5.3)
// ---------------------------------------------------------------------------

struct WorkingContextStorePolicy final {
    // Retained snapshots per session; older ones fall out of the ring. Epoch
    // changes open a new identity chain inside the same ring.
    std::size_t max_snapshots_per_session = 8;

    [[nodiscard]] Result<void> validate() const;
};

// Store of committed snapshots. Volatile reference implementation (same tier
// as the checkpoint store): the snapshot is a rebuildable projection
// (RULE-07) — losing it costs continuity, never correctness. Internally
// serialized; safe to share across Executor-supervised calls.
class IWorkingContextStore {
  public:
    virtual ~IWorkingContextStore() = default;
    // Rejects regressed watermarks within the same
    // (session, task, task_epoch, environment_epoch) chain and snapshots that
    // fail validation; a newer epoch chain commits alongside the old one.
    virtual Result<void> put(const WorkingContextSnapshot &snapshot) = 0;
    [[nodiscard]] virtual Result<std::optional<WorkingContextSnapshot>>
    latest(SessionId session) const = 0;
    [[nodiscard]] virtual Result<std::optional<WorkingContextSnapshot>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const = 0;
    [[nodiscard]] virtual Result<std::size_t> count(SessionId session) const = 0;
    virtual Result<std::size_t> erase_session(SessionId session, std::string reason) = 0;
};

class InMemoryWorkingContextStore final : public IWorkingContextStore {
  public:
    explicit InMemoryWorkingContextStore(
        WorkingContextStorePolicy policy = WorkingContextStorePolicy{});
    ~InMemoryWorkingContextStore() override;

    InMemoryWorkingContextStore(const InMemoryWorkingContextStore &) = delete;
    InMemoryWorkingContextStore &operator=(const InMemoryWorkingContextStore &) = delete;

    Result<void> put(const WorkingContextSnapshot &snapshot) override;
    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    latest(SessionId session) const override;
    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const override;
    [[nodiscard]] Result<std::size_t> count(SessionId session) const override;
    Result<std::size_t> erase_session(SessionId session, std::string reason) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Live control-plane state observed at commit time; together with the
// candidate's identity it forms the commit-validation tuple (design §5.2).
struct WorkingContextCommitState final {
    SessionId session;
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    bool session_terminal = false;
    bool task_terminal = false;
};

enum class WorkingContextCommitDisposition : std::uint8_t {
    Committed,         // replaced the stored snapshot (or none existed)
    IdempotentNoOp,    // same watermark and digest as the stored snapshot
    DiscardedStale,    // tuple/watermark mismatch, conflict or invalid input
    DiscardedTerminal, // session/task went terminal; late results are dropped
};

[[nodiscard]] std::string working_context_commit_disposition_name(
    WorkingContextCommitDisposition disposition);

struct WorkingContextCommitOutcome final {
    WorkingContextCommitDisposition disposition =
        WorkingContextCommitDisposition::DiscardedStale;
    // Stable reason code, e.g. "conflicting-watermark"; free text never
    // reaches the audit surface.
    std::string reason_code;
    std::optional<WorkingContextSnapshot> committed;
};

// Validates a snapshot candidate against live state and commits it (design
// §5.2): terminal sessions/tasks discard late results; any tuple mismatch
// discards the candidate and keeps the stored snapshot; an equal watermark
// with the same digest is an idempotent no-op, with a different digest it
// fails closed as a conflict (the recovery path of design §10 rebuilds from
// the source checkpoint). Pure function over the store; hosts run it inside
// an Executor-supervised operation.
[[nodiscard]] WorkingContextCommitOutcome
commit_working_context(IWorkingContextStore &store, const WorkingContextSnapshot &candidate,
                       const WorkingContextCommitState &live);

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

// Converts snapshot statements into ordinary `ContextItem` candidates for the
// Layer 0 pipeline (design §7 — the snapshot never self-admits): constraints
// become P1 `UserConstraint` items, decisions and open issues become P3
// `CheckpointSummary` items. All items carry `UntrustedExternalData` authority
// (model-mediated derived projection, RULE-09), their source event provenance,
// the statement's own `source_sequence` and the snapshot's epoch stamping so
// stale projections stay detectable by the existing stale-build checks. Item
// ids derive deterministically from the snapshot id in their own seed space,
// so checkpoint items and snapshot items never collide when a host audits
// both projections side by side.
[[nodiscard]] std::vector<ContextItem>
context_items_from_working_context(const WorkingContextSnapshot &snapshot);

} // namespace mira
