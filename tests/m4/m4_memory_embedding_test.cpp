#include "../support/test.hpp"

#include <mira/memory_embedding.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {

using namespace mira;

constexpr std::size_t kDims = 16;

// Deterministic token-hash embedding producer: FNV-1a per token into signed
// slots, L2-normalized. Two texts that share tokens land near each other,
// so the vector leg has a deterministic answer in tests.
class HashingProvider final : public IEmbeddingProvider {
  public:
    std::atomic<int> calls{0};
    bool fail = false;
    // Tests that need a second identity override this after construction.
    EmbedderId id = EmbedderId::generate();

    EmbedderId embedder_id() const override { return id; }

    Result<std::vector<float>> embed(std::string_view text) override {
        ++calls;
        if (fail) {
            return make_memory_error(MemoryDomainCode::IndexDegraded, "producer unavailable");
        }
        std::vector<float> values(kDims, 0.0F);
        std::string token;
        const auto flush = [&]() {
            if (token.empty()) {
                return;
            }
            std::uint32_t hash = 2'166'136'261U;
            for (const char character : token) {
                hash ^= static_cast<std::uint8_t>(character);
                hash *= 16'777'619U;
            }
            values[hash % kDims] += (hash & 0x8000U) != 0U ? -1.0F : 1.0F;
            token.clear();
        };
        for (const char character : text) {
            const bool word = (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9') || character == '_';
            if (word) {
                token += character;
            } else {
                flush();
            }
        }
        flush();
        double norm = 0.0;
        for (const float value : values) {
            norm += static_cast<double>(value) * static_cast<double>(value);
        }
        if (norm > 0.0) {
            const double scale = 1.0 / std::sqrt(norm);
            for (auto &value : values) {
                value = static_cast<float>(static_cast<double>(value) * scale);
            }
        }
        return values;
    }
};

// A fresh directory per process: every test function opens its own store
// file, so the counts one test observes are never another's leftovers.
[[nodiscard]] std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-memory-embedding-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

[[nodiscard]] MemoryScope env_scope(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    return scope;
}

[[nodiscard]] MemoryRecord record(const MemoryScope &scope, std::string statement,
                                  MemoryKind kind = MemoryKind::EnvironmentFact) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = scope;
    value.kind = kind;
    value.statement = std::move(statement);
    value.recorded_at = std::chrono::system_clock::now();
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    value.verification = MemoryVerification::Verified;
    value.confidence = 0.8F;
    return value;
}

[[nodiscard]] MemoryMutation add_mutation(const MemoryRecord &proposed) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.type = MemoryMutationType::Add;
    mutation.scope = proposed.scope;
    mutation.proposed = proposed;
    mutation.evidence = proposed.provenance;
    return mutation;
}

struct Fixture final {
    explicit Fixture(const char *file) {
        executor_.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / file;
        auto opened = SqliteMemoryStore::open(executor_, options);
        store = std::move(opened).value();
    }
    ~Fixture() {
        if (store != nullptr) {
            (void)store->close();
        }
        (void)executor_.shutdown(true);
    }
    kairo::Executor executor_;
    std::unique_ptr<SqliteMemoryStore> store;
};

// The producer seam and the projection sink: attach, probe, count, clear.
int provider_and_index_contract() {
    Fixture fixture("memory-embedding.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider provider;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);

    // A record without a vector of this embedder is index lag.
    const auto lag_before = fixture.store->index_lag(provider.id);
    MIRA_CHECK(lag_before.has_value() && lag_before.value() == 1);
    const auto absent = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(absent.has_value() && !absent.value());

    MIRA_CHECK(indexer.index_record(fact).has_value());
    MIRA_CHECK(provider.calls.load() == 1);

    const auto present = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(present.has_value() && present.value());
    const auto lag_after = fixture.store->index_lag(provider.id);
    MIRA_CHECK(lag_after.has_value() && lag_after.value() == 0);

    // A kind that withholds semantic retrieval is refused: the vector would
    // be inert (design Context/Memory §6.3).
    auto episode = record(scope, "task settled: outcome=completed sequence=3", MemoryKind::Episode);
    episode.verification = MemoryVerification::Observed;
    MIRA_CHECK(fixture.store->apply(add_mutation(episode)).has_value());
    const auto refused = indexer.index_record(episode);
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().domain_code ==
               static_cast<std::int32_t>(MemoryDomainCode::InvalidRecord));
    MIRA_CHECK(provider.calls.load() == 1); // no model call was spent

    // Clearing this embedder's projection leaves the records authoritative.
    const auto cleared = fixture.store->clear_embeddings(provider.id);
    MIRA_CHECK(cleared.has_value() && cleared.value() == 1);
    const auto gone = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(gone.has_value() && !gone.value());
    MIRA_CHECK(fixture.store->get(fact.id, scope).has_value());
    return 0;
}

