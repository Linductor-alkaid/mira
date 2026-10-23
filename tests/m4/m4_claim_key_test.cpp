#include "../support/test.hpp"

#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>

#include <kairo/executor.hpp>

// Conflict detection used to key on the raw `=` prefix of a statement, so
// "volume=30" and "volume = 30" were two different subjects and both
// survived in the same scope. The claim key (design Context/Memory §13.5)
// normalizes the subject, and the store can now narrow a query to it.

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-claim-key-" + std::to_string(device()));
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
    value.confidence = 0.9F;
    return value;
}

[[nodiscard]] MemoryMutation add_mutation(const MemoryRecord &proposed) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.type = MemoryMutationType::Add;
    mutation.scope = proposed.scope;
    mutation.proposed = proposed;
    mutation.evidence = proposed.provenance;
    mutation.reason = MutationReasonCode::Consolidation;
    return mutation;
}

[[nodiscard]] MemoryCandidate candidate(const MemoryScope &scope, std::string statement) {
    MemoryCandidate out;
    out.proposed = record(scope, std::move(statement));
    out.evidence = out.proposed.provenance;
    out.reason = MutationReasonCode::Consolidation;
    return out;
}

struct Fixture final {
    explicit Fixture(std::string name) : name_(std::move(name)) {
        executor_.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / (name_ + ".db");
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
    std::string name_;
};

// The rule itself: the subject is trimmed, its whitespace runs collapse and
// its case folds, so the surface form of a claim stops deciding what it is
// about.
int claim_key_normalizes_the_subject() {
    const auto volume = memory_claim_key("volume=30");
    MIRA_CHECK(volume.keyed);
    MIRA_CHECK(volume.key == "volume");
    MIRA_CHECK(volume.value == "30");

    const auto spaced = memory_claim_key("volume = 30");
    MIRA_CHECK(spaced.key == volume.key);
    MIRA_CHECK(spaced.value == volume.value);
    MIRA_CHECK(memory_same_claim("volume=30", "volume = 30"));

    const auto folded = memory_claim_key("VOLUME=30");
    MIRA_CHECK(folded.key == volume.key);
    MIRA_CHECK(folded.value == volume.value);

    const auto padded = memory_claim_key("  volume  =  30  ");
    MIRA_CHECK(padded.key == volume.key);
    MIRA_CHECK(padded.value == volume.value);

    const auto collapsed = memory_claim_key("device   volume=30");
    MIRA_CHECK(collapsed.key == "device volume");

    // A statement without a value is its own subject and carries no value.
    const auto unkeyed = memory_claim_key("Prefer  Dark Mode");
    MIRA_CHECK(!unkeyed.keyed);
    MIRA_CHECK(unkeyed.key == "prefer dark mode");
    MIRA_CHECK(unkeyed.value.empty());

    // Two claims about different subjects never share a key.
    MIRA_CHECK(memory_claim_key("volume=30").key != memory_claim_key("volume_level=high").key);
    // ... and the same subject with a different value is a different claim.
    MIRA_CHECK(!memory_same_claim("volume=30", "volume=60"));
    MIRA_CHECK(memory_same_claim("volume=30", "volume = 30"));
    MIRA_CHECK(memory_same_claim("prefer dark mode", "Prefer  Dark Mode"));

    // The normalization is idempotent, so a backend may apply it to a key it
    // received from a caller.
    MIRA_CHECK(memory_normalize_claim_key("  Device   Volume ") == "device volume");
    MIRA_CHECK(memory_normalize_claim_key(memory_normalize_claim_key("  Device   Volume ")) ==
               "device volume");
    return 0;
}

// The store narrows a query to one subject before it ranks, so a conflict
// retrieval sees the records about that subject and nothing else.
int claim_key_filter_narrows_the_store() {
    Fixture fixture("filter");
    const auto scope = env_scope("device-42");
    MIRA_CHECK(fixture.store->apply(add_mutation(record(scope, "volume=30"))).has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(record(scope, "volume = 60"))).has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(record(scope, "volume_level=high"))).has_value());

    MemoryQuery query;
    query.scopes = {scope};
    query.claim_key = memory_claim_key("volume=30").key;
    auto narrowed = fixture.store->query(query);
    MIRA_CHECK(narrowed.has_value());
    MIRA_CHECK(narrowed.value().records.size() == 2);
    for (const auto &found : narrowed.value().records) {
        MIRA_CHECK(memory_claim_key(found.statement).key == "volume");
    }

    // A subject nobody wrote about yields nothing.
    MemoryQuery missing;
    missing.scopes = {scope};
    missing.claim_key = memory_claim_key("brightness=80").key;
    auto empty = fixture.store->query(missing);
    MIRA_CHECK(empty.has_value());
    MIRA_CHECK(empty.value().records.empty());

    // Without the filter the scope still answers as a whole.
    MemoryQuery plain;
    plain.scopes = {scope};
    auto scope_wide = fixture.store->query(plain);
    MIRA_CHECK(scope_wide.has_value());
    MIRA_CHECK(scope_wide.value().records.size() == 3);
    return 0;
}

