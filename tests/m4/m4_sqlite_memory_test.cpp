#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>

#include <kairo/executor.hpp>

// The migration test needs to hand-craft a pre-1.1 store file, which the
// public API cannot express: the schema layout is the subject under test.
#include <sqlite3.h>

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

[[nodiscard]] MemoryQueryResult run_query(const SqliteMemoryStore &store, const MemoryScope &scope,
                                          std::string text = {}) {
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
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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

        auto fetched = store.value()->get(base.id, scope);
        MIRA_CHECK(fetched.has_value() && fetched.value().has_value());
        MIRA_CHECK(fetched.value()->statement == base.statement);
        MIRA_CHECK(fetched.value()->version == 1);

        auto found = run_query(*store.value(), scope);
        MIRA_CHECK(found.records.size() == 1);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int update_conflict_and_supersede_chain() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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

        // get() applies the scope ACL but not the lifecycle rules: the
        // superseded predecessor keeps its history and stays readable by id
        // inside its own scope (Tombstone and Supersede mean "no longer
        // retrieved", not "deleted").
        auto predecessor = store.value()->get(original.id, scope);
        MIRA_CHECK(predecessor.has_value() && predecessor.value().has_value());
        MIRA_CHECK(predecessor.value()->status == MemoryStatus::Superseded);
        // The predecessor row holds the statement it carried when it was
        // closed; the earlier "volume=30" era lives in the version history.
        MIRA_CHECK(predecessor.value()->statement == "volume=31");

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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int tombstone_and_ttl_expiry() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
        auto gone = store.value()->get(expiring.id, scope);
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
        auto still_there = store.value()->get(keep.id, scope);
        MIRA_CHECK(still_there.has_value() && still_there.value().has_value());
        MIRA_CHECK(still_there.value()->status == MemoryStatus::Tombstoned);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int scope_acl_and_provenance_enforcement() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
        MIRA_CHECK(store.value()
                       ->apply(add_mutation(record(shared_subject, "lives in utc+8")))
                       .has_value());

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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// A by-id read is bound to the caller's declared scope, exactly like a
// query. A record outside that scope must be indistinguishable from a
// record that does not exist; otherwise the read becomes an existence
// oracle across scopes and tenants.
int get_enforces_scope_acl() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-get-acl.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());

        const auto tenant_a = env_scope("user-1", "tenant-a");
        const auto tenant_b = env_scope("user-1", "tenant-b");
        const auto other_subject = env_scope("user-2", "tenant-a");
        const auto tenant_less = env_scope("user-1");

        const auto owned = record(tenant_a, "prefers dark mode");
        MIRA_CHECK(store.value()->apply(add_mutation(owned)).has_value());

        // The owning scope reads the record back.
        auto own = store.value()->get(owned.id, tenant_a);
        MIRA_CHECK(own.has_value() && own.value().has_value());
        MIRA_CHECK(own.value()->statement == "prefers dark mode");

        // Another tenant with the same subject, another subject in the same
        // tenant, and the tenant-less scope all read as absent.
        for (const auto &foreign : {tenant_b, other_subject, tenant_less}) {
            auto hidden = store.value()->get(owned.id, foreign);
            MIRA_CHECK(hidden.has_value());
            MIRA_CHECK(!hidden.value().has_value());
        }

        // An id that never existed reads as absent in every scope, so a
        // caller cannot tell "wrong scope" from "no such record".
        auto missing = store.value()->get(MemoryId::generate(), tenant_a);
        MIRA_CHECK(missing.has_value());
        MIRA_CHECK(!missing.value().has_value());

        // The rejected reads left the record untouched.
        auto still_own = store.value()->get(owned.id, tenant_a);
        MIRA_CHECK(still_own.has_value() && still_own.value().has_value());
        MIRA_CHECK(still_own.value()->version == 1);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// The scope check must precede the version comparison in apply(). If the
