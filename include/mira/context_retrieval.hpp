#pragma once

#include <mira/context_contracts.hpp>
#include <mira/conversation_log.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>
#include <mira/memory_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Intelligence Layer 1: retrieval recall (DEC-032 §2/§6, M17)
//
// This header is the recall half of the Cold History -> Relevant Context path:
// it defines what can be indexed (conversation segments and the DEC-029
// learning-asset statement surfaces), how embeddings are supplied from the
// outside, and how candidates come back with provenance. Layer 1 only
// recalls; it never ranks finally (Layer 2) and never admits into a model
// request (Layer 0 `StandardContextManager` keeps that authority).
//
// Vector supply follows the `MemoryQuery::query_embedding` /
// `SqliteMemoryStore::index_embedding` external-supply semantics: an
// `IContextEmbedder` is a supplier the host injects, never a storage
// authority, and Core ships no embedder implementation (DEC-032 §5).
// ---------------------------------------------------------------------------

enum class ContextAssetKind : std::uint8_t {
    ConversationSegment, // DEC-016 conversation-view window
    WorkflowEpisode,     // DEC-029 WorkflowEpisodeRecord statement surface
    RecoveryLesson,      // DEC-029 WorkflowRecoveryLesson statement surface
};

[[nodiscard]] std::string context_asset_kind_name(ContextAssetKind kind);
[[nodiscard]] std::optional<ContextAssetKind> context_asset_kind_from(std::string_view name);

struct ContextAssetId final {
    Id128 value{};
    static ContextAssetId generate() { return ContextAssetId{Id128::generate()}; }
    static std::optional<ContextAssetId> parse(std::string_view text) noexcept {
        const auto parsed = Id128::parse(text);
        return parsed ? std::optional<ContextAssetId>(ContextAssetId{*parsed}) : std::nullopt;
    }
    [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }
    [[nodiscard]] std::string to_string() const { return value.to_string(); }
    friend constexpr bool operator==(const ContextAssetId &,
                                     const ContextAssetId &) noexcept = default;
    friend constexpr auto operator<=>(const ContextAssetId &,
                                      const ContextAssetId &) noexcept = default;
};

// Deterministic asset identity: the first 16 bytes of SHA-256(seed). The same
// derivation inputs (session + window anchor for segments, asset-specific
// seeds for learning assets) always yield the same id, so projection rebuilds
// re-register idempotently instead of duplicating entries (RULE-07).
[[nodiscard]] ContextAssetId context_asset_id_from_seed(std::string_view seed);

// Index admission policy. Bounds are RULE-08 ceilings; the marker lists stay
// aligned with `ConsolidationPolicy` so secrets never reach the retrieval
// surface regardless of which pipeline feeds it.
struct ContextIndexPolicy final {
    std::size_t max_asset_text_bytes = 8 * 1024;
    std::size_t max_source_events = 64;
    std::size_t max_assets = 100'000;
    std::size_t max_embedding_dims = 4'096;
    // Content markers that must never enter the retrieval index.
    std::vector<std::string> forbidden_markers = {
        "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};

    [[nodiscard]] Result<void> validate() const;
};

// One indexable Cold-History asset. Conversation segments own a session;
// learning assets own a memory scope — exactly one ACL owner, deny by default.
// `text` is the sanitized statement surface: bounded conversation text, or the
// DEC-029 identifier/reason-code form for episodes and lessons.
struct ContextIndexAsset final {
    ContextAssetId id;
    ContextAssetKind kind = ContextAssetKind::ConversationSegment;
    std::string text;
    std::optional<SessionId> session;
    std::optional<MemoryScope> scope;
    std::vector<EventId> source_events;
    // EventStore sequence watermark observed when the asset was derived.
    // Registering the same id with a strictly newer watermark invalidates a
    // previously attached embedding (the projection went stale); registering
    // different content under the same watermark fails closed.
    std::uint64_t through_event_sequence = 0;

