#pragma once

#include <mira/artifact_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/state_store.hpp>

#include <memory>
#include <span>

namespace mira {

// ---------------------------------------------------------------------------
// Durable SQLite/WAL long-term memory reference backend (M4-09..M4-13)
// ---------------------------------------------------------------------------

// Hybrid retrieval weights (lexical_exact, fts_rank, vector_similarity,
// verification, confidence, recency). Equal weights mean "leg ran";
// implementation details live in the .cpp.
struct RetrievalWeights final {
    double exact = 0.30;
    double fts = 0.20;
    double vector = 0.30;
    double verification = 0.10;
    double confidence = 0.05;
    double recency = 0.05;
    // Prior over the record's declared importance (DEC-112 metadata).
    // Defaults to zero so the shipped ranking is unchanged; hosts opt in.
    double importance = 0.0;

    [[nodiscard]] Result<void> validate() const;
};

// How the ranking legs fuse into one score (DEC-112, DEC-113).
// ReciprocalRank is the shipped default (DEC-113 flipped it after the
// HR-11 quality benchmark): it fuses the FTS and cosine legs by rank
// (weighted RRF, k=60, normalized to [0,1]) so incomparable score
// distributions cannot flip the lexical ordering — and with no query
// vector it degenerates to exactly the lexical ordering. WeightedSum
// keeps the historical score-linear behavior; hosts that prefer the
// top-hit emphasis of a weighted blend (and do not rerank) opt back in.
enum class RetrievalFusion : std::uint8_t {
    WeightedSum,
    ReciprocalRank,
};

// Tuning knobs for the reference backend. Ranking weights are documented
// defaults, not a frozen contract (design Context/Memory §14.2).
struct SqliteMemoryStoreOptions final {
    std::filesystem::path path;
    bool read_only_diagnostic = false;
    std::size_t max_pending_requests = 256;
    std::chrono::milliseconds busy_timeout{5'000};
    std::chrono::milliseconds operation_timeout{30'000};
    // Vector leg bound: cosine scans at most this many embeddings per query.
    std::size_t max_vector_scan = 1'024;
    // Diversity cap: at most this many results per (scope, kind) group.
    std::size_t max_per_scope_kind = 3;
    // Per-record version history kept for bitemporal as-of replay.
    std::size_t keep_versions_per_record = 2;
    // Tombstones and superseded history older than this are purged by
    // compact(); zero keeps them forever.
    std::chrono::seconds purge_horizon{0};
    // Fusion mode of the ranking legs (DEC-112, DEC-113). ReciprocalRank is
    // the default: rank-based fusion cannot be flipped by incomparable leg
    // score distributions, and without a query vector it orders exactly like
    // the lexical leg. Set WeightedSum to restore the historical blend.
    RetrievalFusion fusion = RetrievalFusion::ReciprocalRank;
    // Near-duplicate suppression over the ranked head (DEC-112): candidates
    // whose embedding cosine against an already-kept candidate reaches this
    // threshold are dropped. 0.0 disables the pass; the threshold is a
    // per-embedder empirical value (bge-m3 paraphrase near-dups measured
    // ~0.86) and must be recalibrated when the embedder changes.
    double near_dup_threshold = 0.0;
    // Ranking weights (documented defaults; design Context/Memory §14.2).
    // The importance prior defaults to zero: metadata never changes the
    // shipped ranking unless the host opts in.
    RetrievalWeights weights;

    // DEC-111: optional aggregate file-length budget for this exclusively
    // owned database and its WAL/SHM/rollback journal. Zero preserves legacy
    // unbounded behavior. Does not include artifacts, events or filesystem
    // block/directory overhead. Existing oversized files fail open unchanged.
    std::uint64_t max_storage_bytes = 0;
    std::uint64_t capacity_reserve_bytes = 64 * 1024;

    [[nodiscard]] Result<void> validate() const;
};

struct SqliteMemoryCapacitySnapshot final {
    std::uint64_t limit_bytes = 0;
    std::uint64_t reserve_bytes = 0;
    std::uint64_t database_bytes = 0;
    std::uint64_t wal_bytes = 0;
    std::uint64_t shm_bytes = 0;
    std::uint64_t journal_bytes = 0;
    std::uint64_t file_bytes = 0;
    // Process-local high watermark of admitted file lengths, including a
    // reservation whose underlying I/O may fail. Not physical block usage.
    std::uint64_t peak_admitted_file_bytes = 0;
};

// IMemory over SQLite/WAL. One Executor blocking-I/O worker owns the single
// connection (single writer, bounded admission); FTS5 carries the lexical
// leg and a bounded linear cosine scan carries the semantic leg. The vector
// index is a rebuildable projection: corruption or dimension mismatch only
// degrades retrieval quality and never blocks the control plane.
//
// The store also implements IMemoryEmbeddingIndex, so a host that supplies an
// IEmbeddingProvider can drive the projection (design Context/Memory §14.3),
// and IMemoryRetentionSweep, so the store owner can run a bounded retention
// pass over every scope (design Context/Memory §16.4).
class SqliteMemoryStore final : public IMemory,
                                public IMemoryEmbeddingIndex,
                                public IMemoryRetentionSweep {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteMemoryStore>>
    open(kairo::Executor &executor, SqliteMemoryStoreOptions options,
         IArtifactStore *artifacts = nullptr);
    ~SqliteMemoryStore() override;

    SqliteMemoryStore(const SqliteMemoryStore &) = delete;
    SqliteMemoryStore &operator=(const SqliteMemoryStore &) = delete;

    [[nodiscard]] Result<MemoryQueryResult> query(const MemoryQuery &query) const override;
    [[nodiscard]] Result<std::optional<MemoryRecord>> get(MemoryId record,
                                                          const MemoryScope &scope) const override;
    [[nodiscard]] Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override;
    [[nodiscard]] Result<MemoryCompactionResult> compact(const MemoryScope &scope) override;
    [[nodiscard]] Result<ErasureResult> erase(const ErasureRequest &request) override;

    // ---- Index maintenance (rebuildable projections) ----

    // Attaches an embedding for one record, owned by one embedder identity
    // (DEC-111). Vectors are a projection of the authoritative record; empty
    // or oversized vectors are rejected so a corrupt entry surfaces as index
    // lag instead of poisoning queries.
    [[nodiscard]] Result<void> index_embedding(MemoryId record, EmbedderId embedder,
                                               std::span<const float> vector) override;
    // Whether a record currently carries a vector of this embedder.
    [[nodiscard]] Result<bool> has_embedding(MemoryId record, EmbedderId embedder) const override;
    // Number of Active records without a vector of this embedder; a producer
    // switch therefore reports the full rebuild scope as lag.
    [[nodiscard]] Result<std::size_t> index_lag(EmbedderId embedder) const override;
    // Drops every vector of this embedder (e.g. before a re-index); records
    // stay authoritative and retrieval degrades to exact/FTS until rebuilt.
    [[nodiscard]] Result<std::size_t> clear_embeddings(EmbedderId embedder) override;

    // ---- Retention (cross-scope sweep) ----

    // One bounded pass over every scope: expired records, version history
    // and stale tombstones are reclaimed. The caller repeats the pass
    // while the result reports more_pending (design Context/Memory §16.4).
    [[nodiscard]] Result<MemoryCompactionResult> sweep(MemorySweepOptions options) override;

    [[nodiscard]] const StoreDiagnostics &diagnostics() const noexcept;
    // Queued-but-not-executed request count (bounded-queue observability).
    [[nodiscard]] std::size_t pending_requests() const;
    void set_worker_paused(bool paused);
    Result<void> close() noexcept;
    [[nodiscard]] Result<SqliteMemoryCapacitySnapshot> capacity_snapshot() const;

  private:
    class Impl;
    explicit SqliteMemoryStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
