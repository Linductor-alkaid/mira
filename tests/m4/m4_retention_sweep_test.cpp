#include "../support/test.hpp"

#include <mira/artifact_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>

#include <kairo/executor.hpp>

// The store had no cross-scope retention entry point: compact() takes one
// scope, so a host could only reclaim what it already knew about. The
// sweep is the store owner's bounded pass over every scope, and the
// failures it meets on the way are counted and rolled back rather than
// swallowed.

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-sweep-" + std::to_string(device()));
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

[[nodiscard]] MemoryRecord record(const MemoryScope &scope, std::string statement) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = scope;
    value.kind = MemoryKind::EnvironmentFact;
    value.statement = std::move(statement);
    value.recorded_at = std::chrono::system_clock::now();
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    value.verification = MemoryVerification::Verified;
    value.confidence = 0.8F;
    return value;
}

// A record whose lifetime already ended: the retention sweep must reclaim
// it, the per-kind default TTL never gets a say.
[[nodiscard]] MemoryRecord expired_record(const MemoryScope &scope, std::string statement) {
    MemoryRecord value = record(scope, std::move(statement));
    value.recorded_at = std::chrono::system_clock::now() - std::chrono::hours(2);
    value.validity.valid_from = value.recorded_at;
    value.expires_at = std::chrono::system_clock::now() - std::chrono::hours(1);
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

[[nodiscard]] MemoryMutation update_mutation(const MemoryRecord &existing, std::string statement,
                                             std::uint64_t expected_version) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.type = MemoryMutationType::Update;
    mutation.scope = existing.scope;
    mutation.target = existing.id;
    mutation.expected_version = expected_version;
    mutation.proposed = existing;
    mutation.proposed.statement = std::move(statement);
    mutation.evidence = existing.provenance;
    mutation.reason = MutationReasonCode::Consolidation;
    return mutation;
}

[[nodiscard]] MemoryMutation tombstone_mutation(const MemoryRecord &existing,
                                                std::uint64_t expected_version) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.type = MemoryMutationType::Tombstone;
    mutation.scope = existing.scope;
    mutation.target = existing.id;
    mutation.expected_version = expected_version;
    mutation.proposed = existing;
    mutation.evidence = existing.provenance;
    mutation.reason = MutationReasonCode::RetentionExpiry;
    return mutation;
}

struct Fixture final {
    Fixture(std::string name, IArtifactStore *artifacts = nullptr,
            std::chrono::seconds purge_horizon = std::chrono::seconds(0))
        : name_(std::move(name)) {
        executor_.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / (name_ + ".db");
        options.purge_horizon = purge_horizon;
        auto opened = SqliteMemoryStore::open(executor_, options, artifacts);
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

// The gap: a cross-scope pass reclaims what compact(scope) cannot reach.
int sweep_reclaims_expired_records_across_scopes() {
    Fixture fixture("scopes");
    const auto first = env_scope("device-a");
    const auto second = env_scope("device-b");
    const auto stale = expired_record(first, "display brightness defaults to 80 percent");
    const auto other = expired_record(second, "locale defaults to en-US");
    MIRA_CHECK(fixture.store->apply(add_mutation(stale)).has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(other)).has_value());
    const auto fresh = record(first, "screen layout is a 2x3 grid");
    MIRA_CHECK(fixture.store->apply(add_mutation(fresh)).has_value());

    auto swept = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(swept.has_value());
    MIRA_CHECK(swept.value().expired_records_purged == 2);
    MIRA_CHECK(swept.value().scopes_swept == 2);
    MIRA_CHECK(swept.value().erasures_failed == 0);
    MIRA_CHECK(!swept.value().more_pending);

    // Both expired records are gone, the fresh one stays.
    auto gone = fixture.store->get(stale.id, first);
    MIRA_CHECK(gone.has_value() && !gone.value().has_value());
    auto also_gone = fixture.store->get(other.id, second);
    MIRA_CHECK(also_gone.has_value() && !also_gone.value().has_value());
    auto kept = fixture.store->get(fresh.id, first);
    MIRA_CHECK(kept.has_value() && kept.value().has_value());

    // A second pass finds nothing left to do.
    auto again = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value().expired_records_purged == 0);
    MIRA_CHECK(!again.value().more_pending);
    return 0;
}