// The vector leg answers from the projection: a query carrying the producer's
// vector finds the record the lexical leg would miss.
int semantic_leg_answers_from_the_projection() {
    Fixture fixture("semantic-leg.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider provider;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);
    MIRA_CHECK(indexer.index_record(fact).has_value());

    MemoryQuery query;
    query.scopes = {scope};
    query.text = "indicator amber";
    auto lexical = fixture.store->query(query);
    MIRA_CHECK(lexical.has_value());
    MIRA_CHECK(lexical.value().quality.fts_leg_ran);

    // A query that only carries the vector still reaches the record.
    MemoryQuery semantic;
    semantic.scopes = {scope};
    const auto embedded = indexer.embed_query("the oven indicator is amber");
    MIRA_CHECK(embedded.has_value());
    semantic.query_embedding = embedded.value();
    auto found = fixture.store->query(semantic);
    MIRA_CHECK(found.has_value());
    MIRA_CHECK(found.value().quality.vector_leg_ran);
    MIRA_CHECK(!found.value().records.empty());
    MIRA_CHECK(found.value().records.front().id == fact.id);
    return 0;
}

// The repair pass closes the gap between the records and the projection.
int repair_embeds_records_without_vectors() {
    Fixture fixture("repair.db");
    const auto scope = env_scope("device-42");
    for (int index = 0; index < 3; ++index) {
        auto fact = record(scope, "the oven indicator is amber " + std::to_string(index));
        MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());
    }
    // An episode is not reachable by similarity; the pass must not spend a
    // model call on it.
    auto episode = record(scope, "task settled: outcome=completed sequence=3", MemoryKind::Episode);
    episode.verification = MemoryVerification::Observed;
    MIRA_CHECK(fixture.store->apply(add_mutation(episode)).has_value());

    HashingProvider provider;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);
    const auto report = indexer.repair(*fixture.store, scope);
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().scanned == 4);
    MIRA_CHECK(report.value().repaired == 3);
    MIRA_CHECK(report.value().failed == 0);
    MIRA_CHECK(report.value().ineligible == 1);
    MIRA_CHECK(report.value().index_lag == 1); // the episode is not eligible
    MIRA_CHECK(provider.calls.load() == 3);

    // A second pass finds nothing left to do.
    const auto again = indexer.repair(*fixture.store, scope);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value().repaired == 0);
    MIRA_CHECK(provider.calls.load() == 3);
    return 0;
}

// A producer failure degrades the vector leg; the records stay retrievable
// through the other legs.
int producer_failure_degrades_the_leg() {
    Fixture fixture("producer-failure.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider provider;
    provider.fail = true;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);
    const auto report = indexer.repair(*fixture.store, scope);
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().repaired == 0);
    MIRA_CHECK(report.value().failed == 1);
    MIRA_CHECK(report.value().index_lag == 1);

    const auto failed = indexer.index_record(fact);
    MIRA_CHECK(!failed.has_value());

    // The lexical leg still answers.
    MemoryQuery query;
    query.scopes = {scope};
    query.text = "oven indicator";
    auto found = fixture.store->query(query);
    MIRA_CHECK(found.has_value());
    MIRA_CHECK(!found.value().records.empty());
    MIRA_CHECK(found.value().records.front().id == fact.id);
    return 0;
}

