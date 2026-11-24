#include "memory_capacity_vfs.hpp"
#include "sqlite_store_open.hpp"
#include <mira/embedding_math.hpp>
#include <mira/social_profile.hpp>
#include <mira/sqlite_memory_store.hpp>

#include "sqlite_support.hpp"

#include <mira/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace mira {

namespace {

using storage::DatabaseHandle;
using storage::Statement;
using storage::StoreChannel;
using storage::Transaction;

[[nodiscard]] Error store_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.state_store.memory";
    error.safe_message = std::move(message);
    return error;
}

constexpr SchemaVersion kMemoryStoreSchema{1, 8};
constexpr const char *kStoreKind = "memory";
constexpr const char *kSocialFieldIndex =
    "CREATE INDEX IF NOT EXISTS memory_records_by_social_field"
    " ON memory_records(scope_key, json_extract(document, '$.social_attribute.field'),"
    " recorded_at DESC, id) WHERE kind = 'social-attribute';";
constexpr const char *kSocialRelationshipIndexes =
    "CREATE INDEX IF NOT EXISTS memory_records_by_social_predicate"
    " ON memory_records(scope_key, json_extract(document, '$.social_relationship.predicate'),"
    " recorded_at DESC, id) WHERE kind = 'social-relationship';"
    "CREATE INDEX IF NOT EXISTS memory_records_by_social_from"
    " ON memory_records(tenant_id, substr(subject_id, 1, 65)) WHERE kind = 'social-relationship';"
    "CREATE INDEX IF NOT EXISTS memory_records_by_social_to"
    " ON memory_records(tenant_id, substr(subject_id, 1, 33) || substr(subject_id, 67))"
    " WHERE kind = 'social-relationship';";
constexpr const char *kSocialEnvironmentIndex =
    "CREATE INDEX IF NOT EXISTS memory_records_by_social_environment"
    " ON memory_records(tenant_id, subject_id)"
    " WHERE kind IN ('social-attribute', 'social-relationship');";
constexpr const char *kWorldPropertyIndex =
    "CREATE INDEX IF NOT EXISTS memory_records_by_world_property"
    " ON memory_records(scope_key, json_extract(document, '$.world_property.entity'),"
    " json_extract(document, '$.world_property.property'), valid_from DESC, recorded_at DESC, id)"
    " WHERE scope_kind IN ('world-environment','world-application');";
constexpr const char *kRecoveryLessonKeySql =
    "json_extract(CASE WHEN json_valid(statement) THEN statement END, '$.workflow_id',"
    " '$.ir_digest', '$.failure.workflow_id', '$.failure.step_id', '$.failure.step_kind',"
    " '$.failure.reason_code')";

[[nodiscard]] std::string recovery_lesson_index_sql() {
    return std::string("CREATE INDEX IF NOT EXISTS memory_records_by_recovery_key"
                       " ON memory_records(scope_key, ") +
           kRecoveryLessonKeySql + ", recorded_at DESC, id) WHERE kind='recovery-lesson';";
}

[[nodiscard]] std::string recovery_lesson_key_value(const RecoveryLessonKey &key) {
    const auto workflow = key.workflow.to_string();
    return canonical_json_string(
        JsonValue::Array{workflow, key.ir_digest.to_string(), workflow,
                         key.step ? JsonValue{key.step->to_string()} : JsonValue{},
                         key.step_kind ? JsonValue{*key.step_kind} : JsonValue{}, key.reason_code});
}

constexpr const char *kSchemaDdl =
    "CREATE TABLE IF NOT EXISTS store_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS memory_records("
    " id TEXT PRIMARY KEY,"
    " scope_key TEXT NOT NULL,"
    " scope_kind TEXT NOT NULL,"
    " subject_id TEXT NOT NULL,"
    " tenant_id TEXT,"
    " kind TEXT NOT NULL,"
    " statement TEXT NOT NULL,"
    " evidence TEXT,"
    " valid_from INTEGER NOT NULL,"
    " valid_until INTEGER,"
    " recorded_at INTEGER NOT NULL,"
    " verification TEXT NOT NULL,"
    " confidence REAL NOT NULL,"
    " sensitivity TEXT NOT NULL,"
    " sensitivity_rank INTEGER NOT NULL,"
    " status TEXT NOT NULL,"
    " supersedes TEXT,"
    " expires_at INTEGER,"
    " source_namespace TEXT,"
    " labels TEXT NOT NULL DEFAULT '[]',"
    " importance REAL NOT NULL DEFAULT 0.5,"
    " schema_major INTEGER NOT NULL,"
    " schema_minor INTEGER NOT NULL,"
    " version INTEGER NOT NULL,"
    " updated_at INTEGER NOT NULL,"
    " document TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS memory_records_by_scope"
    " ON memory_records(scope_key, status, recorded_at DESC);"
    "CREATE TABLE IF NOT EXISTS memory_versions("
    " record_id TEXT NOT NULL,"
    " version INTEGER NOT NULL,"
    " recorded_at INTEGER NOT NULL,"
    " valid_from INTEGER NOT NULL,"
    " valid_until INTEGER,"
    " status TEXT NOT NULL,"
    " document TEXT NOT NULL,"
    " PRIMARY KEY(record_id, version));"
    "CREATE TABLE IF NOT EXISTS memory_provenance("
    " record_id TEXT NOT NULL,"
    " event_id TEXT NOT NULL,"
    " PRIMARY KEY(record_id, event_id));"
    "CREATE TABLE IF NOT EXISTS memory_mutations("
    " mutation_id TEXT PRIMARY KEY,"
    " scope_key TEXT NOT NULL DEFAULT '',"
    " applied_type TEXT NOT NULL,"
    " record_id TEXT NOT NULL,"
    " result_version INTEGER NOT NULL,"
    " applied_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS memory_embeddings("
    " record_id TEXT PRIMARY KEY,"
    " embedder TEXT NOT NULL DEFAULT '',"
    " dim INTEGER NOT NULL,"
    " vector BLOB NOT NULL,"
    " embedded_at INTEGER NOT NULL);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5"
    "(record_id UNINDEXED, statement, tokenize='unicode61');"
    "CREATE TABLE IF NOT EXISTS memory_erasure_log("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " scope_key TEXT,"
    " record_id TEXT,"
    " reason TEXT NOT NULL,"
    " requested_at INTEGER NOT NULL,"
    " completed_at INTEGER,"
    " status TEXT NOT NULL,"
    " removed_records INTEGER NOT NULL DEFAULT 0,"
    " removed_versions INTEGER NOT NULL DEFAULT 0,"
    " removed_fts INTEGER NOT NULL DEFAULT 0,"
    " removed_embeddings INTEGER NOT NULL DEFAULT 0,"
    " artifacts_erased INTEGER NOT NULL DEFAULT 0,"
    " note TEXT);"
    "CREATE TABLE IF NOT EXISTS memory_scope_holds("
    " scope_key TEXT PRIMARY KEY,"
    " reason TEXT NOT NULL,"
    " since INTEGER NOT NULL);"
    "";

[[nodiscard]] std::string scope_key_of(const MemoryScope &scope) {
    std::string key = memory_scope_kind_name(scope.kind);
    key += '|';
    key += scope.subject_id;
    key += '|';
    key += scope.tenant_id.value_or(std::string{});
    return key;
}

enum class LegacyAddState { Missing, Recorded, Owned };

[[nodiscard]] Result<LegacyAddState> legacy_add_state(sqlite3 *db, const LegacyMemoryAdd &legacy,
                                                      const std::string &scope_key) {
    Statement row(db, "SELECT scope_key, applied_type, record_id, result_version"
                      " FROM memory_mutations WHERE mutation_id = ?1");
    const auto id = legacy.mutation_id.to_string();
    if (!row.valid() ||
        sqlite3_bind_text(row.get(), 1, id.c_str(), -1, storage::transient_copy()) != SQLITE_OK)
        return store_error(ErrorCode::Internal, "legacy add lookup failed to prepare");
    const int step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return LegacyAddState::Missing;
    if (step != SQLITE_ROW)
        return store_error(ErrorCode::Internal, "legacy add lookup failed");

    const auto *owner = reinterpret_cast<const char *>(sqlite3_column_text(row.get(), 0));
    if (owner == nullptr || *owner == '\0')
        return make_memory_error(MemoryDomainCode::ScopeDenied, "legacy add scope is unknown");
    if (owner != scope_key)
        return LegacyAddState::Recorded;

    const auto *type = reinterpret_cast<const char *>(sqlite3_column_text(row.get(), 1));
    const auto *record = reinterpret_cast<const char *>(sqlite3_column_text(row.get(), 2));
    const auto version = sqlite3_column_int64(row.get(), 3);
    if (type == nullptr || record == nullptr || record != legacy.record_id.to_string())
        return store_error(ErrorCode::DataLoss, "legacy add receipt does not match its identity");
    if (std::string_view(type) == "add" && version == 1)
        return LegacyAddState::Owned;
    if (std::string_view(type) == "noop" && version == 0)
        return LegacyAddState::Recorded;
    return store_error(ErrorCode::DataLoss, "legacy add receipt has an invalid result");
}

[[nodiscard]] Result<void> reserve_legacy_add(sqlite3 *db, const LegacyMemoryAdd &legacy,
                                              const std::string &scope_key) {
    // A writer that still uses the old identity sees a completed Noop or a
    // scope denial through the normal replay path, so it cannot insert a copy.
    Statement log(db, "INSERT INTO memory_mutations(mutation_id, scope_key, applied_type,"
                      " record_id, result_version, applied_at) VALUES(?1, ?2, 'noop', ?3, 0, ?4)");
    const auto mutation_id = legacy.mutation_id.to_string();
    const auto record_id = legacy.record_id.to_string();
    if (!log.valid() ||
        sqlite3_bind_text(log.get(), 1, mutation_id.c_str(), -1, storage::transient_copy()) !=
            SQLITE_OK ||
        sqlite3_bind_text(log.get(), 2, scope_key.c_str(), -1, storage::transient_copy()) !=
            SQLITE_OK ||
        sqlite3_bind_text(log.get(), 3, record_id.c_str(), -1, storage::transient_copy()) !=
            SQLITE_OK ||
        sqlite3_bind_int64(log.get(), 4, wall_nanos(std::chrono::system_clock::now())) !=
            SQLITE_OK ||
        sqlite3_step(log.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return store_error(ErrorCode::Internal, "legacy add reservation failed");
    return {};
}

// True when the mutation ledger already carries the scope that owns each
// entry (schema 1.1 and later).
[[nodiscard]] bool ledger_has_scope_column(sqlite3 *db) {
    Statement query(db, "PRAGMA table_info(memory_mutations)");
    if (!query.valid()) {
        return false;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 1));
        if (name != nullptr && std::string_view(name) == "scope_key") {
            return true;
        }
    }
    return false;
}

// Schema 1.0 -> 1.1: the mutation ledger records the scope that owns each
// entry, so an idempotent replay can be refused when the caller declares a
// different scope than the mutation was applied in.
//
// Existing rows are backfilled from the live record and then from the
// erasure audit trail; both ledgers are append-only, so an erased record
// stays attributable there. A row whose record was removed by a scope-level
// erasure has no per-record audit entry and keeps the empty placeholder.
// Such a ledger entry is refused rather than replayed: the alternative would
// hand an unattributable scope's record id to whoever presents the id.
[[nodiscard]] Result<void> migrate_memory_schema_1_0_to_1_1(sqlite3 *db) {
    constexpr const char *kBackfill =
        "UPDATE memory_mutations SET scope_key ="
        " (SELECT m.scope_key FROM memory_records m WHERE m.id = memory_mutations.record_id)"
        " WHERE scope_key = '';"
        "UPDATE memory_mutations SET scope_key ="
        " (SELECT e.scope_key FROM memory_erasure_log e"
        "  WHERE e.record_id = memory_mutations.record_id AND e.scope_key IS NOT NULL"
        "  ORDER BY e.id DESC LIMIT 1)"
        " WHERE scope_key = '';";
    constexpr const char *kAddColumn =
        "ALTER TABLE memory_mutations ADD COLUMN scope_key TEXT NOT NULL DEFAULT '';";
    const std::string sql = std::string(ledger_has_scope_column(db) ? "" : kAddColumn) + kBackfill;
    return storage::exec_sql(db, sql.c_str());
}

// True when the embedding projection already carries the producer identity
// column (schema 1.2 and later).
[[nodiscard]] bool embeddings_have_embedder_column(sqlite3 *db) {
    Statement query(db, "PRAGMA table_info(memory_embeddings)");
    if (!query.valid()) {
        return false;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 1));
        if (name != nullptr && std::string_view(name) == "embedder") {
            return true;
        }
    }
    return false;
}

// True when the record table already carries the metadata columns (schema
// 1.3 and later).
[[nodiscard]] bool records_have_metadata_columns(sqlite3 *db) {
    Statement query(db, "PRAGMA table_info(memory_records)");
    if (!query.valid()) {
        return false;
    }
    bool labels = false;
    bool importance = false;
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 1));
        if (name == nullptr) {
            continue;
        }
        labels = labels || std::string_view(name) == "labels";
        importance = importance || std::string_view(name) == "importance";
    }
    return labels && importance;
}

// Schema 1.2 -> 1.3: structured metadata columns for labels and importance
// (DEC-112). Purely additive - existing rows take the neutral defaults
// (empty annotation, 0.5) and the projection keeps the authoritative copy in
// the document JSON, so nothing is cleared.
[[nodiscard]] Result<void> migrate_memory_schema_1_2_to_1_3(sqlite3 *db) {
    if (records_have_metadata_columns(db)) {
        return {};
    }
    constexpr const char *kAddColumns =
        "ALTER TABLE memory_records ADD COLUMN labels TEXT NOT NULL DEFAULT '[]';"
        "ALTER TABLE memory_records ADD COLUMN importance REAL NOT NULL DEFAULT 0.5;";
    return storage::exec_sql(db, kAddColumns);
}

// Schema 1.1 -> 1.2: every embedding records the identity of the producer
// that created it (DEC-111), so the cosine leg only ever compares vectors of
// the same geometry.
//
// Pre-1.2 rows carry no attribution and no honest identity exists for them,
// so the projection is cleared: it is a rebuildable projection (RULE-07) and
// index lag immediately reports every Active record as pending re-embedding.
// Keeping unattributed rows would fake a zero lag while the vector leg could
// never use them. The store stays writable and exact/FTS legs keep answering
// while the host drives repair passes with the configured producer.
[[nodiscard]] Result<void> migrate_memory_schema_1_1_to_1_2(sqlite3 *db) {
    constexpr const char *kAddColumn =
        "ALTER TABLE memory_embeddings ADD COLUMN embedder TEXT NOT NULL DEFAULT '';";
    constexpr const char *kClearLegacy = "DELETE FROM memory_embeddings;";
    std::string sql;
    if (!embeddings_have_embedder_column(db)) {
        sql += kAddColumn;
    }
    sql += kClearLegacy;
    return storage::exec_sql(db, sql.c_str());
}