// The gap this fixes: two phrasings of one fact are one claim, so the
// second one is a no-op instead of a second record.
int one_fact_two_phrasings_stay_one_record() {
    Fixture fixture("phrasings");
    const auto scope = env_scope("device-42");
    MemoryConsolidator consolidator;

    auto first = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume=30")}, scope, Timestamp::now());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(first.value().entries.front().reason_code == "added");

    auto second = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume = 30")}, scope, Timestamp::now());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().entries.front().reason_code == "duplicate-noop");
    MIRA_CHECK(second.value().count_of(CandidateDisposition::Applied) == 1);

    MemoryQuery check;
    check.scopes = {scope};
    auto visible = fixture.store->query(check);
    MIRA_CHECK(visible.has_value());
    MIRA_CHECK(visible.value().records.size() == 1);
    MIRA_CHECK(visible.value().records.front().statement == "volume=30");
    return 0;
}

// A changed value about the same subject still supersedes — across
// phrasings, because the subject is normalized on both sides.
int changed_value_supersedes_across_phrasings() {
    Fixture fixture("changed");
    const auto scope = env_scope("device-42");
    MemoryConsolidator consolidator;

    auto first = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume=30")}, scope, Timestamp::now());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(first.value().entries.front().reason_code == "added");

    auto second = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume = 60")}, scope, Timestamp::now());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().entries.front().reason_code == "superseded-existing");

    MemoryQuery check;
    check.scopes = {scope};
    auto visible = fixture.store->query(check);
    MIRA_CHECK(visible.has_value());
    MIRA_CHECK(visible.value().records.size() == 1);
    MIRA_CHECK(visible.value().records.front().statement == "volume = 60");
    MIRA_CHECK(visible.value().records.front().supersedes.has_value());
    return 0;
}

// Different subjects never conflict, however similar they look.
int different_subjects_do_not_conflict() {
    Fixture fixture("subjects");
    const auto scope = env_scope("device-42");
    MemoryConsolidator consolidator;

    auto report = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume=30"), candidate(scope, "volume_level=high")},
        scope, Timestamp::now());
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().count_of(CandidateDisposition::Applied) == 2);
    for (const auto &entry : report.value().entries) {
        MIRA_CHECK(entry.reason_code == "added");
    }

    MemoryQuery check;
    check.scopes = {scope};
    auto visible = fixture.store->query(check);
    MIRA_CHECK(visible.has_value());
    MIRA_CHECK(visible.value().records.size() == 2);
    return 0;
}

// A statement without a value is its own subject: only a normalized
// duplicate of it is a no-op.
int unkeyed_statements_conflict_on_normalized_equality() {
    Fixture fixture("unkeyed");
    const auto scope = env_scope("device-42");
    MemoryConsolidator consolidator;

    auto first = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "Prefer dark mode")}, scope, Timestamp::now());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(first.value().entries.front().reason_code == "added");

    auto second = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "prefer  dark  mode")}, scope, Timestamp::now());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().entries.front().reason_code == "duplicate-noop");

    MemoryQuery check;
    check.scopes = {scope};
    auto visible = fixture.store->query(check);
    MIRA_CHECK(visible.has_value());
    MIRA_CHECK(visible.value().records.size() == 1);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += claim_key_normalizes_the_subject();
    failures += claim_key_filter_narrows_the_store();
    failures += one_fact_two_phrasings_stay_one_record();
    failures += changed_value_supersedes_across_phrasings();
    failures += different_subjects_do_not_conflict();
    failures += unkeyed_statements_conflict_on_normalized_equality();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
