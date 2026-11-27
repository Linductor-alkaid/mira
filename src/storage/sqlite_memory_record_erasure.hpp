#pragma once

#include "sqlite_support.hpp"

#include <mira/memory_contracts.hpp>

#include <limits>

namespace mira::storage::record_erasure {

inline constexpr const char *schema =
    "CREATE TABLE IF NOT EXISTS memory_record_erasure_ops("
    " operation_id TEXT PRIMARY KEY, scope_key TEXT NOT NULL, record_id TEXT NOT NULL UNIQUE,"
    " target_version INTEGER NOT NULL, complete INTEGER NOT NULL DEFAULT 0,"
    " records INTEGER NOT NULL DEFAULT 0, versions INTEGER NOT NULL DEFAULT 0,"
    " fts INTEGER NOT NULL DEFAULT 0, embeddings INTEGER NOT NULL DEFAULT 0,"
    " artifacts INTEGER NOT NULL DEFAULT 0);"
    "CREATE UNIQUE INDEX IF NOT EXISTS memory_record_erasure_pending_scope"
    " ON memory_record_erasure_ops(scope_key) WHERE complete = 0;";

inline constexpr const char *pending_scope_query =
    "SELECT scope_key FROM memory_record_erasure_ops WHERE scope_key = ?1 AND complete = 0";

inline Error failure(const char *message) {
    return make_memory_error(MemoryDomainCode::StoreUnavailable, message);
}

inline Error unavailable_record() {
    return make_memory_error(MemoryDomainCode::ScopeDenied, "record erasure target unavailable");
}

inline Error pending_scope() {
    return make_memory_error(MemoryDomainCode::ErasurePending, "scope has a pending erasure");
}

inline Result<void> validate(const MemoryRecordErasureRequest &request) {
    const auto &scope = request.scope;
    if (request.operation.is_nil() || request.record.is_nil() || request.expected_version == 0 ||
        request.expected_version >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "invalid record erasure identity");
    const auto exact_component = [](const std::string &value) {
        return !value.empty() && value.find('|') == std::string::npos &&
               value.find('\0') == std::string::npos;
    };
    if ((scope.kind != MemoryScopeKind::User && scope.kind != MemoryScopeKind::Session) ||
        !exact_component(scope.subject_id) || !scope.tenant_id ||
        !exact_component(*scope.tenant_id))
        return unavailable_record();
    MemoryQuery check;
    check.scopes = {scope};
    return check.validate();
}

inline Result<void> check_version(sqlite3 *db, const MemoryRecordErasureRequest &request,
                                  const std::string &scope_key) {
    Statement read(db, "SELECT version FROM memory_records WHERE id = ?1 AND scope_key = ?2");
    if (!read.valid())
        return failure("record erasure target read failed");
    const auto id = request.record.to_string();
    sqlite3_bind_text(read.get(), 1, id.c_str(), -1, transient_copy());
    sqlite3_bind_text(read.get(), 2, scope_key.c_str(), -1, transient_copy());
    const auto step = sqlite3_step(read.get());
    if (step == SQLITE_DONE)
        return unavailable_record();
    if (step != SQLITE_ROW)
        return failure("record erasure target read failed");
    if (sqlite3_column_int64(read.get(), 0) != static_cast<std::int64_t>(request.expected_version))
        return make_memory_error(MemoryDomainCode::VersionConflict,
                                 "record erasure version changed");
    return {};
}

inline Result<std::optional<MemoryRecordErasureReceipt>>
lookup(sqlite3 *db, const MemoryRecordErasureRequest &request, const std::string &scope_key) {
    Statement read(
        db, "SELECT scope_key, record_id, target_version, complete, records, versions,"
            " fts, embeddings, artifacts FROM memory_record_erasure_ops WHERE operation_id = ?1");
    if (!read.valid())
        return failure("record erasure receipt read failed");
    const auto operation = request.operation.to_string();
    sqlite3_bind_text(read.get(), 1, operation.c_str(), -1, transient_copy());
    const auto step = sqlite3_step(read.get());
    if (step == SQLITE_DONE)
        return std::optional<MemoryRecordErasureReceipt>{};
    if (step != SQLITE_ROW)
        return failure("record erasure receipt read failed");
    const auto *scope = reinterpret_cast<const char *>(sqlite3_column_text(read.get(), 0));
    const auto *record = reinterpret_cast<const char *>(sqlite3_column_text(read.get(), 1));
    if (!scope || scope_key != scope || !record || request.record.to_string() != record ||
        sqlite3_column_int64(read.get(), 2) != static_cast<std::int64_t>(request.expected_version))
        return unavailable_record();
    ErasureResult erasure;
    const bool complete = sqlite3_column_int64(read.get(), 3) == 1;
    erasure.status = complete ? ErasureStatus::Complete : ErasureStatus::Pending;
    erasure.counts = {static_cast<std::size_t>(sqlite3_column_int64(read.get(), 4)),
                      static_cast<std::size_t>(sqlite3_column_int64(read.get(), 5)),
                      static_cast<std::size_t>(sqlite3_column_int64(read.get(), 6)),
                      static_cast<std::size_t>(sqlite3_column_int64(read.get(), 7)),
                      static_cast<std::size_t>(sqlite3_column_int64(read.get(), 8))};
    if (!complete)
        erasure.held_scope = request.scope;
    return std::optional{MemoryRecordErasureReceipt{request, std::move(erasure), complete}};
}

inline Result<void> insert_intent(sqlite3 *db, const MemoryRecordErasureRequest &request,
                                  const std::string &scope_key) {
    Statement write(db, "INSERT INTO memory_record_erasure_ops(operation_id, scope_key, record_id,"
                        " target_version) VALUES (?1, ?2, ?3, ?4)");
    if (!write.valid())
        return failure("record erasure intent write failed");
    const auto operation = request.operation.to_string();
    const auto record = request.record.to_string();
    sqlite3_bind_text(write.get(), 1, operation.c_str(), -1, transient_copy());
    sqlite3_bind_text(write.get(), 2, scope_key.c_str(), -1, transient_copy());
    sqlite3_bind_text(write.get(), 3, record.c_str(), -1, transient_copy());
    sqlite3_bind_int64(write.get(), 4, static_cast<std::int64_t>(request.expected_version));
    if (sqlite3_step(write.get()) != SQLITE_DONE)
        return failure("record erasure intent write failed");
    return {};
}

inline Result<void> complete(sqlite3 *db, OperationId operation, const ErasureCounts &counts) {
    Statement write(db, "UPDATE memory_record_erasure_ops SET complete = 1, records = ?2,"
                        " versions = ?3, fts = ?4, embeddings = ?5, artifacts = ?6"
                        " WHERE operation_id = ?1 AND complete = 0");
    if (!write.valid())
        return failure("record erasure receipt write failed");
    const auto id = operation.to_string();
    sqlite3_bind_text(write.get(), 1, id.c_str(), -1, transient_copy());
    sqlite3_bind_int64(write.get(), 2, static_cast<std::int64_t>(counts.records_removed));
    sqlite3_bind_int64(write.get(), 3, static_cast<std::int64_t>(counts.versions_removed));
    sqlite3_bind_int64(write.get(), 4, static_cast<std::int64_t>(counts.fts_entries_removed));
    sqlite3_bind_int64(write.get(), 5, static_cast<std::int64_t>(counts.embeddings_removed));
    sqlite3_bind_int64(write.get(), 6, static_cast<std::int64_t>(counts.artifacts_erased));
    if (sqlite3_step(write.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return failure("record erasure receipt write failed");
    return {};
}

inline Result<bool> has_entry(sqlite3 *db, const char *sql, const std::string &key) {
    Statement read(db, sql);
    if (!read.valid())
        return failure("record erasure status read failed");
    sqlite3_bind_text(read.get(), 1, key.c_str(), -1, transient_copy());
    const auto step = sqlite3_step(read.get());
    if (step == SQLITE_ROW || step == SQLITE_DONE)
        return step == SQLITE_ROW;
    return failure("record erasure status read failed");
}

inline Result<void> check_not_pending(sqlite3 *db, const std::string &scope_key) {
    const auto held = has_entry(db, pending_scope_query, scope_key);
    if (!held)
        return held.error();
    if (held.value())
        return pending_scope();
    return {};
}

} // namespace mira::storage::record_erasure
