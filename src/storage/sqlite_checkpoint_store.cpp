#include <mira/state_store.hpp>

#include "sqlite_support.hpp"

#include <mira/json.hpp>

#include <atomic>
#include <stdexcept>
#include <utility>

namespace mira {

namespace {

using storage::ChannelShared;
using storage::DatabaseHandle;
using storage::Statement;
using storage::StoreChannel;

[[nodiscard]] Error store_open_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.state_store.checkpoint";
    error.safe_message = std::move(message);
    return error;
}

constexpr SchemaVersion kCheckpointStoreSchema{1, 0};
constexpr const char *kStoreKind = "checkpoint";

constexpr const char *kSchemaDdl =
    "BEGIN;"
    "CREATE TABLE IF NOT EXISTS store_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS task_checkpoints("
    " task_id TEXT NOT NULL,"
    " checkpoint_id TEXT NOT NULL,"
    " sequence INTEGER NOT NULL,"
    " created_at INTEGER NOT NULL,"
    " schema_major INTEGER NOT NULL,"
    " schema_minor INTEGER NOT NULL,"
    " projection_digest TEXT NOT NULL,"
    " document TEXT NOT NULL,"
    " PRIMARY KEY(task_id, checkpoint_id));"
    "CREATE INDEX IF NOT EXISTS task_checkpoints_by_sequence"
    " ON task_checkpoints(task_id, sequence DESC);"
    "CREATE TABLE IF NOT EXISTS checkpoint_erasure_log("
    " task_id TEXT NOT NULL,"
    " reason TEXT NOT NULL,"
    " removed INTEGER NOT NULL,"
    " at INTEGER NOT NULL);"
    "COMMIT;";

[[nodiscard]] std::string schema_version_text(SchemaVersion version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor);
}

[[nodiscard]] std::optional<SchemaVersion> parse_schema_version_text(const std::string &text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) {
        return std::nullopt;
    }
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    try {
        major = std::stoull(text.substr(0, dot));
        minor = std::stoull(text.substr(dot + 1));
    } catch (const std::exception &) {
        return std::nullopt;
    }
    if (major > 0xFFFF || minor > 0xFFFF) {
        return std::nullopt;
    }
    return SchemaVersion{static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor)};
}

[[nodiscard]] std::int64_t wall_nanos(const Timestamp &stamp) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.wall.time_since_epoch())
            .count());
}

struct DecodeOutcome final {
    Result<std::optional<TaskCheckpoint>> value;
};

[[nodiscard]] DecodeOutcome decode_row(sqlite3_stmt *row) {
    const auto *document = reinterpret_cast<const char *>(sqlite3_column_text(row, 7));
    if (document == nullptr) {
        return {store_open_error(ErrorCode::DataLoss, "checkpoint row has no document")};
    }
    auto parsed = parse_json(document);
    if (!parsed) {
        return {store_open_error(ErrorCode::DataLoss, "checkpoint document is not valid JSON")};
    }
    auto checkpoint = checkpoint_from_json(parsed.value());
    if (!checkpoint) {
        return {store_open_error(ErrorCode::UnsupportedVersion,
                                 "checkpoint document schema is not supported by this reader")};
    }
    return {Result<std::optional<TaskCheckpoint>>(std::move(checkpoint).value())};
}

} // namespace

std::string store_schema_disposition_name(StoreSchemaDisposition disposition) {
    switch (disposition) {
    case StoreSchemaDisposition::Created:
        return "Created";
    case StoreSchemaDisposition::UpToDate:
        return "UpToDate";
    case StoreSchemaDisposition::Migrated:
        return "Migrated";
    case StoreSchemaDisposition::ReadOnlyDiagnostic:
        return "ReadOnlyDiagnostic";
    }
    return "Unknown";
}

