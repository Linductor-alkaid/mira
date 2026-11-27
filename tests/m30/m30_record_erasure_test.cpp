#include "support/m36_support.hpp"
#include "support/test.hpp"

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

    const auto pending = store.value()->erase_record(request);
    MIRA_CHECK(pending && pending.value().erasure.status == ErasureStatus::Pending &&
               pending.value().erasure.held_scope == scope &&
               pending.value().erasure.counts.records_removed == 0);
    MIRA_CHECK(reopen());
    const auto held = store.value()->get(anchor.proposed.id, scope);
    MIRA_CHECK(!held &&
               held.error().domain_code == static_cast<int>(MemoryDomainCode::ErasurePending));
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
} // namespace

int main() { return erases_only_the_bound_version_and_recovers_the_receipt(); }
