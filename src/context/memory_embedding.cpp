#include <mira/memory_embedding.hpp>

#include <algorithm>
#include <utility>

namespace mira {

namespace {

// Whether a kind is reachable by embedding similarity at all (design
// Context/Memory §6.3). A canonical structured record — an episode, a
// procedure, a recovery lesson, a skill hint — is recalled by identifier
// and keyword, so embedding it would spend a model call on a vector no
// query can use.
[[nodiscard]] bool semantic_eligible(MemoryKind kind) {
    const auto &policy = memory_kind_policy(kind);
    return std::find(policy.retrieval_legs.begin(), policy.retrieval_legs.end(),
                     MemoryRetrievalLeg::Semantic) != policy.retrieval_legs.end();
}

} // namespace

Result<std::vector<std::vector<float>>>
IEmbeddingProvider::embed_batch(std::span<const std::string> texts) {
    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    for (const auto &text : texts) {
        auto embedded = embed(text);
        if (!embedded) {
            return embedded.error();
        }
        vectors.push_back(std::move(embedded).value());
    }
    return vectors;
}

Result<void> MemoryEmbeddingRepairOptions::validate() const {
    if (max_records == 0 || max_records > 512) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "repair max_records must be within (0, 512]");
    }
    if (deadline.count() < 0) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "repair deadline must not be negative");
    }
    return Result<void>{};
}

MemoryEmbeddingIndexer::MemoryEmbeddingIndexer(IMemoryEmbeddingIndex &index,
                                               IEmbeddingProvider &provider)
    : index_(index), provider_(provider) {}

Result<void> MemoryEmbeddingIndexer::index_record(const MemoryRecord &record) {
    if (!semantic_eligible(record.kind)) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "memory kind does not use semantic retrieval");
    }
    auto embedded = provider_.embed(record.statement);
    if (!embedded) {
        return embedded.error();
    }
    const auto &values = embedded.value();
    return index_.index_embedding(record.id, std::span<const float>(values));
}

Result<std::vector<float>> MemoryEmbeddingIndexer::embed_query(std::string_view text) {
    auto embedded = provider_.embed(text);
    if (!embedded) {
        return embedded.error();
    }
    return embedded;
}

Result<MemoryEmbeddingRepairReport>
MemoryEmbeddingIndexer::repair(IMemory &memory, const MemoryScope &scope,
                               MemoryEmbeddingRepairOptions options) {
    const auto valid = options.validate();
    if (!valid) {
        return valid.error();
    }
    // Enumerate the scope's Active records. The vector leg needs no kind
    // filter here: the projection covers every kind that declares Semantic
    // retrieval eligible (design Context/Memory §6.3), and a vector for a
    // kind that withholds the leg is inert, not harmful.
    MemoryQuery query;
    query.scopes = {scope};
    query.max_results = options.max_records;
    query.deadline = options.deadline;
    auto found = memory.query(query);
    if (!found) {
        return found.error();
    }
    MemoryEmbeddingRepairReport report;
    for (const auto &record : found.value().records) {
        ++report.scanned;
        if (!semantic_eligible(record.kind)) {
            // The kind is not reachable by similarity; a vector would be
            // inert, so the pass does not spend a model call on it.
            ++report.ineligible;
            continue;
        }
        const auto present = index_.has_embedding(record.id);
        if (!present) {
            return present.error();
        }
        if (present.value()) {
            continue; // the projection is current for this record
        }
        const auto embedded = provider_.embed(record.statement);
        if (!embedded) {
            // A producer failure degrades the vector leg; the record stays
            // authoritative and the next pass retries it.
            ++report.failed;
            continue;
        }
        const auto &values = embedded.value();
        const auto attached = index_.index_embedding(record.id, std::span<const float>(values));
        if (!attached) {
            ++report.failed;
            continue;
        }
        ++report.repaired;
    }
    const auto lag = index_.index_lag();
    if (!lag) {
        return lag.error();
    }
    report.index_lag = lag.value();
    return report;
}

} // namespace mira