[[nodiscard]] int sensitivity_rank(Sensitivity sensitivity) {
    switch (sensitivity) {
    case Sensitivity::Public:
        return 0;
    case Sensitivity::Internal:
        return 1;
    case Sensitivity::Sensitive:
        return 2;
    case Sensitivity::Secret:
        return 3;
    }
    return 3;
}

struct Row final {
    std::string id;
    std::string scope_key;
    std::string statement;
    std::int64_t recorded_at = 0;
    double confidence = 0.0;
    std::string verification;
    std::string kind;
    int sensitivity_rank_value = 1;
    double importance = 0.5;
    MemoryRecord decoded;
};

[[nodiscard]] Result<Row> decode_row(sqlite3_stmt *row) {
    const auto *document = reinterpret_cast<const char *>(sqlite3_column_text(row, 11));
    if (document == nullptr) {
        return store_error(ErrorCode::DataLoss, "memory row has no document");
    }
    auto parsed = parse_json(document);
    if (!parsed) {
        return store_error(ErrorCode::DataLoss, "memory document is not valid JSON");
    }
    auto record = memory_record_from_json(parsed.value());
    if (!record) {
        return store_error(ErrorCode::UnsupportedVersion,
                           "memory document schema is not supported");
    }
    MemoryRecord value = std::move(record).value();
    Row decoded;
    decoded.id = value.id.to_string();
    decoded.scope_key = scope_key_of(value.scope);
    decoded.statement = value.statement;
    decoded.recorded_at = wall_nanos(value.recorded_at);
    decoded.confidence = value.confidence;
    decoded.verification = memory_verification_name(value.verification);
    decoded.kind = memory_kind_name(value.kind);
    decoded.sensitivity_rank_value = sensitivity_rank(value.sensitivity);
    decoded.importance = value.importance;
    decoded.decoded = std::move(value);
    return decoded;
}

} // namespace

Result<void> RetrievalWeights::validate() const {
    for (const double weight :
         {exact, fts, vector, verification, confidence, recency, importance}) {
        if (!(weight >= 0.0 && weight <= 1.0) || std::isnan(weight)) {
            return store_error(ErrorCode::InvalidArgument, "ranking weights must be within [0, 1]");
        }
    }
    return Result<void>{};
}

Result<void> SqliteMemoryStoreOptions::validate() const {
    if (path.empty()) {
        return store_error(ErrorCode::InvalidArgument, "store path must not be empty");
    }
    if (max_pending_requests == 0) {
        return store_error(ErrorCode::InvalidArgument, "store bounds must be greater than zero");
    }
    if (operation_timeout.count() <= 0 || busy_timeout.count() < 0) {
        return store_error(ErrorCode::InvalidArgument, "store timeouts must be positive");
    }
    if (max_vector_scan == 0 || keep_versions_per_record == 0) {
        return store_error(ErrorCode::InvalidArgument, "store scan bounds must be positive");
    }
    if (!(near_dup_threshold >= 0.0 && near_dup_threshold <= 1.0) ||
        std::isnan(near_dup_threshold)) {
        return store_error(ErrorCode::InvalidArgument,
                           "near duplicate threshold must be within [0, 1]");
    }
    if (const auto valid = weights.validate(); !valid) {
        return valid.error();
    }
    if (max_storage_bytes != 0 &&
        (max_storage_bytes < 1024 * 1024 || max_storage_bytes > (1ULL << 40) ||
         capacity_reserve_bytes >= max_storage_bytes || read_only_diagnostic))
        return store_error(ErrorCode::InvalidArgument, "invalid exclusive memory capacity limits");
    return Result<void>{};
}

class SqliteMemoryStore::Impl final {
  public:
    Impl(SqliteMemoryStoreOptions options, StoreDiagnostics diagnostics, DatabaseHandle database,
         std::unique_ptr<StoreChannel> channel, IArtifactStore *artifacts,
         std::unique_ptr<storage::MemoryCapacityVfs> capacity)
        : options_(std::move(options)), diagnostics_(std::move(diagnostics)),
          capacity_(std::move(capacity)), database_(std::move(database)),
          channel_(std::move(channel)), artifacts_(artifacts), weights_(options_.weights) {}

    template <class T, class Op> Result<T> run(Op op) const {
        return channel_->run<T>([this, op = std::move(op)](sqlite3 *db) -> Result<T> {
            struct ResetCapacity {
                storage::MemoryCapacityVfs *capacity;
                ~ResetCapacity() {
                    if (capacity) {
                        capacity->reset_rejection();
                    }
                }
            };
            ResetCapacity reset{capacity_.get()};
            if (capacity_) {
                capacity_->reset_rejection();
            }
            auto result = op(db);
            if (!result && capacity_ && capacity_->rejected())
                return storage::memory_capacity_error();
            return result;
        });
    }

    [[nodiscard]] Result<void> check_writable() const {
        if (read_only_) {
            return store_error(ErrorCode::InvalidState,
                               "memory store is open in read-only diagnostic mode");
        }
        return Result<void>{};
    }

    // ---- scope holds (pending erasure fail-closed) ----

