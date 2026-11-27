#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <sqlite3.h>

namespace {
using namespace mira;
using namespace mira::testing;

class InterruptedArtifacts final : public IArtifactStore {
  public:
    explicit InterruptedArtifacts(const std::filesystem::path &path) : store_(path) {}
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &spec) override {
        return store_.begin(spec);
    }
    Result<ArtifactDescriptor> commit(ArtifactWriter &writer) override {
        return store_.commit(writer);
    }
    Result<ArtifactReader> open(const ArtifactDescriptor &artifact) const override {
        return store_.open(artifact);
    }
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &request) override {
        // Simulate a successful tombstone whose payload deletion has not been acknowledged.
        if (interrupt_) {
            interrupt_ = false;
            return ErasureReceipt{request.id, false, true};
        }
        return store_.erase(request);
    }

  private:
    FileArtifactStore store_;
    bool interrupt_ = true;
};

MemoryMutation add(const MemoryScope &scope, std::string statement) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = scope;
    mutation.proposed.id = MemoryId::generate();
    mutation.proposed.scope = scope;
    mutation.proposed.kind = MemoryKind::Preference;
    mutation.proposed.statement = std::move(statement);
    mutation.proposed.verification = MemoryVerification::Observed;
    mutation.proposed.provenance = {EventId::generate()};
    mutation.evidence = mutation.proposed.provenance;
    return mutation;
}

MemoryUseReceipt use_receipt(const MemoryScope &scope, MemoryId record, std::uint64_t version) {
    return {ModelRequestId::generate(), ModelRequestId::generate(),
            TaskId::generate(),         digest_string("selected memory"),
            Timestamp::now().wall,      {{memory_erasure_scope_digest(scope), record, version}}};
}

