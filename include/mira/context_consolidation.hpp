#pragma once

#include <mira/context_contracts.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>
#include <mira/model_provider.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Intelligence Layer 3: semantic consolidation (DEC-032 §2/§4, M19)
//
// Information that only holds across several messages (user constraints,
// decisions, unresolved threads, preference candidates) is consolidated into
// a structured `ConversationCheckpoint` — a session-domain Warm projection
// that sits next to the task-domain `TaskCheckpoint` and enters model
// requests only through Layer 0 admission, never by itself (semantic
// components have no admission authority, DEC-032 §2).
//
// Discipline carried over from RULE-07/RULE-09 (DEC-032 §3): the checkpoint
// is a derived projection with `source_events` provenance and a
// `through_event_sequence` watermark; the EventStore stays the only source
// of truth and a stale checkpoint is repaired by re-consolidating from its
// source events. Consolidation is model-mediated, so every model statement
// is untrusted input: markers are filtered, citations outside the input
// segment are dropped (no fabricated provenance), and all outputs are
// bounded (RULE-08).
//
// The consolidation model is configured through `IModelProvider`; any
// available source model works, including the model that produced the
// original context — no dedicated small model is required (DEC-032 §5 as
// revised by DEC-036). Core ships the request/response adapter below, never
// a model or inference backend.
// ---------------------------------------------------------------------------

// One consolidated statement. All four checkpoint sections share this shape;
// the aliases keep the design vocabulary (design §5.4) without duplicating
// layout. `source_events` are the EventIds of the conversation entries the
// statement was extracted from (never fabricated: consolidators drop
// statements whose citations fall outside the input segment).
// `source_sequence` is the smallest cited entry's session sequence so Layer 0
// can order the statement by its real recency relative to live instructions.
struct ConversationStatement final {
    std::string content;
    std::vector<EventId> source_events;
    SessionSequence source_sequence = 0;
    double confidence = 0.0;
};

using ConversationConstraint = ConversationStatement; // e.g. "confirm with the user before sending to Zhang San"
using ConversationDecision = ConversationStatement;   // e.g. "chose the overnight batch provider"
using ConversationThread = ConversationStatement;     // e.g. "waiting for the tenant quota reply"
// Preference candidates never auto-enter a model request: promotion to
// Memory goes through the existing `MemoryConsolidator` human-approval
// pipeline (design §5.4; DEC-029 discipline).
using ConversationPreference = ConversationStatement;

// Task-side identity stamped into the checkpoint's commit-validation tuple;
// the session and watermark come from the consolidated segment itself.
struct ConsolidationIdentity final {
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
};

