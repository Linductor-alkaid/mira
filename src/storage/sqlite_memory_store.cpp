#include <mira/sqlite_memory_store.hpp>

#include "sqlite_support.hpp"

#include <mira/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
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

constexpr SchemaVersion kMemoryStoreSchema{1, 0};
constexpr const char *kStoreKind = "memory";

constexpr const char *kSchemaDdl =
    "BEGIN;"
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
    " applied_type TEXT NOT NULL,"
    " record_id TEXT NOT NULL,"
    " result_version INTEGER NOT NULL,"
    " applied_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS memory_embeddings("
    " record_id TEXT PRIMARY KEY,"
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
    "COMMIT;";

[[nodiscard]] std::int64_t wall_nanos(const std::chrono::system_clock::time_point &stamp) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count());
}


[[nodiscard]] std::string scope_key_of(const MemoryScope &scope) {
    std::string key = memory_scope_kind_name(scope.kind);
    key += '|';
    key += scope.subject_id;
    key += '|';
    key += scope.tenant_id.value_or(std::string{});
    return key;
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

[[nodiscard]] std::string sensitivity_text(Sensitivity sensitivity) {
    switch (sensitivity) {
    case Sensitivity::Public:
        return "public";
    case Sensitivity::Internal:
        return "internal";
    case Sensitivity::Sensitive:
        return "sensitive";
    case Sensitivity::Secret:
        return "secret";
    }
    return "unknown";
}


[[nodiscard]] int verification_rank(MemoryVerification verification) {
    switch (verification) {
    case MemoryVerification::Unverified:
        return 0;
    case MemoryVerification::Observed:
        return 1;
    case MemoryVerification::Verified:
        return 2;
    case MemoryVerification::HumanConfirmed:
        return 3;
    }
    return 0;
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
    decoded.decoded = std::move(value);
    return decoded;
}

[[nodiscard]] std::string fts_match_expression(const std::string &text) {
    // Neutralize FTS5 syntax: every whitespace token becomes a quoted
    // phrase and terms combine with AND, so user text can never inject
    // operators while multi-word queries still match like a keyword AND.
    std::string expression;
    std::string token;
    const auto flush = [&expression, &token]() {
        if (token.empty()) {
            return;
        }
        std::string escaped;
        for (const char character : token) {
            if (character == '"') {
                escaped += "\"\"";
            } else {
                escaped += character;
            }
        }
        if (!expression.empty()) {
            expression += " AND ";
        }
        expression += "\"" + escaped + "\"";
        token.clear();
    };
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            flush();
        } else {
            token += character;
        }
    }
    flush();
    return expression;
}

[[nodiscard]] double cosine_similarity(const float *left, const float *right, std::size_t dim) {
    double dot = 0.0;
    double left_norm = 0.0;
    double right_norm = 0.0;
    for (std::size_t index = 0; index < dim; ++index) {
        dot += static_cast<double>(left[index]) * static_cast<double>(right[index]);
        left_norm += static_cast<double>(left[index]) * static_cast<double>(left[index]);
        right_norm += static_cast<double>(right[index]) * static_cast<double>(right[index]);
    }
    if (left_norm <= 0.0 || right_norm <= 0.0) {
        return -1.0; // zero vector cannot establish similarity
    }
    return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

} // namespace

Result<void> SqliteMemoryStoreOptions::validate() const {
    if (path.empty()) {
        return store_error(ErrorCode::InvalidArgument, "store path must not be empty");
    }
    if (max_pending_requests == 0) {
        return store_error(ErrorCode::InvalidArgument,
                           "store bounds must be greater than zero");
    }
    if (operation_timeout.count() <= 0 || busy_timeout.count() < 0) {
        return store_error(ErrorCode::InvalidArgument, "store timeouts must be positive");
    }
    if (max_vector_scan == 0 || keep_versions_per_record == 0) {
        return store_error(ErrorCode::InvalidArgument, "store scan bounds must be positive");
    }
    return Result<void>{};
}

