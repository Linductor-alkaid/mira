#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

#include <executor/executor.hpp>

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-retrieval-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

struct Fixture final {
    explicit Fixture(std::filesystem::path path) {
        executor_.initialize(executor::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = std::move(path);
        auto opened = SqliteMemoryStore::open(executor_, options);
        store = std::move(opened).value();
    }
    ~Fixture() {
        if (store != nullptr) {
            (void)store->close();
        }
        (void)executor_.shutdown(true);
    }
    executor::Executor executor_;
    std::unique_ptr<SqliteMemoryStore> store;
};

[[nodiscard]] MemoryScope scope_of(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    return scope;
}

[[nodiscard]] MemoryRecord make_record(const MemoryScope &scope, std::string statement,
                                       std::uint64_t offset_seconds = 0) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = scope;
    value.kind = MemoryKind::EnvironmentFact;
    value.statement = std::move(statement);
    value.recorded_at = std::chrono::system_clock::now() - std::chrono::seconds(offset_seconds);
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    value.verification = MemoryVerification::Verified;
    value.confidence = 0.8F;
    return value;
}

[[nodiscard]] MemoryMutation add_for(const MemoryRecord &record) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = record.scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return mutation;
}

int exact_fts_and_vector_legs_merge() {
    Fixture fixture(temp_dir() / "retrieval-hybrid.db");
    auto *store = fixture.store.get();
    const auto scope = scope_of("kitchen");

    MIRA_CHECK(store->apply(add_for(make_record(scope, "the oven manual is in drawer two", 300)))
                   .has_value());
    MIRA_CHECK(
        store->apply(add_for(make_record(scope, "fridge filter expires in may", 200))).has_value());
    MIRA_CHECK(store->apply(add_for(make_record(scope, "kitchen light switch is dimmable", 100)))
                   .has_value());

    // Exact leg: verbatim substring.
    MemoryQuery exact;
    exact.scopes = {scope};
    exact.exact_terms = {"drawer two"};
    auto exact_result = store->query(exact);
    MIRA_CHECK(exact_result.has_value());
    MIRA_CHECK(exact_result.value().records.size() == 1);
    MIRA_CHECK(exact_result.value().quality.exact_leg_ran);
    MIRA_CHECK(exact_result.value().records.front().statement.find("drawer two") !=
               std::string::npos);

    // FTS leg: phrase match ranks the matching record first.
    MemoryQuery phrase;
    phrase.scopes = {scope};
    phrase.text = "oven manual";
    auto phrase_result = store->query(phrase);
    MIRA_CHECK(phrase_result.has_value());
    MIRA_CHECK(!phrase_result.value().records.empty());
    MIRA_CHECK(phrase_result.value().records.front().statement.find("oven") != std::string::npos);
    MIRA_CHECK(phrase_result.value().quality.fts_leg_ran);

    // Vector leg: embeddings index and cosine match; orthogonal vectors do
    // not outrank aligned ones.
    std::vector<MemoryQueryResult> seeded;
    {
        MemoryQuery all;
        all.scopes = {scope};
        auto everything = store->query(all);
        MIRA_CHECK(everything.has_value());
        seeded.push_back(std::move(everything).value());
    }
    const auto &records = seeded.front().records;
    MIRA_CHECK(records.size() == 3);
    // "oven" record aligned with the query axis; others orthogonal.
    std::size_t attached = 0;
    for (const auto &entry : records) {
        const bool oven = entry.statement.find("oven") != std::string::npos;
        const std::vector<float> vector = oven ? std::vector<float>{1.0F, 0.0F}
                                               : std::vector<float>{0.0F, 1.0F};
        MIRA_CHECK(store->index_embedding(entry.id, vector).has_value());
        ++attached;
    }
    MIRA_CHECK(attached == 3);
    MIRA_CHECK(store->index_lag().has_value() && store->index_lag().value() == 0);

    MemoryQuery vector_query;
    vector_query.scopes = {scope};
    vector_query.query_embedding = {0.9F, 0.1F};
    auto vector_result = store->query(vector_query);
    MIRA_CHECK(vector_result.has_value());
    MIRA_CHECK(vector_result.value().quality.vector_leg_ran);
    MIRA_CHECK(!vector_result.value().records.empty());
    MIRA_CHECK(vector_result.value().records.front().statement.find("oven") != std::string::npos);

    // Index lag reports records without embeddings.
    MIRA_CHECK(store->apply(add_for(make_record(scope, "dishwasher tablet brand is finish")))
                   .has_value());
    auto lag = store->index_lag();
    MIRA_CHECK(lag.has_value() && lag.value() == 1);
    return 0;
}

