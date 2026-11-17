// Deterministic store-level fusion comparison (DEC-112): WeightedSum (the
// shipped default) vs ReciprocalRank over a fixed corpus, plus gates for the
// labels/importance metadata and the near-duplicate suppression. All ids and
// timestamps are counter-built (m17/m18 technique) so every run of one mode
// - and both modes against each other - see identical inputs.
#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {

using namespace mira;

// MIRA_CHECK returns 1 and only fits int-returning test functions; helper
// lambdas and non-int members use this aborting variant instead.
#define MIRA_REQUIRE(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << __FILE__ << ':' << __LINE__ << ": fatal check failed: " #cond << '\n';    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

// Staggered live timestamps: relative order is deterministic (kind TTL adds
// a 7-day expiry at write time, so the records must be anchored near now).
[[nodiscard]] std::chrono::system_clock::time_point staggered_time(std::uint64_t ordinal) {
    return std::chrono::system_clock::now() + std::chrono::seconds(static_cast<int64_t>(ordinal));
}

[[nodiscard]] MemoryScope env_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "fusion-lab";
    return scope;
}

struct Seeded final {
    const char *statement;
    std::vector<float> vector;
    std::vector<std::string> labels;
    float importance = 0.5F;
};

// Fixed geometry: the paraphrase pair projects onto +x, the keyword record
// onto +y, noise onto -x (filtered by the non-negative cosine gate).
const Seeded kCorpus[] = {
    {"API_KEY 环境变量必须在重启前设置", {0.0F, 1.0F}, {"ops"}, 0.5F},
    {"用户偏好简洁回复", {1.0F, 0.0F}, {"tone"}, 0.5F},
    {"回答要尽量简短一些", {0.95F, 0.31F}, {"tone"}, 0.5F},
    {"白板马克笔已干", {-1.0F, 0.0F}, {}, 0.5F},
};

struct Fixture final {
    explicit Fixture(const char *file, RetrievalFusion fusion) {
        executor_.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / file;
        options.fusion = fusion;
        auto opened = SqliteMemoryStore::open(executor_, options);
        store = std::move(opened).value();
    }
    ~Fixture() {
        if (store != nullptr) {
            (void)store->close();
        }
        (void)executor_.shutdown(true);
    }

    [[nodiscard]] bool seed(const std::vector<std::string> &which) {
        const EmbedderId producer = EmbedderId::generate();
        producer_ = producer;
        std::size_t ordinal = 10;
        for (const auto &seeded : kCorpus) {
            const std::string id_hint = seeded.statement;
            if (std::find(which.begin(), which.end(), id_hint) == which.end()) {
                continue;
            }
            MemoryRecord record;
            record.id = MemoryId{ordinal_id(ordinal)};
            record.scope = env_scope();
            record.kind = MemoryKind::EnvironmentFact;
            record.statement = seeded.statement;
            record.labels = seeded.labels;
            record.importance = seeded.importance;
            // The paraphrase pair shares one timestamp so the recency prior
            // cannot decide between them (the leg must).
            record.recorded_at =
                staggered_time(id_hint == std::string_view("回答要尽量简短一些") ? 11 : ordinal);
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId{ordinal_id(ordinal + 500)}};
            record.verification = MemoryVerification::Verified;
            record.confidence = 0.8F;
            MemoryMutation mutation;
            mutation.id = MutationId{ordinal_id(ordinal + 1000)};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = record.scope;
            mutation.proposed = record;
            mutation.evidence = record.provenance;
            if (!store->apply(mutation).has_value() ||
                !store->index_embedding(record.id, producer, seeded.vector).has_value()) {
                return false;
            }
            auto readable = store->get(record.id, record.scope);
            if (!readable.has_value() || !readable.value().has_value()) {
                std::cerr << "[debug] seeded record not readable: " << record.statement << "\n";
                return false;
            }
            ids_[id_hint] = record.id;
            ++ordinal;
        }
        return true;
    }

    [[nodiscard]] MemoryId id_of(const std::string &statement) const { return ids_.at(statement); }
    [[nodiscard]] EmbedderId producer() const { return producer_; }

    [[nodiscard]] MemoryQueryResult run(const std::string &text, const float *embedding,
                                        const std::vector<std::string> *labels) const {
        MemoryQuery query;
        query.scopes = {env_scope()};
        query.text = text;
        if (embedding != nullptr) {
            query.query_embedding =
                MemoryEmbedding{std::vector<float>(embedding, embedding + 2), producer_};
        }
        if (labels != nullptr) {
            query.labels = *labels;
        }
        auto result = store->query(query);
        MIRA_REQUIRE(result.has_value());
        return std::move(result).value();
    }

    [[nodiscard]] static std::vector<std::string> all_corpus() {
        std::vector<std::string> all;
        for (const auto &seeded : kCorpus) {
            all.push_back(seeded.statement);
        }
        return all;
    }

    [[nodiscard]] static std::filesystem::path temp_dir() {
        static const auto dir = [] {
            auto base = std::filesystem::temp_directory_path();
            std::random_device device;
            auto path = base / ("mira-m4-fusion-eval-" + std::to_string(device()));
            std::filesystem::create_directories(path);
            return path;
        }();
        return dir;
    }

    kairo::Executor executor_;
    std::unique_ptr<SqliteMemoryStore> store;

  private:
    EmbedderId producer_;
    std::map<std::string, MemoryId> ids_;
};