// A bounded pass stops at its bound and says so, so a large store is swept
// over several passes instead of one long transaction.
int sweep_is_bounded_and_resumable() {
    Fixture fixture("bounded");
    const auto scope = env_scope("device-a");
    std::vector<MemoryId> ids;
    for (int index = 0; index < 3; ++index) {
        const auto stale = expired_record(scope, "expired entry " + std::to_string(index));
        ids.push_back(stale.id);
        MIRA_CHECK(fixture.store->apply(add_mutation(stale)).has_value());
    }

    MemorySweepOptions options;
    options.max_records = 2;
    auto first = fixture.store->sweep(options);
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(first.value().expired_records_purged == 2);
    MIRA_CHECK(first.value().more_pending);

    auto second = fixture.store->sweep(options);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().expired_records_purged == 1);
    MIRA_CHECK(!second.value().more_pending);

    for (const auto &id : ids) {
        auto gone = fixture.store->get(id, scope);
        MIRA_CHECK(gone.has_value() && !gone.value().has_value());
    }
    return 0;
}

// Version history beyond the keep window is reclaimed across every scope.
int sweep_prunes_versions_beyond_the_keep_window() {
    Fixture fixture("versions");
    const auto scope = env_scope("device-a");
    const auto live = record(scope, "screen layout is a 2x3 grid");
    MIRA_CHECK(fixture.store->apply(add_mutation(live)).has_value());
    for (std::uint64_t version = 1; version <= 3; ++version) {
        const auto applied = fixture.store->apply(
            update_mutation(live, "screen layout revision " + std::to_string(version), version));
        MIRA_CHECK(applied.has_value());
    }

    auto swept = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(swept.has_value());
    // Four versions exist, the keep window holds the newest two.
    MIRA_CHECK(swept.value().versions_pruned == 2);

    // The record itself is untouched: pruning history is not reclaiming a
    // claim.
    auto kept = fixture.store->get(live.id, scope);
    MIRA_CHECK(kept.has_value() && kept.value().has_value());
    MIRA_CHECK(kept.value().value().version == 4);
    return 0;
}

// Tombstones older than the purge horizon are reclaimed; inside the
// horizon they stay.
int sweep_purges_stale_tombstones_past_the_horizon() {
    Fixture fixture("tombstones", nullptr, std::chrono::seconds(1));
    const auto scope = env_scope("device-a");
    const auto stale = record(scope, "forgotten fact");
    MIRA_CHECK(fixture.store->apply(add_mutation(stale)).has_value());
    MIRA_CHECK(fixture.store->apply(tombstone_mutation(stale, 1)).has_value());

    // Inside the horizon the tombstone is kept: it is still audit material.
    auto early = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(early.has_value());
    MIRA_CHECK(early.value().tombstones_purged == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
    auto late = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(late.has_value());
    MIRA_CHECK(late.value().tombstones_purged == 1);

    // The tombstone row is gone; the record was already out of retrieval.
    auto gone = fixture.store->get(stale.id, scope);
    MIRA_CHECK(gone.has_value() && !gone.value().has_value());
    return 0;
}

// An artifact store that rejects the first erasure, so a privacy erasure
// fails closed and holds its scope.
class FlakyArtifacts final : public IArtifactStore {
  public:
    explicit FlakyArtifacts(bool reject_next) : reject_next_(reject_next) {}
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &spec) override {
        return backing_.begin(spec);
    }
    Result<ArtifactDescriptor> commit(ArtifactWriter &writer) override {
        return backing_.commit(writer);
    }
    Result<ArtifactReader> open(const ArtifactDescriptor &descriptor) const override {
        return backing_.open(descriptor);
    }
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &request) override {
        if (reject_next_.exchange(false)) {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "test";
            error.safe_message = "artifact backend down";
            return error;
        }
        return backing_.erase(request);
    }

  private:
    MemoryArtifactStore backing_;
    std::atomic<bool> reject_next_;
};