Result<void> SqliteStoreOptions::validate() const {
    if (path.empty()) {
        return store_open_error(ErrorCode::InvalidArgument, "store path must not be empty");
    }
    if (max_pending_requests == 0 || max_checkpoints_per_task == 0) {
        return store_open_error(ErrorCode::InvalidArgument,
                                "store bounds must be greater than zero");
    }
    if (operation_timeout.count() <= 0 || busy_timeout.count() < 0) {
        return store_open_error(ErrorCode::InvalidArgument, "store timeouts must be positive");
    }
    return Result<void>{};
}

JsonValue store_diagnostics_to_json(const StoreDiagnostics &diagnostics) {
    JsonValue::Object object;
    object.emplace_back(
        "disposition", store_schema_disposition_name(diagnostics.disposition));
    object.emplace_back("file_schema",
                        JsonValue::Object{
                            {"major", static_cast<std::int64_t>(diagnostics.file_schema.major)},
                            {"minor", static_cast<std::int64_t>(diagnostics.file_schema.minor)}});
    object.emplace_back("reader_schema",
                        JsonValue::Object{
                            {"major", static_cast<std::int64_t>(diagnostics.reader_schema.major)},
                            {"minor", static_cast<std::int64_t>(diagnostics.reader_schema.minor)}});
    object.emplace_back("read_only", diagnostics.read_only);
    object.emplace_back("journal_mode", diagnostics.journal_mode);
    object.emplace_back("page_count", diagnostics.page_count);
    object.emplace_back("note", diagnostics.note);
    return JsonValue(std::move(object));
}

class SqliteCheckpointStore::Impl final {
  public:
    Impl(SqliteStoreOptions options, StoreDiagnostics diagnostics, DatabaseHandle database,
         std::unique_ptr<StoreChannel> channel)
        : options_(std::move(options)), diagnostics_(std::move(diagnostics)),
          database_(std::move(database)), channel_(std::move(channel)) {}

    [[nodiscard]] Result<void> check_writable() const {
        if (read_only_) {
            return store_open_error(ErrorCode::InvalidState,
                                    "store is open in read-only diagnostic mode");
        }
        return Result<void>{};
    }