int vector_corruption_degrades_without_blocking() {
    Fixture fixture(temp_dir() / "retrieval-degrade.db");
    auto *store = fixture.store.get();
    const auto scope = scope_of("lab");

    auto target = make_record(scope, "calibration file is on the shared drive");
    MIRA_CHECK(store->apply(add_for(target)).has_value());
    MIRA_CHECK(store->apply(add_for(make_record(scope, "whiteboard markers are dry", 50)))
                   .has_value());

    // Dimension mismatch between query and index degrades the vector leg but
    // exact/FTS still answer.
    MIRA_CHECK(store->index_embedding(target.id, std::vector<float>{1.0F, 2.0F, 3.0F}).has_value());
    MemoryQuery mismatch;
    mismatch.scopes = {scope};
    mismatch.text = "calibration";
    mismatch.query_embedding = {1.0F, 0.0F};
    auto degraded = store->query(mismatch);
    MIRA_CHECK(degraded.has_value());
    MIRA_CHECK(!degraded.value().records.empty());
    MIRA_CHECK(degraded.value().records.front().statement.find("calibration") !=
               std::string::npos);
    MIRA_CHECK(!degraded.value().quality.vector_leg_ran);

    // Clearing the index (embedding-model change) reports lag and keeps
    // serving exact/FTS results.
    auto cleared = store->clear_embeddings();
    MIRA_CHECK(cleared.has_value() && cleared.value() >= 1);
    MemoryQuery after_clear;
    after_clear.scopes = {scope};
    after_clear.text = "calibration";
    after_clear.query_embedding = {1.0F, 0.0F, 0.0F};
    auto fallback = store->query(after_clear);
    MIRA_CHECK(fallback.has_value());
    MIRA_CHECK(!fallback.value().records.empty());
    MIRA_CHECK(fallback.value().quality.index_lag >= 2);
    return 0;
}

int diversity_dedup_and_token_packing() {
    Fixture fixture(temp_dir() / "retrieval-packing.db");
    auto *store = fixture.store.get();
    const auto scope = scope_of("com.example.app");

    // Near-duplicate statements must not flood the selection.
    for (int index = 0; index < 6; ++index) {
        auto duplicate = make_record(scope, "app theme is dark");
        duplicate.recorded_at -= std::chrono::seconds(index);
        MIRA_CHECK(store->apply(add_for(duplicate)).has_value());
    }
    MIRA_CHECK(store->apply(add_for(make_record(scope, "app language is german"))).has_value());

    MemoryQuery query;
    query.scopes = {scope};
    auto packed = store->query(query);
    MIRA_CHECK(packed.has_value());
    MIRA_CHECK(packed.value().records.size() == 2); // statement dedup keeps one of each

    // Token budget trims the tail and keeps the estimate within budget.
    MemoryQuery tight;
    tight.scopes = {scope};
    tight.token_budget = 1; // a single record cannot fit
    auto trimmed = store->query(tight);
    MIRA_CHECK(trimmed.has_value());
    MIRA_CHECK(trimmed.value().records.empty());
    MIRA_CHECK(trimmed.value().tokens_estimate == 0);

    MemoryQuery one_only;
    one_only.scopes = {scope};
    one_only.token_budget = 24;
    auto single = store->query(one_only);
    MIRA_CHECK(single.has_value());
    MIRA_CHECK(single.value().records.size() == 1);
    MIRA_CHECK(single.value().tokens_estimate <= 24);
    return 0;
}

int deadline_returns_partial_with_quality_flag() {
    Fixture fixture(temp_dir() / "retrieval-deadline.db");
    auto *store = fixture.store.get();
    const auto scope = scope_of("office");

    MIRA_CHECK(store->apply(add_for(make_record(scope, "standing desk is near the window")))
                   .has_value());

    // A zero deadline expires before the FTS/vector legs can run: the exact
    // meta leg still returns the record, flagged as a partial answer.
    MemoryQuery rushed;
    rushed.scopes = {scope};
    rushed.text = "standing desk";
    rushed.deadline = std::chrono::milliseconds(0);
    auto partial = store->query(rushed);
    MIRA_CHECK(partial.has_value());
    MIRA_CHECK(!partial.value().records.empty());
    MIRA_CHECK(partial.value().quality.deadline_exceeded);
    MIRA_CHECK(partial.value().quality.degraded);
    return 0;
}

int fts_operator_text_is_never_injected() {
    Fixture fixture(temp_dir() / "retrieval-injection.db");
    auto *store = fixture.store.get();
    const auto scope = scope_of("lab");

    MIRA_CHECK(store->apply(add_for(make_record(scope, "laser interlock requires badge")))
                   .has_value());
    MIRA_CHECK(store->apply(add_for(make_record(scope, "glove box is left handed")))
                   .has_value());

    // Operator-shaped text is treated as a literal phrase, never as FTS
    // syntax: no results rather than an unintended match set.
    MemoryQuery injection;
    injection.scopes = {scope};
    injection.text = "\" NEAR( ";
    auto literal = store->query(injection);
    MIRA_CHECK(literal.has_value());
    MIRA_CHECK(literal.value().records.empty());
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += exact_fts_and_vector_legs_merge();
    failures += vector_corruption_degrades_without_blocking();
    failures += diversity_dedup_and_token_packing();
    failures += deadline_returns_partial_with_quality_flag();
    failures += fts_operator_text_is_never_injected();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