    [[nodiscard]] static Result<void> check_no_hold(sqlite3 *db, const std::string &scope_key) {
        Statement hold(db, "SELECT reason FROM memory_scope_holds WHERE scope_key = ?1");
        if (!hold.valid()) {
            return store_error(ErrorCode::Internal, "scope hold check failed to prepare");
        }
        sqlite3_bind_text(hold.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
        const int step = sqlite3_step(hold.get());
        if (step == SQLITE_ROW) {
            return make_memory_error(
                MemoryDomainCode::ErasurePending,
                "scope is held out of context by a pending erasure; retry the erasure");
        }
        if (step != SQLITE_DONE)
            return store_error(ErrorCode::Internal, "scope hold check failed");
        return Result<void>{};
    }

    [[nodiscard]] static Result<void> check_scope_holds(sqlite3 *db, const MemoryScope &scope,
                                                        const std::string &scope_key) {
        auto primary = check_no_hold(db, scope_key);
        if (!primary || !is_social_scope(scope.kind) ||
            scope.kind == MemoryScopeKind::SocialEnvironment)
            return primary;
        const auto environment = social_environment_scope_of(scope);
        if (!environment)
            return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid social scope");
        if (auto held = check_no_hold(db, scope_key_of(*environment)); !held)
            return held.error();
        if (scope.kind != MemoryScopeKind::SocialRelationship)
            return {};
        const auto endpoints = social_relationship_endpoints(scope);
        if (!endpoints)
            return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid relationship scope");
        for (const auto &endpoint : {endpoints->from, endpoints->to}) {
            const auto object_scope = social_object_scope(endpoint);
            if (!object_scope)
                return object_scope.error();
            if (auto held = check_no_hold(db, scope_key_of(object_scope.value())); !held)
                return held.error();
        }
        return {};
    }

    // ---- record plumbing ----

    static void bind_record_insert(Statement &insert, const MemoryRecord &record,
                                   const std::string &scope_key, std::uint64_t version) {
        const std::string id = record.id.to_string();
        sqlite3_bind_text(insert.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(insert.get(), 2, scope_key.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(insert.get(), 3, memory_scope_kind_name(record.scope.kind).c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_text(insert.get(), 4, record.scope.subject_id.c_str(), -1,
                          storage::transient_copy());
        if (record.scope.tenant_id.has_value()) {
            sqlite3_bind_text(insert.get(), 5, record.scope.tenant_id->c_str(), -1,
                              storage::transient_copy());
        } else {
            sqlite3_bind_null(insert.get(), 5);
        }
        sqlite3_bind_text(insert.get(), 6, memory_kind_name(record.kind).c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_text(insert.get(), 7, record.statement.c_str(), -1, storage::transient_copy());
        sqlite3_bind_int64(insert.get(), 9, wall_nanos(record.validity.valid_from));
        if (record.validity.valid_until.has_value()) {
            sqlite3_bind_int64(insert.get(), 10, wall_nanos(*record.validity.valid_until));
        } else {
            sqlite3_bind_null(insert.get(), 10);
        }
        sqlite3_bind_int64(insert.get(), 11, wall_nanos(record.recorded_at));
        sqlite3_bind_text(insert.get(), 12, memory_verification_name(record.verification).c_str(),
                          -1, storage::transient_copy());
        sqlite3_bind_double(insert.get(), 13, static_cast<double>(record.confidence));
        sqlite3_bind_text(insert.get(), 14, sensitivity_name(record.sensitivity).c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_int(insert.get(), 15, sensitivity_rank(record.sensitivity));
        sqlite3_bind_text(insert.get(), 16, memory_status_name(record.status).c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_int64(insert.get(), 20, record.schema_version.major);
        sqlite3_bind_int64(insert.get(), 21, record.schema_version.minor);
        sqlite3_bind_int64(insert.get(), 22, static_cast<std::int64_t>(version));
    }

    [[nodiscard]] static Result<void> sync_derived_rows(sqlite3 *db, const MemoryRecord &record,
                                                        std::uint64_t version) {
        const std::string id = record.id.to_string();
        Statement prune_provenance(db, "DELETE FROM memory_provenance WHERE record_id = ?1");
        if (!prune_provenance.valid()) {
            return store_error(ErrorCode::Internal, "provenance sync failed to prepare");
        }
        sqlite3_bind_text(prune_provenance.get(), 1, id.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(prune_provenance.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "provenance sync failed");
        }
        Statement provenance(db, "INSERT OR IGNORE INTO memory_provenance(record_id, event_id)"
                                 " VALUES(?1, ?2)");
        if (!provenance.valid()) {
            return store_error(ErrorCode::Internal, "provenance insert failed to prepare");
        }
        for (const auto &event : record.provenance) {
            const std::string event_id = event.to_string();
            sqlite3_bind_text(provenance.get(), 1, id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(provenance.get(), 2, event_id.c_str(), -1, storage::transient_copy());
            if (sqlite3_step(provenance.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "provenance insert failed");
            }
            provenance.reset();
        }
        Statement drop_fts(db, "DELETE FROM memory_fts WHERE record_id = ?1");
        if (!drop_fts.valid()) {
            return store_error(ErrorCode::Internal, "fts sync failed to prepare");
        }
        sqlite3_bind_text(drop_fts.get(), 1, id.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(drop_fts.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "fts sync failed");
        }
        if (record.status == MemoryStatus::Active && !is_social_assertion(record.kind) &&
            !record.world_property) {
            Statement insert_fts(db, "INSERT INTO memory_fts(record_id, statement) VALUES(?1, ?2)");
            if (!insert_fts.valid()) {
                return store_error(ErrorCode::Internal, "fts insert failed to prepare");
            }
            sqlite3_bind_text(insert_fts.get(), 1, id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(insert_fts.get(), 2, record.statement.c_str(), -1,
                              storage::transient_copy());
            if (sqlite3_step(insert_fts.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "fts insert failed");
            }
        }
        Statement drop_version(db, "DELETE FROM memory_versions WHERE record_id = ?1"
                                   " AND version = ?2");
        if (!drop_version.valid()) {
            return store_error(ErrorCode::Internal, "version sync failed to prepare");
        }
        sqlite3_bind_text(drop_version.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_int64(drop_version.get(), 2, static_cast<std::int64_t>(version));
        if (sqlite3_step(drop_version.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "version sync failed");
        }
        Statement insert_version(
            db, "INSERT INTO memory_versions(record_id, version, recorded_at, valid_from,"
                " valid_until, status, document) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
        if (!insert_version.valid()) {
            return store_error(ErrorCode::Internal, "version insert failed to prepare");
        }
        sqlite3_bind_text(insert_version.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_int64(insert_version.get(), 2, static_cast<std::int64_t>(version));
        const auto knowledge_time =
            is_social_assertion(record.kind) && record.status == MemoryStatus::Tombstoned
                ? std::max(record.recorded_at, std::chrono::system_clock::now())
                : record.recorded_at;
        sqlite3_bind_int64(insert_version.get(), 3, wall_nanos(knowledge_time));
        sqlite3_bind_int64(insert_version.get(), 4, wall_nanos(record.validity.valid_from));
        if (record.validity.valid_until.has_value()) {
            sqlite3_bind_int64(insert_version.get(), 5, wall_nanos(*record.validity.valid_until));
        } else {
            sqlite3_bind_null(insert_version.get(), 5);
        }
        sqlite3_bind_text(insert_version.get(), 6, memory_status_name(record.status).c_str(), -1,
                          storage::transient_copy());
        const std::string document = to_json_string(memory_record_to_json(record));
        sqlite3_bind_text(insert_version.get(), 7, document.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(insert_version.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "version insert failed");
        }
        return Result<void>{};
    }

    [[nodiscard]] static Result<std::optional<std::uint64_t>> current_version(sqlite3 *db,
                                                                              const MemoryId &id) {
        Statement query(db, "SELECT version FROM memory_records WHERE id = ?1");
        if (!query.valid()) {
            return store_error(ErrorCode::Internal, "version read failed to prepare");
        }
        const std::string text = id.to_string();
        sqlite3_bind_text(query.get(), 1, text.c_str(), -1, storage::transient_copy());
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) {
            return std::optional<std::uint64_t>{};
        }
        if (step != SQLITE_ROW) {
            return store_error(ErrorCode::Internal, "version read failed");
        }
        return std::optional<std::uint64_t>(
            static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0)));
    }

    // Drops the vector of one record. The vector index is a projection of
    // the authoritative statement (design §14.1/§16.2): a record whose text
    // changed, or which left the live plane, must not keep a vector that no
    // longer describes it. The record then counts as index lag until the
    // projection is rebuilt.
    [[nodiscard]] static Result<void> drop_embedding(sqlite3 *db, const MemoryId &id) {
        Statement drop(db, "DELETE FROM memory_embeddings WHERE record_id = ?1");
        if (!drop.valid()) {
            return store_error(ErrorCode::Internal, "embedding drop failed to prepare");
        }
        const std::string text = id.to_string();
        sqlite3_bind_text(drop.get(), 1, text.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(drop.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "embedding drop failed");
        }
        return Result<void>{};
    }

    [[nodiscard]] static Result<std::optional<MemoryRecord>> read_record(sqlite3 *db,
                                                                         const MemoryId &id) {
        Statement query(
            db, "SELECT id, scope_key, kind, statement, recorded_at, confidence, verification,"
                " sensitivity_rank, status, evidence, valid_from, document FROM memory_records"
                " WHERE id = ?1");
        if (!query.valid()) {
            return store_error(ErrorCode::Internal, "record read failed to prepare");
        }
        const std::string text = id.to_string();
        sqlite3_bind_text(query.get(), 1, text.c_str(), -1, storage::transient_copy());
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) {
            return std::optional<MemoryRecord>{};
        }
        if (step != SQLITE_ROW) {
            return store_error(ErrorCode::Internal, "record read failed");
        }
        auto row = decode_row(query.get());
        if (!row) {
            return row.error();
        }
        return std::optional<MemoryRecord>(row.value().decoded);
    }

    // Reads one record under the caller's declared scope. A record outside
    // that scope is reported as absent: a by-id read must not be able to
    // distinguish it from a record that never existed, or it becomes an
    // existence oracle across scopes (design Context/Memory §8.3).
    //
    // Lifecycle is deliberately NOT filtered here. Tombstone means "no longer
    // retrieved", not "deleted" (design §13.4: privacy erasure has its own
    // physical deletion path, and Tombstone must not be used to fake one), so
    // a tombstoned, superseded or expired row stays readable by id for audit
    // and retention work inside its own scope.
    [[nodiscard]] static Result<std::optional<MemoryRecord>>
    read_scoped_record(sqlite3 *db, const MemoryId &id, const MemoryScope &scope) {
        auto existing = read_record(db, id);
        if (!existing) {
            return existing.error();
        }
        if (!existing.value().has_value()) {
            return std::optional<MemoryRecord>{};
        }
        const auto scope_key = scope_key_of(scope);
        if (scope_key_of(existing.value()->scope) != scope_key) {
            return std::optional<MemoryRecord>{};
        }
        if (is_social_scope(scope.kind) || is_world_scope(scope.kind)) {
            if (auto held = check_scope_holds(db, scope, scope_key); !held)
                return held.error();
        }
        return existing.value();
    }

    // Writes one record row (24 columns, ?1..?24) at the given version.
    [[nodiscard]] static Result<void> write_record(sqlite3 *db, const MemoryRecord &record,
                                                   const std::string &scope_key,
                                                   std::uint64_t version) {
        // The stored document carries the store's version, not the caller's:
        // the version is the optimistic-concurrency token the backend sets on
        // read (mira/memory_contracts.hpp), so a read-modify-write cycle
        // must not observe a stale one.
        MemoryRecord indexed = record;
        indexed.version = version;
        const std::string document = to_json_string(memory_record_to_json(indexed));
        Statement upsert(
            db, "INSERT INTO memory_records(id, scope_key, scope_kind, subject_id, tenant_id,"
                " kind, statement, evidence, valid_from, valid_until, recorded_at,"
                " verification, confidence, sensitivity, sensitivity_rank, status, supersedes,"
                " expires_at, source_namespace, schema_major, schema_minor, version,"
                " updated_at, document, labels, importance)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15,"
                " ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26)"
                " ON CONFLICT(id) DO UPDATE SET"
                " statement = excluded.statement, evidence = excluded.evidence,"
                " valid_from = excluded.valid_from, valid_until = excluded.valid_until,"
                " recorded_at = excluded.recorded_at, verification = excluded.verification,"
                " confidence = excluded.confidence, sensitivity = excluded.sensitivity,"
                " sensitivity_rank = excluded.sensitivity_rank, status = excluded.status,"
                " supersedes = excluded.supersedes, expires_at = excluded.expires_at,"
                " source_namespace = excluded.source_namespace,"
                " version = excluded.version, updated_at = excluded.updated_at,"
                " document = excluded.document, labels = excluded.labels,"
                " importance = excluded.importance");
        if (!upsert.valid()) {
            return store_error(ErrorCode::Internal, "record upsert failed to prepare");
        }
        bind_record_insert(upsert, indexed, scope_key, version);
        if (record.evidence.has_value()) {
            const std::string evidence =
                to_json_string(JsonValue::Object{{"id", record.evidence->id.to_string()}});
            sqlite3_bind_text(upsert.get(), 8, evidence.c_str(), -1, storage::transient_copy());
        } else {
            sqlite3_bind_null(upsert.get(), 8);
        }
        if (record.supersedes.has_value()) {
            const std::string supersedes = record.supersedes->to_string();
            sqlite3_bind_text(upsert.get(), 17, supersedes.c_str(), -1, storage::transient_copy());
        } else {
            sqlite3_bind_null(upsert.get(), 17);
        }
        if (record.expires_at.has_value()) {
            sqlite3_bind_int64(upsert.get(), 18, wall_nanos(*record.expires_at));
        } else {
            sqlite3_bind_null(upsert.get(), 18);
        }
        if (record.source_namespace.has_value()) {
            sqlite3_bind_text(upsert.get(), 19, record.source_namespace->c_str(), -1,
                              storage::transient_copy());
        } else {
            sqlite3_bind_null(upsert.get(), 19);
        }
        sqlite3_bind_int64(upsert.get(), 23, wall_nanos(std::chrono::system_clock::now()));
        sqlite3_bind_text(upsert.get(), 24, document.c_str(), -1, storage::transient_copy());
        JsonValue::Array label_values;
        for (const auto &label : record.labels) {
            label_values.emplace_back(JsonValue(label));
        }
        const std::string labels_json = to_json_string(JsonValue(std::move(label_values)));
        sqlite3_bind_text(upsert.get(), 25, labels_json.c_str(), -1, storage::transient_copy());
        sqlite3_bind_double(upsert.get(), 26, static_cast<double>(record.importance));
        if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "record upsert failed");
        }
        return sync_derived_rows(db, indexed, version);
    }

    [[nodiscard]] Result<MemoryMutationResult> apply(sqlite3 *db, const MemoryMutation &mutation,
                                                     const LegacyMemoryAdd *legacy = nullptr) {
        const auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        const auto valid = mutation.validate();
        if (!valid) {
            return valid.error();
        }
        if (mutation.type == MemoryMutationType::Noop) {
            MemoryMutationResult result;
            result.applied = MemoryMutationType::Noop;
            result.record = mutation.proposed.id;
            return result;
        }
        const std::string scope_key = scope_key_of(mutation.scope);
        Transaction transaction(db);
        if (!transaction.valid()) {
            return store_error(ErrorCode::Internal, "mutation transaction rejected");
        }
        const auto hold = check_scope_holds(db, mutation.scope, scope_key);
        if (!hold) {
            return hold.error();
        }
        const std::string mutation_id = mutation.id.to_string();
        {
            Statement replay(db, "SELECT scope_key, applied_type, record_id, result_version"
                                 " FROM memory_mutations WHERE mutation_id = ?1");
            if (!replay.valid()) {
                return store_error(ErrorCode::Internal, "mutation replay failed to prepare");
            }
            sqlite3_bind_text(replay.get(), 1, mutation_id.c_str(), -1, storage::transient_copy());
            const int step = sqlite3_step(replay.get());
            if (step == SQLITE_ROW) {
                // The ledger is keyed by mutation id alone. Honouring a replay
                // whose entry was written in another scope would hand that
                // scope's record id and version to the caller, so a mutation
                // id is only replayable from the scope it was applied in. A
                // ledger row without a scope (a pre-migration entry that
                // could not be attributed) is refused as well.
                const auto *scope_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(replay.get(), 0));
                const std::string replay_scope = scope_text != nullptr ? scope_text : "";
                if (replay_scope != scope_key) {
                    return make_memory_error(MemoryDomainCode::ScopeDenied,
                                             "mutation id belongs to another scope");
                }
                MemoryMutationResult result;
                const auto *type_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(replay.get(), 1));
                result.applied =
                    type_text != nullptr && memory_mutation_type_from(type_text).has_value()
                        ? *memory_mutation_type_from(type_text)
                        : MemoryMutationType::Noop;
                const auto *record_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(replay.get(), 2));
                if (auto parsed = MemoryId::parse(record_text != nullptr ? record_text : "");
                    parsed) {
                    result.record = *parsed;
                }
                result.new_version =
                    static_cast<std::uint64_t>(sqlite3_column_int64(replay.get(), 3));
                result.idempotent_replay = true;
                return result; // read-only; the transaction rolls back
            }
            if (step != SQLITE_DONE)
                return store_error(ErrorCode::Internal, "mutation replay failed");
        }

        bool reserve_legacy = false;
        if (legacy != nullptr) {
            const auto state = legacy_add_state(db, *legacy, scope_key);
            if (!state)
                return state.error();
            if (state.value() == LegacyAddState::Owned)
                return MemoryMutationResult{MemoryMutationType::Noop, legacy->record_id, 1, true};
            reserve_legacy = state.value() == LegacyAddState::Missing;
        }
        if (capacity_) {
            const auto current = capacity_->snapshot();
            if (!current)
                return current.error();
            if (current.value().file_bytes >=
                options_.max_storage_bytes - options_.capacity_reserve_bytes)
                return storage::memory_capacity_error();
            capacity_->set_operation_limit(options_.max_storage_bytes -
                                           options_.capacity_reserve_bytes);
        }
        if (reserve_legacy) {
            if (auto reserved = reserve_legacy_add(db, *legacy, scope_key); !reserved)
                return reserved.error();
        }
        MemoryRecord proposed = mutation.proposed;
        proposed.scope = mutation.scope;
        // The kind's default lifetime is a store default, not a caller duty:
        // a claim that states no expiry of its own expires with its kind's
        // TTL (design Context/Memory §6.2). An unstamped recording time has
        // no meaningful base, so the write clock is used instead.
        if (!proposed.expires_at.has_value() && mutation.type != MemoryMutationType::Tombstone) {
            auto base = proposed.recorded_at;
            if (base.time_since_epoch().count() <= 0) {
                base = std::chrono::system_clock::now();
            }
            proposed.expires_at = memory_default_expiry(proposed.kind, base);
        }
        std::uint64_t new_version = 1;
        switch (mutation.type) {
        case MemoryMutationType::Add: {
            const auto existing = current_version(db, proposed.id);
            if (!existing) {
                return existing.error();
            }
            if (existing.value().has_value()) {
                if ((is_social_assertion(proposed.kind) || proposed.world_property) &&
                    *existing.value() == 1) {
                    auto prior = read_record(db, proposed.id);
                    if (!prior)
                        return prior.error();
                    if (prior.value() && prior.value()->scope == proposed.scope &&
                        memory_record_to_json(*prior.value()) == memory_record_to_json(proposed)) {
                        return MemoryMutationResult{MemoryMutationType::Add, proposed.id, 1, true};
                    }
                }
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "add target already exists");
            }
            proposed.status = MemoryStatus::Active;
            new_version = 1;
            break;
        }
        case MemoryMutationType::Update:
        case MemoryMutationType::Supersede:
        case MemoryMutationType::Tombstone: {
            // Read the target and settle the scope before comparing versions.
            // The scope check must precede the version comparison: otherwise
            // the two different error codes let a caller outside the record's
            // scope binary-search its current version.
            auto existing = read_record(db, *mutation.target);
            if (!existing) {
                return existing.error();
            }
            if (!existing.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "mutation target does not exist");
            }
            if (scope_key_of(existing.value()->scope) != scope_key) {
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "mutations cannot move a record across scopes");
            }
            auto existing_version = current_version(db, *mutation.target);
            if (!existing_version) {
                return existing_version.error();
            }
            if (!existing_version.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "mutation target vanished mid-transaction");
            }
            if (*existing_version.value() != *mutation.expected_version) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "memory version moved on; re-read and re-plan");
            }
            if (mutation.type == MemoryMutationType::Tombstone) {
                proposed = *existing.value();
                proposed.status = MemoryStatus::Tombstoned;
                // The tombstone retires the claim before its lifetime ends.
                // The retired row is retained by the purge horizon instead
                // (design §13.4), so the claim's expiry no longer applies.
                proposed.expires_at.reset();
                const auto dropped = drop_embedding(db, *mutation.target);
                if (!dropped) {
                    return dropped.error();
                }
                new_version = *existing_version.value() + 1;
                break;
            }
            if (mutation.type == MemoryMutationType::Supersede) {
                if (proposed.id == *mutation.target) {
                    return make_memory_error(MemoryDomainCode::InvalidMutation,
                                             "supersede must introduce a fresh record id");
                }
                // Close the predecessor: its own version history records the
                // interval end so bitemporal as-of queries exclude it after
                // the successor's valid_from.
                MemoryRecord closed = *existing.value();
                closed.status = MemoryStatus::Superseded;
                closed.validity.valid_until = proposed.validity.valid_from;
                // A closed claim is history, not a live claim: its lifetime
                // ends with the supersession, and the row stays readable for
                // the bitemporal as-of chain.
                closed.expires_at.reset();
                // The predecessor left the live plane, so its vector stops
                // describing a retrievable record.
                const auto dropped = drop_embedding(db, *mutation.target);
                if (!dropped) {
                    return dropped.error();
                }
                const auto closed_write =
                    write_record(db, closed, scope_key, *existing_version.value() + 1);
                if (!closed_write) {
                    return closed_write.error();
                }
                proposed.status = MemoryStatus::Active;
                proposed.supersedes = *mutation.target;
                new_version = 1;
                break;
            }
            proposed.status = proposed.status == MemoryStatus::Superseded ? MemoryStatus::Active
                                                                          : proposed.status;
            // Update is in place: the target keeps its identity and version
            // history; only Supersede introduces a fresh record id.
            proposed.id = *mutation.target;
            // The vector index projects the statement. When the statement
            // changes, the stored vector no longer describes the record, so
            // it is dropped: the record counts as index lag until the
            // projection is rebuilt, rather than answering semantic queries
            // with the previous text.
            if (proposed.statement != existing.value()->statement) {
                const auto dropped = drop_embedding(db, *mutation.target);
                if (!dropped) {
                    return dropped.error();
                }
            }
            new_version = *existing_version.value() + 1;
            break;
        }
        case MemoryMutationType::AppendEvidence: {
            // DEC-115 evidence accumulation: merge new event ids into the
            // incumbent's provenance. CAS is checked but not consumed — the
            // fact identity, its statement and its version chain stay
            // untouched, so concurrent evidence merges cannot clobber each
            // other's content (the merge itself is idempotent by event id).
            auto incumbent = read_record(db, *mutation.target);
            if (!incumbent) {
                return incumbent.error();
            }
            if (!incumbent.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "append-evidence target does not exist");
            }
            if (scope_key_of(incumbent.value()->scope) != scope_key) {
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "append-evidence crosses scopes");
            }
            auto incumbent_version = current_version(db, *mutation.target);
            if (!incumbent_version) {
                return incumbent_version.error();
            }
            if (!incumbent_version.value().has_value() ||
                *incumbent_version.value() != *mutation.expected_version) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "memory version moved on; re-read and re-plan");
            }
            if (incumbent.value()->statement != proposed.statement) {
                return make_memory_error(MemoryDomainCode::InvalidMutation,
                                         "append-evidence must restate the incumbent claim");
            }
            MemoryRecord merged = std::move(incumbent).value().value();
            std::set<std::string> known;
            for (const auto &event : merged.provenance) {
                known.insert(event.to_string());
            }
            for (const auto &event : mutation.evidence) {
                if (known.insert(event.to_string()).second) {
                    merged.provenance.push_back(event);
                }
            }
            merged.alternative_of.reset();
            const auto written = write_record(db, merged, scope_key, *incumbent_version.value());
            if (!written) {
                return written.error();
            }
            {
                Statement log(db, "INSERT INTO memory_mutations(mutation_id, scope_key,"
                                  " applied_type, record_id, result_version, applied_at)"
                                  " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
                if (!log.valid()) {
                    return store_error(ErrorCode::Internal, "mutation log failed to prepare");
                }
                const std::string record_id = merged.id.to_string();
                sqlite3_bind_text(log.get(), 1, mutation_id.c_str(), -1, storage::transient_copy());
                sqlite3_bind_text(log.get(), 2, scope_key.c_str(), -1, storage::transient_copy());
                sqlite3_bind_text(log.get(), 3, memory_mutation_type_name(mutation.type).c_str(),
                                  -1, storage::transient_copy());
                sqlite3_bind_text(log.get(), 4, record_id.c_str(), -1, storage::transient_copy());
                sqlite3_bind_int64(log.get(), 5,
                                   static_cast<std::int64_t>(*incumbent_version.value()));
                sqlite3_bind_int64(log.get(), 6, wall_nanos(std::chrono::system_clock::now()));
                if (sqlite3_step(log.get()) != SQLITE_DONE) {
                    return store_error(ErrorCode::Internal, "mutation log failed");
                }
            }
            const auto append_commit = transaction.commit();
            if (!append_commit) {
                return append_commit.error();
            }
            MemoryMutationResult append_result;
            append_result.applied = MemoryMutationType::AppendEvidence;
            append_result.record = merged.id;
            append_result.new_version = *incumbent_version.value();
            return append_result;
        }
        case MemoryMutationType::PreserveAlternative: {
            // DEC-115 coexistence: the alternative joins as a fresh Active
            // record linked to its incumbent. The incumbent is neither read
            // for CAS nor modified — preserving a view must not fail because
            // the incumbent's version moved.
            auto incumbent = read_record(db, *proposed.alternative_of);
            if (!incumbent) {
                return incumbent.error();
            }
            if (!incumbent.value().has_value() ||
                incumbent.value()->status != MemoryStatus::Active) {
                return make_memory_error(MemoryDomainCode::InvalidMutation,
                                         "preserve-alternative incumbent must be active");
            }
            if (scope_key_of(incumbent.value()->scope) != scope_key) {
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "preserve-alternative crosses scopes");
            }
            const auto clash = current_version(db, proposed.id);
            if (!clash) {
                return clash.error();
            }
            if (clash.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "alternative record id already exists");
            }
            proposed.status = MemoryStatus::Active;
            new_version = 1;
            break;
        }
        case MemoryMutationType::Noop:
            break;
        }

        const auto written = write_record(db, proposed, scope_key, new_version);
        if (!written) {
            return written.error();
        }
        {
            Statement log(db, "INSERT INTO memory_mutations(mutation_id, scope_key, applied_type,"
                              " record_id, result_version, applied_at)"
                              " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            if (!log.valid()) {
                return store_error(ErrorCode::Internal, "mutation log failed to prepare");
            }
            const std::string record_id = proposed.id.to_string();
            sqlite3_bind_text(log.get(), 1, mutation_id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 2, scope_key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 3, memory_mutation_type_name(mutation.type).c_str(), -1,
                              storage::transient_copy());
            sqlite3_bind_text(log.get(), 4, record_id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 5, static_cast<std::int64_t>(new_version));
            sqlite3_bind_int64(log.get(), 6, wall_nanos(std::chrono::system_clock::now()));
            if (sqlite3_step(log.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "mutation log failed");
            }
        }
        const auto commit = transaction.commit();
        if (!commit) {
            return commit.error();
        }
        MemoryMutationResult result;
        result.applied = mutation.type;
        result.record = proposed.id;
        result.new_version = new_version;
        return result;
    }

    // ---- retrieval ----

    [[nodiscard]] static Result<WorldFactLookup> lookup_world_fact(sqlite3 *db,
                                                                   const WorldFactQuery &request) {
        if (auto valid = request.validate(); !valid)
            return valid.error();
        const auto scope_key = scope_key_of(request.scope);
        if (auto held = check_no_hold(db, scope_key); !held)
            return held.error();
        const auto now = std::chrono::system_clock::now();
        const auto valid_at = wall_nanos(request.as_of_valid.value_or(now));
        Statement latest(
            db, "SELECT id,valid_from,recorded_at,valid_until FROM memory_records"
                " WHERE scope_key=?1 AND scope_kind IN ('world-environment','world-application')"
                " AND json_extract(document, '$.world_property.entity')=?2"
                " AND json_extract(document, '$.world_property.property')=?3"
                " AND valid_from<=?4 AND recorded_at<=?5 AND expires_at>?6"
                " ORDER BY valid_from DESC, recorded_at DESC, id LIMIT 2");
        if (!latest.valid())
            return store_error(ErrorCode::Internal, "world fact lookup failed to prepare");
        const auto entity = request.property.entity.to_string();
        sqlite3_bind_text(latest.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(latest.get(), 2, entity.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(latest.get(), 3, request.property.property.c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_int64(latest.get(), 4, valid_at);
        sqlite3_bind_int64(latest.get(), 5, wall_nanos(request.as_of_recorded.value_or(now)));
        sqlite3_bind_int64(latest.get(), 6, wall_nanos(now));
        const auto first = sqlite3_step(latest.get());
        if (first == SQLITE_DONE)
            return WorldFactLookup{};
        if (first != SQLITE_ROW)
            return store_error(ErrorCode::Internal, "world fact lookup failed");
        const auto *stored = reinterpret_cast<const char *>(sqlite3_column_text(latest.get(), 0));
        const auto id = MemoryId::parse(stored != nullptr ? stored : "");
        const auto seen_at = sqlite3_column_int64(latest.get(), 1);
        const auto known_at = sqlite3_column_int64(latest.get(), 2);
        const auto until = sqlite3_column_int64(latest.get(), 3);
        if (!id || id->is_nil() || until <= seen_at)
            return store_error(ErrorCode::DataLoss, "world fact index row is invalid");
        const auto second = sqlite3_step(latest.get());
        if (second != SQLITE_ROW && second != SQLITE_DONE)
            return store_error(ErrorCode::Internal, "world fact lookup failed");
        if (second == SQLITE_ROW && sqlite3_column_int64(latest.get(), 1) == seen_at &&
            sqlite3_column_int64(latest.get(), 2) == known_at)
            return WorldFactLookup{WorldFactState::Ambiguous, std::nullopt};
        if (until <= valid_at)
            return WorldFactLookup{WorldFactState::Expired, std::nullopt};
        auto record = read_record(db, *id);
        if (!record)
            return record.error();
        if (!record.value() || record.value()->world_property != request.property ||
            record.value()->scope != request.scope ||
            wall_nanos(record.value()->validity.valid_from) != seen_at ||
            wall_nanos(record.value()->recorded_at) != known_at ||
            !record.value()->validity.valid_until ||
            wall_nanos(*record.value()->validity.valid_until) != until)
            return store_error(ErrorCode::DataLoss, "world fact index does not match its record");
        return WorldFactLookup{WorldFactState::Current, std::move(record).value()};
    }

    [[nodiscard]] Result<MemoryQueryResult> query(sqlite3 *db, const MemoryQuery &request) const {
        const auto valid = request.validate();
        if (!valid) {
            return valid.error();
        }
        MemoryQueryResult result;
        const auto deadline_point = std::chrono::steady_clock::now() + request.deadline;
        std::vector<std::string> scope_keys;
        scope_keys.reserve(request.scopes.size());
        for (const auto &scope : request.scopes) {
            scope_keys.push_back(scope_key_of(scope));
            const auto hold = check_scope_holds(db, scope, scope_keys.back());
            if (!hold) {
                return hold.error();
            }
        }
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        const bool bitemporal =
            request.as_of_recorded.has_value() || request.as_of_valid.has_value();
        const bool relationship = request.social_relationship_predicate.has_value();
        const bool social = relationship || request.social_attribute_field.has_value();
        const bool recovery = request.recovery_lesson_key.has_value();
        const bool kind_filter = request.kinds.has_value() && !social;

        // Base universe: exact scope equality, nothing crosses by similarity.
        std::string sql =
            "SELECT id, scope_key, kind, statement, recorded_at, confidence, verification,"
            " sensitivity_rank, status, evidence, valid_from, document FROM memory_records WHERE (";
        for (std::size_t index = 0; index < scope_keys.size(); ++index) {
            sql += index == 0 ? "scope_key = ?" : " OR scope_key = ?";
        }
        sql += ")";
        if (!bitemporal) {
            sql += " AND status = 'active' AND (expires_at IS NULL OR expires_at > ?)";
        }
        if (request.record_id) {
            sql += " AND id = ?";
        }
        if (kind_filter) {
            sql += " AND kind IN (";
            for (std::size_t index = 0; index < request.kinds->size(); ++index) {
                sql += index == 0 ? "?" : ", ?";
            }
            sql += ")";
        }
        // Any-of label prefilter (DEC-112): a hard filter like the scope
        // allowlist, evaluated inside the authorized candidate universe.
        if (request.labels.has_value()) {
            sql += " AND EXISTS (SELECT 1 FROM json_each(memory_records.labels)"
                   " WHERE value IN (";
            for (std::size_t index = 0; index < request.labels->size(); ++index) {
                sql += index == 0 ? "?" : ", ?";
            }
            sql += "))";
        }
        if (social) {
            sql += relationship
                       ? " AND kind = 'social-relationship'"
                         " AND json_extract(document, '$.social_relationship.predicate') = ?"
                       : " AND kind = 'social-attribute'"
                         " AND json_extract(document, '$.social_attribute.field') = ?";
        }
        if (recovery) {
            sql += " AND kind='recovery-lesson' AND source_namespace IS NULL"
                   " AND verification IN ('verified','human-confirmed') AND ";
            sql += kRecoveryLessonKeySql;
            sql += " = ?";
        }
        if (social || recovery)
            sql += " AND recorded_at <= ? AND valid_from <= ?"
                   " AND (valid_until IS NULL OR valid_until > ?)";
        sql += social || recovery ? " ORDER BY recorded_at DESC, id LIMIT 4096"
                                  : " ORDER BY recorded_at DESC LIMIT 4096";
        Statement universe(db, sql.c_str());
        if (!universe.valid()) {
            return store_error(ErrorCode::Internal, "memory query failed to prepare");
        }
        int next_param = 1;
        for (const auto &key : scope_keys) {
            sqlite3_bind_text(universe.get(), next_param, key.c_str(), -1,
                              storage::transient_copy());
            ++next_param;
        }
        if (!bitemporal) {
            sqlite3_bind_int64(universe.get(), next_param, now);
            ++next_param;
        }
        if (request.record_id) {
            const auto id = request.record_id->to_string();
            sqlite3_bind_text(universe.get(), next_param++, id.c_str(), -1,
                              storage::transient_copy());
        }
        if (kind_filter) {
            for (const auto kind : *request.kinds) {
                const std::string name = memory_kind_name(kind);
                sqlite3_bind_text(universe.get(), next_param, name.c_str(), -1,
                                  storage::transient_copy());
                ++next_param;
            }
        }
        if (request.labels.has_value()) {
            for (const auto &label : *request.labels) {
                sqlite3_bind_text(universe.get(), next_param, label.c_str(), -1,
                                  storage::transient_copy());
                ++next_param;
            }
        }
        if (social) {
            const auto &predicate = relationship ? *request.social_relationship_predicate
                                                 : *request.social_attribute_field;
            sqlite3_bind_text(universe.get(), next_param++, predicate.c_str(), -1,
                              storage::transient_copy());
        }
        if (recovery) {
            const auto key = recovery_lesson_key_value(*request.recovery_lesson_key);
            sqlite3_bind_text(universe.get(), next_param++, key.c_str(), -1,
                              storage::transient_copy());
        }
        if (social || recovery) {
            const auto recorded_at =
                request.as_of_recorded ? wall_nanos(*request.as_of_recorded) : now;
            const auto valid_at = request.as_of_valid ? wall_nanos(*request.as_of_valid) : now;
            sqlite3_bind_int64(universe.get(), next_param++, recorded_at);
            sqlite3_bind_int64(universe.get(), next_param++, valid_at);
            sqlite3_bind_int64(universe.get(), next_param, valid_at);
        }
        std::vector<Row> candidates;
        int step = sqlite3_step(universe.get());
        while (step == SQLITE_ROW) {
            auto row = decode_row(universe.get());
            if (!row) {
                return row.error();
            }
            candidates.push_back(std::move(row).value());
            step = sqlite3_step(universe.get());
        }
        if (step != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "memory query failed");
        }
        if (request.max_sensitivity.has_value()) {
            const int cap = sensitivity_rank(*request.max_sensitivity);
            std::erase_if(candidates,
                          [cap](const Row &row) { return row.sensitivity_rank_value > cap; });
        }
        if (bitemporal) {
            // Resolve each record to the version mandated by the bitemporal
            // query: latest version known by as_of_recorded whose validity
            // covers as_of_valid (open bounds mean "now").
            const std::int64_t recorded_cap =
                wall_nanos(request.as_of_recorded.value_or(std::chrono::system_clock::now()));
            std::optional<std::int64_t> valid_at;
            if (request.as_of_valid.has_value() || social) {
                valid_at = request.as_of_valid ? wall_nanos(*request.as_of_valid) : now;
            }
            std::vector<Row> resolved;
            resolved.reserve(candidates.size());
            // Era chaining: a Superseded version closes the previous era at
            // its valid_until, so older Active versions only answer as-of
            // queries inside that capped interval (a superseded record must
            // not answer for eras owned by its successor).
            for (auto &row : candidates) {
                std::optional<std::int64_t> effective_until;
                Statement versions(
                    db, "SELECT version, document FROM memory_versions WHERE record_id = ?1"
                        " AND recorded_at <= ?2 ORDER BY version DESC");
                if (!versions.valid()) {
                    return store_error(ErrorCode::Internal, "version scan failed to prepare");
                }
                sqlite3_bind_text(versions.get(), 1, row.id.c_str(), -1, storage::transient_copy());
                sqlite3_bind_int64(versions.get(), 2, recorded_cap);
                bool matched = false;
                int vstep = sqlite3_step(versions.get());
                while (vstep == SQLITE_ROW) {
                    const auto *document =
                        reinterpret_cast<const char *>(sqlite3_column_text(versions.get(), 1));
                    if (document == nullptr) {
                        vstep = sqlite3_step(versions.get());
                        continue;
                    }
                    auto parsed = parse_json(document);
                    if (!parsed) {
                        vstep = sqlite3_step(versions.get());
                        continue;
                    }
                    auto record = memory_record_from_json(parsed.value());
                    if (!record) {
                        vstep = sqlite3_step(versions.get());
                        continue;
                    }
                    MemoryRecord version_record = std::move(record).value();
                    if (version_record.status == MemoryStatus::Superseded) {
                        if (version_record.validity.valid_until.has_value()) {
                            const std::int64_t closed =
                                wall_nanos(*version_record.validity.valid_until);
                            effective_until =
                                !effective_until.has_value() || closed < *effective_until
                                    ? std::optional<std::int64_t>(closed)
                                    : effective_until;
                        }
                        vstep = sqlite3_step(versions.get());
                        continue;
                    }
                    if (version_record.status == MemoryStatus::Tombstoned) {
                        // Tombstoned knowledge ends the record's retrieval
                        // life; older eras answer only through older K.
                        matched = true; // treated as resolved: excluded
                        break;
                    }
                    const std::int64_t until_limit =
                        effective_until.has_value()
                            ? *effective_until
                            : (version_record.validity.valid_until.has_value()
                                   ? wall_nanos(*version_record.validity.valid_until)
                                   : std::numeric_limits<std::int64_t>::max());
                    const bool validity_covers =
                        !valid_at.has_value() ||
                        (wall_nanos(version_record.validity.valid_from) <= *valid_at &&
                         until_limit > *valid_at);
                    if (validity_covers) {
                        Row resolved_row;
                        resolved_row.id = version_record.id.to_string();
                        resolved_row.scope_key = scope_key_of(version_record.scope);
                        resolved_row.statement = version_record.statement;
                        resolved_row.recorded_at = wall_nanos(version_record.recorded_at);
                        resolved_row.confidence = version_record.confidence;
                        resolved_row.verification =
                            memory_verification_name(version_record.verification);
                        resolved_row.kind = memory_kind_name(version_record.kind);
                        resolved_row.sensitivity_rank_value =
                            sensitivity_rank(version_record.sensitivity);
                        resolved_row.decoded = std::move(version_record);
                        if (resolved_row.decoded.status == MemoryStatus::Active &&
                            (!resolved_row.decoded.expires_at.has_value() ||
                             wall_nanos(*resolved_row.decoded.expires_at) > now)) {
                            resolved.push_back(std::move(resolved_row));
                            matched = true;
                        }
                        break;
                    }
                    vstep = sqlite3_step(versions.get());
                }
                if (vstep != SQLITE_DONE && vstep != SQLITE_ROW && !matched) {
                    return store_error(ErrorCode::Internal, "version scan failed");
                }
            }
            candidates = std::move(resolved);
        }

        // Claim-key filter: only records about the requested subject take
        // part. The filter is hard and runs before the ranking legs, so the
        // conflict retrieval of design Context/Memory §13.5 sees every
        // record about one subject instead of the whole scope — and the
        // diversity cap below cannot hide a sibling behind an unrelated
        // record of the same kind.
        if (request.claim_key.has_value()) {
            const std::string wanted = memory_normalize_claim_key(*request.claim_key);
            std::erase_if(candidates, [&wanted](const Row &row) {
                return memory_claim_key(row.statement).key != wanted;
            });
        }

        // Leg limits belong to this authorized, filtered universe. Applying
        // a global LIMIT before intersecting IDs lets unrelated scopes (or
        // excluded claims/sensitivities) consume another query's recall budget.
        // One bound JSON array avoids a placeholder per candidate (up to 4096)
        // and is expanded by the bundled SQLite's built-in json_each table.
        std::string eligible_json;
        if (!request.text.empty() || !request.query_embedding.values.empty()) {
            JsonValue::Array eligible_ids;
            eligible_ids.reserve(candidates.size());
            for (const auto &row : candidates) {
                eligible_ids.emplace_back(row.id);
            }
            eligible_json = to_json_string(JsonValue{std::move(eligible_ids)});
        }

        // Exact leg: verbatim substrings.
        std::set<std::string> exact_hits;
        if (!request.exact_terms.empty()) {
            result.quality.exact_leg_ran = true;
            for (const auto &row : candidates) {
                bool all = true;
                for (const auto &term : request.exact_terms) {
                    if (row.statement.find(term) == std::string::npos) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    exact_hits.insert(row.id);
                }
            }
        }

        // FTS leg: phrase match restricted to the scope universe.
        std::map<std::string, double> fts_rank;
        if (!request.text.empty() && std::chrono::steady_clock::now() < deadline_point) {
            Statement fts(db, "SELECT record_id, rank FROM memory_fts WHERE memory_fts MATCH ?1"
                              " AND record_id IN (SELECT value FROM json_each(?2))"
                              " ORDER BY rank LIMIT 512");
            if (fts.valid()) {
                result.quality.fts_leg_ran = true;
                const std::string expression = storage::fts_match_expression(request.text);
                sqlite3_bind_text(fts.get(), 1, expression.c_str(), -1, storage::transient_copy());
                sqlite3_bind_text(fts.get(), 2, eligible_json.c_str(), -1,
                                  storage::transient_copy());
                int fstep = sqlite3_step(fts.get());
                std::size_t position = 0;
                while (fstep == SQLITE_ROW) {
                    const auto *id_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(fts.get(), 0));
                    if (id_text != nullptr) {
                        // FTS5 bm25 ranks are negative-better; normalize the
                        // magnitude into a (0,1] contribution.
                        const double rank = sqlite3_column_double(fts.get(), 1);
                        fts_rank[id_text] = 1.0 / (1.0 + std::max(0.0, -rank) +
                                                   static_cast<double>(position) * 0.01);
                    }
                    ++position;
                    fstep = sqlite3_step(fts.get());
                }
                if (fstep != SQLITE_DONE) {
                    // Malformed text or index issue: degrade, keep going.
                    result.quality.degraded = true;
                    result.quality.note = "fts leg degraded; falling back to exact ranking";
                    fts_rank.clear();
                    result.quality.fts_leg_ran = false;
                }
            }
        }

        // Vector leg: bounded cosine over indexed embeddings.
        //
        // The per-kind retrieval policy decides whether embedding similarity
        // is a legitimate way to reach the queried kinds: a kind whose claim
        // is a canonical structured record (an episode, a procedure, a
        // lesson, a skill hint) is recalled by identifier and keyword, not
        // by similarity (design Context/Memory §6.2, §14.1).
        const bool semantic_eligible =
            !request.kinds.has_value() ||
            std::all_of(request.kinds->begin(), request.kinds->end(), [](MemoryKind kind) {
                const auto &policy = memory_kind_policy(kind);
                return std::find(policy.retrieval_legs.begin(), policy.retrieval_legs.end(),
                                 MemoryRetrievalLeg::Semantic) != policy.retrieval_legs.end();
            });
        const bool semantic_withheld =
            !request.query_embedding.values.empty() && !semantic_eligible;
        if (semantic_withheld) {
            result.quality.semantic_leg_withheld = true;
            result.quality.note = "semantic retrieval is not eligible for the queried kinds";
        }
        std::map<std::string, double> vector_scores;
        if (!request.query_embedding.values.empty() && semantic_eligible &&
            std::chrono::steady_clock::now() < deadline_point) {
            const std::size_t query_dim = request.query_embedding.values.size();
            const std::string query_producer = request.query_embedding.embedder.to_string();
            Statement embeddings(db, "SELECT record_id, embedder, vector FROM memory_embeddings"
                                     " WHERE record_id IN (SELECT value FROM json_each(?2))"
                                     " ORDER BY embedded_at DESC LIMIT ?1");
            std::size_t vector_errors = 0;
            if (embeddings.valid()) {
                std::vector<float> decoded(query_dim);
                sqlite3_bind_int64(embeddings.get(), 1,
                                   static_cast<std::int64_t>(options_.max_vector_scan));
                sqlite3_bind_text(embeddings.get(), 2, eligible_json.c_str(), -1,
                                  storage::transient_copy());
                int estep = sqlite3_step(embeddings.get());
                while (estep == SQLITE_ROW) {
                    const auto *id_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(embeddings.get(), 0));
                    const auto *producer_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(embeddings.get(), 1));
                    const auto *blob = sqlite3_column_blob(embeddings.get(), 2);
                    const int bytes = sqlite3_column_bytes(embeddings.get(), 2);
                    if (id_text == nullptr || blob == nullptr || bytes <= 0 || bytes % 4 != 0) {
                        ++vector_errors;
                        estep = sqlite3_step(embeddings.get());
                        continue;
                    }
                    // Vectors of a different embedder describe a different
                    // geometry (DEC-111): they must not meet this query in
                    // the cosine leg. The mismatch is a diagnosable config
                    // error, counted like a dimension mismatch rather than
                    // silently treated as "no rows".
                    if (producer_text == nullptr ||
                        std::string_view(producer_text) != query_producer) {
                        ++vector_errors;
                        estep = sqlite3_step(embeddings.get());
                        continue;
                    }
                    const auto dim = static_cast<std::size_t>(bytes / 4);
                    if (dim != query_dim) {
                        ++vector_errors;
                        estep = sqlite3_step(embeddings.get());
                        continue;
                    }
                    // SQLite blobs need not have float alignment. Decode to
                    // aligned storage before the shared numeric boundary.
                    std::memcpy(decoded.data(), blob, static_cast<std::size_t>(bytes));
                    const auto similarity =
                        embedding_cosine_similarity(decoded, request.query_embedding.values);
                    if (!similarity) {
                        ++vector_errors;
                    } else if (*similarity >= 0.0) {
                        vector_scores[id_text] = *similarity;
                    }
                    estep = sqlite3_step(embeddings.get());
                }
                if (estep != SQLITE_DONE) {
                    ++vector_errors;
                }
            }
            if (!vector_scores.empty() || vector_errors == 0) {
                result.quality.vector_leg_ran = true;
            } else {
                result.quality.vector_degraded = true;
                result.quality.degraded = true;
                result.quality.note = "vector index unusable; exact/fts legs answered the query";
            }
        }

        if (std::chrono::steady_clock::now() >= deadline_point) {
            result.quality.deadline_exceeded = true;
            result.quality.degraded = true;
            result.quality.note = "retrieval deadline exceeded; partial results returned";
        }

        // Merge, score, dedup and diversify.
        const auto &weights = weights_;
        struct Scored final {
            const Row *row = nullptr;
            double score = 0.0;
        };
        std::vector<Scored> scored;
        scored.reserve(candidates.size());
        std::vector<Scored> rrf_raw;
        std::vector<double> rrf_priors;
        rrf_raw.reserve(candidates.size());
        rrf_priors.reserve(candidates.size());
        const std::int64_t newest = candidates.empty()
                                        ? now
                                        : std::max_element(candidates.begin(), candidates.end(),
                                                           [](const Row &a, const Row &b) {
                                                               return a.recorded_at < b.recorded_at;
                                                           })
                                              ->recorded_at;
        const std::int64_t oldest = candidates.empty()
                                        ? now
                                        : std::min_element(candidates.begin(), candidates.end(),
                                                           [](const Row &a, const Row &b) {
                                                               return a.recorded_at < b.recorded_at;
                                                           })
                                              ->recorded_at;
        const double span = newest > oldest ? static_cast<double>(newest - oldest) : 1.0;
        const bool any_leg = !exact_hits.empty() || !fts_rank.empty() || !vector_scores.empty();
        // A query that asked for ranking (text, terms or embedding) and got
        // no reliable hits must not dump the whole scope as a "fallback".
        // An embedding the per-kind policy withheld is not a ranking request:
        // the kind-filtered set is the answer, ordered by the remaining
        // signals (design Context/Memory §6.2).
        const bool ranking_attempted =
            !request.text.empty() || !request.exact_terms.empty() ||
            (!request.query_embedding.values.empty() && !semantic_withheld);
        std::set<std::string> seen_statements;
        // Reciprocal-rank fusion (DEC-112): ranks over the FTS and cosine
        // legs, score desc with a deterministic id tie-break. exact stays a
        // mandatory filter and contributes one shared constant vote instead
        // of a ranking over its members.
        const bool rrf_fusion = options_.fusion == RetrievalFusion::ReciprocalRank;
        constexpr std::size_t kRrfConstant = 60;
        const auto rank_order = [](const std::map<std::string, double> &scores) {
            std::vector<std::pair<std::string, double>> entries(scores.begin(), scores.end());
            std::sort(entries.begin(), entries.end(), [](const auto &lhs, const auto &rhs) {
                if (lhs.second != rhs.second) {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            });
            std::map<std::string, std::size_t> positions;
            for (std::size_t position = 0; position < entries.size(); ++position) {
                positions[entries[position].first] = position;
            }
            return positions;
        };
        const auto fts_positions =
            rrf_fusion ? rank_order(fts_rank) : std::map<std::string, std::size_t>{};
        const auto vector_positions =
            rrf_fusion ? rank_order(vector_scores) : std::map<std::string, std::size_t>{};
        if (ranking_attempted && !any_leg && !result.quality.deadline_exceeded) {
            result.quality.degraded = result.quality.degraded || result.quality.vector_degraded;
            return result; // nothing matched; an empty answer beats a scope dump
        }
        for (const auto &row : candidates) {
            const bool exact_hit = exact_hits.count(row.id) != 0;
            const auto fts_entry = fts_rank.find(row.id);
            const auto vector_entry = vector_scores.find(row.id);
            const bool touched =
                exact_hit || fts_entry != fts_rank.end() || vector_entry != vector_scores.end();
            if (any_leg && !touched) {
                continue; // ranking legs ran; only matches participate
            }
            if (!exact_hits.empty() && !exact_hit) {
                continue; // explicit exact terms are mandatory filters
            }
            if (!social && !seen_statements.insert(row.statement).second) {
                continue; // near-duplicate suppression at statement equality
            }
            // Priors are shared by both fusion modes and are computed on the
            // same scale; the fused leg signal differs per mode.
            double prior =
                weights.verification *
                (static_cast<double>(memory_verification_rank(row.decoded.verification)) / 3.0);
            prior += weights.confidence * row.confidence;
            prior += weights.importance * static_cast<double>(row.decoded.importance);
            if (newest >= oldest) {
                const double recency =
                    span > 0.0 ? static_cast<double>(row.recorded_at - oldest) / span : 1.0;
                prior += weights.recency * recency;
            }
            double score = 0.0;
            if (rrf_fusion) {
                // Raw reciprocal-rank vote; normalized against the candidate
                // set after the loop so the leg spread cannot be swamped by
                // the (much smaller) prior spread.
                double fused = 0.0;
                if (exact_hit) {
                    // Shared constant vote: a filter role, not a ranking.
                    fused += weights.exact / static_cast<double>(kRrfConstant + 1);
                }
                if (const auto it = fts_positions.find(row.id); it != fts_positions.end()) {
                    fused += weights.fts / static_cast<double>(kRrfConstant + it->second + 1);
                }
                if (const auto it = vector_positions.find(row.id); it != vector_positions.end()) {
                    fused += weights.vector / static_cast<double>(kRrfConstant + it->second + 1);
                }
                rrf_raw.push_back({&row, fused});
                rrf_priors.push_back(prior);
                continue;
            }
            if (exact_hit) {
                score += weights.exact;
            }
            if (fts_entry != fts_rank.end()) {
                score += weights.fts * fts_entry->second;
            }
            if (vector_entry != vector_scores.end()) {
                score += weights.vector * vector_entry->second;
            }
            score += prior;
            scored.push_back({&row, score});
        }
        if (rrf_fusion) {
            // Min-max the fused votes inside the touched candidate set, then
            // add the priors. The leg spread therefore always dominates the
            // (much smaller) prior spread, and a signal-free set degrades to
            // the prior order instead of an arbitrary one.
            double fused_min = 0.0;
            double fused_max = 0.0;
            if (!rrf_raw.empty()) {
                fused_min = fused_max = rrf_raw.front().score;
                for (const auto &entry : rrf_raw) {
                    fused_min = std::min(fused_min, entry.score);
                    fused_max = std::max(fused_max, entry.score);
                }
            }
            const double leg_total = weights.exact + weights.fts + weights.vector;
            const double span_normalized = fused_max - fused_min;
            for (std::size_t index = 0; index < rrf_raw.size(); ++index) {
                const double normalized = span_normalized > 0.0
                                              ? (rrf_raw[index].score - fused_min) / span_normalized
                                              : 1.0;
                scored.push_back({rrf_raw[index].row, normalized * leg_total + rrf_priors[index]});
            }
        }
        std::sort(scored.begin(), scored.end(), [social](const Scored &lhs, const Scored &rhs) {
            if (social && lhs.score == rhs.score)
                return lhs.row->id < rhs.row->id;
            return lhs.score > rhs.score;
        });
        // Near-duplicate suppression (DEC-112): a bounded head window;
        // candidates carrying an embedding of the query's producer identity
        // are compared against the already-kept set. Records without such a
        // vector are exempt - the pass never pretends to compare.
        if (options_.near_dup_threshold > 0.0 && !request.query_embedding.values.empty() &&
            scored.size() > 1) {
            const std::size_t window = std::min<std::size_t>(scored.size(), 64);
            JsonValue::Array window_ids;
            for (std::size_t index = 0; index < window; ++index) {
                window_ids.emplace_back(JsonValue(scored[index].row->id));
            }
            Statement dup_index(
                db, "SELECT record_id, vector FROM memory_embeddings WHERE embedder = ?1"
                    " AND record_id IN (SELECT value FROM json_each(?2))");
            if (dup_index.valid()) {
                const std::string producer = request.query_embedding.embedder.to_string();
                sqlite3_bind_text(dup_index.get(), 1, producer.c_str(), -1,
                                  storage::transient_copy());
                const std::string ids_json = to_json_string(JsonValue(std::move(window_ids)));
                sqlite3_bind_text(dup_index.get(), 2, ids_json.c_str(), -1,
                                  storage::transient_copy());
                std::map<std::string, std::vector<float>> window_vectors;
                int dup_step = sqlite3_step(dup_index.get());
                while (dup_step == SQLITE_ROW) {
                    const auto *id_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(dup_index.get(), 0));
                    const auto *blob = sqlite3_column_blob(dup_index.get(), 1);
                    const int bytes = sqlite3_column_bytes(dup_index.get(), 1);
                    if (id_text != nullptr && blob != nullptr && bytes > 0 && bytes % 4 == 0) {
                        std::vector<float> decoded(static_cast<std::size_t>(bytes) / 4);
                        std::memcpy(decoded.data(), blob, static_cast<std::size_t>(bytes));
                        window_vectors[id_text] = std::move(decoded);
                    }
                    dup_step = sqlite3_step(dup_index.get());
                }
                std::vector<Scored> kept;
                std::vector<std::vector<float>> kept_vectors;
                kept.reserve(scored.size());
                for (std::size_t index = 0; index < scored.size(); ++index) {
                    const auto &entry = scored[index];
                    const auto vector_it =
                        index < window ? window_vectors.find(entry.row->id) : window_vectors.end();
                    bool duplicate = false;
                    if (vector_it != window_vectors.end()) {
                        for (const auto &kept_vector : kept_vectors) {
                            const auto similarity =
                                embedding_cosine_similarity(vector_it->second, kept_vector);
                            if (similarity && *similarity >= options_.near_dup_threshold) {
                                duplicate = true;
                                break;
                            }
                        }
                    }
                    if (duplicate) {
                        ++result.quality.dedup_suppressed;
                        continue;
                    }
                    kept.push_back(entry);
                    if (vector_it != window_vectors.end()) {
                        kept_vectors.push_back(vector_it->second);
                    }
                }
                scored = std::move(kept);
            }
        }
        std::map<std::string, std::size_t> group_counts;
        for (const auto &entry : scored) {
            if (result.records.size() >= request.max_results) {
                break;
            }
            const std::string group = entry.row->scope_key + "|" + entry.row->kind;
            // The diversity cap keeps one (scope, kind) group from flooding
            // the result set. A claim-key filtered query asked for one
            // subject, so the cap would only hide the siblings the caller
            // came for.
            if (!request.claim_key.has_value() && !social &&
                group_counts[group] >= options_.max_per_scope_kind) {
                continue;
            }
            const std::uint64_t tokens =
                std::max<std::uint64_t>(16, entry.row->statement.size() / 4 + 8);
            if (result.tokens_estimate + tokens > request.token_budget) {
                continue;
            }
            group_counts[group] += 1;
            result.tokens_estimate += tokens;
            result.records.push_back(entry.row->decoded);
            result.scores.push_back(entry.score);
        }
        // Vector index lag: Active records without an embedding of the
        // query's producer identity (DEC-111).
        if (!request.query_embedding.embedder.is_nil()) {
            Statement lag(db, "SELECT COUNT(*) FROM memory_records r WHERE r.status = 'active'"
                              " AND NOT EXISTS(SELECT 1 FROM memory_embeddings e"
                              " WHERE e.record_id = r.id AND e.embedder = ?1)");
            if (lag.valid()) {
                const std::string producer = request.query_embedding.embedder.to_string();
                sqlite3_bind_text(lag.get(), 1, producer.c_str(), -1, storage::transient_copy());
                if (sqlite3_step(lag.get()) == SQLITE_ROW) {
                    result.quality.index_lag =
                        static_cast<std::size_t>(sqlite3_column_int64(lag.get(), 0));
                }
            }
        }
        return result;
    }

    // ---- erasure ----

    struct ErasureTargets final {
        std::vector<std::string> ids;
        std::optional<std::string> scope_key;
    };

    [[nodiscard]] static Result<ErasureTargets> resolve_targets(sqlite3 *db,
                                                                const ErasureRequest &request) {
        ErasureTargets targets;
        if (request.scope.has_value()) {
            const auto &scope = *request.scope;
            targets.scope_key = scope_key_of(scope);
            const bool social_object = scope.kind == MemoryScopeKind::SocialObject;
            const bool social_environment = scope.kind == MemoryScopeKind::SocialEnvironment;
            if (social_object && !is_social_object_scope(scope))
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "invalid social erasure scope");
            if (social_environment && !social_environment_scope_of(scope))
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "invalid social environment erasure scope");
            // The first two components of a relationship subject are the
            // source object key; its first and third are the target key.
            Statement query(
                db,
                social_object
                    ? "SELECT id FROM memory_records WHERE scope_key = ?1"
                      " UNION ALL SELECT id FROM memory_records WHERE kind = 'social-relationship'"
                      " AND tenant_id = ?2 AND substr(subject_id, 1, 65) = ?3"
                      " UNION ALL SELECT id FROM memory_records WHERE kind = 'social-relationship'"
                      " AND tenant_id = ?2 AND substr(subject_id, 1, 33) || substr(subject_id, 67) "
                      "= ?3"
                : social_environment
                    ? "SELECT id FROM memory_records WHERE kind IN ('social-attribute', "
                      "'social-relationship')"
                      " AND tenant_id = ?1 AND subject_id >= ?2 AND subject_id < ?3"
                    : "SELECT id FROM memory_records WHERE scope_key = ?1");
            if (!query.valid()) {
                return store_error(ErrorCode::Internal, "erasure scan failed to prepare");
            }
            if (social_environment) {
                // '/' starts the next component, and '0' is its successor in
                // the binary collation. Together they bound this prefix.
                const auto lower = scope.subject_id + '/';
                const auto upper = scope.subject_id + '0';
                sqlite3_bind_text(query.get(), 1, scope.tenant_id->c_str(), -1,
                                  storage::transient_copy());
                sqlite3_bind_text(query.get(), 2, lower.c_str(), -1, storage::transient_copy());
                sqlite3_bind_text(query.get(), 3, upper.c_str(), -1, storage::transient_copy());
            } else {
                sqlite3_bind_text(query.get(), 1, targets.scope_key->c_str(), -1,
                                  storage::transient_copy());
            }
            if (social_object) {
                sqlite3_bind_text(query.get(), 2, scope.tenant_id->c_str(), -1,
                                  storage::transient_copy());
                sqlite3_bind_text(query.get(), 3, scope.subject_id.c_str(), -1,
                                  storage::transient_copy());
            }
            int step = sqlite3_step(query.get());
            while (step == SQLITE_ROW) {
                const auto *id_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
                if (id_text != nullptr) {
                    targets.ids.emplace_back(id_text);
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "erasure scan failed");
            }
        } else {
            targets.ids.push_back(request.record->to_string());
            Statement scope_query(db, "SELECT scope_key FROM memory_records WHERE id = ?1");
            if (!scope_query.valid()) {
                return store_error(ErrorCode::Internal, "erasure scope read failed to prepare");
            }
            sqlite3_bind_text(scope_query.get(), 1, targets.ids.front().c_str(), -1,
                              storage::transient_copy());
            if (sqlite3_step(scope_query.get()) == SQLITE_ROW) {
                const auto *key_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(scope_query.get(), 0));
                if (key_text != nullptr) {
                    targets.scope_key = key_text;
                }
            }
        }
        return targets;
    }

    [[nodiscard]] static Result<std::optional<ArtifactId>>
    evidence_artifact(sqlite3 *db, const std::string &id) {
        Statement query(db, "SELECT evidence FROM memory_records WHERE id = ?1");
        if (!query.valid()) {
            return store_error(ErrorCode::Internal, "evidence read failed to prepare");
        }
        sqlite3_bind_text(query.get(), 1, id.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(query.get()) != SQLITE_ROW) {
            return std::optional<ArtifactId>{};
        }
        if (sqlite3_column_type(query.get(), 0) == SQLITE_NULL) {
            return std::optional<ArtifactId>{};
        }
        const auto *document = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
        if (document == nullptr) {
            return std::optional<ArtifactId>{};
        }
        auto parsed = parse_json(document);
        if (!parsed) {
            return std::optional<ArtifactId>{};
        }
        const auto *artifact = parsed.value().find("id");
        if (artifact == nullptr || !artifact->is_string()) {
            return std::optional<ArtifactId>{};
        }
        return ArtifactId::parse(*artifact->as_string());
    }

    // Runs inside an open transaction (caller owns BEGIN/COMMIT). Erasure
    // itself bypasses the pending-erasure hold: the retry is the only way a
    // held scope can be released, so blocking it would wedge the scope.
    [[nodiscard]] Result<ErasureResult> erase_locked(sqlite3 *db, const ErasureRequest &request) {
        const auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        const auto valid = request.validate();
        if (!valid) {
            return valid.error();
        }
        const auto targets = resolve_targets(db, request);
        if (!targets) {
            return targets.error();
        }
        const std::string scope_key = targets.value().scope_key.value_or(std::string{});
        ErasureResult result;
        result.status = ErasureStatus::Complete;
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        bool failed = false;
        std::string failure_note;
        std::optional<MemoryScope> record_scope;
        for (const auto &id : targets.value().ids) {
            auto evidence = evidence_artifact(db, id);
            if (!evidence) {
                failed = true;
                failure_note = "evidence lookup failed";
                break;
            }
            // Resolve the record's scope before the deletions: a partial
            // failure has to name the scope it holds out of Context, and
            // the row is gone by the time the failure is detected.
            if (!request.scope.has_value() && !record_scope.has_value()) {
                const auto parsed = MemoryId::parse(id);
                if (parsed) {
                    auto existing = read_record(db, *parsed);
                    if (existing && existing.value().has_value()) {
                        record_scope = existing.value()->scope;
                    }
                }
            }
            struct Step final {
                const char *sql = nullptr;
                const char *label = nullptr;
            };
            const Step steps[] = {
                {"DELETE FROM memory_fts WHERE record_id = ?1", "fts"},
                {"DELETE FROM memory_embeddings WHERE record_id = ?1", "embeddings"},
                {"DELETE FROM memory_versions WHERE record_id = ?1", "versions"},
                {"DELETE FROM memory_provenance WHERE record_id = ?1", "provenance"},
                {"DELETE FROM memory_records WHERE id = ?1", "records"},
            };
            for (const auto &step : steps) {
                Statement statement(db, step.sql);
                if (!statement.valid()) {
                    failed = true;
                    failure_note = std::string("erasure step failed to prepare: ") + step.label;
                    break;
                }
                sqlite3_bind_text(statement.get(), 1, id.c_str(), -1, storage::transient_copy());
                const int outcome = sqlite3_step(statement.get());
                const int removed = sqlite3_changes(db);
                if (outcome != SQLITE_DONE ||
                    (request.scope && std::string_view(step.label) == "records" && removed != 1)) {
                    failed = true;
                    failure_note = std::string("erasure step failed: ") + step.label;
                    break;
                }
                if (removed > 0) {
                    if (std::string_view(step.label) == "fts") {
                        result.counts.fts_entries_removed += 1;
                    } else if (std::string_view(step.label) == "embeddings") {
                        result.counts.embeddings_removed += 1;
                    } else if (std::string_view(step.label) == "versions") {
                        result.counts.versions_removed += 1;
                    } else if (std::string_view(step.label) == "records") {
                        result.counts.records_removed += 1;
                    }
                }
            }
            if (failed) {
                break;
            }
            if (request.include_artifacts && artifacts_ != nullptr &&
                evidence.value().has_value()) {
                ArtifactErasureRequest artifact_request;
                artifact_request.id = *evidence.value();
                artifact_request.reason = request.reason;
                const auto artifact_result = artifacts_->erase(artifact_request);
                if (!artifact_result) {
                    failed = true;
                    failure_note = "artifact erasure failed";
                    break;
                }
                result.counts.artifacts_erased += 1;
            }
        }
        // Audit row: ids and counts only, never the erased statements.
        Statement log(db, "INSERT INTO memory_erasure_log(scope_key, record_id, reason,"
                          " requested_at, completed_at, status, removed_records, removed_versions,"
                          " removed_fts, removed_embeddings, artifacts_erased, note)"
                          " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
        if (log.valid()) {
            sqlite3_bind_text(log.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
            if (request.record.has_value()) {
                const std::string record_id = request.record->to_string();
                sqlite3_bind_text(log.get(), 2, record_id.c_str(), -1, storage::transient_copy());
            } else {
                sqlite3_bind_null(log.get(), 2);
            }
            sqlite3_bind_text(log.get(), 3, request.reason.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 4, now);
            sqlite3_bind_int64(log.get(), 5, failed ? 0 : now);
            sqlite3_bind_text(log.get(), 6, failed ? "pending" : "complete", -1,
                              storage::transient_copy());
            sqlite3_bind_int64(log.get(), 7,
                               static_cast<std::int64_t>(result.counts.records_removed));
            sqlite3_bind_int64(log.get(), 8,
                               static_cast<std::int64_t>(result.counts.versions_removed));
            sqlite3_bind_int64(log.get(), 9,
                               static_cast<std::int64_t>(result.counts.fts_entries_removed));
            sqlite3_bind_int64(log.get(), 10,
                               static_cast<std::int64_t>(result.counts.embeddings_removed));
            sqlite3_bind_int64(log.get(), 11,
                               static_cast<std::int64_t>(result.counts.artifacts_erased));
            sqlite3_bind_text(log.get(), 12, failure_note.c_str(), -1, storage::transient_copy());
            (void)sqlite3_step(log.get());
        }
        if (failed) {
            // Fail closed. The caller rolls the partial deletions back and
            // records the hold in its own transaction so the block survives.
            result.status = ErasureStatus::Pending;
            result.note = failure_note + "; scope held out of context, retry the erasure";
            if (request.scope.has_value()) {
                result.held_scope = *request.scope;
            } else if (record_scope.has_value()) {
                // A record-scoped erasure still holds its own scope out of
                // Context until the retry completes (design §8.3).
                result.held_scope = *record_scope;
            }
            return result;
        }
        // A completed scope erasure releases any hold on the scope. A
        // record-level erase does not: the hold means a privacy erasure is
        // still pending for that scope, and a retention purge of one
        // unrelated record must not put the scope back into Context.
        if (!scope_key.empty() && request.scope.has_value()) {
            Statement release(db, "DELETE FROM memory_scope_holds WHERE scope_key = ?1");
            if (!release.valid())
                return store_error(ErrorCode::Internal, "erasure hold release failed to prepare");
            sqlite3_bind_text(release.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
            if (sqlite3_step(release.get()) != SQLITE_DONE)
                return store_error(ErrorCode::Internal, "erasure hold release failed");
        }
        return result;
    }

    [[nodiscard]] static Result<void> record_pending_hold(sqlite3 *db, const std::string &scope_key,
                                                          const std::string &reason) {
        if (scope_key.empty()) {
            return store_error(ErrorCode::Internal, "erasure failed without a scope to hold");
        }
        Transaction hold_transaction(db);
        if (!hold_transaction.valid()) {
            return store_error(ErrorCode::Internal, "erasure hold transaction rejected");
        }
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        Statement hold(db, "INSERT INTO memory_scope_holds(scope_key, reason, since)"
                           " VALUES(?1, ?2, ?3)"
                           " ON CONFLICT(scope_key) DO UPDATE SET reason = excluded.reason");
        if (!hold.valid())
            return store_error(ErrorCode::Internal, "erasure hold write failed to prepare");
        sqlite3_bind_text(hold.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(hold.get(), 2, reason.c_str(), -1, storage::transient_copy());
        sqlite3_bind_int64(hold.get(), 3, now);
        if (sqlite3_step(hold.get()) != SQLITE_DONE)
            return store_error(ErrorCode::Internal, "erasure hold write failed");
        Statement log(db, "INSERT INTO memory_erasure_log(scope_key, reason, requested_at,"
                          " status, note) VALUES(?1, ?2, ?3, 'pending', 'partial failure;"
                          " scope held out of context')");
        if (log.valid()) {
            sqlite3_bind_text(log.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 2, reason.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 3, now);
            (void)sqlite3_step(log.get());
        }
        return hold_transaction.commit();
    }

    [[nodiscard]] Result<ErasureResult> erase(sqlite3 *db, const ErasureRequest &request) {
        auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        std::string pending_scope_key;
        Result<ErasureResult> settled = store_error(ErrorCode::Internal, "unsettled erasure");
        {
            Transaction transaction(db);
            if (!transaction.valid()) {
                return store_error(ErrorCode::Internal, "erasure transaction rejected");
            }
            settled = erase_locked(db, request);
            if (!settled) {
                return settled; // rolls back
            }
            if (settled.value().status == ErasureStatus::Pending) {
                // Roll the partial deletions back (scope guard below): a
                // retried erasure must start from the pre-failure state.
                if (settled.value().held_scope.has_value()) {
                    pending_scope_key = scope_key_of(*settled.value().held_scope);
                }
            } else {
                const auto commit = transaction.commit();
                if (!commit) {
                    return commit.error();
                }
            }
        }
        // Outside the rolled-back transaction: the fail-closed hold and its
        // audit row persist in their own transaction.
        if (settled.value().status == ErasureStatus::Pending) {
            if (auto held = record_pending_hold(db, pending_scope_key, request.reason); !held)
                return held.error();
        }
        return settled;
    }

    // Purges one record inside the caller's transaction.
    //
    // A failed erasure is rolled back to its savepoint: the partial
    // deletions erase_locked already made must not survive a failure, so
    // the record stays whole and the next pass retries it. Returns true
    // when the record was purged and false when the erasure failed and was
    // rolled back — a failed purge is counted, never silently skipped.
    //
    // A retention failure does not hold the scope out of Context: the
    // record it could not reclaim is already expired, so it is excluded
    // from retrieval anyway, and the scope's live claims are untouched.
    // Holding the scope is the privacy path's rule (design §8.3).
    [[nodiscard]] Result<bool> purge_record(sqlite3 *db, const std::string &id,
                                            bool include_artifacts, const char *reason) {
        const auto parsed = MemoryId::parse(id);
        if (!parsed) {
            return Result<bool>(false);
        }
        const auto savepoint = storage::exec_sql(db, "SAVEPOINT mira_purge");
        if (!savepoint) {
            return savepoint.error();
        }
        ErasureRequest request;
        request.record = *parsed;
        request.reason = reason;
        request.include_artifacts = include_artifacts;
        auto erased = erase_locked(db, request);
        if (!erased) {
            const auto undone = storage::exec_sql(db, "ROLLBACK TO mira_purge");
            if (!undone) {
                return undone.error();
            }
            (void)storage::exec_sql(db, "RELEASE mira_purge");
            return erased.error();
        }
        if (erased.value().status == ErasureStatus::Complete) {
            const auto released = storage::exec_sql(db, "RELEASE mira_purge");
            if (!released) {
                return released.error();
            }
            return Result<bool>(true);
        }
        // Partial failure: undo the partial deletions. The record survives
        // whole and the next pass retries the reclamation.
        const auto undone = storage::exec_sql(db, "ROLLBACK TO mira_purge");
        if (!undone) {
            return undone.error();
        }
        (void)storage::exec_sql(db, "RELEASE mira_purge");
        return Result<bool>(false);
    }

    [[nodiscard]] Result<MemoryCompactionResult> compact(sqlite3 *db, const MemoryScope &scope) {
        const auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        MemoryCompactionResult result;
        const std::string key = scope_key_of(scope);
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        Transaction transaction(db);
        if (!transaction.valid()) {
            return store_error(ErrorCode::Internal, "compaction transaction rejected");
        }
        // Retention expiry: physical purge through the erasure path semantics.
        {
            std::vector<std::string> expired;
            Statement query(db, "SELECT id FROM memory_records WHERE scope_key = ?1"
                                " AND expires_at IS NOT NULL AND expires_at <= ?2");
            if (!query.valid()) {
                return store_error(ErrorCode::Internal, "expiry scan failed to prepare");
            }
            sqlite3_bind_text(query.get(), 1, key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(query.get(), 2, now);
            int step = sqlite3_step(query.get());
            while (step == SQLITE_ROW) {
                const auto *id_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
                if (id_text != nullptr) {
                    expired.emplace_back(id_text);
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "expiry scan failed");
            }
            for (const auto &id : expired) {
                const auto purged = purge_record(db, id, true, "retention-expiry");
                if (!purged) {
                    return purged.error();
                }
                if (purged.value()) {
                    result.expired_records_purged += 1;
                } else {
                    result.erasures_failed += 1;
                }
            }
        }
        // Version pruning beyond the keep window.
        {
            Statement prune(
                db, "DELETE FROM memory_versions WHERE record_id IN (SELECT id FROM"
                    " memory_records WHERE scope_key = ?1) AND version <= (SELECT MAX(version)"
                    " FROM memory_versions v2 WHERE v2.record_id = memory_versions.record_id)"
                    " - ?2");
            if (!prune.valid()) {
                return store_error(ErrorCode::Internal, "version prune failed to prepare");
            }
            sqlite3_bind_text(prune.get(), 1, key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(prune.get(), 2,
                               static_cast<std::int64_t>(options_.keep_versions_per_record));
            if (sqlite3_step(prune.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "version prune failed");
            }
            result.versions_pruned += static_cast<std::size_t>(sqlite3_changes(db));
        }
        // Tombstone purge past the horizon (0 keeps them forever).
        if (options_.purge_horizon.count() > 0) {
            const std::int64_t horizon =
                now - std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::duration_cast<std::chrono::seconds>(options_.purge_horizon))
                          .count();
            std::vector<std::string> tombstoned;
            Statement query(db, "SELECT id FROM memory_records WHERE scope_key = ?1"
                                " AND status = 'tombstoned' AND updated_at <= ?2");
            if (!query.valid()) {
                return store_error(ErrorCode::Internal, "tombstone scan failed to prepare");
            }
            sqlite3_bind_text(query.get(), 1, key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(query.get(), 2, horizon);
            int step = sqlite3_step(query.get());
            while (step == SQLITE_ROW) {
                const auto *id_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
                if (id_text != nullptr) {
                    tombstoned.emplace_back(id_text);
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "tombstone scan failed");
            }
            for (const auto &id : tombstoned) {
                const auto purged = purge_record(db, id, false, "retention-expiry");
                if (!purged) {
                    return purged.error();
                }
                if (purged.value()) {
                    result.tombstones_purged += 1;
                } else {
                    result.erasures_failed += 1;
                }
            }
        }
        const auto commit = transaction.commit();
        if (!commit) {
            return commit.error();
        }
        result.scopes_swept = 1;
        if (result.erasures_failed > 0) {
            result.note =
                "some retention erasures failed and were rolled back; the next pass retries them";
        }
        return result;
    }

    // One bounded pass over every scope (design Context/Memory §16.4).
    //
    // The pass is the same three phases the per-scope compaction runs, but
    // unscoped and bounded: expired records, version history and stale
    // tombstones are reclaimed across the whole store, and the pass stops
    // at its bounds and reports more_pending so a large store is swept
    // over several passes instead of one long transaction.
    [[nodiscard]] Result<MemoryCompactionResult> sweep(sqlite3 *db, MemorySweepOptions options) {
        const auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        const auto valid = options.validate();
        if (!valid) {
            return valid.error();
        }
        MemoryCompactionResult result;
        std::set<std::string> scopes;
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        const auto deadline_point = std::chrono::steady_clock::now() + options.deadline;
        bool deadline_hit = false;

        Transaction transaction(db);
        if (!transaction.valid()) {
            return store_error(ErrorCode::Internal, "sweep transaction rejected");
        }

        // A record is a candidate when it is expired (or a stale tombstone)
        // and its scope is not held out of Context. Held scopes are skipped
        // entirely: a privacy erasure owns them until it completes, and a
        // retention pass must neither touch them nor release their hold.
        struct Candidate final {
            std::string id;
            std::string scope_key;
        };
        const auto collect = [&db](const char *sql, std::int64_t bound, std::size_t limit,
                                   std::vector<Candidate> &out) -> Result<void> {
            Statement query(db, sql);
            if (!query.valid()) {
                return store_error(ErrorCode::Internal, "sweep scan failed to prepare");
            }
            sqlite3_bind_int64(query.get(), 1, bound);
            sqlite3_bind_int64(query.get(), 2, static_cast<std::int64_t>(limit));
            int step = sqlite3_step(query.get());
            while (step == SQLITE_ROW) {
                const auto *id_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
                const auto *scope_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 1));
                if (id_text != nullptr) {
                    Candidate candidate;
                    candidate.id = id_text;
                    if (scope_text != nullptr) {
                        candidate.scope_key = scope_text;
                    }
                    out.push_back(std::move(candidate));
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "sweep scan failed");
            }
            return Result<void>{};
        };

        // Phase 1: expired records across every scope.
        {
            std::vector<Candidate> expired;
            const auto scanned =
                collect("SELECT id, scope_key FROM memory_records WHERE expires_at IS NOT NULL"
                        " AND expires_at <= ?1 AND scope_key NOT IN (SELECT scope_key FROM"
                        " memory_scope_holds) ORDER BY expires_at LIMIT ?2",
                        now, options.max_records, expired);
            if (!scanned) {
                return scanned.error();
            }
            if (expired.size() >= options.max_records) {
                result.more_pending = true;
            }
            for (const auto &candidate : expired) {
                if (std::chrono::steady_clock::now() >= deadline_point) {
                    deadline_hit = true;
                    break;
                }
                const auto purged = purge_record(db, candidate.id, true, "retention-expiry");
                if (!purged) {
                    return purged.error();
                }
                scopes.insert(candidate.scope_key);
                if (purged.value()) {
                    result.expired_records_purged += 1;
                } else {
                    result.erasures_failed += 1;
                }
            }
        }
        // Phase 2: version history beyond the keep window, every scope.
        {
            Statement prune(db,
                            "DELETE FROM memory_versions WHERE rowid IN (SELECT rowid FROM"
                            " memory_versions WHERE record_id IN (SELECT id FROM memory_records)"
                            " AND version <= (SELECT MAX(version) FROM memory_versions v2"
                            " WHERE v2.record_id = memory_versions.record_id) - ?1 LIMIT ?2)");
            if (!prune.valid()) {
                return store_error(ErrorCode::Internal, "version prune failed to prepare");
            }
            sqlite3_bind_int64(prune.get(), 1,
                               static_cast<std::int64_t>(options_.keep_versions_per_record));
            sqlite3_bind_int64(prune.get(), 2, static_cast<std::int64_t>(options.max_versions));
            if (sqlite3_step(prune.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "version prune failed");
            }
            const auto pruned = static_cast<std::size_t>(sqlite3_changes(db));
            result.versions_pruned += pruned;
            if (pruned >= options.max_versions) {
                result.more_pending = true;
            }
        }
        // Phase 3: tombstones past the purge horizon (0 keeps them forever).
        if (options_.purge_horizon.count() > 0) {
            const std::int64_t horizon =
                now - std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::duration_cast<std::chrono::seconds>(options_.purge_horizon))
                          .count();
            std::vector<Candidate> tombstoned;
            const auto scanned =
                collect("SELECT id, scope_key FROM memory_records WHERE status = 'tombstoned'"
                        " AND updated_at <= ?1 AND scope_key NOT IN (SELECT scope_key FROM"
                        " memory_scope_holds) ORDER BY updated_at LIMIT ?2",
                        horizon, options.max_records, tombstoned);
            if (!scanned) {
                return scanned.error();
            }
            if (tombstoned.size() >= options.max_records) {
                result.more_pending = true;
            }
            for (const auto &candidate : tombstoned) {
                if (std::chrono::steady_clock::now() >= deadline_point) {
                    deadline_hit = true;
                    break;
                }
                const auto purged = purge_record(db, candidate.id, false, "retention-expiry");
                if (!purged) {
                    return purged.error();
                }
                scopes.insert(candidate.scope_key);
                if (purged.value()) {
                    result.tombstones_purged += 1;
                } else {
                    result.erasures_failed += 1;
                }
            }
        }

        const auto commit = transaction.commit();
        if (!commit) {
            return commit.error();
        }
        result.scopes_swept = scopes.size();
        if (deadline_hit) {
            result.more_pending = true;
        }
        if (result.erasures_failed > 0) {
            result.note =
                "some retention erasures failed and were rolled back; the next pass retries them";
        } else if (deadline_hit) {
            result.note = "sweep deadline reached; more work remains";
        }
        return result;
    }

    SqliteMemoryStoreOptions options_;
    StoreDiagnostics diagnostics_;
    std::unique_ptr<storage::MemoryCapacityVfs> capacity_;
    DatabaseHandle database_;
    std::unique_ptr<StoreChannel> channel_;
    IArtifactStore *artifacts_ = nullptr;
    RetrievalWeights weights_{};
    std::atomic<bool> read_only_{false};
};

SqliteMemoryStore::SqliteMemoryStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<SqliteMemoryStore>> SqliteMemoryStore::open(kairo::Executor &executor,
                                                                   SqliteMemoryStoreOptions options,
                                                                   IArtifactStore *artifacts) {
    const auto valid = options.validate();
    if (!valid) {
        return valid.error();
    }
    storage::DbOpenOptions db_options;
    db_options.path = options.path;
    db_options.busy_timeout = options.busy_timeout;
    db_options.mode = options.read_only_diagnostic ? storage::DbOpenMode::ReadOnlyDiagnostic
                                                   : storage::DbOpenMode::ReadWrite;
    std::unique_ptr<storage::MemoryCapacityVfs> capacity;
    if (options.max_storage_bytes != 0) {
        auto guarded = storage::MemoryCapacityVfs::create(options);
        if (!guarded)
            return guarded.error();
        capacity = std::move(guarded.value());
        db_options.vfs_name = capacity->name();
        db_options.exclusive_owner = true;
    }
    const std::string schema_ddl = std::string(kSchemaDdl) + kSocialFieldIndex +
                                   kSocialRelationshipIndexes + kSocialEnvironmentIndex +
                                   kWorldPropertyIndex + recovery_lesson_index_sql();
    auto opened = storage::open_reference_store(
        db_options, {kStoreKind, kMemoryStoreSchema, schema_ddl.c_str(), store_error,
                     [](sqlite3 *db, SchemaVersion previous) -> Result<void> {
                         // The callback receives the file's recorded version
                         // once, so the chain must fall through each step.
                         if (previous.major == 1 && previous.minor < 1) {
                             auto migrated = migrate_memory_schema_1_0_to_1_1(db);
                             if (!migrated) {
                                 return migrated.error();
                             }
                         }
                         if (previous.major == 1 && previous.minor < 2) {
                             auto migrated = migrate_memory_schema_1_1_to_1_2(db);
                             if (!migrated) {
                                 return migrated.error();
                             }
                         }
                         if (previous.major == 1 && previous.minor < 3) {
                             auto migrated = migrate_memory_schema_1_2_to_1_3(db);
                             if (!migrated) {
                                 return migrated.error();
                             }
                         }
                         if (previous.minor < 4) {
                             auto migrated = storage::exec_sql(db, kSocialFieldIndex);
                             if (!migrated)
                                 return migrated.error();
                         }
                         if (previous.minor < 5) {
                             auto migrated = storage::exec_sql(db, kSocialRelationshipIndexes);
                             if (!migrated)
                                 return migrated.error();
                         }
                         if (previous.minor < 6) {
                             auto migrated = storage::exec_sql(db, kSocialEnvironmentIndex);
                             if (!migrated)
                                 return migrated.error();
                         }
                         if (previous.minor < 7) {
                             if (auto migrated = storage::exec_sql(db, kWorldPropertyIndex);
                                 !migrated)
                                 return migrated.error();
                         }
                         const auto sql = recovery_lesson_index_sql();
                         return storage::exec_sql(db, sql.c_str());
                     }});
    if (!opened) {
        if (capacity && capacity->rejected())
            return storage::memory_capacity_error();
        return opened.error();
    }
    auto handle = std::move(opened.value().database);
    auto diagnostics = std::move(opened.value().diagnostics);

    StoreChannel::Config channel_config;
    channel_config.worker_name = "mira-memory-store";
    channel_config.max_pending = options.max_pending_requests;
    channel_config.operation_timeout = options.operation_timeout;
    auto channel = std::make_unique<StoreChannel>(executor, handle.get(), channel_config);

    const bool read_only = options.read_only_diagnostic ||
                           diagnostics.disposition == StoreSchemaDisposition::ReadOnlyDiagnostic;
    auto impl = std::make_unique<Impl>(std::move(options), diagnostics, std::move(handle),
                                       std::move(channel), artifacts, std::move(capacity));
    impl->read_only_.store(read_only);
    return std::unique_ptr<SqliteMemoryStore>(new SqliteMemoryStore(std::move(impl)));
}

SqliteMemoryStore::~SqliteMemoryStore() {
    if (impl_ != nullptr) {
        (void)impl_->channel_->close();
    }
}

Result<MemoryQueryResult> SqliteMemoryStore::query(const MemoryQuery &query) const {
    return impl_->run<MemoryQueryResult>(
        [this, query](sqlite3 *db) { return impl_->query(db, query); });
}

Result<WorldFactLookup> SqliteMemoryStore::lookup_world_fact(const WorldFactQuery &query) const {
    return impl_->run<WorldFactLookup>(
        [query](sqlite3 *db) { return Impl::lookup_world_fact(db, query); });
}

Result<std::optional<MemoryRecord>> SqliteMemoryStore::get(MemoryId record,
                                                           const MemoryScope &scope) const {
    return impl_->run<std::optional<MemoryRecord>>(
        [record, scope](sqlite3 *db) -> Result<std::optional<MemoryRecord>> {
            return Impl::read_scoped_record(db, record, scope);
        });
}

Result<MemoryMutationResult> SqliteMemoryStore::apply(const MemoryMutation &mutation) {
    return impl_->run<MemoryMutationResult>(
        [this, &mutation](sqlite3 *db) { return impl_->apply(db, mutation); });
}

Result<MemoryMutationResult>
SqliteMemoryStore::apply_add_with_legacy_identity(const MemoryMutation &mutation,
                                                  const LegacyMemoryAdd &legacy) {
    if (auto valid = legacy.validate_for(mutation); !valid)
        return valid.error();
    return impl_->run<MemoryMutationResult>(
        [this, mutation, legacy](sqlite3 *db) { return impl_->apply(db, mutation, &legacy); });
}

Result<MemoryCompactionResult> SqliteMemoryStore::compact(const MemoryScope &scope) {
    return impl_->run<MemoryCompactionResult>(
        [this, &scope](sqlite3 *db) { return impl_->compact(db, scope); });
}

Result<MemoryCompactionResult> SqliteMemoryStore::sweep(MemorySweepOptions options) {
    return impl_->run<MemoryCompactionResult>(
        [this, options](sqlite3 *db) { return impl_->sweep(db, options); });
}

Result<ErasureResult> SqliteMemoryStore::erase(const ErasureRequest &request) {
    return impl_->run<ErasureResult>(
        [this, request](sqlite3 *db) { return impl_->erase(db, request); });
}

Result<Id128> SqliteMemoryStore::social_erasure_store_id() {
    return impl_->run<Id128>([this](sqlite3 *db) -> Result<Id128> {
        if (auto writable = impl_->check_writable(); !writable)
            return writable.error();
        Transaction transaction(db);
        if (!transaction.valid())
            return store_error(ErrorCode::Unavailable,
                               "memory store identity transaction unavailable");
        const auto stored = storage::meta_read(db, "social_erasure_store_id");
        if (!stored)
            return stored.error();
        if (stored.value()) {
            const auto id = Id128::parse(*stored.value());
            if (!id || id->is_nil() || id->to_string() != *stored.value())
                return store_error(ErrorCode::DataLoss, "memory store identity is malformed");
            return *id;
        }
        const auto id = Id128::generate();
        if (auto written = storage::meta_write(db, "social_erasure_store_id", id.to_string());
            !written)
            return written.error();
        if (auto committed = transaction.commit(); !committed)
            return committed.error();
        return id;
    });
}

Result<void> SqliteMemoryStore::index_embedding(MemoryId record, EmbedderId embedder,
                                                std::span<const float> vector) {
    if (vector.empty() || vector.size() > 4'096) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "embedding dimensions must be within (0, 4096]");
    }
    if (embedder.is_nil()) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "embedding requires a producer identity");
    }
    return impl_->run<void>([record, embedder, vector](sqlite3 *db) -> Result<void> {
        const std::string id = record.to_string();
        const std::string producer = embedder.to_string();
        Statement exists(db, "SELECT 1 FROM memory_records WHERE id = ?1 AND status = 'active'");
        if (!exists.valid()) {
            return store_error(ErrorCode::Internal, "embedding check failed to prepare");
        }
        sqlite3_bind_text(exists.get(), 1, id.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(exists.get()) != SQLITE_ROW) {
            return make_memory_error(MemoryDomainCode::InvalidRecord,
                                     "embedding target is not an active record");
        }
        Transaction transaction(db);
        if (!transaction.valid()) {
            return store_error(ErrorCode::Internal, "embedding transaction rejected");
        }
        Statement upsert(db, "INSERT INTO memory_embeddings(record_id, embedder, dim,"
                             " vector, embedded_at) VALUES(?1, ?2, ?3, ?4, ?5)"
                             " ON CONFLICT(record_id) DO UPDATE SET embedder ="
                             " excluded.embedder, dim = excluded.dim,"
                             " vector = excluded.vector, embedded_at = excluded.embedded_at");
        if (!upsert.valid()) {
            return store_error(ErrorCode::Internal, "embedding upsert failed to prepare");
        }
        const auto bytes = static_cast<int>(vector.size() * sizeof(float));
        sqlite3_bind_text(upsert.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(upsert.get(), 2, producer.c_str(), -1, storage::transient_copy());
        sqlite3_bind_int64(upsert.get(), 3, static_cast<std::int64_t>(vector.size()));
        sqlite3_bind_blob(upsert.get(), 4, vector.data(), bytes, storage::transient_copy());
        sqlite3_bind_int64(upsert.get(), 5, wall_nanos(std::chrono::system_clock::now()));
        if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "embedding upsert failed");
        }
        return transaction.commit();
    });
}

