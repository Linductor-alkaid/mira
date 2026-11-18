#include "../support/test.hpp"

#include <mira/memory_consolidation.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-write-evolution-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

struct Fixture final {
    explicit Fixture(std::string name) {
        executor_.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / (std::move(name) + ".db");
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

[[nodiscard]] MemoryScope scope_of(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    return scope;
}

// A candidate with explicit evidence, verification and era; each call mints
// a fresh event id so "same claim re-observed" genuinely carries new evidence.
[[nodiscard]] MemoryCandidate
candidate(const MemoryScope &scope, std::string statement, MemoryVerification verification,
          MutationReasonCode reason = MutationReasonCode::VerifiedEvent) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = MemoryKind::EnvironmentFact;
    record.statement = std::move(statement);
    record.recorded_at = std::chrono::system_clock::now();
    record.validity.valid_from = record.recorded_at;
    record.provenance = {EventId::generate()};
    record.verification = verification;
    record.confidence = 0.9F;
    MemoryCandidate out;
    out.proposed = std::move(record);
    out.evidence = out.proposed.provenance;
    out.reason = reason;
    return out;
}

// ---------------------------------------------------------------------------
// 1. AppendEvidence: merge, CAS, version stability, idempotent replay.
// ---------------------------------------------------------------------------
int append_evidence_merge_and_replay() {
    Fixture fixture("append");
    const auto scope = scope_of("append");
    MemoryConsolidator consolidator;

    auto first = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume=30", MemoryVerification::Observed)}, scope,
        Timestamp::now());
    MIRA_CHECK(first.has_value() && first.value().count_of(CandidateDisposition::Applied) == 1);
    MemoryQuery probe;
    probe.scopes = {scope};
    probe.claim_key = memory_claim_key("volume=30").key;
    auto visible = fixture.store->query(probe);
    MIRA_CHECK(visible.has_value() && visible.value().records.size() == 1);
    const MemoryRecord incumbent = visible.value().records.front();
    MIRA_CHECK(incumbent.provenance.size() == 1);

    // The same claim re-observed with a fresh event: evidence accumulates,
    // the version chain does not move.
    auto second = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "volume=30", MemoryVerification::Observed)}, scope,
        Timestamp::now());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().entries.front().reason_code == "evidence-appended");
    MIRA_CHECK(second.value().entries.front().incumbent.has_value() &&
               second.value().entries.front().incumbent.value() == incumbent.id);
    auto grown = fixture.store->query(probe);
    MIRA_CHECK(grown.has_value() && grown.value().records.size() == 1);
    MIRA_CHECK(grown.value().records.front().provenance.size() == 2);
    MIRA_CHECK(grown.value().records.front().version == incumbent.version);

    // Direct store-level gates: statement mismatch and a stale CAS version.
    const auto observed = grown.value().records.front();
    MemoryMutation mismatch = {};
    mismatch.id = MutationId::generate();
    mismatch.type = MemoryMutationType::AppendEvidence;
    mismatch.scope = scope;
    mismatch.target = observed.id;
    mismatch.expected_version = observed.version;
    mismatch.proposed = observed;
    mismatch.proposed.statement = "volume=999";
    mismatch.evidence = {EventId::generate()};
    auto rejected = fixture.store->apply(mismatch);
    MIRA_CHECK(!rejected.has_value() &&
               rejected.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::InvalidMutation));

    MemoryMutation stale = mismatch;
    stale.id = MutationId::generate();
    stale.proposed.statement = observed.statement;
    stale.expected_version = observed.version + 5;
    auto conflict = fixture.store->apply(stale);
    MIRA_CHECK(!conflict.has_value() &&
               conflict.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::VersionConflict));
    MIRA_CHECK(fixture.store->query(probe).value().records.front().provenance.size() == 2);

    // Replay of the applied AppendEvidence mutation id is idempotent.
    MemoryMutation replay = {};
    replay.id = second.value().entries.front().mutation;
    replay.type = MemoryMutationType::AppendEvidence;
    replay.scope = scope;
    replay.target = observed.id;
    replay.expected_version = observed.version;
    replay.proposed = observed;
    replay.evidence = {EventId::generate()};
    auto replayed = fixture.store->apply(replay);
    MIRA_CHECK(replayed.has_value());
    MIRA_CHECK(replayed.value().idempotent_replay);
    MIRA_CHECK(replayed.value().new_version == observed.version);
    return 0;
}

