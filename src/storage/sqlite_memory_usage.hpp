#pragma once

#include "sqlite_support.hpp"

#include <mira/sqlite_memory_store.hpp>

#include <limits>

namespace mira::storage::memory_usage {

// DEC-187. The records table is the owner even when erased by retention or
// a social cascade. Triggers keep every deletion path in the same transaction.
inline constexpr const char *schema =
    "CREATE TABLE IF NOT EXISTS memory_use_requests("
    " record_id TEXT NOT NULL, version INTEGER NOT NULL, request_id TEXT NOT NULL,"
    " PRIMARY KEY(record_id, version, request_id)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS memory_usage("
    " record_id TEXT PRIMARY KEY, version INTEGER NOT NULL,"
    " use_count INTEGER NOT NULL CHECK(typeof(use_count) = 'integer' AND use_count > 0),"
    " last_used INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TRIGGER IF NOT EXISTS memory_usage_on_update"
    " AFTER UPDATE OF version, status ON memory_records"
    " WHEN NEW.version <> OLD.version OR NEW.status <> 'active' BEGIN"
    " DELETE FROM memory_use_requests WHERE record_id = OLD.id;"
    " DELETE FROM memory_usage WHERE record_id = OLD.id; END;"
    "CREATE TRIGGER IF NOT EXISTS memory_usage_on_delete AFTER DELETE ON memory_records BEGIN"
    " DELETE FROM memory_use_requests WHERE record_id = OLD.id;"
    " DELETE FROM memory_usage WHERE record_id = OLD.id; END;";

struct CurrentVersion final {
    MemoryScope scope;
    std::int64_t written_at;
};

[[nodiscard]] inline bool valid_version(std::uint64_t version) {
    return version > 0 &&
           version <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] inline Result<std::optional<CurrentVersion>>
current_version(sqlite3 *db, const std::string &id, std::uint64_t version, std::int64_t now) {
    Statement stmt(db, "SELECT scope_kind, subject_id, tenant_id, updated_at FROM memory_records"
                       " WHERE id = ?1 AND version = ?2 AND status = 'active'"
                       " AND (expires_at IS NULL OR expires_at > ?3)"
                       " AND valid_from <= ?3 AND (valid_until IS NULL OR valid_until > ?3)");
    if (!stmt.valid() ||
        sqlite3_bind_text(stmt.get(), 1, id.c_str(), -1, transient_copy()) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 2, static_cast<std::int64_t>(version)) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 3, now) != SQLITE_OK)
        return channel_error("memory usage version lookup failed to prepare", ErrorCode::Internal);
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE)
        return std::optional<CurrentVersion>{};
    if (step != SQLITE_ROW)
        return channel_error("memory usage version lookup failed", ErrorCode::Internal);
    const auto *kind_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
    const auto *subject = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));
    const auto *tenant = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 2));
    const auto kind = kind_text ? memory_scope_kind_from(kind_text) : std::nullopt;
    if (!kind || !subject)
        return channel_error("memory usage record scope is invalid", ErrorCode::DataLoss);
    return std::optional{
        CurrentVersion{{*kind, subject, tenant ? std::optional<std::string>{tenant} : std::nullopt},
                       sqlite3_column_int64(stmt.get(), 3)}};
}

[[nodiscard]] inline Result<bool> insert(sqlite3 *db, const std::string &id, std::uint64_t version,
                                         const std::string &request, std::int64_t used_at) {
    const auto stored_version = static_cast<std::int64_t>(version);
    Statement seen(db, "INSERT INTO memory_use_requests(record_id, version, request_id)"
                       " VALUES(?1, ?2, ?3) ON CONFLICT DO NOTHING");
    if (!seen.valid() ||
        sqlite3_bind_text(seen.get(), 1, id.c_str(), -1, transient_copy()) != SQLITE_OK ||
        sqlite3_bind_int64(seen.get(), 2, stored_version) != SQLITE_OK ||
        sqlite3_bind_text(seen.get(), 3, request.c_str(), -1, transient_copy()) != SQLITE_OK ||
        sqlite3_step(seen.get()) != SQLITE_DONE)
        return channel_error("memory use deduplication failed", ErrorCode::Internal);
    if (sqlite3_changes(db) == 0)
        return false;
    Statement save(db, "INSERT INTO memory_usage(record_id, version, use_count, last_used)"
                       " VALUES(?1, ?2, 1, ?3) ON CONFLICT(record_id) DO UPDATE SET"
                       " use_count = use_count + 1, last_used = max(last_used, excluded.last_used)"
                       " WHERE version = excluded.version");
    if (!save.valid() ||
        sqlite3_bind_text(save.get(), 1, id.c_str(), -1, transient_copy()) != SQLITE_OK ||
        sqlite3_bind_int64(save.get(), 2, stored_version) != SQLITE_OK ||
        sqlite3_bind_int64(save.get(), 3, used_at) != SQLITE_OK ||
        sqlite3_step(save.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return channel_error("memory usage tally failed", ErrorCode::Internal);
    return true;
}

[[nodiscard]] inline Result<MemoryUsage> read(sqlite3 *db, const std::string &id,
                                              std::uint64_t version) {
    Statement stmt(db, "SELECT use_count, last_used FROM memory_usage"
                       " WHERE record_id = ?1 AND version = ?2");
    if (!stmt.valid() ||
        sqlite3_bind_text(stmt.get(), 1, id.c_str(), -1, transient_copy()) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 2, static_cast<std::int64_t>(version)) != SQLITE_OK)
        return channel_error("memory usage read failed to prepare", ErrorCode::Internal);
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE)
        return MemoryUsage{};
    if (step != SQLITE_ROW)
        return channel_error("memory usage read failed", ErrorCode::Internal);
    return MemoryUsage{
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0)),
        WallTimePoint{std::chrono::nanoseconds{sqlite3_column_int64(stmt.get(), 1)}}};
}

} // namespace mira::storage::memory_usage