int erases_only_the_bound_version_and_recovers_the_receipt() {
    MemoryAgentFixture fixture;
    InterruptedArtifacts artifacts(fixture.root / "evidence");
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "guarded-erasure.db";
    auto store = SqliteMemoryStore::open(fixture.executor, options, &artifacts);
    MIRA_CHECK(store);
    const auto reopen = [&] {
        if (!store.value()->close())
            return false;
        store = SqliteMemoryStore::open(fixture.executor, options, &artifacts);
        return static_cast<bool>(store);
    };
    const auto scope = memory_user_scope(fixture.principal);
    const auto other = memory_session_scope(fixture.principal, SessionId::generate());
    auto added = add(scope, "answer.style=brief");
    auto writer = artifacts.begin(ArtifactWriteSpec{});
    MIRA_CHECK(writer && writer.value().write("evidence", 8));
    const auto evidence = artifacts.commit(writer.value());
    MIRA_CHECK(evidence);
    const auto &artifact = evidence.value();
    added.proposed.evidence = ArtifactRef{artifact.id, artifact.digest, artifact.byte_size,
                                          artifact.media_type, artifact.sensitivity};
    MIRA_CHECK(store.value()->apply(added));
    const auto anchor = add(scope, "timezone=UTC");
    const auto outsider = add(other, "units=metric");
    MIRA_CHECK(store.value()->apply(anchor) && store.value()->apply(outsider));
    auto expired = add(scope, "locale=en");
    expired.proposed.recorded_at = std::chrono::system_clock::now() - 2h;
    expired.proposed.expires_at = expired.proposed.recorded_at + 1h;
    MIRA_CHECK(store.value()->apply(expired));

    auto updated = added;
    updated.id = MutationId::generate();
    updated.type = MemoryMutationType::Update;
    updated.target = added.proposed.id;
    updated.expected_version = 1;
    updated.proposed.statement = "answer.style=concise";
    const auto changed = store.value()->apply(updated);
    MIRA_CHECK(changed && changed.value().new_version == 2);
    const std::vector<float> vector{1.0F, 0.0F};
    MIRA_CHECK(store.value()->index_embedding(added.proposed.id, EmbedderId::generate(), vector));
    const MemoryRecordErasureRequest request{OperationId::generate(), scope, added.proposed.id, 2};
    auto wrong = request;
    wrong.scope = other;
    const auto foreign = store.value()->erase_record(wrong);
    wrong.record = MemoryId::generate();
    const auto missing = store.value()->erase_record(wrong);
    MIRA_CHECK(!foreign && !missing && foreign.error().domain_code == missing.error().domain_code &&
               foreign.error().safe_message == missing.error().safe_message);
    wrong = request;
    wrong.expected_version = 1;
    const auto stale = store.value()->erase_record(wrong);
    MIRA_CHECK(!stale &&
               stale.error().domain_code == static_cast<int>(MemoryDomainCode::VersionConflict));
    MIRA_CHECK(store.value()->get(added.proposed.id, scope).value()->version == 2);
    const auto use = use_receipt(scope, added.proposed.id, 2);
    MIRA_CHECK(store.value()->record_memory_use(use).value().recorded == 1);

    const auto pending = store.value()->erase_record(request);
    MIRA_CHECK(pending && pending.value().erasure.status == ErasureStatus::Pending &&
               pending.value().erasure.held_scope == scope &&
               pending.value().erasure.counts.records_removed == 0);
    MIRA_CHECK(reopen());
    const auto held = store.value()->get(anchor.proposed.id, scope);
    MIRA_CHECK(!held &&
               held.error().domain_code == static_cast<int>(MemoryDomainCode::ErasurePending));
    const auto held_usage = store.value()->memory_usage(scope, added.proposed.id, 2);
    MIRA_CHECK(!held_usage && held_usage.error().domain_code == held.error().domain_code);
    MIRA_CHECK(store.value()->record_memory_use(use).value().skipped == 1);
    MIRA_CHECK(store.value()->get(outsider.proposed.id, other).value().has_value());
    MIRA_CHECK(!store.value()->compact(scope));
    MIRA_CHECK(!store.value()->erase({scope, {}, "scope erase while a record erasure is pending"}));
    const auto swept = store.value()->sweep(MemorySweepOptions{});
    MIRA_CHECK(swept && swept.value().expired_records_purged == 0 &&
               swept.value().erasures_failed == 0);
    wrong = request;
    wrong.operation = OperationId::generate();
    const auto competing = store.value()->erase_record(wrong);
    MIRA_CHECK(!competing &&
               competing.error().domain_code == static_cast<int>(MemoryDomainCode::ErasurePending));

    const auto completed = store.value()->erase_record(request);
    MIRA_CHECK(completed && !completed.value().idempotent_replay &&
               completed.value().erasure.status == ErasureStatus::Complete &&
               !completed.value().erasure.held_scope);
    const auto &counts = completed.value().erasure.counts;
    MIRA_CHECK(counts.records_removed == 1 && counts.versions_removed == 2 &&
               counts.fts_entries_removed == 1 && counts.embeddings_removed == 1 &&
               counts.artifacts_erased == 1);
    MIRA_CHECK(!store.value()->get(added.proposed.id, scope).value().has_value() &&
               store.value()->get(anchor.proposed.id, scope).value().has_value() &&
               store.value()->get(expired.proposed.id, scope).value().has_value() &&
               !artifacts.open(artifact));
    MemoryQuery query;
    query.scopes = {scope};
    query.text = "concise";
    MIRA_CHECK(store.value()->query(query).value().records.empty());
    MIRA_CHECK(reopen());
    MIRA_CHECK(!store.value()->memory_usage(scope, added.proposed.id, 2).value());
    MIRA_CHECK(store.value()->record_memory_use(use).value().skipped == 1);
    const auto replay = store.value()->erase_record(request);
    MIRA_CHECK(replay && replay.value().idempotent_replay &&
               replay.value().erasure.counts.versions_removed == counts.versions_removed &&
               replay.value().erasure.counts.artifacts_erased == counts.artifacts_erased);
    added.id = MutationId::generate();
    const auto resurrected = store.value()->apply(added);
    MIRA_CHECK(!resurrected && resurrected.error().domain_code ==
                                   static_cast<int>(MemoryDomainCode::VersionConflict));
    MIRA_CHECK(store.value()->close());
    return 0;
}