// An artifact store that rejects every retention erasure but serves the
// privacy path.
class RetentionFailures final : public IArtifactStore {
  public:
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &spec) override {
        return backing_.begin(spec);
    }
    Result<ArtifactDescriptor> commit(ArtifactWriter &writer) override {
        return backing_.commit(writer);
    }
    Result<ArtifactReader> open(const ArtifactDescriptor &descriptor) const override {
        return backing_.open(descriptor);
    }
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &request) override {
        if (request.reason == "retention-expiry") {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "test";
            error.safe_message = "artifact backend down";
            return error;
        }
        return backing_.erase(request);
    }

  private:
    MemoryArtifactStore backing_;
};

// A record backed by a real evidence artifact.
[[nodiscard]] MemoryRecord with_artifact(IArtifactStore &artifacts, const MemoryScope &scope,
                                         std::string statement) {
    MemoryRecord value = expired_record(scope, std::move(statement));
    ArtifactWriteSpec spec;
    spec.media_type = "text/plain";
    auto writer = artifacts.begin(spec);
    if (!writer) {
        return value;
    }
    (void)writer.value().write("evidence payload", 17);
    auto committed = artifacts.commit(writer.value());
    if (!committed) {
        return value;
    }
    value.evidence =
        ArtifactRef{committed.value().id, committed.value().digest, committed.value().byte_size,
                    committed.value().media_type, committed.value().sensitivity};
    return value;
}

// A scope held for a pending privacy erasure belongs to that erasure: the
// sweep neither touches it nor releases its hold.
int sweep_leaves_held_scopes_alone() {
    FlakyArtifacts artifacts(true);
    Fixture fixture("held", &artifacts);
    const auto held_scope = env_scope("device-held");
    const auto other_scope = env_scope("device-other");
    const auto attached = with_artifact(artifacts, held_scope, "screenshot analysis attached");
    MIRA_CHECK(attached.evidence.has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(attached)).has_value());
    const auto purgeable = expired_record(other_scope, "locale defaults to en-US");
    MIRA_CHECK(fixture.store->apply(add_mutation(purgeable)).has_value());

    // The privacy erasure fails: the scope is held out of Context.
    ErasureRequest request;
    request.scope = held_scope;
    request.reason = "user deletion request";
    auto pending = fixture.store->erase(request);
    MIRA_CHECK(pending.has_value());
    MIRA_CHECK(pending.value().status == ErasureStatus::Pending);
    MIRA_CHECK(pending.value().held_scope.has_value());

    auto swept = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(swept.has_value());
    // The held scope's record is not a candidate; the other scope's is.
    MIRA_CHECK(swept.value().expired_records_purged == 1);
    auto still_held = fixture.store->get(attached.id, held_scope);
    MIRA_CHECK(still_held.has_value() && still_held.value().has_value());

    // The hold survives the sweep: the scope stays out of Context.
    MemoryQuery query;
    query.scopes = {held_scope};
    auto blocked = fixture.store->query(query);
    MIRA_CHECK(!blocked.has_value());
    MIRA_CHECK(blocked.error().domain_code ==
               static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));
    return 0;
}

