#pragma once

#include "sqlite_support.hpp"

#include <mira/sqlite_goal_store.hpp>

#include <array>
#include <limits>
#include <utility>

namespace mira::storage {

inline constexpr const char *kGoalDispatchSchema =
    "CREATE TABLE goal_dispatches(tenant TEXT NOT NULL,owner TEXT NOT NULL,goal_id TEXT NOT NULL,"
    " revision INTEGER NOT NULL CHECK(revision>0),operation TEXT NOT NULL UNIQUE,"
    " prepared_at INTEGER NOT NULL CHECK(prepared_at>0), session TEXT,task TEXT,submitted_at "
    "INTEGER,"
    " CHECK((session IS NULL)=(task IS NULL) AND (session IS NULL)=(submitted_at IS NULL)),"
    " CHECK(submitted_at IS NULL OR submitted_at>=prepared_at),"
    " PRIMARY KEY(tenant,owner,goal_id));";

inline constexpr const char *kRecurringDispatchSchema =
    "ALTER TABLE recurring_occurrences ADD COLUMN dispatch_operation TEXT;"
    "ALTER TABLE recurring_occurrences ADD COLUMN dispatch_session TEXT;"
    "ALTER TABLE recurring_occurrences ADD COLUMN dispatch_task TEXT;"
    "ALTER TABLE recurring_occurrences ADD COLUMN dispatched_at INTEGER;"
    "CREATE UNIQUE INDEX recurring_occurrences_operation ON recurring_occurrences"
    " (dispatch_operation) WHERE dispatch_operation IS NOT NULL;";

inline constexpr auto kMaxSqlInteger = std::numeric_limits<std::int64_t>::max();

inline Error goal_error(ErrorCode code, std::string message) {
    return store_error(code, "mira.goal_store", 0, std::move(message));
}

inline bool valid_owner(const GoalOwner &owner) {
    return !owner.tenant.is_nil() && !owner.user.is_nil();
}

inline bool valid_key(const GoalKey &key) { return valid_owner(key.owner) && !key.id.is_nil(); }

inline bool valid_time(GoalTime time) { return time.time_since_epoch().count() > 0; }

inline bool bind_text(Statement &s, int position, const std::string &text) {
    return sqlite3_bind_text(s.get(), position, text.data(), static_cast<int>(text.size()),
                             transient_copy()) == SQLITE_OK;
}

inline bool bind_owner(Statement &s, const GoalOwner &owner) {
    return bind_text(s, 1, owner.tenant.to_string()) && bind_text(s, 2, owner.user.to_string());
}

inline bool bind_key(Statement &s, const GoalKey &key) {
    return bind_owner(s, key.owner) && bind_text(s, 3, key.id.to_string());
}

inline bool bind_time(Statement &s, int position, std::optional<GoalTime> time) {
    return (time ? sqlite3_bind_int64(s.get(), position, time->time_since_epoch().count())
                 : sqlite3_bind_null(s.get(), position)) == SQLITE_OK;
}

inline std::string column_text(Statement &s, int index) {
    const auto *text = sqlite3_column_text(s.get(), index);
    return text ? std::string(reinterpret_cast<const char *>(text),
                              static_cast<std::size_t>(sqlite3_column_bytes(s.get(), index)))
                : std::string{};
}

inline std::optional<GoalTime> column_time(Statement &s, int index) {
    if (sqlite3_column_type(s.get(), index) != SQLITE_INTEGER)
        return std::nullopt;
    return GoalTime{std::chrono::milliseconds{sqlite3_column_int64(s.get(), index)}};
}

inline bool bind_revision(Statement &statement, int parameter, std::uint64_t revision) {
    return sqlite3_bind_int64(statement.get(), parameter, static_cast<std::int64_t>(revision)) ==
           SQLITE_OK;
}

inline Result<void> require_goal_capacity(sqlite3 *db, const char *sql, std::size_t limit,
                                          const GoalOwner *owner = nullptr) {
    Statement count(db, sql);
    if (!count.valid() || (owner && !bind_owner(count, *owner)) ||
        sqlite3_step(count.get()) != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "goal capacity check failed");
    if (sqlite3_column_int64(count.get(), 0) >= static_cast<std::int64_t>(limit))
        return goal_error(ErrorCode::ResourceExhausted, "goal store quota reached");
    return {};
}

Result<std::optional<RecurringPlanRecord>> read_goal_plan(sqlite3 *db, const GoalKey &key);

} // namespace mira::storage

namespace mira {

class SqliteGoalStore::Impl final {
  public:
    Impl(kairo::Executor &executor, storage::DatabaseHandle handle, SqliteGoalStoreOptions config,
         GoalStoreAuthorizer authorize)
        : options(std::move(config)), authorizer(std::move(authorize)), database(std::move(handle)),
          channel(executor, database.get(),
                  {"mira-goal-store", options.max_pending_requests, options.operation_timeout}) {}

    template <typename T, typename Op> Result<T> run(GoalStoreAccess access, Op op) {
        return run_for_accesses<T>(std::array{access}, std::move(op));
    }

    template <typename T, typename Accesses, typename Op>
    Result<T> run_for_accesses(Accesses accesses, Op op) {
        return channel.run<T>([this, accesses = std::move(accesses),
                               op = std::move(op)](sqlite3 *db) mutable -> Result<T> {
            for (const auto &access : accesses) {
                if (!allows(access))
                    return storage::goal_error(ErrorCode::PermissionDenied,
                                               "goal store access denied");
            }
            return op(db);
        });
    }

    bool allows(const GoalStoreAccess &access) const noexcept {
        try {
            return !authorizer || authorizer(access);
        } catch (...) {
            return false;
        }
    }

    const SqliteGoalStoreOptions options;
    const GoalStoreAuthorizer authorizer;
    storage::DatabaseHandle database;
    storage::StoreChannel channel;
};

} // namespace mira
