#pragma once

#include <mira/memory_contracts.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Embedding supply for the memory vector leg (design Context/Memory §14.3)
// ---------------------------------------------------------------------------

// The memory vector index is a rebuildable projection (RULE-07): losing it
// costs recall quality, never correctness, and every vector can be
// re-derived from the authoritative record. The embedding model itself is
// isolated behind this contract — Core owns no inference, exactly as for the
// model provider and the context embedder: the host supplies the
// implementation and owns its Executor routing.
class IEmbeddingProvider {
  public:
    virtual ~IEmbeddingProvider() = default;

    // One vector for one text. The dimension is the provider's choice; a
    // dimension that does not match the indexed vectors degrades the vector
    // leg, never the query.
    [[nodiscard]] virtual Result<std::vector<float>> embed(std::string_view text) = 0;

    // Batch supply for repair passes. The default loops over embed(); hosts
    // with a batch API override it. The results keep the input order.
    [[nodiscard]] virtual Result<std::vector<std::vector<float>>>
    embed_batch(std::span<const std::string> texts);
};

// The projection sink. A durable store without a vector leg does not
// implement it; the memory query contract still reports index lag, and the
// vector leg degrades to exact/lexical recall.
class IMemoryEmbeddingIndex {
  public:
    virtual ~IMemoryEmbeddingIndex() = default;

    // Attaches (or replaces) the vector of one record. Empty or oversized
    // vectors are rejected so corrupt supply surfaces as index lag rather
    // than query poisoning.
    [[nodiscard]] virtual Result<void> index_embedding(MemoryId record,
                                                       std::span<const float> vector) = 0;
    // Whether a record currently carries a usable vector.
    [[nodiscard]] virtual Result<bool> has_embedding(MemoryId record) const = 0;
    // Active records without a usable vector.
    [[nodiscard]] virtual Result<std::size_t> index_lag() const = 0;
    // Drops every vector (e.g. after an embedding-model change); returns how
    // many were dropped.
    [[nodiscard]] virtual Result<std::size_t> clear_embeddings() = 0;
};

// What one repair pass achieved.
struct MemoryEmbeddingRepairReport final {
    // Active records the pass looked at.
    std::size_t scanned = 0;
    // Records that received a vector.
    std::size_t repaired = 0;
    // Records whose embedding failed; the pass continued past them.
    std::size_t failed = 0;
    // Records whose kind does not use semantic retrieval at all (design
    // Context/Memory §6.3): embedding them would buy nothing.
    std::size_t ineligible = 0;
    // Index-wide lag after the pass (not scope-local).
    std::size_t index_lag = 0;
};

struct MemoryEmbeddingRepairOptions final {
    // Upper bound on records examined in one pass.
    std::size_t max_records = 512;
    // Soft deadline for the enumeration query; exceeding it returns the
    // records gathered so far.
    std::chrono::milliseconds deadline{30'000};

    [[nodiscard]] Result<void> validate() const;
};

// Drives the memory vector projection from a producer.
//
// Every operation is a synchronous facade: hosts run them on
// Executor-managed workers. An embedding failure degrades the vector leg
// instead of failing the caller — the authoritative records stay readable
// and the projection is repaired later (design Context/Memory §14.1/§16.2).
//
// The write path is deliberately not wired here: embedding on every write
// would put a model call on the control path, and the vector index is a
// rebuildable projection (design Context/Memory §23).
class MemoryEmbeddingIndexer final {
  public:
    MemoryEmbeddingIndexer(IMemoryEmbeddingIndex &index, IEmbeddingProvider &provider);

    MemoryEmbeddingIndexer(const MemoryEmbeddingIndexer &) = delete;
    MemoryEmbeddingIndexer &operator=(const MemoryEmbeddingIndexer &) = delete;

    // Embeds one record's text and attaches the vector.
    //
    // A kind that withholds semantic retrieval (design Context/Memory §6.3)
    // is refused: its statement is a canonical structured record, and
    // embedding it would spend a model call on a vector no query can use.
    [[nodiscard]] Result<void> index_record(const MemoryRecord &record);

    // Bounded repair over one scope: embeds the Active records that carry no
    // vector yet. A record whose embedding fails is counted and skipped, not
    // fatal.
    //
    // The pass sees the newest records of the scope first and is bounded by
    // `max_records`; a scope holding more Active records than that needs
    // the host to drive index_record() for the remaining ids.
    [[nodiscard]] Result<MemoryEmbeddingRepairReport>
    repair(IMemory &memory, const MemoryScope &scope,
           MemoryEmbeddingRepairOptions options = MemoryEmbeddingRepairOptions{});

    // The query vector for MemoryQuery::query_embedding. A failure is the
    // caller's to handle: the vector leg is optional, the lexical and exact
    // legs are not.
    [[nodiscard]] Result<std::vector<float>> embed_query(std::string_view text);

  private:
    IMemoryEmbeddingIndex &index_;
    IEmbeddingProvider &provider_;
};

} // namespace mira
