#pragma once

#include <mira/artifact_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/state_store.hpp>

#include <memory>
#include <span>

namespace mira {

// ---------------------------------------------------------------------------
// Durable SQLite/WAL long-term memory reference backend (M4-09..M4-13)
// ---------------------------------------------------------------------------

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

    [[nodiscard]] Result<void> validate() const;
};

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
};

// IMemory over SQLite/WAL. One Executor blocking-I/O worker owns the single
// connection (single writer, bounded admission); FTS5 carries the lexical
// leg and a bounded linear cosine scan carries the semantic leg. The vector
// index is a rebuildable projection: corruption or dimension mismatch only
// degrades retrieval quality and never blocks the control plane.
class SqliteMemoryStore final : public IMemory {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteMemoryStore>>
    open(executor::Executor &executor, SqliteMemoryStoreOptions options,
         IArtifactStore *artifacts = nullptr);
    ~SqliteMemoryStore() override;

    SqliteMemoryStore(const SqliteMemoryStore &) = delete;
    SqliteMemoryStore &operator=(const SqliteMemoryStore &) = delete;

    [[nodiscard]] Result<MemoryQueryResult> query(const MemoryQuery &query) const override;
    [[nodiscard]] Result<std::optional<MemoryRecord>> get(MemoryId record) const override;
    [[nodiscard]] Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override;
    [[nodiscard]] Result<MemoryCompactionResult> compact(const MemoryScope &scope) override;
    [[nodiscard]] Result<ErasureResult> erase(const ErasureRequest &request) override;

    // ---- Index maintenance (rebuildable projections) ----

    // Attaches an embedding for one record. Vectors are a projection of the
    // authoritative record; empty or oversized vectors are rejected so a
    // corrupt entry surfaces as index lag instead of poisoning queries.
    [[nodiscard]] Result<void> index_embedding(MemoryId record, std::span<const float> vector);
    // Number of Active records without a usable embedding.
    [[nodiscard]] Result<std::size_t> index_lag() const;
    // Drops every embedding (e.g. after an embedding-model change); records
    // stay authoritative and retrieval degrades to exact/FTS until rebuilt.
    [[nodiscard]] Result<std::size_t> clear_embeddings();

    [[nodiscard]] const StoreDiagnostics &diagnostics() const noexcept;
    // Queued-but-not-executed request count (bounded-queue observability).
    [[nodiscard]] std::size_t pending_requests() const;
    void set_worker_paused(bool paused);
    Result<void> close() noexcept;

  private:
    class Impl;
    explicit SqliteMemoryStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