// A retention erasure that fails is rolled back and counted: the record
// survives whole — row and lexical index — and the next pass retries it.
int failed_retention_erasure_is_rolled_back_and_counted() {
    RetentionFailures artifacts;
    Fixture fixture("failure", &artifacts);
    const auto scope = env_scope("device-a");
    const auto stale = with_artifact(artifacts, scope, "screenshot analysis attached");
    MIRA_CHECK(stale.evidence.has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(stale)).has_value());

    auto swept = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(swept.has_value());
    MIRA_CHECK(swept.value().expired_records_purged == 0);
    MIRA_CHECK(swept.value().erasures_failed == 1);
    MIRA_CHECK(!swept.value().note.empty());

    // The partial deletion was rolled back: the record is whole. The
    // erasure deletes the lexical row before the record row in one
    // sequence, so a surviving record row proves the whole sequence was
    // undone.
    auto survived = fixture.store->get(stale.id, scope);
    MIRA_CHECK(survived.has_value() && survived.value().has_value());
    // The record is expired, so no query sees it: it is out of retrieval
    // either way, and only the sweep can reclaim it.
    MemoryQuery plain;
    plain.scopes = {scope};
    plain.text = "screenshot analysis";
    auto invisible = fixture.store->query(plain);
    MIRA_CHECK(invisible.has_value());
    MIRA_CHECK(invisible.value().records.empty());
    // The live claims of the scope are untouched: a retention failure does
    // not take the scope out of Context.
    const auto live = record(scope, "screen layout is a 2x3 grid");
    MIRA_CHECK(fixture.store->apply(add_mutation(live)).has_value());
    MemoryQuery query;
    query.scopes = {scope};
    query.text = "screen layout";
    auto found = fixture.store->query(query);
    MIRA_CHECK(found.has_value());
    MIRA_CHECK(found.value().records.size() == 1);

    // The next pass retries the reclamation and reports the same failure.
    auto retried = fixture.store->sweep(MemorySweepOptions{});
    MIRA_CHECK(retried.has_value());
    MIRA_CHECK(retried.value().erasures_failed == 1);
    return 0;
}

// A per-scope compaction is a retention operation too: reclaiming an
// expired record must not release the hold a pending privacy erasure put
// on that scope. Clearing one unrelated record is not that erasure
// finishing.
int compaction_does_not_release_a_privacy_hold() {
    FlakyArtifacts artifacts(true);
    Fixture fixture("compact-held", &artifacts);
    const auto scope = env_scope("device-held");
    const auto attached = with_artifact(artifacts, scope, "screenshot analysis attached");
    MIRA_CHECK(attached.evidence.has_value());
    MIRA_CHECK(fixture.store->apply(add_mutation(attached)).has_value());

    // The privacy erasure fails: the scope is held out of Context.
    ErasureRequest request;
    request.scope = scope;
    request.reason = "user deletion request";
    auto pending = fixture.store->erase(request);
    MIRA_CHECK(pending.has_value());
    MIRA_CHECK(pending.value().status == ErasureStatus::Pending);
    MIRA_CHECK(pending.value().held_scope.has_value());

    // The retention compaction reclaims the expired record, and the hold
    // survives it.
    auto compacted = fixture.store->compact(scope);
    MIRA_CHECK(compacted.has_value());
    MIRA_CHECK(compacted.value().expired_records_purged == 1);

    MemoryQuery query;
    query.scopes = {scope};
    auto blocked = fixture.store->query(query);
    MIRA_CHECK(!blocked.has_value());
    MIRA_CHECK(blocked.error().domain_code ==
               static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));
    return 0;
}

// A pass that may not touch anything is rejected instead of silently doing
// nothing.
int sweep_rejects_an_empty_pass() {
    Fixture fixture("invalid");
    MemorySweepOptions options;
    options.max_records = 0;
    auto rejected = fixture.store->sweep(options);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(MemoryDomainCode::InvalidRecord));
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += sweep_reclaims_expired_records_across_scopes();
    failures += sweep_is_bounded_and_resumable();
    failures += sweep_prunes_versions_beyond_the_keep_window();
    failures += sweep_purges_stale_tombstones_past_the_horizon();
    failures += sweep_leaves_held_scopes_alone();
    failures += failed_retention_erasure_is_rolled_back_and_counted();
    failures += compaction_does_not_release_a_privacy_hold();
    failures += sweep_rejects_an_empty_pass();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