[[nodiscard]] bool contains(const MemoryQueryResult &result, const std::string &statement) {
    for (const auto &record : result.records) {
        if (record.statement == statement) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::size_t position_of(const MemoryQueryResult &result,
                                      const std::string &statement) {
    for (std::size_t index = 0; index < result.records.size(); ++index) {
        if (result.records[index].statement == statement) {
            return index;
        }
    }
    return result.records.size();
}

constexpr const char *kKeyword = "API_KEY 环境变量必须在重启前设置";
constexpr const char *kParaphraseA = "用户偏好简洁回复";
constexpr const char *kParaphraseB = "回答要尽量简短一些";

// Archetype 1 (identifier): a vector signal aligned with the paraphrase pair
// flips the weighted ranking away from the verbatim keyword hit; the RRF
// fusion keeps the lexical rank on top. This is the documented behavioral
// difference the mode exists for (DEC-112, live-API probe Q2).
int identifier_query_ranking() {
    Fixture weighted("fusion-id-weighted.db", RetrievalFusion::WeightedSum);
    Fixture rrf("fusion-id-rrf.db", RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(weighted.seed(Fixture::all_corpus()));
    MIRA_CHECK(rrf.seed(Fixture::all_corpus()));
    const float embedding[2] = {0.95F, 0.31F};

    const auto weighted_result = weighted.run("API_KEY", embedding, nullptr);
    const auto rrf_result = rrf.run("API_KEY", embedding, nullptr);
    MIRA_CHECK(position_of(weighted_result, kKeyword) > position_of(weighted_result, kParaphraseB));
    MIRA_CHECK(position_of(rrf_result, kKeyword) < position_of(rrf_result, kParaphraseB));
    MIRA_CHECK(rrf_result.quality.vector_leg_ran);
    return 0;
}

// Archetype 2 (paraphrase): with no lexical overlap the vector leg answers
// alone in both modes; the semantic prior is the recall path for paraphrase.
int paraphrase_query_recalled_by_the_vector_leg() {
    Fixture weighted("fusion-para-weighted.db", RetrievalFusion::WeightedSum);
    Fixture rrf("fusion-para-rrf.db", RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(weighted.seed(Fixture::all_corpus()));
    MIRA_CHECK(rrf.seed(Fixture::all_corpus()));
    const float embedding[2] = {1.0F, 0.05F};

    const auto weighted_result = weighted.run("别太啰嗦", embedding, nullptr);
    const auto rrf_result = rrf.run("别太啰嗦", embedding, nullptr);
    MIRA_CHECK(weighted_result.quality.vector_leg_ran);
    MIRA_CHECK(rrf_result.quality.vector_leg_ran);
    MIRA_CHECK(position_of(weighted_result, kParaphraseA) == 0);
    MIRA_CHECK(position_of(rrf_result, kParaphraseA) == 0);
    MIRA_CHECK(position_of(weighted_result, kParaphraseA) <
               position_of(weighted_result, kParaphraseB));
    MIRA_CHECK(position_of(rrf_result, kParaphraseA) < position_of(rrf_result, kParaphraseB));
    return 0;
}

// The empty-answer discipline is fusion independent: a ranking request whose
// legs all came back empty returns empty, never a scope dump.
int empty_legs_return_empty_in_both_modes() {
    Fixture weighted("fusion-empty-weighted.db", RetrievalFusion::WeightedSum);
    Fixture rrf("fusion-empty-rrf.db", RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(weighted.seed(Fixture::all_corpus()));
    MIRA_CHECK(rrf.seed(Fixture::all_corpus()));

    // Pure lexical miss: the ranking request has no leg that matched, and
    // an empty answer must beat a scope dump in both fusion modes.
    const auto weighted_result = weighted.run("zzz qq", nullptr, nullptr);
    const auto rrf_result = rrf.run("zzz qq", nullptr, nullptr);
    MIRA_CHECK(weighted_result.records.empty());
    MIRA_CHECK(rrf_result.records.empty());
    return 0;
}

// Determinism: reopening the same store file and re-running the query
// reproduces the ranking byte-for-byte (ids, scores, order).
int reopen_reproduces_the_ranking() {
    const float embedding[2] = {0.95F, 0.31F};
    std::string first_order;
    {
        Fixture fixture("fusion-reopen.db", RetrievalFusion::ReciprocalRank);
        MIRA_CHECK(fixture.seed(Fixture::all_corpus()));
        const auto result = fixture.run("API_KEY", embedding, nullptr);
        for (const auto &record : result.records) {
            first_order += record.id.to_string() + "|";
        }
    }
    {
        Fixture fixture("fusion-reopen.db", RetrievalFusion::ReciprocalRank);
        MIRA_CHECK(fixture.seed(Fixture::all_corpus()));
        const auto result = fixture.run("API_KEY", embedding, nullptr);
        std::string second_order;
        for (const auto &record : result.records) {
            second_order += record.id.to_string() + "|";
        }
        MIRA_CHECK(first_order == second_order);
        MIRA_CHECK(!first_order.empty());
    }
    return 0;
}

// labels: an any-of hard prefilter inside the authorized candidate universe.
int labels_prefilter_narrows_the_universe() {
    Fixture fixture("fusion-labels.db", RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(fixture.seed(Fixture::all_corpus()));
    const std::vector<std::string> ops{"ops"};
    const auto filtered = fixture.run("", nullptr, &ops);
    MIRA_CHECK(filtered.records.size() == 1);
    MIRA_CHECK(contains(filtered, kKeyword));
    const auto unfiltered = fixture.run("", nullptr, nullptr);
    // No ranking requested, so every candidate participates - but the
    // (scope, kind) diversity cap still bounds one group to 3.
    MIRA_CHECK(unfiltered.records.size() == 3);
    return 0;
}

// importance: a retrieval prior that only acts when the host opts in
// (default weight zero keeps the shipped ranking unchanged). The two records
// share every other signal - identical timestamps, verification and
// confidence, and a query with no text and no embedding - so importance is
// the only differentiating term.
int importance_prior_is_opt_in() {
    const auto build = [&](const char *file, double importance_weight) {
        kairo::Executor executor;
        MIRA_REQUIRE(executor.initialize(kairo::ExecutorConfig{}));
        SqliteMemoryStoreOptions options;
        options.path = Fixture::temp_dir() / file;
        options.fusion = RetrievalFusion::ReciprocalRank;
        options.weights.verification = 0.0;
        options.weights.confidence = 0.0;
        options.weights.recency = 0.0;
        options.weights.importance = importance_weight;
        auto store = std::move(SqliteMemoryStore::open(executor, options)).value();
        const auto seed_one = [&](std::uint64_t ordinal, std::string statement, float value) {
            MemoryRecord record;
            record.id = MemoryId{ordinal_id(ordinal)};
            record.scope = env_scope();
            record.kind = MemoryKind::EnvironmentFact;
            record.statement = std::move(statement);
            record.importance = value;
            record.recorded_at = staggered_time(ordinal);
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId{ordinal_id(ordinal + 500)}};
            record.verification = MemoryVerification::Verified;
            record.confidence = 0.8F;
            MemoryMutation mutation;
            mutation.id = MutationId{ordinal_id(ordinal + 1000)};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = record.scope;
            mutation.proposed = record;
            mutation.evidence = record.provenance;
            MIRA_REQUIRE(store->apply(mutation).has_value());
            return record.id;
        };
        (void)seed_one(10, "high importance notice", 0.9F);
        (void)seed_one(11, "low importance notice", 0.1F);
        MemoryQuery query;
        query.scopes = {env_scope()};
        auto result = store->query(query);
        MIRA_REQUIRE(result.has_value());
        struct Outcome final {
            std::size_t high_position = 0;
            std::size_t count = 0;
        } outcome;
        outcome.count = result.value().records.size();
        outcome.high_position = position_of(result.value(), "high importance notice");
        MIRA_REQUIRE(store->close().has_value());
        MIRA_REQUIRE(executor.shutdown(true) == kairo::ShutdownResult::Completed);
        return std::pair<std::size_t, std::size_t>{outcome.high_position, outcome.count};
    };

    // Opted in: the prior reorders.
    const auto [prior_position, prior_count] = build("fusion-importance-on.db", 0.5);
    MIRA_CHECK(prior_count == 2);
    MIRA_CHECK(prior_position == 0);
    // Default weights: importance is inert, both records come back.
    const auto [neutral_position, neutral_count] = build("fusion-importance-off.db", 0.0);
    MIRA_CHECK(neutral_count == 2);
    MIRA_CHECK(neutral_position <= 1);
    return 0;
}

// Near-duplicate suppression: threshold on -> the near pair collapses with a
// count; threshold off -> both survive; a record without a vector of the
// query identity is exempt even when its text is a near duplicate.
int near_duplicate_suppression() {
    const float near_pair[2] = {0.99F, 0.141F};
    const float anchor[2] = {1.0F, 0.0F};
    const float unrelated[2] = {0.0F, 1.0F};

    const auto build = [&](const char *file, double threshold) {
        kairo::Executor executor;
        MIRA_REQUIRE(executor.initialize(kairo::ExecutorConfig{}));
        SqliteMemoryStoreOptions options;
        options.path = Fixture::temp_dir() / file;
        options.near_dup_threshold = threshold;
        // Lift the (scope, kind) diversity cap so it cannot interfere with
        // the near-duplicate counts.
        options.max_per_scope_kind = 4;
        auto store = std::move(SqliteMemoryStore::open(executor, options)).value();
        const EmbedderId producer = EmbedderId::generate();
        const auto seed_one = [&](std::uint64_t ordinal, std::string statement,
                                  std::span<const float> vector) {
            MemoryRecord record;
            record.id = MemoryId{ordinal_id(ordinal)};
            record.scope = env_scope();
            record.kind = MemoryKind::EnvironmentFact;
            record.statement = std::move(statement);
            record.recorded_at = staggered_time(ordinal);
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId{ordinal_id(ordinal + 500)}};
            record.verification = MemoryVerification::Observed;
            record.confidence = 0.6F;
            MemoryMutation mutation;
            mutation.id = MutationId{ordinal_id(ordinal + 1000)};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = record.scope;
            mutation.proposed = record;
            mutation.evidence = record.provenance;
            MIRA_REQUIRE(store->apply(mutation).has_value());
            if (!vector.empty()) {
                MIRA_REQUIRE(store->index_embedding(record.id, producer, vector).has_value());
            }
            return record.id;
        };
        const auto anchor_id = seed_one(10, "docker compose 重启流程说明", anchor);
        const auto near_id = seed_one(11, "docker compose 的重启流程说明文档", near_pair);
        const auto unrelated_id = seed_one(12, "打印机 IP 是 192.168.1.42", unrelated);
        // A textual near duplicate without any vector of this identity:
        // the pass must exempt it instead of pretending to compare.
        const auto exempt_id =
            seed_one(13, "docker compose 重启流程说明!!", std::span<const float>{});
        MemoryQuery query;
        query.scopes = {env_scope()};
        query.text = "docker";
        query.query_embedding = MemoryEmbedding{std::vector<float>{1.0F, 0.05F}, producer};
        auto result = store->query(query);
        MIRA_REQUIRE(result.has_value());
        struct Outcome final {
            std::size_t suppressed = 0;
            bool anchor = false;
            bool near = false;
            bool unrelated = false;
            bool exempt = false;
        } outcome;
        outcome.suppressed = result.value().quality.dedup_suppressed;
        for (const auto &record : result.value().records) {
            outcome.anchor = outcome.anchor || record.id == anchor_id;
            outcome.near = outcome.near || record.id == near_id;
            outcome.unrelated = outcome.unrelated || record.id == unrelated_id;
            outcome.exempt = outcome.exempt || record.id == exempt_id;
        }
        MIRA_REQUIRE(store->close().has_value());
        MIRA_REQUIRE(executor.shutdown(true) == kairo::ShutdownResult::Completed);
        return outcome;
    };

    const auto on = build("fusion-dedup-on.db", 0.85);
    // Exactly one member of the near pair survives; which one depends on the
    // fused ranking (the higher-ranked member is kept).
    MIRA_CHECK(on.suppressed == 1);
    MIRA_CHECK(on.anchor != on.near);
    MIRA_CHECK(on.unrelated && on.exempt);

    const auto off = build("fusion-dedup-off.db", 0.0);
    MIRA_CHECK(off.suppressed == 0);
    MIRA_CHECK(off.anchor && off.near && off.unrelated && off.exempt);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += identifier_query_ranking();
    failures += paraphrase_query_recalled_by_the_vector_leg();
    failures += empty_legs_return_empty_in_both_modes();
    failures += reopen_reproduces_the_ranking();
    failures += labels_prefilter_narrows_the_universe();
    failures += importance_prior_is_opt_in();
    failures += near_duplicate_suppression();
    std::filesystem::remove_all(Fixture::temp_dir());
    return failures == 0 ? 0 : 1;
}
