#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <thread>
#include <iostream>

#include <executor/executor.hpp>

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-memory-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

[[nodiscard]] MemoryRecord record(const MemoryScope &scope, std::string statement,
                                  MemoryKind kind = MemoryKind::EnvironmentFact,
                                  MemoryVerification verification = MemoryVerification::Verified) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = scope;
    value.kind = kind;
    value.statement = std::move(statement);
    value.recorded_at = std::chrono::system_clock::now();
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    value.verification = verification;
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

[[nodiscard]] MemoryScope env_scope(std::string subject, std::optional<std::string> tenant = {}) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    scope.tenant_id = std::move(tenant);
    return scope;
}

[[nodiscard]] MemoryQueryResult run_query(const SqliteMemoryStore &store,
                                          const MemoryScope &scope, std::string text = {}) {
    MemoryQuery query;
    query.scopes = {scope};
    query.text = std::move(text);
    auto result = store.query(query);
    if (!result.has_value()) {
        std::cerr << "run_query failed: " << result.error().safe_message << '\n';
        std::abort();
    }
    return std::move(result).value();
}

int add_get_and_idempotent_replay() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-basic.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());
        MIRA_CHECK(store.value()->diagnostics().journal_mode == "wal");

        const auto scope = env_scope("device-42");
        auto base = record(scope, "display brightness defaults to 80 percent");
        const MemoryMutation mutation = add_mutation(base);
        auto applied = store.value()->apply(mutation);
        MIRA_CHECK(applied.has_value());
        MIRA_CHECK(applied.value().applied == MemoryMutationType::Add);
        MIRA_CHECK(applied.value().new_version == 1);

        // Replaying the same mutation id must not create a second record.
        auto replay = store.value()->apply(mutation);
        MIRA_CHECK(replay.has_value());
        MIRA_CHECK(replay.value().idempotent_replay);
        MIRA_CHECK(replay.value().new_version == 1);

        auto fetched = store.value()->get(base.id);
        MIRA_CHECK(fetched.has_value() && fetched.value().has_value());
        MIRA_CHECK(fetched.value()->statement == base.statement);
        MIRA_CHECK(fetched.value()->version == 1);

        auto found = run_query(*store.value(), scope);
        MIRA_CHECK(found.records.size() == 1);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int update_conflict_and_supersede_chain() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-versions.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());
        const auto scope = env_scope("device-42");
        auto original = record(scope, "volume=30");
        MIRA_CHECK(store.value()->apply(add_mutation(original)).has_value());

        // Update with a stale version must conflict.
        MemoryMutation stale;
        stale.id = MutationId::generate();
        stale.type = MemoryMutationType::Update;
        stale.scope = scope;
        stale.target = original.id;
        stale.expected_version = 99;
        stale.proposed = record(scope, "volume=31");
        stale.evidence = stale.proposed.provenance;
        auto conflict = store.value()->apply(stale);
        MIRA_CHECK(!conflict.has_value());
        MIRA_CHECK(conflict.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::VersionConflict));

        // Correct version updates and bumps the version. The update takes
        // effect later than the original so as-of queries can see each era.
        const auto original_time = original.recorded_at;
        MemoryMutation update = stale;
        update.id = MutationId::generate();
        update.expected_version = 1;
        update.proposed.validity.valid_from = original_time + std::chrono::seconds(60);
        auto updated = store.value()->apply(update);
        MIRA_CHECK(updated.has_value());
        MIRA_CHECK(updated.value().new_version == 2);

        // Supersede introduces a fresh record and closes the old interval.
        auto successor = record(scope, "volume=50");
        successor.validity.valid_from = original_time + std::chrono::seconds(120);
        MemoryMutation supersede;
        supersede.id = MutationId::generate();
        supersede.type = MemoryMutationType::Supersede;
        supersede.scope = scope;
        supersede.target = original.id;
        supersede.expected_version = 2;
        supersede.proposed = successor;
        supersede.proposed.supersedes = original.id;
        supersede.evidence = successor.provenance;
        auto superseded = store.value()->apply(supersede);
        MIRA_CHECK(superseded.has_value());

        // Now-mode retrieval only sees the successor.
        auto now_result = run_query(*store.value(), scope);
        MIRA_CHECK(now_result.records.size() == 1);
        MIRA_CHECK(now_result.records.front().statement == "volume=50");

        // Bitemporal replay: each validity era returns its own truth.
        MemoryQuery historical;
        historical.scopes = {scope};
        historical.as_of_valid = original_time + std::chrono::seconds(1);
        auto era_one = store.value()->query(historical);
        MIRA_CHECK(era_one.has_value());
        MIRA_CHECK(era_one.value().records.size() == 1);
        MIRA_CHECK(era_one.value().records.front().statement == "volume=30");
        historical.as_of_valid = original_time + std::chrono::seconds(70);
        auto era_two = store.value()->query(historical);
        MIRA_CHECK(era_two.has_value());
        MIRA_CHECK(era_two.value().records.size() == 1);
        MIRA_CHECK(era_two.value().records.front().statement == "volume=31");

        // A knowledge-time query before anything was recorded sees nothing.
        MemoryQuery prehistoric;
        prehistoric.scopes = {scope};
        prehistoric.as_of_recorded = original.recorded_at - std::chrono::hours(1);
        auto nothing = store.value()->query(prehistoric);
        MIRA_CHECK(nothing.has_value());
        MIRA_CHECK(nothing.value().records.empty());
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int tombstone_and_ttl_expiry() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-ttl.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());
        const auto scope = env_scope("device-42");

        auto keep = record(scope, "wifi band is 5ghz");
        MIRA_CHECK(store.value()->apply(add_mutation(keep)).has_value());

        auto expiring = record(scope, "temporary guest network active");
        expiring.recorded_at -= std::chrono::seconds(10);
        expiring.validity.valid_from = expiring.recorded_at;
        expiring.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(1);
        MIRA_CHECK(store.value()->apply(add_mutation(expiring)).has_value());

        // TTL-expired records never surface in queries.
        auto visible = run_query(*store.value(), scope);
        MIRA_CHECK(visible.records.size() == 1);
        MIRA_CHECK(visible.records.front().statement == "wifi band is 5ghz");

        // compact() purges the expired record physically (retention).
        auto swept = store.value()->compact(scope);
        MIRA_CHECK(swept.has_value());
        MIRA_CHECK(swept.value().expired_records_purged == 1);
        auto gone = store.value()->get(expiring.id);
        MIRA_CHECK(gone.has_value() && !gone.value().has_value());

        // Tombstone removes a record from retrieval without deleting it.
        MemoryMutation tombstone;
        tombstone.id = MutationId::generate();
        tombstone.type = MemoryMutationType::Tombstone;
        tombstone.scope = scope;
        tombstone.target = keep.id;
        tombstone.expected_version = 1;
        tombstone.reason = MutationReasonCode::HumanCorrection;
        tombstone.evidence = {EventId::generate()};
        tombstone.proposed = keep;
        auto buried = store.value()->apply(tombstone);
        MIRA_CHECK(buried.has_value());
        auto after_bury = run_query(*store.value(), scope);
        MIRA_CHECK(after_bury.records.empty());
        auto still_there = store.value()->get(keep.id);
        MIRA_CHECK(still_there.has_value() && still_there.value().has_value());
        MIRA_CHECK(still_there.value()->status == MemoryStatus::Tombstoned);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int scope_acl_and_provenance_enforcement() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-acl.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());

        const auto tenant_a = env_scope("user-1", "tenant-a");
        const auto tenant_b = env_scope("user-1", "tenant-b");
        const auto shared_subject = env_scope("user-1");

        MIRA_CHECK(
            store.value()->apply(add_mutation(record(tenant_a, "prefers dark mode"))).has_value());
        MIRA_CHECK(
            store.value()->apply(add_mutation(record(shared_subject, "lives in utc+8"))).has_value());

        // Tenant B never sees tenant A's record, even with identical content
        // and a matching subject id: scope equality is the ACL, not ranking.
        auto from_b = run_query(*store.value(), tenant_b, "dark mode");
        MIRA_CHECK(from_b.records.empty());

        // Same tenant, correct scope: visible.
        auto from_a = run_query(*store.value(), tenant_a, "dark mode");
        MIRA_CHECK(from_a.records.size() == 1);

        // Tenant-less scope is a different scope: no cross-tenant leak in
        // either direction.
        // A text query that only matches out-of-scope content returns an
        // empty answer, never the whole scope as a fallback.
        auto anonymous = run_query(*store.value(), shared_subject, "dark mode");
        MIRA_CHECK(anonymous.records.empty());
        auto anonymous_own = run_query(*store.value(), shared_subject, "utc+8");
        MIRA_CHECK(anonymous_own.records.size() == 1);
        MIRA_CHECK(anonymous_own.records.front().statement == "lives in utc+8");

        // Queries without an explicit scope allowlist are rejected outright.
        MemoryQuery no_scope;
        auto denied = store.value()->query(no_scope);
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().code == ErrorCode::PermissionDenied);

        // Imported records cannot claim HumanConfirmed.
        auto imported = record(tenant_a, "imported fact");
        imported.source_namespace = "web-import";
        imported.verification = MemoryVerification::HumanConfirmed;
        auto rejected = store.value()->apply(add_mutation(imported));
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::InvalidRecord));

        // Secret sensitivity never enters memory.
        auto secret = record(tenant_a, "bearer secret-token-value");
        secret.sensitivity = Sensitivity::Secret;
        auto secret_rejected = store.value()->apply(add_mutation(secret));
        MIRA_CHECK(!secret_rejected.has_value());

        // Sensitivity filter applies at query time.
        auto sensitive = record(tenant_a, "home address recorded");
        sensitive.sensitivity = Sensitivity::Sensitive;
        MIRA_CHECK(store.value()->apply(add_mutation(sensitive)).has_value());
        MemoryQuery capped;
        capped.scopes = {tenant_a};
        capped.max_sensitivity = Sensitivity::Internal;
        auto filtered = store.value()->query(capped);
        MIRA_CHECK(filtered.has_value());
        for (const auto &entry : filtered.value().records) {
            MIRA_CHECK(entry.sensitivity == Sensitivity::Public ||
                       entry.sensitivity == Sensitivity::Internal);
        }
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int erasure_complete_and_pending_hold() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        // Artifact store that rejects the first erasure to force the Pending
        // path deterministically.
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

        FlakyArtifacts artifacts(true);
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-erasure.db";
        auto store = SqliteMemoryStore::open(exec, options, &artifacts);
        MIRA_CHECK(store.has_value());
        const auto scope = env_scope("user-9", "tenant-x");

        // One record with a real evidence artifact, one without.
        auto with_artifact = record(scope, "screenshot analysis attached");
        ArtifactWriteSpec spec;
        spec.media_type = "text/plain";
        auto writer_result = artifacts.begin(spec);
        MIRA_CHECK(writer_result.has_value());
        MIRA_CHECK(writer_result.value().write("evidence payload", 17).has_value());
        auto committed = artifacts.commit(writer_result.value());
        MIRA_CHECK(committed.has_value());
        with_artifact.evidence = ArtifactRef{committed.value().id, committed.value().digest,
                                             committed.value().byte_size,
                                             committed.value().media_type,
                                             committed.value().sensitivity};
        MIRA_CHECK(store.value()->apply(add_mutation(with_artifact)).has_value());
        const auto plain = record(scope, "plain note");
        MIRA_CHECK(store.value()->apply(add_mutation(plain)).has_value());
        MIRA_CHECK(store.value()->get(plain.id).has_value());

        // First erasure fails on the artifact backend: stays Pending, holds
        // the scope out of Context (queries fail closed).
        ErasureRequest failing;
        failing.scope = scope;
        failing.reason = "user deletion request";
        auto pending = store.value()->erase(failing);
        MIRA_CHECK(pending.has_value());
        MIRA_CHECK(pending.value().status == ErasureStatus::Pending);
        MIRA_CHECK(pending.value().held_scope.has_value());

        MemoryQuery held;
        held.scopes = {scope};
        auto blocked = store.value()->query(held);
        MIRA_CHECK(!blocked.has_value());
        MIRA_CHECK(blocked.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));

        // Writes into the held scope are rejected too.
        auto refused = store.value()->apply(add_mutation(record(scope, "post-hold write")));
        MIRA_CHECK(!refused.has_value());

        // Retry completes: payload, versions, FTS, embeddings and artifact
        // references all go away and the hold clears.
        ErasureRequest retry;
        retry.scope = scope;
        retry.reason = "user deletion request (retry)";
        auto complete = store.value()->erase(retry);
        MIRA_CHECK(complete.has_value());
        MIRA_CHECK(complete.value().status == ErasureStatus::Complete);
        MIRA_CHECK(complete.value().counts.records_removed == 2);
        MIRA_CHECK(complete.value().counts.artifacts_erased == 1);

        auto reopened = run_query(*store.value(), scope);
        MIRA_CHECK(reopened.records.empty());
        ArtifactDescriptor descriptor;
        descriptor.id = committed.value().id;
        auto erased_artifact = artifacts.open(descriptor);
        MIRA_CHECK(!erased_artifact.has_value());
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int reopen_reuses_existing_schema() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-reopen.db";
        const auto scope = env_scope("device-7");
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition == StoreSchemaDisposition::Created);
            MIRA_CHECK(store.value()->apply(add_mutation(record(scope, "reboot at 3am"))).has_value());
            MIRA_CHECK(store.value()->close().has_value());
        }
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition == StoreSchemaDisposition::UpToDate);
            auto found = run_query(*store.value(), scope, "reboot");
            MIRA_CHECK(found.records.size() == 1);
            MIRA_CHECK(store.value()->close().has_value());
        }
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += add_get_and_idempotent_replay();
    failures += update_conflict_and_supersede_chain();
    failures += tombstone_and_ttl_expiry();
    failures += scope_acl_and_provenance_enforcement();
    failures += erasure_complete_and_pending_hold();
    failures += reopen_reuses_existing_schema();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