    [[nodiscard]] Result<void> validate(const ContextIndexPolicy &policy) const;
};

// ---------------------------------------------------------------------------
// Embedding supply (external; hosts own real inference and its Executor
// routing per design §7 — failures degrade the vector leg, never the request)
// ---------------------------------------------------------------------------

struct ContextEmbedding final {
    std::vector<float> values;
    // Which embedder profile produced the vector; query and entry vectors
    // only compare inside the same profile.
    ModelProfileId profile_id;
};

struct ContextEmbeddingInput final {
    ContextAssetId asset_id;
    ContextAssetKind kind = ContextAssetKind::ConversationSegment;
    std::string text;
    std::vector<EventId> source_events;
};

[[nodiscard]] ContextEmbeddingInput context_embedding_input(const ContextIndexAsset &asset);

class IContextEmbedder {
  public:
    virtual ~IContextEmbedder() = default;
    // Synchronous supply contract. Implementations that call models must run
    // that work on Executor-managed workers and surface failures as errors;
    // the retrieval layer treats any failure as vector-leg degradation.
    [[nodiscard]] virtual Result<ContextEmbedding> embed(const ContextEmbeddingInput &input) = 0;
};

// ---------------------------------------------------------------------------
// Query, budget and candidates
// ---------------------------------------------------------------------------

struct ContextQuery final {
    // Deny-by-default ACL allowlist: conversation segments require a session
    // match, learning assets an exact scope match. A query with neither grant
    // is rejected, mirroring `MemoryQuery` scope semantics.
    std::optional<SessionId> session;
    std::vector<MemoryScope> scopes;
    std::optional<std::vector<ContextAssetKind>> kinds;
    // Lexical and embedding query surface; hosts assemble goal, task frame
    // and recent user messages into this text.
    std::string text;
    // Mandatory verbatim substring filters (exact leg).
    std::vector<std::string> exact_terms;
    std::optional<ContextEmbedding> query_embedding;

    [[nodiscard]] Result<void> validate() const;
};

struct RetrievalBudget final {
    std::size_t top_k = 32;
    std::uint64_t token_budget = 4'096;
    // Soft deadline per retrieval; exceeding returns partial results flagged
    // `deadline_exceeded` instead of blocking the caller.
    std::chrono::milliseconds deadline{2'000};
    // Bounded linear cosine scan (no ANN in the first stage, design §5.2).
    std::size_t max_vector_scan = 1'024;

    [[nodiscard]] Result<void> validate() const;
};

// One recalled Cold-History asset with provenance and per-leg evidence.
// Scores are recall signals for Layer 2, not admission decisions.
struct ContextCandidate final {
    ContextAssetId asset_id;
    ContextAssetKind kind = ContextAssetKind::ConversationSegment;
    std::string text;
    std::optional<SessionId> session;
    std::optional<MemoryScope> scope;
    std::vector<EventId> source_events;
    std::uint64_t through_event_sequence = 0;
    double score = 0.0;
    bool exact_hit = false;
    double lexical_score = 0.0;
    bool vector_hit = false;
    double vector_similarity = 0.0;
};

struct ContextRetrievalResult final {
    std::vector<ContextCandidate> candidates;
    // Reused per design §5.2: `fts_leg_*` reports the lexical leg here.
    MemoryQueryQuality quality;
    std::uint64_t tokens_estimate = 0;
};

class IContextRetriever {
  public:
    virtual ~IContextRetriever() = default;
    [[nodiscard]] virtual Result<ContextRetrievalResult>
    retrieve(const ContextQuery &query, const RetrievalBudget &budget) = 0;
};

// ---------------------------------------------------------------------------
// Reference index (volatile, rebuildable projection)
// ---------------------------------------------------------------------------

// Documented defaults, not a frozen contract (mirrors `RetrievalWeights`).
// Layer 1 fuses three legs only — verification/confidence/recency ranking is
// Layer 2 territory and intentionally absent here.
struct ContextRetrievalWeights final {
    double exact = 0.35;
    double lexical = 0.25;
    double vector = 0.40;
};

// In-memory hybrid-retrieval index over registered assets: exact (verbatim
// substrings, mandatory filter), lexical (token-overlap coverage — the
// in-process stand-in for the FTS5 leg of durable stores) and vector (bounded
// linear cosine over externally supplied embeddings). The index is a
// rebuildable projection (RULE-07): losing it costs recall quality, never
// correctness, and every entry can be re-derived from the EventStore.
// Internally serialized; safe to share across Executor-supervised calls.
class InMemoryContextIndex final : public IContextRetriever {
  public:
    explicit InMemoryContextIndex(ContextIndexPolicy policy = ContextIndexPolicy{},
                                  ContextRetrievalWeights weights = ContextRetrievalWeights{});
    ~InMemoryContextIndex() override;