// version were compared first, the two different error codes would let a
// caller outside the record's scope binary-search its current version.
int apply_scope_check_precedes_version_check() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-apply-scope.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());

        const auto owner = env_scope("user-1", "tenant-a");
        const auto foreign = env_scope("user-1", "tenant-b");
        const auto original = record(owner, "volume=30");
        MIRA_CHECK(store.value()->apply(add_mutation(original)).has_value());

        // Wrong scope, wrong version: the scope denial wins, so the caller
        // learns nothing about the version.
        MemoryMutation probe;
        probe.id = MutationId::generate();
        probe.type = MemoryMutationType::Update;
        probe.scope = foreign;
        probe.target = original.id;
        probe.expected_version = 99;
        probe.proposed = record(foreign, "volume=31");
        probe.evidence = probe.proposed.provenance;
        auto denied = store.value()->apply(probe);
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::ScopeDenied));

        // Wrong scope, correct version: still denied, with the identical
        // error, so the pair reveals no version.
        MemoryMutation right_version = probe;
        right_version.id = MutationId::generate();
        right_version.expected_version = 1;
        auto denied_too = store.value()->apply(right_version);
        MIRA_CHECK(!denied_too.has_value());
        MIRA_CHECK(denied_too.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::ScopeDenied));

        // The owner still gets the version conflict: the reorder did not
        // weaken the optimistic-concurrency check.
        MemoryMutation owner_probe = probe;
        owner_probe.id = MutationId::generate();
        owner_probe.scope = owner;
        owner_probe.proposed = record(owner, "volume=31");
        auto conflict = store.value()->apply(owner_probe);
        MIRA_CHECK(!conflict.has_value());
        MIRA_CHECK(conflict.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::VersionConflict));

        // None of the denied attempts modified the record.
        auto unchanged = store.value()->get(original.id, owner);
        MIRA_CHECK(unchanged.has_value() && unchanged.value().has_value());
        MIRA_CHECK(unchanged.value()->version == 1);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// The idempotency ledger is keyed by mutation id alone. A caller holding
