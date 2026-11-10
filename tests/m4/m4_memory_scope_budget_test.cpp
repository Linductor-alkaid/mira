#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <kairo/executor.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using namespace mira;

struct Fixture final {
    Fixture() {
        root = std::filesystem::temp_directory_path() /
               ("mira-scope-budget-" + Id128::generate().to_string());
        std::filesystem::create_directories(root);
        if (!executor.initialize(kairo::ExecutorConfig{}))
            std::abort();
        options.path = root / "memory.db";
        options.max_vector_scan = 2;
        reopen();
    }
    ~Fixture() {
        if (store)
            (void)store->close();
        (void)executor.shutdown(true);
        std::filesystem::remove_all(root);
    }
    void reopen() {
        if (store && !store->close())
            std::abort();
        store.reset();
        auto opened = SqliteMemoryStore::open(executor, options);
        if (!opened)
            std::abort();
        store = std::move(opened).value();
    }
    kairo::Executor executor;
    SqliteMemoryStoreOptions options;
    std::filesystem::path root;
    std::unique_ptr<SqliteMemoryStore> store;
};

MemoryScope scope(std::string subject) {
    MemoryScope value;
    value.kind = MemoryScopeKind::Environment;
    value.subject_id = std::move(subject);
    return value;
}

MemoryRecord record(const MemoryScope &owner, std::string text) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = owner;
    value.kind = MemoryKind::EnvironmentFact;
    value.statement = std::move(text);
    value.verification = MemoryVerification::Verified;
    value.confidence = 0.9F;
    value.sensitivity = Sensitivity::Public;
    value.recorded_at = std::chrono::system_clock::now();
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    return value;
}

bool add(SqliteMemoryStore &store, const MemoryRecord &value) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = value.scope;
    mutation.proposed = value;
    mutation.evidence = value.provenance;
    return store.apply(mutation).has_value();
}

int fts_budget_is_local_after_restart() {
    Fixture fixture;
    const auto owner = scope("authorized-device");
    const auto other = scope("unrelated-device");
    // A longer authorized document has a weaker BM25 rank than the short
    // foreign documents. The old global LIMIT 512 excluded it entirely.
    const auto target = record(owner, "needle target configuration " + std::string(2048, 'x'));
    MIRA_CHECK(add(*fixture.store, target));
    for (int i = 0; i < 514; ++i) {
        MIRA_CHECK(add(*fixture.store, record(other, "needle")));
    }
    fixture.reopen();
    MemoryQuery query;
    query.scopes = {owner};
    query.text = "needle";
    auto found = fixture.store->query(query);
    MIRA_CHECK(found && found.value().quality.fts_leg_ran);
    MIRA_CHECK(found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().id == target.id);
    query.text = "missingneedle";
    auto missing = fixture.store->query(query);
    MIRA_CHECK(missing && missing.value().records.empty());
    query.scopes.clear();
    MIRA_CHECK(!fixture.store->query(query));
    return 0;
}

int fts_filters_precede_budget() {
    Fixture fixture;
    const auto owner = scope("filtered-device");
    const auto target = record(owner, "target=needle " + std::string(2048, 'x'));
    MIRA_CHECK(add(*fixture.store, target));
    for (int i = 0; i < 514; ++i) {
        auto excluded = record(owner, "other=needle");
        excluded.sensitivity = Sensitivity::Sensitive;
        MIRA_CHECK(add(*fixture.store, excluded));
    }
    MemoryQuery query;
    query.scopes = {owner};
    query.text = "needle";
    query.max_sensitivity = Sensitivity::Public;
    auto found = fixture.store->query(query);
    MIRA_CHECK(found && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().id == target.id);
    query.max_sensitivity.reset();
    query.claim_key = "target";
    found = fixture.store->query(query);
    MIRA_CHECK(found && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().id == target.id);
    return 0;
}

int vector_budget_is_local() {
    Fixture fixture;
    const auto owner = scope("vector-owner");
    const auto target = record(owner, "target=calibration");
    const std::vector<float> embedding{1.F, 0.F};
    MIRA_CHECK(add(*fixture.store, target));
    MIRA_CHECK(fixture.store->index_embedding(target.id, embedding));
    for (int i = 0; i < 3; ++i) {
        const auto foreign = record(scope("foreign-vector-owner"), "other=calibration");
        MIRA_CHECK(add(*fixture.store, foreign));
        MIRA_CHECK(fixture.store->index_embedding(foreign.id, embedding));
    }
    fixture.reopen();
    MemoryQuery query;
    query.scopes = {owner};
    query.query_embedding = embedding;
    auto found = fixture.store->query(query);
    MIRA_CHECK(found && found.value().quality.vector_leg_ran);
    MIRA_CHECK(found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().id == target.id);
    // Same scope, different claim: also excluded before the scan limit.
    for (int i = 0; i < 3; ++i) {
        const auto excluded = record(owner, "other=calibration");
        MIRA_CHECK(add(*fixture.store, excluded));
        MIRA_CHECK(fixture.store->index_embedding(excluded.id, embedding));
    }
    query.claim_key = "target";
    found = fixture.store->query(query);
    MIRA_CHECK(found && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().id == target.id);
    return 0;
}
} // namespace

int main() {
    int failures = 0;
    failures += fts_budget_is_local_after_restart();
    failures += fts_filters_precede_budget();
    failures += vector_budget_is_local();
    return failures == 0 ? 0 : 1;
}