    InMemoryContextIndex(const InMemoryContextIndex &) = delete;
    InMemoryContextIndex &operator=(const InMemoryContextIndex &) = delete;

    // Registers or refreshes one asset (idempotent by asset id). A strictly
    // newer watermark drops a stale embedding; content changing under the
    // same watermark fails closed.
    [[nodiscard]] Result<void> upsert_asset(const ContextIndexAsset &asset);
    // External vector supply. Empty, oversized or non-finite vectors are
    // rejected so corrupt supply surfaces as index lag, not query poisoning.
    [[nodiscard]] Result<void> attach_embedding(const ContextAssetId &id, ContextEmbedding embedding);
    [[nodiscard]] Result<void> drop_asset(const ContextAssetId &id);
    // Registered assets without a usable embedding.
    [[nodiscard]] std::size_t index_lag() const;
    // Drops every embedding (e.g. after an embedder-model change); returns
    // how many were dropped.
    [[nodiscard]] std::size_t clear_embeddings();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool contains(const ContextAssetId &id) const;
    [[nodiscard]] bool has_embedding(const ContextAssetId &id) const;
    [[nodiscard]] std::optional<ContextIndexAsset> asset(const ContextAssetId &id) const;

    [[nodiscard]] Result<ContextRetrievalResult>
    retrieve(const ContextQuery &query, const RetrievalBudget &budget) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Conversation segmentation (deterministic window split of the DEC-016 view)
// ---------------------------------------------------------------------------

struct ConversationSegmentationOptions final {
    // Entries per segment window.
    std::size_t window_entries = 12;
    // Upper bound for one segment's joined text; a window closes early when
    // the next entry would exceed it, and a single oversized entry is emitted
    // truncated with a "[truncated]" marker.
    std::size_t max_segment_bytes = 4'096;

    [[nodiscard]] Result<void> validate() const;
};

// One window entry with its provenance, added in M19 so Layer 3 consolidation
// can bind model citations back to source events (additive change; the joined
// `text` and flattened `source_events` are unchanged and remain the retrieval
// surface). `text` mirrors the exact line carried in the segment text,
// including the "[truncated]" marker for oversized entries, so the whole
// segment stays bounded (RULE-08).
struct ConversationSegmentEntry final {
    std::string text;
    EventId origin;
    SessionSequence session_sequence = 0;
};

struct ConversationSegment final {
    SessionId session;
    std::size_t first_entry = 0;
    std::size_t entry_count = 0;
    std::string text;
    std::vector<EventId> source_events;
    SessionSequence through_sequence = 0;
    // Per-entry view aligned with [first_entry, first_entry + entry_count);
    // size() == entry_count for segments produced by segment_conversation().
    std::vector<ConversationSegmentEntry> entries;
    // Deterministic identity (session + first entry sequence + count), so
    // re-segmenting the same log re-registers the same assets.
    ContextAssetId asset_id;
};

[[nodiscard]] Result<std::vector<ConversationSegment>>
segment_conversation(const SessionId &session, std::span<const ConversationEntry> entries,
                     const ConversationSegmentationOptions &options = {});

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

// Pure conversion of a recalled candidate into a P4 `RetrievedMemory`
// `ContextItem`. The item id derives deterministically from the asset id and
// provenance is carried through; `StandardContextManager` retains the final
// token/safety admission authority (DEC-032 §2 — retrieval never self-admits).
[[nodiscard]] ContextItem context_item_from_candidate(const ContextCandidate &candidate);

// ---------------------------------------------------------------------------
// Versioned JSON serialization (schema "mira.context.candidate.v1"); readers
// ignore unknown members and reject unsupported schema majors (DEC-002).
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue context_candidate_to_json(const ContextCandidate &candidate);
[[nodiscard]] Result<ContextCandidate> context_candidate_from_json(const JsonValue &json);

} // namespace mira