// another scope's mutation id must neither read that scope's ledger entry
// nor write through it.
int mutation_ledger_is_scope_bound() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-ledger-scope.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());

        const auto owner = env_scope("user-1", "tenant-a");
        const auto foreign = env_scope("user-1", "tenant-b");

        // The owner applies one mutation under a known mutation id.
        const auto owned = record(owner, "volume=30");
        const MemoryMutation applied = add_mutation(owned);
        auto first = store.value()->apply(applied);
        MIRA_CHECK(first.has_value());
        MIRA_CHECK(!first.value().idempotent_replay);
        MIRA_CHECK(first.value().record == owned.id);

        // The owner replaying the same mutation id still gets the stored
        // result: the ledger stays idempotent inside its scope.
        auto replay = store.value()->apply(applied);
        MIRA_CHECK(replay.has_value());
        MIRA_CHECK(replay.value().idempotent_replay);
        MIRA_CHECK(replay.value().record == owned.id);
        MIRA_CHECK(replay.value().new_version == first.value().new_version);

        // Another scope presenting the same mutation id is refused. It must
        // not learn the owner's record id or version.
        MemoryMutation stolen = applied; // same mutation id, foreign scope
        stolen.scope = foreign;
        stolen.proposed = record(foreign, "volume=31");
        stolen.evidence = stolen.proposed.provenance;
        auto denied = store.value()->apply(stolen);
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::ScopeDenied));

        // The refusal wrote nothing in the foreign scope.
        auto foreign_view = run_query(*store.value(), foreign);
        MIRA_CHECK(foreign_view.records.empty());

        // The owner is unaffected by the refused attempt.
        auto owner_view = run_query(*store.value(), owner);
        MIRA_CHECK(owner_view.records.size() == 1);
        MIRA_CHECK(owner_view.records.front().statement == "volume=30");
        MIRA_CHECK(owner_view.records.front().version == 1);

        // A mutation id the foreign scope has never used still applies
        // normally: scopes are independent, only the id is not shared.
        MemoryMutation independent = add_mutation(record(foreign, "volume=31"));
        auto own_write = store.value()->apply(independent);
        MIRA_CHECK(own_write.has_value());
        MIRA_CHECK(!own_write.value().idempotent_replay);
        auto foreign_after = run_query(*store.value(), foreign);
        MIRA_CHECK(foreign_after.records.size() == 1);
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// A store file written by a 1.0 reader carries no scope on its mutation
// ledger. Reopening it with the current reader migrates the ledger in place,
// keeps the records queryable and makes the pre-migration mutation ids
// replayable inside their own scope again.
int migration_adds_ledger_scope() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        const auto path = temp_dir() / "memory-migration.db";
        SqliteMemoryStoreOptions options;
        options.path = path;
        const auto scope = env_scope("device-legacy");

        const MemoryMutation applied = add_mutation(record(scope, "legacy fact"));
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition == StoreSchemaDisposition::Created);
            MIRA_CHECK(store.value()->apply(applied).has_value());
            MIRA_CHECK(store.value()->close().has_value());
        }
        // Rewrite the file into the 1.0 layout: no scope on the ledger and
        // the older recorded schema version.
        {
            sqlite3 *db = nullptr;
            // sqlite3_open takes a narrow path; std::filesystem::path::value_type
            // is wchar_t on Windows.
            MIRA_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
            MIRA_CHECK(sqlite3_exec(db, "ALTER TABLE memory_mutations DROP COLUMN scope_key;",
                                    nullptr, nullptr, nullptr) == SQLITE_OK);
            MIRA_CHECK(sqlite3_exec(db,
                                    "UPDATE store_meta SET value = '1.0'"
                                    " WHERE key = 'schema_version';",
                                    nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(db);
        }
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition ==
                       StoreSchemaDisposition::Migrated);
            // The record survived the migration.
            auto found = run_query(*store.value(), scope, "legacy");
            MIRA_CHECK(found.records.size() == 1);
            MIRA_CHECK(found.records.front().statement == "legacy fact");
            // The backfilled ledger still replays the pre-migration mutation
            // id inside its own scope.
            auto replay = store.value()->apply(applied);
            MIRA_CHECK(replay.has_value());
            MIRA_CHECK(replay.value().idempotent_replay);
            MIRA_CHECK(store.value()->close().has_value());
        }
        {
            // The migration is not repeated on the next open.
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition ==
                       StoreSchemaDisposition::UpToDate);
            auto found = run_query(*store.value(), scope, "legacy");
            MIRA_CHECK(found.records.size() == 1);
            MIRA_CHECK(store.value()->close().has_value());
        }
        {
            // A file stamped with a newer minor revision is refused, not
            // accepted as an older one and re-stamped with this reader's
            // version.
            sqlite3 *db = nullptr;
            MIRA_CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
            MIRA_CHECK(sqlite3_exec(db,
                                    "UPDATE store_meta SET value = '1.9'"
                                    " WHERE key = 'schema_version';",
                                    nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(db);
        }
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(!store.has_value());
            MIRA_CHECK(store.error().code == ErrorCode::UnsupportedVersion);
        }
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int kind_policy_bounds_the_store() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-kind-policy.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());
        const auto scope = env_scope("device-42");

        // The kind's default lifetime is a store default: a claim that
        // states no expiry of its own expires with its kind's TTL
        // (design Context/Memory §6.2).
        auto fact = record(scope, "the oven indicator is amber");
        fact.kind = MemoryKind::EnvironmentFact;
        MIRA_CHECK(store.value()->apply(add_mutation(fact)).has_value());
        auto stored = store.value()->get(fact.id, scope);
        MIRA_CHECK(stored.has_value() && stored.value().has_value());
        MIRA_CHECK(stored.value()->expires_at.has_value());
        MIRA_CHECK(*stored.value()->expires_at ==
                   fact.recorded_at + memory_kind_policy(MemoryKind::EnvironmentFact).default_ttl);

        // A record below the kind's verification floor never lands.
        auto unobserved = record(scope, "the oven indicator is amber");
        unobserved.kind = MemoryKind::EnvironmentFact;
        unobserved.verification = MemoryVerification::Unverified;
        auto refused = store.value()->apply(add_mutation(unobserved));
        MIRA_CHECK(!refused.has_value());
        MIRA_CHECK(refused.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::InvalidRecord));

        // A kind outside its scope allowlist never lands either: a procedure
        // is agent-owned operating knowledge, not user-profile content.
        MemoryScope user_scope;
        user_scope.kind = MemoryScopeKind::User;
        user_scope.subject_id = "user-7";
        auto misplaced = record(user_scope, "deploy only on tuesdays");
        misplaced.kind = MemoryKind::Procedure;
        auto denied = store.value()->apply(add_mutation(misplaced));
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::ScopeDenied));

        // Namespaced content is governed by the import contract instead.
        auto namespaced = misplaced;
        namespaced.source_namespace = "import/graphiti";
        MIRA_CHECK(store.value()->apply(add_mutation(namespaced)).has_value());

        // A tombstone retires the claim before its lifetime ends; the
        // retired row stays readable and is retained by the purge horizon
        // (design §13.4), so the claim's expiry no longer applies.
        MemoryMutation tombstone;
        tombstone.id = MutationId::generate();
        tombstone.type = MemoryMutationType::Tombstone;
        tombstone.scope = scope;
        tombstone.target = fact.id;
        tombstone.expected_version = 1;
        tombstone.reason = MutationReasonCode::HumanCorrection;
        tombstone.evidence = {EventId::generate()};
        tombstone.proposed = fact;
        MIRA_CHECK(store.value()->apply(tombstone).has_value());
        auto retired = store.value()->get(fact.id, scope);
        MIRA_CHECK(retired.has_value() && retired.value().has_value());
        MIRA_CHECK(retired.value()->status == MemoryStatus::Tombstoned);
        MIRA_CHECK(!retired.value()->expires_at.has_value());

        // A supersession closes the predecessor's claim: the predecessor
        // keeps its bitemporal era but loses its live lifetime, and the
        // successor carries the kind's own default.
        auto first = record(scope, "the oven indicator is amber");
        first.kind = MemoryKind::EnvironmentFact;
        MIRA_CHECK(store.value()->apply(add_mutation(first)).has_value());
        auto second = record(scope, "the oven indicator is green");
        second.kind = MemoryKind::EnvironmentFact;
        MemoryMutation supersede;
        supersede.id = MutationId::generate();
        supersede.type = MemoryMutationType::Supersede;
        supersede.scope = scope;
        supersede.target = first.id;
        supersede.expected_version = 1;
        supersede.proposed = second;
        supersede.proposed.supersedes = first.id;
        supersede.evidence = second.provenance;
        supersede.reason = MutationReasonCode::VerifiedEvent;
        MIRA_CHECK(store.value()->apply(supersede).has_value());
        auto predecessor = store.value()->get(first.id, scope);
        MIRA_CHECK(predecessor.has_value() && predecessor.value().has_value());
        MIRA_CHECK(predecessor.value()->status == MemoryStatus::Superseded);
        MIRA_CHECK(!predecessor.value()->expires_at.has_value());
        auto successor = store.value()->get(second.id, scope);
        MIRA_CHECK(successor.has_value() && successor.value().has_value());
        MIRA_CHECK(successor.value()->expires_at.has_value());
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int kind_policy_withholds_the_semantic_leg() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-kind-legs.db";
        auto store = SqliteMemoryStore::open(exec, options);
        MIRA_CHECK(store.has_value());
        const auto scope = env_scope("device-42");

        auto episode = record(scope, "task settled: outcome=completed sequence=7");
        episode.kind = MemoryKind::Episode;
        episode.verification = MemoryVerification::Observed;
        MIRA_CHECK(store.value()->apply(add_mutation(episode)).has_value());
        MIRA_CHECK(
            store.value()->index_embedding(episode.id, std::vector<float>{1.0F, 0.0F}).has_value());

        // An episode is a canonical structured record: embedding similarity
        // is not a retrieval mode for it (design Context/Memory §6.2).
        MemoryQuery filtered;
        filtered.scopes = {scope};
        filtered.kinds = std::vector<MemoryKind>{MemoryKind::Episode};
        filtered.query_embedding = {0.9F, 0.1F};
        auto withheld = store.value()->query(filtered);
        MIRA_CHECK(withheld.has_value());
        MIRA_CHECK(withheld.value().quality.semantic_leg_withheld);
        MIRA_CHECK(!withheld.value().quality.vector_leg_ran);
        // The withheld embedding is not a ranking request: the kind-filtered
        // set is the answer.
        MIRA_CHECK(withheld.value().records.size() == 1);
        MIRA_CHECK(withheld.value().records.front().id == episode.id);

        // Without a kind filter the semantic leg runs as before.
        MemoryQuery unfiltered;
        unfiltered.scopes = {scope};
        unfiltered.query_embedding = {0.9F, 0.1F};
        auto semantic = store.value()->query(unfiltered);
        MIRA_CHECK(semantic.has_value());
        MIRA_CHECK(semantic.value().quality.vector_leg_ran);
        MIRA_CHECK(!semantic.value().quality.semantic_leg_withheld);
        MIRA_CHECK(!semantic.value().records.empty());
        MIRA_CHECK(store.value()->close().has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int erasure_complete_and_pending_hold() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
        with_artifact.evidence =
            ArtifactRef{committed.value().id, committed.value().digest, committed.value().byte_size,
                        committed.value().media_type, committed.value().sensitivity};
        MIRA_CHECK(store.value()->apply(add_mutation(with_artifact)).has_value());
        const auto plain = record(scope, "plain note");
        MIRA_CHECK(store.value()->apply(add_mutation(plain)).has_value());
        MIRA_CHECK(store.value()->get(plain.id, scope).has_value());

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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int reopen_reuses_existing_schema() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / "memory-reopen.db";
        const auto scope = env_scope("device-7");
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition == StoreSchemaDisposition::Created);
            MIRA_CHECK(
                store.value()->apply(add_mutation(record(scope, "reboot at 3am"))).has_value());
            MIRA_CHECK(store.value()->close().has_value());
        }
        {
            auto store = SqliteMemoryStore::open(exec, options);
            MIRA_CHECK(store.has_value());
            MIRA_CHECK(store.value()->diagnostics().disposition ==
                       StoreSchemaDisposition::UpToDate);
            auto found = run_query(*store.value(), scope, "reboot");
            MIRA_CHECK(found.records.size() == 1);
            MIRA_CHECK(store.value()->close().has_value());
        }
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += add_get_and_idempotent_replay();
    failures += update_conflict_and_supersede_chain();
    failures += tombstone_and_ttl_expiry();
    failures += scope_acl_and_provenance_enforcement();
    failures += get_enforces_scope_acl();
    failures += apply_scope_check_precedes_version_check();
    failures += mutation_ledger_is_scope_bound();
    failures += migration_adds_ledger_scope();
    failures += kind_policy_bounds_the_store();
    failures += kind_policy_withholds_the_semantic_leg();
    failures += erasure_complete_and_pending_hold();
    failures += reopen_reuses_existing_schema();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