class SqliteMemoryStore::Impl final {
  public:
    Impl(SqliteMemoryStoreOptions options, StoreDiagnostics diagnostics, DatabaseHandle database,
         std::unique_ptr<StoreChannel> channel, IArtifactStore *artifacts)
        : options_(std::move(options)), diagnostics_(std::move(diagnostics)),
          database_(std::move(database)), channel_(std::move(channel)), artifacts_(artifacts) {}

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
        if (sqlite3_step(hold.get()) == SQLITE_ROW) {
            return make_memory_error(
                MemoryDomainCode::ErasurePending,
                "scope is held out of context by a pending erasure; retry the erasure");
        }
        return Result<void>{};
    }

    // ---- record plumbing ----

    static void bind_record_insert(Statement &insert, const MemoryRecord &record,
                                   const std::string &scope_key, std::uint64_t version) {
        const std::string id = record.id.to_string();
        sqlite3_bind_text(insert.get(), 1, id.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(insert.get(), 2, scope_key.c_str(), -1, storage::transient_copy());
        sqlite3_bind_text(insert.get(), 3, memory_scope_kind_name(record.scope.kind).c_str(), -1,
                          storage::transient_copy());
        sqlite3_bind_text(insert.get(), 4, record.scope.subject_id.c_str(), -1, storage::transient_copy());
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
        sqlite3_bind_text(insert.get(), 14, sensitivity_text(record.sensitivity).c_str(), -1,
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
        Statement provenance(db,
                             "INSERT OR IGNORE INTO memory_provenance(record_id, event_id)"
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
        if (record.status == MemoryStatus::Active) {
            Statement insert_fts(
                db, "INSERT INTO memory_fts(record_id, statement) VALUES(?1, ?2)");
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
        sqlite3_bind_int64(insert_version.get(), 3, wall_nanos(record.recorded_at));
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

    [[nodiscard]] static Result<std::optional<std::uint64_t>>
    current_version(sqlite3 *db, const MemoryId &id) {
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

    // Writes one record row (24 columns, ?1..?24) at the given version.
    [[nodiscard]] static Result<void> write_record(sqlite3 *db, const MemoryRecord &record,
                                                   const std::string &scope_key,
                                                   std::uint64_t version) {
        const std::string document = to_json_string(memory_record_to_json(record));
        Statement upsert(
            db, "INSERT INTO memory_records(id, scope_key, scope_kind, subject_id, tenant_id,"
                " kind, statement, evidence, valid_from, valid_until, recorded_at,"
                " verification, confidence, sensitivity, sensitivity_rank, status, supersedes,"
                " expires_at, source_namespace, schema_major, schema_minor, version,"
                " updated_at, document)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15,"
                " ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24)"
                " ON CONFLICT(id) DO UPDATE SET"
                " statement = excluded.statement, evidence = excluded.evidence,"
                " valid_from = excluded.valid_from, valid_until = excluded.valid_until,"
                " recorded_at = excluded.recorded_at, verification = excluded.verification,"
                " confidence = excluded.confidence, sensitivity = excluded.sensitivity,"
                " sensitivity_rank = excluded.sensitivity_rank, status = excluded.status,"
                " supersedes = excluded.supersedes, expires_at = excluded.expires_at,"
                " source_namespace = excluded.source_namespace,"
                " version = excluded.version, updated_at = excluded.updated_at,"
                " document = excluded.document");
        if (!upsert.valid()) {
            return store_error(ErrorCode::Internal, "record upsert failed to prepare");
        }
        MemoryRecord indexed = record;
        indexed.version = version;
        bind_record_insert(upsert, indexed, scope_key, version);
        if (record.evidence.has_value()) {
            const std::string evidence = to_json_string(
                JsonValue::Object{{"id", record.evidence->id.to_string()}});
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
        if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
            return store_error(ErrorCode::Internal, "record upsert failed");
        }
        return sync_derived_rows(db, indexed, version);
    }

    [[nodiscard]] Result<MemoryMutationResult> apply(sqlite3 *db, const MemoryMutation &mutation) {
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
        const auto hold = check_no_hold(db, scope_key);
        if (!hold) {
            return hold.error();
        }
        const std::string mutation_id = mutation.id.to_string();
        {
            Statement replay(db, "SELECT applied_type, record_id, result_version"
                                 " FROM memory_mutations WHERE mutation_id = ?1");
            if (!replay.valid()) {
                return store_error(ErrorCode::Internal, "mutation replay failed to prepare");
            }
            sqlite3_bind_text(replay.get(), 1, mutation_id.c_str(), -1, storage::transient_copy());
            if (sqlite3_step(replay.get()) == SQLITE_ROW) {
                MemoryMutationResult result;
                const auto *type_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(replay.get(), 0));
                result.applied = type_text != nullptr &&
                                         memory_mutation_type_from(type_text).has_value()
                                     ? *memory_mutation_type_from(type_text)
                                     : MemoryMutationType::Noop;
                const auto *record_text =
                    reinterpret_cast<const char *>(sqlite3_column_text(replay.get(), 1));
                if (auto parsed = MemoryId::parse(record_text != nullptr ? record_text : "");
                    parsed) {
                    result.record = *parsed;
                }
                result.new_version =
                    static_cast<std::uint64_t>(sqlite3_column_int64(replay.get(), 2));
                result.idempotent_replay = true;
                return result; // read-only; the transaction rolls back
            }
        }

        MemoryRecord proposed = mutation.proposed;
        proposed.scope = mutation.scope;
        std::uint64_t new_version = 1;
        switch (mutation.type) {
        case MemoryMutationType::Add: {
            const auto existing = current_version(db, proposed.id);
            if (!existing) {
                return existing.error();
            }
            if (existing.value().has_value()) {
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
            const auto existing_version = current_version(db, *mutation.target);
            if (!existing_version) {
                return existing_version.error();
            }
            if (!existing_version.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "mutation target does not exist");
            }
            if (*existing_version.value() != *mutation.expected_version) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "memory version moved on; re-read and re-plan");
            }
            auto existing = read_record(db, *mutation.target);
            if (!existing) {
                return existing.error();
            }
            if (!existing.value().has_value()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "mutation target vanished mid-transaction");
            }
            if (scope_key_of(existing.value()->scope) != scope_key) {
                return make_memory_error(MemoryDomainCode::ScopeDenied,
                                         "mutations cannot move a record across scopes");
            }
            if (mutation.type == MemoryMutationType::Tombstone) {
                proposed = *existing.value();
                proposed.status = MemoryStatus::Tombstoned;
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
            proposed.status = proposed.status == MemoryStatus::Superseded
                                  ? MemoryStatus::Active
                                  : proposed.status;
            // Update is in place: the target keeps its identity and version
            // history; only Supersede introduces a fresh record id.
            proposed.id = *mutation.target;
            new_version = *existing_version.value() + 1;
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
            Statement log(db, "INSERT INTO memory_mutations(mutation_id, applied_type, record_id,"
                              " result_version, applied_at) VALUES(?1, ?2, ?3, ?4, ?5)");
            if (!log.valid()) {
                return store_error(ErrorCode::Internal, "mutation log failed to prepare");
            }
            const std::string record_id = proposed.id.to_string();
            sqlite3_bind_text(log.get(), 1, mutation_id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 2, memory_mutation_type_name(mutation.type).c_str(), -1,
                              storage::transient_copy());
            sqlite3_bind_text(log.get(), 3, record_id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 4, static_cast<std::int64_t>(new_version));
            sqlite3_bind_int64(log.get(), 5, wall_nanos(std::chrono::system_clock::now()));
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
            const auto hold = check_no_hold(db, scope_keys.back());
            if (!hold) {
                return hold.error();
            }
        }
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        const bool bitemporal =
            request.as_of_recorded.has_value() || request.as_of_valid.has_value();

        // Base universe: exact scope equality, nothing crosses by similarity.
        std::string sql =
            "SELECT id, scope_key, kind, statement, recorded_at, confidence, verification,"
            " sensitivity_rank, status, evidence, valid_from, document FROM memory_records WHERE (";
        for (std::size_t index = 0; index < scope_keys.size(); ++index) {
            sql += index == 0 ? "scope_key = ?" + std::to_string(index + 1)
                              : " OR scope_key = ?" + std::to_string(index + 1);
        }
        sql += ")";
        if (!bitemporal) {
            sql += " AND status = 'active' AND (expires_at IS NULL OR expires_at > ?" +
                   std::to_string(scope_keys.size() + 1) + ")";
        }
        if (request.kinds.has_value()) {
            sql += " AND kind IN (";
            for (std::size_t index = 0; index < request.kinds->size(); ++index) {
                sql += index == 0 ? "?" + std::to_string(scope_keys.size() + 2 + index)
                                  : ", ?" + std::to_string(scope_keys.size() + 2 + index);
            }
            sql += ")";
        }
        sql += " ORDER BY recorded_at DESC LIMIT 4096";
        Statement universe(db, sql.c_str());
        if (!universe.valid()) {
            return store_error(ErrorCode::Internal, "memory query failed to prepare");
        }
        int next_param = 1;
        for (const auto &key : scope_keys) {
            sqlite3_bind_text(universe.get(), next_param, key.c_str(), -1, storage::transient_copy());
            ++next_param;
        }
        if (!bitemporal) {
            sqlite3_bind_int64(universe.get(), next_param, now);
            ++next_param;
        }
        if (request.kinds.has_value()) {
            for (const auto kind : *request.kinds) {
                const std::string name = memory_kind_name(kind);
                sqlite3_bind_text(universe.get(), next_param, name.c_str(), -1, storage::transient_copy());
                ++next_param;
            }
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
            std::erase_if(candidates, [cap](const Row &row) {
                return row.sensitivity_rank_value > cap;
            });
        }
        if (bitemporal) {
            // Resolve each record to the version mandated by the bitemporal
            // query: latest version known by as_of_recorded whose validity
            // covers as_of_valid (open bounds mean "now").
            const std::int64_t recorded_cap =
                wall_nanos(request.as_of_recorded.value_or(std::chrono::system_clock::now()));
            std::optional<std::int64_t> valid_at;
            if (request.as_of_valid.has_value()) {
                valid_at = wall_nanos(*request.as_of_valid);
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
                            effective_until = !effective_until.has_value() || closed < *effective_until
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
        if (!request.text.empty() &&
            std::chrono::steady_clock::now() < deadline_point) {
            Statement fts(db, "SELECT record_id, rank FROM memory_fts WHERE memory_fts MATCH ?1"
                              " ORDER BY rank LIMIT 512");
            if (fts.valid()) {
                result.quality.fts_leg_ran = true;
                const std::string expression = fts_match_expression(request.text);
                sqlite3_bind_text(fts.get(), 1, expression.c_str(), -1, storage::transient_copy());
                int fstep = sqlite3_step(fts.get());
                std::size_t position = 0;
                while (fstep == SQLITE_ROW) {
                    const auto *id_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(fts.get(), 0));
                    if (id_text != nullptr) {
                        // FTS5 bm25 ranks are negative-better; normalize the
                        // magnitude into a (0,1] contribution.
                        const double rank = sqlite3_column_double(fts.get(), 1);
                        fts_rank[id_text] =
                            1.0 / (1.0 + std::max(0.0, -rank) + static_cast<double>(position) * 0.01);
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
        std::map<std::string, double> vector_scores;
        if (!request.query_embedding.empty() &&
            std::chrono::steady_clock::now() < deadline_point) {
            const std::size_t query_dim = request.query_embedding.size();
            Statement embeddings(
                db, "SELECT record_id, vector FROM memory_embeddings"
                    " ORDER BY embedded_at DESC LIMIT ?");
            std::size_t vector_errors = 0;
            if (embeddings.valid()) {
                sqlite3_bind_int64(embeddings.get(), 1,
                                   static_cast<std::int64_t>(options_.max_vector_scan));
                int estep = sqlite3_step(embeddings.get());
                while (estep == SQLITE_ROW) {
                    const auto *id_text =
                        reinterpret_cast<const char *>(sqlite3_column_text(embeddings.get(), 0));
                    const auto *blob = sqlite3_column_blob(embeddings.get(), 1);
                    const int bytes = sqlite3_column_bytes(embeddings.get(), 1);
                    if (id_text == nullptr || blob == nullptr ||
                        bytes <= 0 || bytes % 4 != 0) {
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
                    const auto *vector = static_cast<const float *>(blob);
                    const double similarity =
                        cosine_similarity(vector, request.query_embedding.data(), dim);
                    if (similarity >= 0.0) {
                        vector_scores[id_text] = similarity;
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
        const std::int64_t newest = candidates.empty()
                                        ? now
                                        : std::max_element(candidates.begin(), candidates.end(),
                                                           [](const Row &a, const Row &b) {
                                                               return a.recorded_at <
                                                                      b.recorded_at;
                                                           })
                                              ->recorded_at;
        const std::int64_t oldest =
            candidates.empty() ? now
                               : std::min_element(candidates.begin(), candidates.end(),
                                                  [](const Row &a, const Row &b) {
                                                      return a.recorded_at < b.recorded_at;
                                                  })
                                     ->recorded_at;
        const double span = newest > oldest ? static_cast<double>(newest - oldest) : 1.0;
        const bool any_leg = !exact_hits.empty() || !fts_rank.empty() || !vector_scores.empty();
        // A query that asked for ranking (text, terms or embedding) and got
        // no reliable hits must not dump the whole scope as a "fallback".
        const bool ranking_attempted = !request.text.empty() || !request.exact_terms.empty() ||
                                       !request.query_embedding.empty();
        std::set<std::string> seen_statements;
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
            if (!seen_statements.insert(row.statement).second) {
                continue; // near-duplicate suppression at statement equality
            }
            double score = 0.0;
            if (exact_hit) {
                score += weights.exact;
            }
            if (fts_entry != fts_rank.end()) {
                score += weights.fts * fts_entry->second;
            }
            if (vector_entry != vector_scores.end()) {
                score += weights.vector * vector_entry->second;
            }
            score += weights.verification *
                     (static_cast<double>(
                          verification_rank(row.decoded.verification)) /
                      3.0);
            score += weights.confidence * row.confidence;
            if (newest >= oldest) {
                const double recency =
                    span > 0.0
                        ? static_cast<double>(row.recorded_at - oldest) / span
                        : 1.0;
                score += weights.recency * recency;
            }
            scored.push_back({&row, score});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const Scored &lhs, const Scored &rhs) { return lhs.score > rhs.score; });
        std::map<std::string, std::size_t> group_counts;
        for (const auto &entry : scored) {
            if (result.records.size() >= request.max_results) {
                break;
            }
            const std::string group = entry.row->scope_key + "|" + entry.row->kind;
            if (group_counts[group] >= options_.max_per_scope_kind) {
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
        // Vector index lag: Active records without usable embeddings.
        {
            Statement lag(db, "SELECT COUNT(*) FROM memory_records r WHERE r.status = 'active'"
                              " AND NOT EXISTS(SELECT 1 FROM memory_embeddings e"
                              " WHERE e.record_id = r.id)");
            if (lag.valid() && sqlite3_step(lag.get()) == SQLITE_ROW) {
                result.quality.index_lag =
                    static_cast<std::size_t>(sqlite3_column_int64(lag.get(), 0));
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
            targets.scope_key = scope_key_of(*request.scope);
            Statement query(db, "SELECT id FROM memory_records WHERE scope_key = ?1");
            if (!query.valid()) {
                return store_error(ErrorCode::Internal, "erasure scan failed to prepare");
            }
            sqlite3_bind_text(query.get(), 1, targets.scope_key->c_str(), -1, storage::transient_copy());
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

    [[nodiscard]] static Result<std::optional<ArtifactId>> evidence_artifact(sqlite3 *db,
                                                                            const std::string &id) {
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
        for (const auto &id : targets.value().ids) {
            auto evidence = evidence_artifact(db, id);
            if (!evidence) {
                failed = true;
                failure_note = "evidence lookup failed";
                break;
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
                if (outcome != SQLITE_DONE) {
                    failed = true;
                    failure_note = std::string("erasure step failed: ") + step.label;
                    break;
                }
                if (sqlite3_changes(db) > 0) {
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
            if (request.include_artifacts && artifacts_ != nullptr && evidence.value().has_value()) {
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
            sqlite3_bind_text(log.get(), 6, failed ? "pending" : "complete", -1, storage::transient_copy());
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
            }
            return result;
        }
        // A completed erasure releases any hold on the scope.
        if (!scope_key.empty()) {
            Statement release(db, "DELETE FROM memory_scope_holds WHERE scope_key = ?1");
            if (release.valid()) {
                sqlite3_bind_text(release.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
                (void)sqlite3_step(release.get());
            }
        }
        return result;
    }

    static void record_pending_hold(sqlite3 *db, const std::string &scope_key,
                                     const std::string &reason) {
        if (scope_key.empty()) {
            return;
        }
        Transaction hold_transaction(db);
        if (!hold_transaction.valid()) {
            return;
        }
        const std::int64_t now = wall_nanos(std::chrono::system_clock::now());
        Statement hold(db, "INSERT INTO memory_scope_holds(scope_key, reason, since)"
                           " VALUES(?1, ?2, ?3)"
                           " ON CONFLICT(scope_key) DO UPDATE SET reason = excluded.reason");
        if (hold.valid()) {
            sqlite3_bind_text(hold.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(hold.get(), 2, reason.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(hold.get(), 3, now);
            (void)sqlite3_step(hold.get());
        }
        Statement log(db, "INSERT INTO memory_erasure_log(scope_key, reason, requested_at,"
                          " status, note) VALUES(?1, ?2, ?3, 'pending', 'partial failure;"
                          " scope held out of context')");
        if (log.valid()) {
            sqlite3_bind_text(log.get(), 1, scope_key.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 2, reason.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 3, now);
            (void)sqlite3_step(log.get());
        }
        (void)hold_transaction.commit();
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
            record_pending_hold(db, pending_scope_key, request.reason);
        }
        return settled;
    }

    [[nodiscard]] Result<MemoryCompactionResult> compact(sqlite3 *db, const MemoryScope &scope) {
        const auto writable = check_writable();
        if (!writable) {
            return writable.error();
        }
        MemoryCompactionResult result;
        std::vector<std::string> pending_holds;
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
                ErasureRequest request;
                if (auto parsed = MemoryId::parse(id); parsed) {
                    request.record = parsed;
                } else {
                    continue;
                }
                request.reason = "retention-expiry";
                request.include_artifacts = true;
                const auto erased = erase_locked(db, request);
                if (!erased) {
                    return erased.error();
                }
                if (erased.value().status == ErasureStatus::Complete) {
                    result.expired_records_purged += 1;
                } else if (erased.value().held_scope.has_value()) {
                    pending_holds.emplace_back(
                        scope_key_of(*erased.value().held_scope));
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
                          std::chrono::duration_cast<std::chrono::seconds>(
                              options_.purge_horizon))
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
                ErasureRequest request;
                if (auto parsed = MemoryId::parse(id); parsed) {
                    request.record = parsed;
                } else {
                    continue;
                }
                request.reason = "retention-expiry";
                request.include_artifacts = false;
                const auto erased = erase_locked(db, request);
                if (!erased) {
                    return erased.error();
                }
                if (erased.value().status == ErasureStatus::Complete) {
                    result.tombstones_purged += 1;
                } else if (erased.value().held_scope.has_value()) {
                    pending_holds.emplace_back(
                        scope_key_of(*erased.value().held_scope));
                }
            }
        }
        const auto commit = transaction.commit();
        if (!commit) {
            return commit.error();
        }
        for (const auto &hold_key : pending_holds) {
            record_pending_hold(db, hold_key, "retention-expiry");
        }
        return result;
    }

    SqliteMemoryStoreOptions options_;
    StoreDiagnostics diagnostics_;
    DatabaseHandle database_;
    std::unique_ptr<StoreChannel> channel_;
    IArtifactStore *artifacts_ = nullptr;
    RetrievalWeights weights_{};
    std::atomic<bool> read_only_{false};
};

SqliteMemoryStore::SqliteMemoryStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<SqliteMemoryStore>>
SqliteMemoryStore::open(executor::Executor &executor, SqliteMemoryStoreOptions options,
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
    auto database = storage::open_database(db_options);
    if (!database) {
        return database.error();
    }
    storage::DatabaseHandle handle(database.value());

    StoreDiagnostics diagnostics;
    diagnostics.reader_schema = kMemoryStoreSchema;
    diagnostics.read_only = options.read_only_diagnostic;

    const auto tables = storage::list_tables(handle.get());
    if (!tables) {
        return tables.error();
    }
    const bool fresh = tables.value().empty();
    if (!fresh && std::find(tables.value().begin(), tables.value().end(), "store_meta") ==
                      tables.value().end()) {
        return store_error(ErrorCode::InvalidState,
                           "store file holds an unrecognized schema; refusing to touch it");
    }
    std::string schema_text = std::to_string(kMemoryStoreSchema.major) + "." +
                              std::to_string(kMemoryStoreSchema.minor);
    if (fresh) {
        if (options.read_only_diagnostic) {
            diagnostics.disposition = StoreSchemaDisposition::Created;
            diagnostics.journal_mode = "readonly";
            diagnostics.note = "empty database opened read-only; no schema installed";
        } else {
            const auto install = storage::exec_sql(handle.get(), kSchemaDdl);
            if (!install) {
                return install.error();
            }
            const auto kind = storage::meta_write(handle.get(), "store_kind", kStoreKind);
            if (!kind) {
                return kind.error();
            }
            const auto version = storage::meta_write(handle.get(), "schema_version", schema_text);
            if (!version) {
                return version.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Created;
            diagnostics.file_schema = kMemoryStoreSchema;
        }
    } else {
        const auto kind = storage::meta_read(handle.get(), "store_kind");
        if (!kind) {
            return kind.error();
        }
        if (!kind.value().has_value() || *kind.value() != kStoreKind) {
            return store_error(ErrorCode::InvalidArgument,
                               "store file belongs to a different store kind");
        }
        const auto stored = storage::meta_read(handle.get(), "schema_version");
        if (!stored) {
            return stored.error();
        }
        if (!stored.value().has_value()) {
            return store_error(ErrorCode::InvalidState, "store schema version is missing");
        }
        std::uint64_t major = 0;
        std::uint64_t minor = 0;
        const std::string &text = *stored.value();
        const auto dot = text.find('.');
        if (dot == std::string::npos) {
            return store_error(ErrorCode::DataLoss, "store schema version is malformed");
        }
        try {
            major = std::stoull(text.substr(0, dot));
            minor = std::stoull(text.substr(dot + 1));
        } catch (const std::exception &) {
            return store_error(ErrorCode::DataLoss, "store schema version is malformed");
        }
        const SchemaVersion file_version{static_cast<std::uint16_t>(major),
                                         static_cast<std::uint16_t>(minor)};
        diagnostics.file_schema = file_version;
        if (file_version.major > kMemoryStoreSchema.major) {
            if (options.read_only_diagnostic) {
                diagnostics.disposition = StoreSchemaDisposition::ReadOnlyDiagnostic;
                diagnostics.note = "file schema is newer than this reader; writes are rejected";
            } else {
                return store_error(ErrorCode::UnsupportedVersion,
                                   "store schema is newer than this reader");
            }
        } else if (file_version == kMemoryStoreSchema) {
            diagnostics.disposition = StoreSchemaDisposition::UpToDate;
        } else {
            const auto supported = validate_schema_version(file_version, kMemoryStoreSchema);
            if (!supported) {
                return store_error(ErrorCode::UnsupportedVersion,
                                   "store schema is too old for this reader");
            }
            const auto bump = storage::meta_write(handle.get(), "schema_version", schema_text);
            if (!bump) {
                return bump.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Migrated;
        }
    }

    Statement journal(handle.get(), "PRAGMA journal_mode");
    if (journal.valid() && sqlite3_step(journal.get()) == SQLITE_ROW) {
        const auto *mode = reinterpret_cast<const char *>(sqlite3_column_text(journal.get(), 0));
        diagnostics.journal_mode = mode != nullptr ? mode : "unknown";
    }
    Statement pages(handle.get(), "PRAGMA page_count");
    if (pages.valid() && sqlite3_step(pages.get()) == SQLITE_ROW) {
        diagnostics.page_count = sqlite3_column_int64(pages.get(), 0);
    }

    StoreChannel::Config channel_config;
    channel_config.worker_name = "mira-memory-store";
    channel_config.max_pending = options.max_pending_requests;
    channel_config.operation_timeout = options.operation_timeout;
    auto channel = std::make_unique<StoreChannel>(executor, handle.get(), channel_config);

    auto impl = std::make_unique<Impl>(std::move(options), diagnostics, std::move(handle),
                                       std::move(channel), artifacts);
    impl->read_only_.store(
        options.read_only_diagnostic ||
        diagnostics.disposition == StoreSchemaDisposition::ReadOnlyDiagnostic);
    return std::unique_ptr<SqliteMemoryStore>(new SqliteMemoryStore(std::move(impl)));
}

SqliteMemoryStore::~SqliteMemoryStore() {
    if (impl_ != nullptr) {
        (void)impl_->channel_->close();
    }
}

Result<MemoryQueryResult> SqliteMemoryStore::query(const MemoryQuery &query) const {
    return impl_->channel_->run<MemoryQueryResult>(
        [this, &query](sqlite3 *db) { return impl_->query(db, query); });
}

Result<std::optional<MemoryRecord>> SqliteMemoryStore::get(MemoryId record) const {
    return impl_->channel_->run<std::optional<MemoryRecord>>(
        [record](sqlite3 *db) -> Result<std::optional<MemoryRecord>> {
            return Impl::read_record(db, record);
        });
}

Result<MemoryMutationResult> SqliteMemoryStore::apply(const MemoryMutation &mutation) {
    return impl_->channel_->run<MemoryMutationResult>(
        [this, &mutation](sqlite3 *db) { return impl_->apply(db, mutation); });
}

Result<MemoryCompactionResult> SqliteMemoryStore::compact(const MemoryScope &scope) {
    return impl_->channel_->run<MemoryCompactionResult>(
        [this, &scope](sqlite3 *db) { return impl_->compact(db, scope); });
}

Result<ErasureResult> SqliteMemoryStore::erase(const ErasureRequest &request) {
    return impl_->channel_->run<ErasureResult>(
        [this, &request](sqlite3 *db) { return impl_->erase(db, request); });
}

Result<void> SqliteMemoryStore::index_embedding(MemoryId record, std::span<const float> vector) {
    if (vector.empty() || vector.size() > 4'096) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "embedding dimensions must be within (0, 4096]");
    }
    return impl_->channel_->run<void>(
        [record, vector](sqlite3 *db) -> Result<void> {
            const std::string id = record.to_string();
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
            Statement upsert(db, "INSERT INTO memory_embeddings(record_id, dim, vector,"
                                 " embedded_at) VALUES(?1, ?2, ?3, ?4)"
                                 " ON CONFLICT(record_id) DO UPDATE SET dim = excluded.dim,"
                                 " vector = excluded.vector, embedded_at = excluded.embedded_at");
            if (!upsert.valid()) {
                return store_error(ErrorCode::Internal, "embedding upsert failed to prepare");
            }
            const auto bytes = static_cast<int>(vector.size() * sizeof(float));
            sqlite3_bind_text(upsert.get(), 1, id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(upsert.get(), 2, static_cast<std::int64_t>(vector.size()));
            sqlite3_bind_blob(upsert.get(), 3, vector.data(), bytes, storage::transient_copy());
            sqlite3_bind_int64(upsert.get(), 4, wall_nanos(std::chrono::system_clock::now()));
            if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
                return store_error(ErrorCode::Internal, "embedding upsert failed");
            }
            return transaction.commit();
        });
}

Result<std::size_t> SqliteMemoryStore::index_lag() const {
    return impl_->channel_->run<std::size_t>(
        [](sqlite3 *db) -> Result<std::size_t> {
            Statement lag(db, "SELECT COUNT(*) FROM memory_records r WHERE r.status = 'active'"
                              " AND NOT EXISTS(SELECT 1 FROM memory_embeddings e"
                              " WHERE e.record_id = r.id)");
            if (!lag.valid()) {
                return store_error(ErrorCode::Internal, "index lag failed to prepare");
            }
            if (sqlite3_step(lag.get()) != SQLITE_ROW) {
                return store_error(ErrorCode::Internal, "index lag failed");
            }
            return static_cast<std::size_t>(sqlite3_column_int64(lag.get(), 0));
        });
}

Result<std::size_t> SqliteMemoryStore::clear_embeddings() {
    const auto writable = impl_->check_writable();
    if (!writable) {
        return writable.error();
    }
    return impl_->channel_->run<std::size_t>(
        [](sqlite3 *db) -> Result<std::size_t> {
            Transaction transaction(db);
            if (!transaction.valid()) {
                return store_error(ErrorCode::Internal, "index reset transaction rejected");
            }
            Statement clear(db, "DELETE FROM memory_embeddings");
            if (!clear.valid()) {
                return store_error(ErrorCode::Internal, "index reset failed to prepare");
            }
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

std::size_t SqliteMemoryStore::pending_requests() const {
    return impl_->channel_->pending_count();
}

void SqliteMemoryStore::set_worker_paused(bool paused) {
    impl_->channel_->set_paused(paused);
}

Result<void> SqliteMemoryStore::close() noexcept {
    return impl_->channel_->close();
}

} // namespace mira