    [[nodiscard]] Result<void> put(const TaskCheckpoint &checkpoint) {
        const auto writable = check_writable();
        if (!writable) {
            return writable;
        }
        const auto valid = checkpoint.validate();
        if (!valid) {
            return valid;
        }
        const std::string task = checkpoint.task_id.to_string();
        const std::string id = checkpoint.id.to_string();
        const std::string digest = checkpoint.projection_digest().to_string();
        const std::string document = to_json_string(checkpoint_to_json(checkpoint));
        return channel_->run<void>([&](sqlite3 *db) -> Result<void> {
            storage::Transaction transaction(db);
            if (!transaction.valid()) {
                return store_open_error(ErrorCode::Internal, "checkpoint transaction rejected");
            }
            Statement insert(
                db, "INSERT INTO task_checkpoints(task_id, checkpoint_id, sequence, created_at,"
                    " schema_major, schema_minor, projection_digest, document)"
                    " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
                    " ON CONFLICT(task_id, checkpoint_id) DO UPDATE SET"
                    " sequence = excluded.sequence, created_at = excluded.created_at,"
                    " schema_major = excluded.schema_major, schema_minor = excluded.schema_minor,"
                    " projection_digest = excluded.projection_digest,"
                    " document = excluded.document");
            if (!insert.valid()) {
                return store_open_error(ErrorCode::Internal, "checkpoint insert failed to prepare");
            }
            sqlite3_bind_text(insert.get(), 1, task.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(insert.get(), 2, id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(insert.get(), 3,
                               static_cast<std::int64_t>(checkpoint.through_event_sequence));
            sqlite3_bind_int64(insert.get(), 4, wall_nanos(checkpoint.created_at));
            sqlite3_bind_int64(insert.get(), 5, checkpoint.schema_version.major);
            sqlite3_bind_int64(insert.get(), 6, checkpoint.schema_version.minor);
            sqlite3_bind_text(insert.get(), 7, digest.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(insert.get(), 8, document.c_str(), -1, storage::transient_copy());
            const int step = sqlite3_step(insert.get());
            if (step != SQLITE_DONE) {
                return store_open_error(ErrorCode::Internal, "checkpoint insert failed");
            }
            Statement prune(
                db, "DELETE FROM task_checkpoints WHERE task_id = ?1 AND checkpoint_id NOT IN"
                    " (SELECT checkpoint_id FROM task_checkpoints WHERE task_id = ?1"
                    " ORDER BY sequence DESC, created_at DESC LIMIT ?2)");
            if (!prune.valid()) {
                return store_open_error(ErrorCode::Internal, "checkpoint prune failed to prepare");
            }
            sqlite3_bind_text(prune.get(), 1, task.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(prune.get(), 2,
                               static_cast<std::int64_t>(options_.max_checkpoints_per_task));
            const int prune_step = sqlite3_step(prune.get());
            if (prune_step != SQLITE_DONE) {
                return store_open_error(ErrorCode::Internal, "checkpoint prune failed");
            }
            return transaction.commit();
        });
    }

    [[nodiscard]] Result<std::optional<TaskCheckpoint>>
    select_latest(sqlite3 *db, const std::string &task, const std::optional<std::uint64_t> cap) {
        // UINT64_MAX means "no sequence bound"; binding it would exclude
        // every sequence once narrowed to int64.
        const std::optional<std::uint64_t> bounded =
            cap.has_value() && *cap < UINT64_MAX ? cap : std::optional<std::uint64_t>{};
        Statement query(
            db, bounded.has_value()
                    ? "SELECT task_id, checkpoint_id, sequence, created_at, schema_major,"
                      " schema_minor, projection_digest, document FROM task_checkpoints"
                      " WHERE task_id = ?1 AND sequence <= ?2"
                      " ORDER BY sequence DESC, created_at DESC LIMIT 1"
                : "SELECT task_id, checkpoint_id, sequence, created_at, schema_major,"
                  " schema_minor, projection_digest, document FROM task_checkpoints"
                  " WHERE task_id = ?1 ORDER BY sequence DESC, created_at DESC LIMIT 1");
        if (!query.valid()) {
            return store_open_error(ErrorCode::Internal, "checkpoint query failed to prepare");
        }
        sqlite3_bind_text(query.get(), 1, task.c_str(), -1, storage::transient_copy());
        if (bounded.has_value()) {
            sqlite3_bind_int64(query.get(), 2, static_cast<std::int64_t>(*bounded));
        }
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) {
            return std::optional<TaskCheckpoint>{};
        }
        if (step != SQLITE_ROW) {
            return store_open_error(ErrorCode::Internal, "checkpoint query failed");
        }
        return decode_row(query.get()).value;
    }

    SqliteStoreOptions options_;
    StoreDiagnostics diagnostics_;
    DatabaseHandle database_;
    std::unique_ptr<StoreChannel> channel_;
    std::atomic<bool> read_only_{false};
};

SqliteCheckpointStore::SqliteCheckpointStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

Result<std::unique_ptr<SqliteCheckpointStore>>
SqliteCheckpointStore::open(executor::Executor &executor, SqliteStoreOptions options) {
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
    diagnostics.reader_schema = kCheckpointStoreSchema;
    diagnostics.read_only = options.read_only_diagnostic;

    const auto tables = storage::list_tables(handle.get());
    if (!tables) {
        return tables.error();
    }
    const bool fresh = tables.value().empty();
    if (!fresh && std::find(tables.value().begin(), tables.value().end(), "store_meta") ==
                      tables.value().end()) {
        return store_open_error(
            ErrorCode::InvalidState,
            "store file holds an unrecognized schema; refusing to touch it "
            "(reopen with read_only_diagnostic to inspect)");
    }

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
            const auto version = storage::meta_write(
                handle.get(), "schema_version", schema_version_text(kCheckpointStoreSchema));
            if (!version) {
                return version.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Created;
            diagnostics.file_schema = kCheckpointStoreSchema;
        }
    } else {
        const auto kind = storage::meta_read(handle.get(), "store_kind");
        if (!kind) {
            return kind.error();
        }
        if (kind.value().has_value() && *kind.value() != kStoreKind) {
            return store_open_error(ErrorCode::InvalidArgument,
                                    "store file belongs to a different store kind");
        }
        if (!kind.value().has_value()) {
            // Pre-meta database from an earlier development build: recognized
            // tables exist but provenance is unknown; fail closed.
            return store_open_error(ErrorCode::InvalidState,
                                    "store file predates store metadata; refusing migration");
        }
        const auto stored = storage::meta_read(handle.get(), "schema_version");
        if (!stored) {
            return stored.error();
        }
        if (!stored.value().has_value()) {
            return store_open_error(ErrorCode::InvalidState, "store schema version is missing");
        }
        const auto parsed = parse_schema_version_text(*stored.value());
        if (!parsed) {
            return store_open_error(ErrorCode::DataLoss,
                                    "store schema version is malformed; inspect read-only");
        }
        diagnostics.file_schema = *parsed;
        if (parsed->major > kCheckpointStoreSchema.major) {
            if (options.read_only_diagnostic) {
                diagnostics.disposition = StoreSchemaDisposition::ReadOnlyDiagnostic;
                diagnostics.note = "file schema is newer than this reader; writes are rejected";
            } else {
                return store_open_error(
                    ErrorCode::UnsupportedVersion,
                    "store schema is newer than this reader; reopen read_only_diagnostic");
            }
        } else if (*parsed == kCheckpointStoreSchema) {
            diagnostics.disposition = StoreSchemaDisposition::UpToDate;
        } else {
            // Explicit migration step from an older compatible schema. There
            // is no older released schema today; the branch documents where
            // future migrations run and refuses unknown older minors only when
            // the major regressed more than one generation.
            const auto supported = validate_schema_version(*parsed, kCheckpointStoreSchema);
            if (!supported) {
                return store_open_error(ErrorCode::UnsupportedVersion,
                                        "store schema is too old for this reader");
            }
            const auto bump = storage::meta_write(
                handle.get(), "schema_version", schema_version_text(kCheckpointStoreSchema));
            if (!bump) {
                return bump.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Migrated;
        }
    }

    // Runtime diagnostics from the live connection.
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
    channel_config.worker_name = "mira-checkpoint-store";
    channel_config.max_pending = options.max_pending_requests;
    channel_config.operation_timeout = options.operation_timeout;
    auto channel = std::make_unique<StoreChannel>(executor, handle.get(), channel_config);

    auto impl = std::make_unique<Impl>(std::move(options), diagnostics, std::move(handle),
                                       std::move(channel));
    impl->read_only_.store(options.read_only_diagnostic ||
                           diagnostics.disposition == StoreSchemaDisposition::ReadOnlyDiagnostic);
    return std::unique_ptr<SqliteCheckpointStore>(new SqliteCheckpointStore(std::move(impl)));
}

SqliteCheckpointStore::~SqliteCheckpointStore() {
    if (impl_ != nullptr) {
        (void)impl_->channel_->close();
    }
}

Result<void> SqliteCheckpointStore::put(const TaskCheckpoint &checkpoint) {
    return impl_->put(checkpoint);
}

Result<std::optional<TaskCheckpoint>> SqliteCheckpointStore::latest(TaskId task) const {
    return impl_->channel_->run<std::optional<TaskCheckpoint>>(
        [this, task](sqlite3 *db) -> Result<std::optional<TaskCheckpoint>> {
            return impl_->select_latest(db, task.to_string(), std::nullopt);
        });
}

Result<std::optional<TaskCheckpoint>>
SqliteCheckpointStore::latest_at_or_before(TaskId task, std::uint64_t max_sequence) const {
    return impl_->channel_->run<std::optional<TaskCheckpoint>>(
        [this, task, max_sequence](sqlite3 *db) -> Result<std::optional<TaskCheckpoint>> {
            return impl_->select_latest(db, task.to_string(), max_sequence);
        });
}

Result<std::size_t> SqliteCheckpointStore::count(TaskId task) const {
    return impl_->channel_->run<std::size_t>(
        [task](sqlite3 *db) -> Result<std::size_t> {
            Statement query(db, "SELECT COUNT(*) FROM task_checkpoints WHERE task_id = ?1");
            if (!query.valid()) {
                return store_open_error(ErrorCode::Internal, "checkpoint count failed to prepare");
            }
            const std::string id = task.to_string();
            sqlite3_bind_text(query.get(), 1, id.c_str(), -1, storage::transient_copy());
            const int step = sqlite3_step(query.get());
            if (step != SQLITE_ROW) {
                return store_open_error(ErrorCode::Internal, "checkpoint count failed");
            }
            return static_cast<std::size_t>(sqlite3_column_int64(query.get(), 0));
        });
}

Result<std::size_t> SqliteCheckpointStore::erase_task(TaskId task, std::string reason) {
    const auto writable = impl_->check_writable();
    if (!writable) {
        return writable.error();
    }
    return impl_->channel_->run<std::size_t>(
        [task, reason = std::move(reason)](sqlite3 *db) -> Result<std::size_t> {
            storage::Transaction transaction(db);
            if (!transaction.valid()) {
                return store_open_error(ErrorCode::Internal, "erasure transaction rejected");
            }
            Statement count(db, "SELECT COUNT(*) FROM task_checkpoints WHERE task_id = ?1");
            if (!count.valid()) {
                return store_open_error(ErrorCode::Internal, "erasure count failed to prepare");
            }
            const std::string id = task.to_string();
            sqlite3_bind_text(count.get(), 1, id.c_str(), -1, storage::transient_copy());
            if (sqlite3_step(count.get()) != SQLITE_ROW) {
                return store_open_error(ErrorCode::Internal, "erasure count failed");
            }
            const auto removed = static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
            Statement erase(db, "DELETE FROM task_checkpoints WHERE task_id = ?1");
            if (!erase.valid()) {
                return store_open_error(ErrorCode::Internal, "erasure failed to prepare");
            }
            sqlite3_bind_text(erase.get(), 1, id.c_str(), -1, storage::transient_copy());
            if (sqlite3_step(erase.get()) != SQLITE_DONE) {
                return store_open_error(ErrorCode::Internal, "erasure failed");
            }
            Statement log(db, "INSERT INTO checkpoint_erasure_log(task_id, reason, removed, at)"
                              " VALUES(?1, ?2, ?3, ?4)");
            if (!log.valid()) {
                return store_open_error(ErrorCode::Internal, "erasure log failed to prepare");
            }
            const std::int64_t now = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            sqlite3_bind_text(log.get(), 1, id.c_str(), -1, storage::transient_copy());
            sqlite3_bind_text(log.get(), 2, reason.c_str(), -1, storage::transient_copy());
            sqlite3_bind_int64(log.get(), 3, static_cast<std::int64_t>(removed));
            sqlite3_bind_int64(log.get(), 4, now);
            if (sqlite3_step(log.get()) != SQLITE_DONE) {
                return store_open_error(ErrorCode::Internal, "erasure log failed");
            }
            const auto commit = transaction.commit();
            if (!commit) {
                return commit.error();
            }
            return removed;
        });
}

const StoreDiagnostics &SqliteCheckpointStore::diagnostics() const noexcept {
    return impl_->diagnostics_;
}

std::size_t SqliteCheckpointStore::pending_requests() const {
    return impl_->channel_->pending_count();
}

void SqliteCheckpointStore::set_worker_paused(bool paused) {
    impl_->channel_->set_paused(paused);
}

Result<void> SqliteCheckpointStore::close() noexcept {
    return impl_->channel_->close();
}

} // namespace mira