// The projection must not answer with the previous text: an update that
// changes the statement drops the stale vector and the record becomes lag
// again.
int stale_vector_is_dropped_when_the_statement_changes() {
    Fixture fixture("stale.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider provider;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);
    MIRA_CHECK(indexer.index_record(fact).has_value());
    const auto attached = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(attached.has_value() && attached.value());

    MemoryMutation update;
    update.id = MutationId::generate();
    update.type = MemoryMutationType::Update;
    update.scope = scope;
    update.target = fact.id;
    update.expected_version = 1;
    update.proposed = fact;
    update.proposed.statement = "the oven indicator is green";
    update.evidence = update.proposed.provenance;
    update.reason = MutationReasonCode::VerifiedEvent;
    MIRA_CHECK(fixture.store->apply(update).has_value());

    const auto dropped = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(dropped.has_value() && !dropped.value());
    const auto lag = fixture.store->index_lag(provider.id);
    MIRA_CHECK(lag.has_value() && lag.value() == 1);

    // The repair pass re-embeds the new text.
    const auto report = indexer.repair(*fixture.store, scope);
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().repaired == 1);
    MIRA_CHECK(report.value().index_lag == 0);
    return 0;
}

// A tombstone retires the record, so its vector stops describing a
// retrievable record.
int tombstone_drops_the_vector() {
    Fixture fixture("tombstone.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider provider;
    MemoryEmbeddingIndexer indexer(*fixture.store, provider);
    MIRA_CHECK(indexer.index_record(fact).has_value());

    MemoryMutation tombstone;
    tombstone.id = MutationId::generate();
    tombstone.type = MemoryMutationType::Tombstone;
    tombstone.scope = scope;
    tombstone.target = fact.id;
    tombstone.expected_version = 1;
    tombstone.proposed = fact;
    tombstone.evidence = fact.provenance;
    tombstone.reason = MutationReasonCode::HumanCorrection;
    MIRA_CHECK(fixture.store->apply(tombstone).has_value());

    const auto dropped = fixture.store->has_embedding(fact.id, provider.id);
    MIRA_CHECK(dropped.has_value() && !dropped.value());
    // The record itself stays readable for audit (design §13.4).
    const auto retained = fixture.store->get(fact.id, scope);
    MIRA_CHECK(retained.has_value() && retained.value().has_value());
    return 0;
}

// Vectors of different embedders never meet in the cosine leg: probes, lag
// and queries are answered per identity, and a query carrying a foreign
// identity surfaces the mismatch as a vector error instead of silently
// answering "no rows" (DEC-111).
int vectors_are_quarantined_per_embedder() {
    Fixture fixture("identity-quarantine.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "the oven indicator is amber");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    HashingProvider first;
    MemoryEmbeddingIndexer first_indexer(*fixture.store, first);
    MIRA_CHECK(first_indexer.index_record(fact).has_value());

    HashingProvider second; // different identity, same geometry source
    MemoryEmbeddingIndexer second_indexer(*fixture.store, second);
    MIRA_CHECK(!(first.embedder_id() == second.embedder_id()));

    // Probes and lag are per identity.
    const auto seen_by_first = fixture.store->has_embedding(fact.id, first.id);
    const auto seen_by_second = fixture.store->has_embedding(fact.id, second.id);
    MIRA_CHECK(seen_by_first.has_value() && seen_by_first.value());
    MIRA_CHECK(seen_by_second.has_value() && !seen_by_second.value());
    const auto lag_first = fixture.store->index_lag(first.id);
    const auto lag_second = fixture.store->index_lag(second.id);
    MIRA_CHECK(lag_first.has_value() && lag_first.value() == 0);
    MIRA_CHECK(lag_second.has_value() && lag_second.value() == 1);

    // A query carrying the producer's identity answers from the vector leg.
    MemoryQuery owned;
    owned.scopes = {scope};
    const auto owned_vector = first_indexer.embed_query("the oven indicator is amber");
    MIRA_CHECK(owned_vector.has_value());
    owned.query_embedding = owned_vector.value();
    auto owned_result = fixture.store->query(owned);
    MIRA_CHECK(owned_result.has_value() && owned_result.value().quality.vector_leg_ran);
    MIRA_CHECK(!owned_result.value().records.empty());

    // The same query under the foreign identity refuses to compare: the
    // mismatch counts as a vector error and degrades the leg visibly.
    MemoryQuery foreign;
    foreign.scopes = {scope};
    const auto foreign_vector = second_indexer.embed_query("the oven indicator is amber");
    MIRA_CHECK(foreign_vector.has_value());
    foreign.query_embedding = foreign_vector.value();
    auto foreign_result = fixture.store->query(foreign);
    MIRA_CHECK(foreign_result.has_value());
    MIRA_CHECK(!foreign_result.value().quality.vector_leg_ran);
    MIRA_CHECK(foreign_result.value().quality.vector_degraded);
    MIRA_CHECK(foreign_result.value().records.empty()); // no cross-identity hit

    // Repairing under the second identity re-embeds the record and
    // re-attributes the projection: one record carries one vector (the
    // statement's projection), so the old identity truthfully reports the
    // record as lagging after the switch (DEC-111).
    const auto report = second_indexer.repair(*fixture.store, scope);
    MIRA_CHECK(report.has_value() && report.value().repaired == 1);
    MIRA_CHECK(fixture.store->has_embedding(fact.id, second.id).value());
    MIRA_CHECK(fixture.store->index_lag(second.id).value() == 0);
    const auto stale_first = fixture.store->has_embedding(fact.id, first.id);
    MIRA_CHECK(stale_first.has_value() && !stale_first.value());
    MIRA_CHECK(fixture.store->index_lag(first.id).value() == 1);

    // Clearing the retired identity removes nothing; clearing the active
    // one drops the single projection row.
    const auto cleared_stale = fixture.store->clear_embeddings(first.id);
    MIRA_CHECK(cleared_stale.has_value() && cleared_stale.value() == 0);
    const auto cleared_active = fixture.store->clear_embeddings(second.id);
    MIRA_CHECK(cleared_active.has_value() && cleared_active.value() == 1);
    MIRA_CHECK(!fixture.store->has_embedding(fact.id, second.id).value());
    return 0;
}

// Plumbing proof for the semantic prior: a paraphrase query that shares no
// lexical token with the statement is answered by the vector leg alone.
// (Retrieval quality with a real embedding model is evidence for the
// real-provider smoke run recorded in the maintenance plan, not this test.)
int paraphrase_is_answered_by_the_vector_leg() {
    Fixture fixture("paraphrase.db");
    const auto scope = env_scope("device-42");
    auto fact = record(scope, "用户偏好简洁回复");
    MIRA_CHECK(fixture.store->apply(add_mutation(fact)).has_value());

    // Hand-authored geometry: the statement projects onto +x, the paraphrase
    // query nearly onto +x; an unrelated record projects onto +y.
    const std::vector<float> statement_vector{1.0F, 0.0F};
    const std::vector<float> noise_vector{0.0F, 1.0F};
    auto unrelated = record(scope, "白板马克笔已干");
    MIRA_CHECK(fixture.store->apply(add_mutation(unrelated)).has_value());
    const EmbedderId producer = EmbedderId::generate();
    MIRA_CHECK(fixture.store->index_embedding(fact.id, producer, statement_vector).has_value());
    MIRA_CHECK(fixture.store->index_embedding(unrelated.id, producer, noise_vector).has_value());

    // The lexical leg cannot bridge the paraphrase.
    MemoryQuery lexical;
    lexical.scopes = {scope};
    lexical.text = "别太啰嗦";
    const auto lexical_result = fixture.store->query(lexical);
    MIRA_CHECK(lexical_result.has_value() && lexical_result.value().records.empty());

    // The vector leg answers and ranks the aligned statement first.
    MemoryQuery semantic;
    semantic.scopes = {scope};
    semantic.query_embedding = MemoryEmbedding{{0.95F, 0.05F}, producer};
    const auto semantic_result = fixture.store->query(semantic);
    MIRA_CHECK(semantic_result.has_value());
    MIRA_CHECK(semantic_result.value().quality.vector_leg_ran);
    MIRA_CHECK(!semantic_result.value().records.empty());
    MIRA_CHECK(semantic_result.value().records.front().id == fact.id);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += provider_and_index_contract();
    failures += semantic_leg_answers_from_the_projection();
    failures += repair_embeds_records_without_vectors();
    failures += producer_failure_degrades_the_leg();
    failures += stale_vector_is_dropped_when_the_statement_changes();
    failures += tombstone_drops_the_vector();
    failures += vectors_are_quarantined_per_embedder();
    failures += paraphrase_is_answered_by_the_vector_leg();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