// ---------------------------------------------------------------------------
// 2. PreserveAlternative: coexistence, claim-key visibility, explicit cap.
// ---------------------------------------------------------------------------
int preserve_alternative_coexistence_and_cap() {
    Fixture fixture("preserve");
    const auto scope = scope_of("preserve");
    ConsolidationPolicy policy;
    policy.max_alternatives_per_claim = 1;
    MemoryConsolidator consolidator(policy);

    auto first = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "mirror=primary", MemoryVerification::Verified)}, scope,
        Timestamp::now());
    MIRA_CHECK(first.has_value() && first.value().entries.front().reason_code == "added");

    // Consolidation-source candidates never supersede: the changed value
    // coexists as an alternative linked to its incumbent.
    auto second = consolidator.consolidate_candidates(
        *fixture.store,
        {candidate(scope, "mirror=backup", MemoryVerification::Verified,
                   MutationReasonCode::Consolidation)},
        scope, Timestamp::now());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().entries.front().reason_code == "alternative-preserved");
    MIRA_CHECK(second.value().entries.front().incumbent.has_value());

    // Both sides are retrievable under the same claim key, and the
    // alternative names its incumbent.
    MemoryQuery probe;
    probe.scopes = {scope};
    probe.claim_key = memory_claim_key("mirror=primary").key;
    auto visible = fixture.store->query(probe);
    MIRA_CHECK(visible.has_value() && visible.value().records.size() == 2);
    bool incumbent_linked = false;
    for (const auto &record : visible.value().records) {
        if (record.statement == "mirror=backup") {
            incumbent_linked = record.alternative_of.has_value() &&
                               record.alternative_of->to_string() ==
                                   second.value().entries.front().incumbent->to_string();
        }
    }
    MIRA_CHECK(incumbent_linked);

    // The cap is explicit: a third value is rejected with a named reason.
    auto third = consolidator.consolidate_candidates(
        *fixture.store,
        {candidate(scope, "mirror=offline", MemoryVerification::Verified,
                   MutationReasonCode::Consolidation)},
        scope, Timestamp::now());
    MIRA_CHECK(third.has_value());
    MIRA_CHECK(third.value().entries.front().reason_code == "alternative-cap-reached");
    MIRA_CHECK(third.value().count_of(CandidateDisposition::RejectedConflict) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// 3. Model candidates never supersede an authoritative claim.
// ---------------------------------------------------------------------------
class FakeModel final : public IConsolidationModel {
  public:
    std::vector<MemoryCandidate> proposals;
    std::vector<MemoryCandidate> propose(std::span<const EventEnvelope>,
                                         const MemoryScope &) override {
        return proposals;
    }
};

int model_candidates_never_supersede() {
    Fixture fixture("model");
    const auto scope = scope_of("model");
    MemoryConsolidator consolidator;
    auto seeded = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "port=8443", MemoryVerification::Verified)}, scope,
        Timestamp::now());
    MIRA_CHECK(seeded.has_value() && seeded.value().entries.front().reason_code == "added");

    FakeModel model;
    model.proposals.push_back(candidate(scope, "port=9999", MemoryVerification::Unverified,
                                        MutationReasonCode::Consolidation));
    MemoryConsolidator with_model(ConsolidationPolicy{}, &model);
    // consolidate() (events + model), not consolidate_candidates(): the
    // model hook runs only on the events entry (memory_consolidation.hpp).
    auto report = with_model.consolidate(*fixture.store, {}, scope, Timestamp::now());
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().entries.size() == 1);
    MIRA_CHECK(report.value().entries.front().reason_code == "alternative-preserved");
    MIRA_CHECK(report.value().entries.front().incumbent.has_value());

    MemoryQuery probe;
    probe.scopes = {scope};
    probe.claim_key = memory_claim_key("port=8443").key;
    auto visible = fixture.store->query(probe);
    MIRA_CHECK(visible.has_value() && visible.value().records.size() == 2);
    for (const auto &record : visible.value().records) {
        if (record.statement == "port=8443") {
            MIRA_CHECK(record.status == MemoryStatus::Active);
            MIRA_CHECK(record.verification == MemoryVerification::Verified);
            MIRA_CHECK(record.version == 1);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 4. Duplicate family: promotion is strict (upward only, evidence-backed,
//    HumanConfirmed terminal); downgraded replays only accumulate evidence.
// ---------------------------------------------------------------------------
int duplicate_promotion_rules() {
    Fixture fixture("promotion");
    const auto scope = scope_of("promotion");
    MemoryConsolidator consolidator;

    // Observed incumbent + Verified replay with fresh evidence: promoted in
    // place (version moves, verification rises, provenance merges).
    auto seeded = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "host=alpha", MemoryVerification::Observed)}, scope,
        Timestamp::now());
    MIRA_CHECK(seeded.has_value());
    MemoryQuery probe;
    probe.scopes = {scope};
    probe.claim_key = memory_claim_key("host=alpha").key;
    auto promoted = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "host=alpha", MemoryVerification::Verified)}, scope,
        Timestamp::now());
    MIRA_CHECK(promoted.has_value());
    MIRA_CHECK(promoted.value().entries.front().reason_code == "verification-promoted");
    auto raised = fixture.store->query(probe);
    MIRA_CHECK(raised.has_value() && raised.value().records.size() == 1);
    MIRA_CHECK(raised.value().records.front().verification == MemoryVerification::Verified);
    MIRA_CHECK(raised.value().records.front().version == 2);
    MIRA_CHECK(raised.value().records.front().provenance.size() == 2);

    // HumanConfirmed is terminal: a Verified replay cannot touch it.
    MemoryCandidate confirmed = candidate(scope, "owner=bob", MemoryVerification::Verified);
    confirmed.proposed.verification = MemoryVerification::HumanConfirmed;
    MIRA_CHECK(confirmed.proposed.validate().has_value());
    auto stored =
        consolidator.consolidate_candidates(*fixture.store, {confirmed}, scope, Timestamp::now());
    MIRA_CHECK(stored.has_value() && stored.value().count_of(CandidateDisposition::Applied) == 1);
    MemoryQuery owner_probe;
    owner_probe.scopes = {scope};
    owner_probe.claim_key = memory_claim_key("owner=bob").key;
    auto human = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "owner=bob", MemoryVerification::Verified)}, scope,
        Timestamp::now());
    // DEC-115: HumanConfirmed is terminal for promotion, but the replay's
    // fresh source event still accumulates as evidence (no claim change).
    MIRA_CHECK(human.has_value());
    MIRA_CHECK(human.value().entries.front().reason_code == "evidence-appended");
    MIRA_CHECK(fixture.store->query(owner_probe).value().records.front().verification ==
               MemoryVerification::HumanConfirmed);

    // A weaker replay only accumulates evidence: never a downgrade.
    auto weakened = consolidator.consolidate_candidates(
        *fixture.store, {candidate(scope, "host=alpha", MemoryVerification::Observed)}, scope,
        Timestamp::now());
    MIRA_CHECK(weakened.has_value());
    MIRA_CHECK(weakened.value().entries.front().reason_code == "evidence-appended");
    auto still = fixture.store->query(probe);
    MIRA_CHECK(still.has_value() &&
               still.value().records.front().verification == MemoryVerification::Verified);
    return 0;
}