Result<std::size_t> SqliteMemoryStore::index_lag(EmbedderId embedder) const {
    return impl_->run<std::size_t>([embedder](sqlite3 *db) -> Result<std::size_t> {
        const std::string producer = embedder.to_string();
        // Lag is answered per identity (DEC-111): after a producer
        // switch the previously indexed vectors stop counting, so the
        // rebuild scope is reported truthfully instead of fake zero.
        Statement lag(db, "SELECT COUNT(*) FROM memory_records r WHERE r.status = 'active'"
                          " AND NOT EXISTS(SELECT 1 FROM memory_embeddings e"
                          " WHERE e.record_id = r.id AND e.embedder = ?1)");
        if (!lag.valid()) {
            return store_error(ErrorCode::Internal, "index lag failed to prepare");
        }
        sqlite3_bind_text(lag.get(), 1, producer.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(lag.get()) != SQLITE_ROW) {
            return store_error(ErrorCode::Internal, "index lag failed");
        }
        return static_cast<std::size_t>(sqlite3_column_int64(lag.get(), 0));
    });
}

Result<bool> SqliteMemoryStore::has_embedding(MemoryId record, EmbedderId embedder) const {
    return impl_->run<bool>([record, embedder](sqlite3 *db) -> Result<bool> {
        Statement query(db, "SELECT 1 FROM memory_embeddings WHERE record_id = ?1"
                            " AND embedder = ?2");
        if (!query.valid()) {
            return store_error(ErrorCode::Internal, "embedding probe failed to prepare");
        }
        const std::string id = record.to_string();
        const std::string producer = embedder.to_string();
        sqlite3_bind_text(query.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(query.get(), 2, producer.c_str(), -1, storage::transient_copy());
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_ROW) {
            return true;
        }
        if (step != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "embedding probe failed");
        }
        return false;
    });
}