int durable_usage_is_deduplicated_and_follows_the_source() {
    MemoryAgentFixture fixture;
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "usage.db";
    auto store = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(store);
    const auto scope = memory_user_scope(fixture.principal);
    const auto other = memory_session_scope(fixture.principal, SessionId::generate());
    const auto added = add(scope, "locale=en");
    const auto anchor = add(scope, "timezone=UTC");
    auto expired_record = add(scope, "theme=dark");
    expired_record.proposed.recorded_at = std::chrono::system_clock::now() - 2h;
    expired_record.proposed.expires_at = expired_record.proposed.recorded_at + 1h;
    MIRA_CHECK(store.value()->apply(added) && store.value()->apply(anchor) &&
               store.value()->apply(expired_record));
    const auto id = added.proposed.id;
    const auto empty = store.value()->memory_usage(scope, id, 1);
    MIRA_CHECK(empty && empty.value() && empty.value()->use_count == 0 &&
               !empty.value()->last_used);
    MIRA_CHECK(!store.value()->memory_usage(other, id, 1).value());

    const auto first = use_receipt(scope, id, 1);
    auto newest = use_receipt(scope, id, 1);
    newest.used_at = first.used_at + 1ns;
    MIRA_CHECK(store.value()->record_memory_use(newest).value().recorded == 1);
    auto mixed = first;
    mixed.records.push_back(first.records.front());
    mixed.records.push_back({memory_erasure_scope_digest(other), anchor.proposed.id, 1});
    mixed.records.push_back({memory_erasure_scope_digest(scope), expired_record.proposed.id, 1});
    const auto written = store.value()->record_memory_use(mixed);
    MIRA_CHECK(written && written.value().recorded == 1 && written.value().duplicates == 1 &&
               written.value().skipped == 2);
    MIRA_CHECK(store.value()->close());
    auto readonly_options = options;
    readonly_options.read_only_diagnostic = true;
    auto readonly = SqliteMemoryStore::open(fixture.executor, readonly_options);
    MIRA_CHECK(readonly);
    const auto diagnostic = readonly.value()->memory_usage(scope, id, 1);
    MIRA_CHECK(diagnostic && diagnostic.value() && diagnostic.value()->use_count == 2);
    MIRA_CHECK(!readonly.value()->record_memory_use(first));
    MIRA_CHECK(readonly.value()->close());
    store = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(store);
    auto replay = first;
    replay.attempt = ModelRequestId::generate();
    replay.used_at = Timestamp::now().wall;
    MIRA_CHECK(store.value()->record_memory_use(replay).value().duplicates == 1);
    const auto projected = store.value()->memory_usage(scope, id, 1);
    MIRA_CHECK(projected && projected.value() && projected.value()->use_count == 2 &&
               projected.value()->last_used == newest.used_at);

    auto updated = added;
    updated.id = MutationId::generate();
    updated.type = MemoryMutationType::Update;
    updated.target = id;
    updated.expected_version = 1;
    updated.proposed.statement = "locale=zh";
    MIRA_CHECK(store.value()->apply(updated).value().new_version == 2);
    MIRA_CHECK(!store.value()->memory_usage(scope, id, 1).value());
    MIRA_CHECK(store.value()->record_memory_use(first).value().skipped == 1);
    MIRA_CHECK(store.value()->memory_usage(scope, id, 2).value()->use_count == 0);
    const auto use_new = use_receipt(scope, id, 2);
    const auto use_anchor = use_receipt(scope, anchor.proposed.id, 1);
    MIRA_CHECK(store.value()->record_memory_use(use_new).value().recorded == 1);
    MIRA_CHECK(store.value()->record_memory_use(use_anchor).value().recorded == 1);
    MIRA_CHECK(store.value()->erase_record({OperationId::generate(), scope, id, 2}));
    MIRA_CHECK(store.value()->memory_usage(scope, anchor.proposed.id, 1).value()->use_count == 1);
    MIRA_CHECK(store.value()->erase({scope, {}, "verified scope erasure"}));
    MIRA_CHECK(store.value()->record_memory_use(use_new).value().skipped == 1);
    MIRA_CHECK(store.value()->record_memory_use(use_anchor).value().skipped == 1);
    MIRA_CHECK(store.value()->close());

    // Verify physical row removal independently of the visibility filter.
    sqlite3 *raw = nullptr;
    const int opened =
        sqlite3_open_v2(options.path.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    MIRA_CHECK(opened == SQLITE_OK);
    sqlite3_stmt *row = nullptr;
    const int prepared = sqlite3_prepare_v2(db.get(),
                                            "SELECT (SELECT count(*) FROM memory_use_requests),"
                                            " (SELECT count(*) FROM memory_usage)",
                                            -1, &row, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(row, sqlite3_finalize);
    MIRA_CHECK(prepared == SQLITE_OK && sqlite3_step(stmt.get()) == SQLITE_ROW &&
               sqlite3_column_int(stmt.get(), 0) == 0 && sqlite3_column_int(stmt.get(), 1) == 0);
    return 0;
}
} // namespace

int main() try {
    const int erasure = erases_only_the_bound_version_and_recovers_the_receipt();
    return erasure == 0 ? durable_usage_is_deduplicated_and_follows_the_source() : erasure;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "record erasure test raised an unknown exception\n";
    return 1;
}