// Bounds, safety filters and the commit-identity stamp for one consolidation
// run. Defaults are documented values, not a frozen contract (mirrors
// `RetrievalWeights`); the marker lists stay aligned with `ConsolidationPolicy`
// so secrets never reach the checkpoint surface.
struct ConsolidationOptions final {
    ConsolidationIdentity identity;
    std::size_t max_statements_per_kind = 32;
    std::size_t max_preferences = 16;
    std::size_t max_summary_chars = 2'048;
    std::size_t max_statement_chars = 512;
    std::size_t max_source_events = 256;
    // Statements the model reports below this confidence are dropped.
    double min_confidence = 0.0;
    // Soft deadline for the whole consolidation (including the model call);
    // exceeding it fails the run — callers keep the previous checkpoint
    // (design §8), never a partial one.
    std::chrono::milliseconds deadline{10'000};
    std::uint64_t max_output_tokens = 1'024;
    // Content markers that must never enter the checkpoint (RULE-09).
    std::vector<std::string> forbidden_markers = {
        "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};
    // Instruction-shaped markers for untrusted model text.
    std::vector<std::string> injection_markers = {
        "ignore previous", "disregard previous", "you are now", "system:",
        "new instructions:", "override policy"};
    // Cooperative cancellation probe owned by the operation supervisor; the
    // adapter maps it into the provider OperationContext.
    std::function<bool()> cancellation_requested;

    [[nodiscard]] bool cancelled() const noexcept {
        return cancellation_requested != nullptr && cancellation_requested();
    }

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] constexpr SchemaVersion conversation_checkpoint_schema_current() noexcept {
    return {1, 0};
}

struct ConversationCheckpointId final {
    Id128 value{};
    static ConversationCheckpointId generate() {
        return ConversationCheckpointId{Id128::generate()};
    }
    static std::optional<ConversationCheckpointId> parse(std::string_view text) noexcept {
        const auto parsed = Id128::parse(text);
        return parsed ? std::optional<ConversationCheckpointId>(
                            ConversationCheckpointId{*parsed})
                      : std::nullopt;
    }
    [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }
    [[nodiscard]] std::string to_string() const { return value.to_string(); }
    friend constexpr bool operator==(const ConversationCheckpointId &,
                                     const ConversationCheckpointId &) noexcept = default;
    friend constexpr auto operator<=>(const ConversationCheckpointId &,
                                      const ConversationCheckpointId &) noexcept = default;
};

// Deterministic checkpoint identity: the same (session, watermark) always
// yields the same id, so rebuilding the projection for the same conversation
// prefix re-derives the same checkpoint instead of minting a new one.
[[nodiscard]] ConversationCheckpointId
conversation_checkpoint_id_from_seed(std::string_view seed);

// Session-domain Warm projection (design §5.4). Commit semantics are ruled
// by the five-tuple `session_id / task_id / task_epoch / environment_epoch /
// through_event_sequence` (DEC-032 §4): any mismatch with live state discards
// the candidate and keeps the stored checkpoint; results arriving after the
// session or task went terminal are dropped (terminal idempotency).
struct ConversationCheckpoint final {
    SchemaVersion schema_version = conversation_checkpoint_schema_current();
    ConversationCheckpointId id;
    SessionId session_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    Timestamp created_at;
    // Narrative summary; never authoritative and excluded from the projection
    // digest (same treatment as `TaskCheckpoint::narrative_summary`).
    std::string summary;
    std::vector<ConversationConstraint> constraints;
    std::vector<ConversationDecision> decisions;
    std::vector<ConversationThread> unresolved_threads;
    std::vector<ConversationPreference> preferences; // pending human approval
    // Union of the statements' provenance, bounded and de-duplicated.
    std::vector<EventId> source_events;
    // Model profile that produced the checkpoint; nil for non-model runners.
    ModelProfileId generated_by;
    double confidence = 0.0;

    [[nodiscard]] Result<void> validate() const;
    // Canonical digest over authoritative fields; excludes id, created_at,
    // summary and confidence so narrative changes never change identity.
    [[nodiscard]] Hash projection_digest() const;
};

[[nodiscard]] JsonValue conversation_checkpoint_to_json(const ConversationCheckpoint &checkpoint);
[[nodiscard]] Result<ConversationCheckpoint>
conversation_checkpoint_from_json(const JsonValue &json);

// ---------------------------------------------------------------------------
// Consolidator contract and model-backed reference implementation
// ---------------------------------------------------------------------------

class ISemanticConsolidator {
  public:
    virtual ~ISemanticConsolidator() = default;
    // Consolidates one conversation-prefix segment into a checkpoint
    // candidate. Failures mean "no new projection": callers keep the stored
    // checkpoint and Layer 0 closes the request as before (design §8) — no
    // partial checkpoint is ever returned.
    [[nodiscard]] virtual Result<ConversationCheckpoint>
    consolidate(const ConversationSegment &segment, const ConsolidationOptions &options) = 0;
};

// JSON schema of the consolidation output contract (mode `StrictJsonSchema`);
// exposed so hosts and tests can pin exactly what the consolidator demands.
[[nodiscard]] JsonSchema consolidation_output_schema();

// Reference consolidator backed by an injected `IModelProvider` (DEC-032 §5):
// renders the segment as a numbered transcript, requests a schema-constrained
// JSON consolidation, then re-validates everything the model returned —
// statement bounds, marker filters, citation-to-event provenance mapping and
// confidence floors — dropping invalid statements instead of admitting them.
// The provider call runs synchronously on the caller's Executor-managed
// operation; deadline and cancellation travel through `options`.
class ProviderSemanticConsolidator final : public ISemanticConsolidator {
  public:
    explicit ProviderSemanticConsolidator(IModelProvider &provider);
    ~ProviderSemanticConsolidator() override;

    ProviderSemanticConsolidator(const ProviderSemanticConsolidator &) = delete;
    ProviderSemanticConsolidator &operator=(const ProviderSemanticConsolidator &) = delete;

    [[nodiscard]] Result<ConversationCheckpoint>
    consolidate(const ConversationSegment &segment, const ConsolidationOptions &options) override;

  private:
    IModelProvider &provider_;
};

// ---------------------------------------------------------------------------
// Checkpoint store and commit validation (design §6.2)
// ---------------------------------------------------------------------------

struct ConversationCheckpointStorePolicy final {
    // Retained checkpoints per session; older ones fall out of the ring.
    std::size_t max_checkpoints_per_session = 8;

    [[nodiscard]] Result<void> validate() const;
};

// Store of committed session checkpoints. Volatile reference implementation
// (same tier as `MemoryCheckpointStore`): the checkpoint is a rebuildable
// projection (RULE-07) — losing it costs continuity, never correctness, and
// every committed checkpoint can be re-derived by re-consolidating its
// source events. Internally serialized; safe to share across
// Executor-supervised calls.
class IConversationCheckpointStore {
  public:
    virtual ~IConversationCheckpointStore() = default;
    // Rejects regressed watermarks (an older checkpoint may never overwrite a
    // newer one) and checkpoints that fail validation.
    virtual Result<void> put(const ConversationCheckpoint &checkpoint) = 0;
    [[nodiscard]] virtual Result<std::optional<ConversationCheckpoint>>
    latest(SessionId session) const = 0;
    [[nodiscard]] virtual Result<std::optional<ConversationCheckpoint>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const = 0;
    [[nodiscard]] virtual Result<std::size_t> count(SessionId session) const = 0;
    virtual Result<std::size_t> erase_session(SessionId session, std::string reason) = 0;
};

class InMemoryConversationCheckpointStore final : public IConversationCheckpointStore {
  public:
    explicit InMemoryConversationCheckpointStore(
        ConversationCheckpointStorePolicy policy = ConversationCheckpointStorePolicy{});
    ~InMemoryConversationCheckpointStore() override;

    InMemoryConversationCheckpointStore(const InMemoryConversationCheckpointStore &) = delete;
    InMemoryConversationCheckpointStore &
    operator=(const InMemoryConversationCheckpointStore &) = delete;

    Result<void> put(const ConversationCheckpoint &checkpoint) override;
    [[nodiscard]] Result<std::optional<ConversationCheckpoint>>
    latest(SessionId session) const override;
    [[nodiscard]] Result<std::optional<ConversationCheckpoint>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const override;
    [[nodiscard]] Result<std::size_t> count(SessionId session) const override;
    Result<std::size_t> erase_session(SessionId session, std::string reason) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Live control-plane state observed at commit time; together with the
// candidate's session it forms the five-tuple
// `session_id / task_id / task_epoch / environment_epoch / through_event_sequence`.
struct ConversationCommitState final {
    SessionId session;
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    bool session_terminal = false;
    bool task_terminal = false;
};

enum class ConversationCommitDisposition : std::uint8_t {
    Committed,       // replaced the stored checkpoint (or none existed)
    IdempotentNoOp,  // same watermark and digest as the stored checkpoint
    DiscardedStale,  // five-tuple or watermark mismatch; old checkpoint kept
    DiscardedTerminal, // session/task went terminal; late results are dropped
};

[[nodiscard]] std::string conversation_commit_disposition_name(
    ConversationCommitDisposition disposition);

struct ConversationCommitOutcome final {
    ConversationCommitDisposition disposition =
        ConversationCommitDisposition::DiscardedStale;
    // Stable reason code, e.g. "task-epoch-mismatch"; free text never reaches
    // the audit surface.
    std::string reason_code;
    std::optional<ConversationCheckpoint> committed;
};

// Validates a consolidation candidate against live state and commits it
// (design §6.2): terminal sessions/tasks discard late results; any five-tuple
// mismatch discards the candidate and keeps the stored checkpoint; an equal
// watermark with the same digest is an idempotent no-op, with a different
// digest it fails closed as a conflict (the re-consolidation path of design
// §6.3 rebuilds from source events). Pure function over the store; hosts run
// it inside an Executor-supervised operation.
[[nodiscard]] ConversationCommitOutcome
commit_conversation_checkpoint(IConversationCheckpointStore &store,
                               const ConversationCheckpoint &candidate,
                               const ConversationCommitState &live);

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

// Converts checkpoint statements into ordinary `ContextItem` candidates for
// the Layer 0 pipeline (DEC-032 §2 — the checkpoint never self-admits):
// constraints become P1 `UserConstraint` items, the summary and decisions and
// unresolved threads become P3 `CheckpointSummary` items. All items carry
// `UntrustedExternalData` authority (model-mediated derived projection,
// RULE-09 — never SystemPolicy), their source event provenance, and the
// statement's own `source_sequence` so ordering reflects real recency.
// Preference candidates are not converted: they wait for the human-approval
// pipeline. Item ids derive deterministically from the checkpoint id.
[[nodiscard]] std::vector<ContextItem>
context_items_from_checkpoint(const ConversationCheckpoint &checkpoint);

} // namespace mira