Result<std::size_t> SqliteMemoryStore::clear_embeddings(EmbedderId embedder) {
    const auto writable = impl_->check_writable();
    if (!writable) {
        return writable.error();
    }
    return impl_->run<std::size_t>([embedder](sqlite3 *db) -> Result<std::size_t> {
        Transaction transaction(db);
        if (!transaction.valid()) {
            return store_error(ErrorCode::Internal, "index reset transaction rejected");
        }
        Statement clear(db, "DELETE FROM memory_embeddings WHERE embedder = ?1");
        if (!clear.valid()) {
            return store_error(ErrorCode::Internal, "index reset failed to prepare");
        }
        const std::string producer = embedder.to_string();
        sqlite3_bind_text(clear.get(), 1, producer.c_str(), -1, storage::transient_copy());
        if (sqlite3_step(clear.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "index reset failed");
        }
        const auto removed = static_cast<std::size_t>(sqlite3_changes(db));
        const auto commit = transaction.commit();
        if (!commit) {
            return commit.error();
        }
        return removed;
    });
}

const StoreDiagnostics &SqliteMemoryStore::diagnostics() const noexcept {
    return impl_->diagnostics_;
}

std::size_t SqliteMemoryStore::pending_requests() const { return impl_->channel_->pending_count(); }

void SqliteMemoryStore::set_worker_paused(bool paused) { impl_->channel_->set_paused(paused); }

Result<void> SqliteMemoryStore::close() noexcept { return impl_->channel_->close(); }

Result<SqliteMemoryCapacitySnapshot> SqliteMemoryStore::capacity_snapshot() const {
    return impl_->run<SqliteMemoryCapacitySnapshot>(
        [this](sqlite3 *) -> Result<SqliteMemoryCapacitySnapshot> {
            if (!impl_->capacity_)
                return store_error(ErrorCode::InvalidState,
                                   "memory file capacity is not configured");
            return impl_->capacity_->snapshot();
        });
}
} // namespace mira