// ---------------------------------------------------------------------------
// 5. Era succession vs observation time: a later observation of an older era
//    never supersedes the current era; a later era at sufficient
//    verification does.
// ---------------------------------------------------------------------------
int era_succession_distinguishes_observation_time() {
    Fixture fixture("era");
    const auto scope = scope_of("era");
    MemoryConsolidator consolidator;

    auto newer_era = candidate(scope, "primary=node-a", MemoryVerification::Verified);
    auto seeded =
        consolidator.consolidate_candidates(*fixture.store, {newer_era}, scope, Timestamp::now());
    MIRA_CHECK(seeded.has_value());

    // An older-era fact (valid_from before the incumbent's) observed later:
    // observation time does not drive succession; the historical claim is
    // preserved as an alternative.
    auto historical = candidate(scope, "primary=node-z", MemoryVerification::Verified);
    historical.proposed.validity.valid_from =
        newer_era.proposed.validity.valid_from - std::chrono::hours(24);
    historical.proposed.recorded_at = newer_era.proposed.recorded_at + std::chrono::hours(1);
    auto late =
        consolidator.consolidate_candidates(*fixture.store, {historical}, scope, Timestamp::now());
    MIRA_CHECK(late.has_value());
    MIRA_CHECK(late.value().entries.front().reason_code == "alternative-preserved");

    // A strictly later era at equal verification succeeds the incumbent.
    auto next_era = candidate(scope, "primary=node-b", MemoryVerification::Verified);
    next_era.proposed.validity.valid_from =
        newer_era.proposed.validity.valid_from + std::chrono::hours(1);
    auto succession =
        consolidator.consolidate_candidates(*fixture.store, {next_era}, scope, Timestamp::now());
    MIRA_CHECK(succession.has_value());
    MIRA_CHECK(succession.value().entries.front().reason_code == "superseded-existing");
    MemoryQuery probe;
    probe.scopes = {scope};
    probe.claim_key = memory_claim_key("primary=node-a").key;
    auto visible = fixture.store->query(probe);
    MIRA_CHECK(visible.has_value() && visible.value().records.size() == 2);
    for (const auto &record : visible.value().records) {
        if (record.statement == "primary=node-a") {
            MIRA_CHECK(record.status == MemoryStatus::Superseded);
        }
        if (record.statement == "primary=node-b") {
            MIRA_CHECK(record.status == MemoryStatus::Active);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 6. Truncation is reported, never silent.
// ---------------------------------------------------------------------------
int truncation_is_reported() {
    Fixture fixture("truncation");
    const auto scope = scope_of("truncation");
    ConsolidationPolicy policy;
    policy.max_candidates_per_run = 2;
    MemoryConsolidator consolidator(policy);
    std::vector<MemoryCandidate> batch = {
        candidate(scope, "alpha=1", MemoryVerification::Verified),
        candidate(scope, "beta=2", MemoryVerification::Verified),
        candidate(scope, "gamma=3", MemoryVerification::Verified),
        candidate(scope, "delta=4", MemoryVerification::Verified),
    };
    auto report =
        consolidator.consolidate_candidates(*fixture.store, batch, scope, Timestamp::now());
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().candidates_examined == 4);
    MIRA_CHECK(report.value().candidates_truncated == 2);
    MIRA_CHECK(report.value().entries.size() == 2);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += append_evidence_merge_and_replay();
    failures += preserve_alternative_coexistence_and_cap();
    failures += model_candidates_never_supersede();
    failures += duplicate_promotion_rules();
    failures += era_succession_distinguishes_observation_time();
    failures += truncation_is_reported();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
