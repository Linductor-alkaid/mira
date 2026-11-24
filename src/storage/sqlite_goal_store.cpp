#include <mira/sqlite_goal_store.hpp>

#include "sqlite_store_open.hpp"

#include <mira/content_markers.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace mira {
namespace {
using storage::Statement;
using storage::Transaction;

constexpr const char *kColumns =
    "id,state,revision,kind,title,source_event,created_at,due_at,"
    "expires_at,resolution_event,resolved_at,(SELECT count(*) FROM goal_reminders r"
    " WHERE r.tenant=goals.tenant AND r.owner=goals.owner AND r.goal_id=goals.id)";
constexpr const char *kGoalTable =
    "CREATE TABLE goals(tenant TEXT NOT NULL,owner TEXT NOT NULL,id TEXT NOT NULL,"
    " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
    " revision INTEGER NOT NULL CHECK(revision >= 1),"
    " kind INTEGER,title TEXT,source_event TEXT,created_at INTEGER,due_at INTEGER,expires_at "
    "INTEGER,"
    " resolution_event TEXT,resolved_at INTEGER,PRIMARY KEY(tenant,owner,id));";
constexpr const char *kGoalIndexes =
    "CREATE INDEX goals_due ON goals(tenant,owner,due_at,id) WHERE state=0;"
    "CREATE INDEX goals_by_owner_state ON goals(tenant,owner,state,id);";
constexpr const char *kGoalHistorySchema =
    "CREATE TABLE goal_corrections(tenant TEXT NOT NULL,owner TEXT NOT NULL,goal_id TEXT NOT NULL,"
    " revision INTEGER NOT NULL,kind INTEGER NOT NULL,title TEXT NOT NULL,source_event TEXT NOT "
    "NULL,"
    " created_at INTEGER NOT NULL,due_at INTEGER,expires_at INTEGER,"
    " PRIMARY KEY(tenant,owner,goal_id,revision), UNIQUE(tenant,owner,goal_id,source_event));"
    "CREATE TABLE goal_reminders(tenant TEXT NOT NULL,owner TEXT NOT NULL,goal_id TEXT NOT NULL,"
    " evidence TEXT NOT NULL,revision INTEGER NOT NULL,delivered_at INTEGER NOT NULL,"
    " PRIMARY KEY(tenant,owner,goal_id,evidence));";
constexpr const char *kRecurringSchema =
    "CREATE TABLE recurring_plans(tenant TEXT NOT NULL,owner TEXT NOT NULL,id TEXT NOT NULL,"
    " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 2),"
    " revision INTEGER NOT NULL CHECK(revision >= 1),"
    " title TEXT,source_event TEXT,first_at INTEGER,interval_ms INTEGER,next_at INTEGER,"
    " cancellation_event TEXT,cancelled_at INTEGER,PRIMARY KEY(tenant,owner,id));"
    "CREATE INDEX recurring_plans_due ON recurring_plans(tenant,owner,next_at,id) WHERE state=0;"
    "CREATE TABLE recurring_occurrences(tenant TEXT NOT NULL,owner TEXT NOT NULL,plan_id TEXT NOT "
    "NULL,"
    " revision INTEGER NOT NULL,scheduled_at INTEGER NOT NULL,recorded_at INTEGER NOT NULL,"
    " PRIMARY KEY(tenant,owner,plan_id,revision,scheduled_at));";
constexpr const char *kPlanColumns =
    "id,state,revision,title,source_event,first_at,interval_ms,next_at,"
    "cancellation_event,cancelled_at";
constexpr auto kMaxSqlInteger = std::numeric_limits<std::int64_t>::max();

Error goal_error(ErrorCode code, std::string message) {
    return storage::store_error(code, "mira.goal_store", 0, std::move(message));
}

bool valid_owner(const GoalOwner &owner) { return !owner.tenant.is_nil() && !owner.user.is_nil(); }

bool valid_key(const GoalKey &key) { return valid_owner(key.owner) && !key.id.is_nil(); }

bool valid_time(GoalTime time) { return time.time_since_epoch().count() > 0; }

bool valid_title(const std::string &title) {
    return title.size() <= 512 && title.find_first_not_of(" \t\r\n") != std::string::npos &&
           title.find('\0') == std::string::npos &&
           parse_json(canonical_json_string(JsonValue{title}));
}

bool valid_definition(const GoalDefinition &value) {
    return (value.kind == GoalKind::Task || (value.kind == GoalKind::Commitment && value.due_at)) &&
           !value.source_event.is_nil() && valid_time(value.created_at) &&
           (!value.due_at || *value.due_at >= value.created_at) &&
           (!value.expires_at || (*value.expires_at > value.created_at &&
                                  (!value.due_at || *value.expires_at > *value.due_at))) &&
           valid_title(value.title);
}

bool valid_resolution(const GoalResolution &resolution) {
    return (resolution.state == GoalState::Fulfilled || resolution.state == GoalState::Cancelled ||
            resolution.state == GoalState::Expired) &&
           !resolution.evidence.is_nil() && valid_time(resolution.at);
}

bool resolution_matches_definition(const GoalResolution &resolution,
                                   const GoalDefinition &definition) {
    if (resolution.at < definition.created_at)
        return false;
    const bool expired = definition.expires_at && resolution.at >= *definition.expires_at;
    if (resolution.state == GoalState::Expired)
        return expired;
    return resolution.state == GoalState::Cancelled || !expired;
}

bool bind_text(Statement &s, int position, const std::string &text) {
    return sqlite3_bind_text(s.get(), position, text.data(), static_cast<int>(text.size()),
                             storage::transient_copy()) == SQLITE_OK;
}

bool bind_owner(Statement &s, const GoalOwner &owner) {
    return bind_text(s, 1, owner.tenant.to_string()) && bind_text(s, 2, owner.user.to_string());
}

bool bind_key(Statement &s, const GoalKey &key) {
    return bind_owner(s, key.owner) && bind_text(s, 3, key.id.to_string());
}

bool bind_time(Statement &s, int position, std::optional<GoalTime> time) {
    return (time ? sqlite3_bind_int64(s.get(), position, time->time_since_epoch().count())
                 : sqlite3_bind_null(s.get(), position)) == SQLITE_OK;
}

std::string column_text(Statement &s, int index) {
    const auto *text = sqlite3_column_text(s.get(), index);
    return text ? std::string(reinterpret_cast<const char *>(text),
                              static_cast<std::size_t>(sqlite3_column_bytes(s.get(), index)))
                : std::string{};
}

std::optional<GoalTime> column_time(Statement &s, int index) {
    if (sqlite3_column_type(s.get(), index) != SQLITE_INTEGER)
        return std::nullopt;
    return GoalTime{std::chrono::milliseconds{sqlite3_column_int64(s.get(), index)}};
}

bool null_column(Statement &s, int index) {
    return sqlite3_column_type(s.get(), index) == SQLITE_NULL;
}

Error corrupt_row() { return goal_error(ErrorCode::DataLoss, "goal store row is invalid"); }

Result<GoalDefinition> decode_definition(Statement &s, int column) {
    const auto kind = sqlite3_column_int64(s.get(), column);
    const auto source = EventId::parse(column_text(s, column + 2));
    const auto created = column_time(s, column + 3);
    const auto due_at = column_time(s, column + 4);
    const auto expires_at = column_time(s, column + 5);
    if (sqlite3_column_type(s.get(), column) != SQLITE_INTEGER || kind < 0 || kind > 1 || !source ||
        !created || (!due_at && !null_column(s, column + 4)) ||
        (!expires_at && !null_column(s, column + 5)))
        return corrupt_row();
    GoalDefinition value{static_cast<GoalKind>(kind),
                         column_text(s, column + 1),
                         *source,
                         *created,
                         due_at,
                         expires_at};
    if (!valid_definition(value))
        return corrupt_row();
    return value;
}

Result<GoalRecord> decode_record(Statement &s, const GoalOwner &owner) {
    const auto id = Id128::parse(column_text(s, 0));
    const int state = sqlite3_column_int(s.get(), 1);
    const auto revision = sqlite3_column_int64(s.get(), 2);
    const auto reminders = sqlite3_column_int64(s.get(), 11);
    if (!id || id->is_nil() || sqlite3_column_type(s.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(s.get(), 2) != SQLITE_INTEGER || state < 0 || state > 4 ||
        revision < 1 || reminders < 0 || reminders > 128)
        return corrupt_row();
    GoalRecord record{
        {owner, *id}, static_cast<GoalState>(state), static_cast<std::uint64_t>(revision), {}, {}};
    record.reminder_count = static_cast<std::size_t>(reminders);
    if (record.state == GoalState::Erased) {
        if (reminders != 0)
            return corrupt_row();
        for (int column = 3; column <= 10; ++column)
            if (!null_column(s, column))
                return corrupt_row();
        return record;
    }
    auto definition = decode_definition(s, 3);
    if (!definition)
        return definition.error();
    record.definition = std::move(definition).value();
    if (record.state == GoalState::Pending) {
        if (!null_column(s, 9) || !null_column(s, 10))
            return corrupt_row();
        return record;
    }
    const auto evidence = EventId::parse(column_text(s, 9));
    const auto resolved_at = column_time(s, 10);
    if (revision < 2 || !evidence || !resolved_at)
        return corrupt_row();
    record.resolution = GoalResolution{record.state, *evidence, *resolved_at};
    if (!valid_resolution(*record.resolution) ||
        !resolution_matches_definition(*record.resolution, *record.definition))
        return corrupt_row();
    return record;
}

Result<std::optional<GoalRecord>> read_goal(sqlite3 *db, const GoalKey &key) {
    const auto sql =
        std::string("SELECT ") + kColumns + " FROM goals WHERE tenant=?1 AND owner=?2 AND id=?3";
    Statement row(db, sql.c_str());
    if (!row.valid() || !bind_key(row, key))
        return goal_error(ErrorCode::Internal, "goal read preparation failed");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<GoalRecord>{};
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "goal read failed");
    auto result = decode_record(row, key.owner);
    if (!result)
        return result.error();
    return std::optional{std::move(result).value()};
}

Result<void> require_capacity(sqlite3 *db, const char *sql, std::size_t limit,
                              const GoalOwner *owner = nullptr) {
    Statement count(db, sql);
    if (!count.valid() || (owner && !bind_owner(count, *owner)) ||
        sqlite3_step(count.get()) != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "goal capacity check failed");
    if (sqlite3_column_int64(count.get(), 0) >= static_cast<std::int64_t>(limit))
        return goal_error(ErrorCode::ResourceExhausted, "goal store quota reached");
    return {};
}

Result<GoalRecord> create_goal(sqlite3 *db, const SqliteGoalStoreOptions &options,
                               const GoalKey &key, GoalDefinition definition) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal creation transaction unavailable");
    auto prior = read_goal(db, key);
    if (!prior)
        return prior.error();
    if (prior.value()) {
        if (prior.value()->state == GoalState::Pending && prior.value()->revision == 1 &&
            prior.value()->definition == definition)
            return std::move(*prior.value());
        return goal_error(ErrorCode::InvalidState, "goal ID is already recorded or erased");
    }
    if (auto room = require_capacity(db, "SELECT count(*) FROM goals", options.max_goals); !room)
        return room.error();
    if (auto room = require_capacity(
            db, "SELECT count(*) FROM goals WHERE tenant=?1 AND owner=?2 AND state=0",
            options.max_pending_per_owner, &key.owner);
        !room)
        return room.error();
    Statement insert(db, "INSERT INTO goals(tenant,owner,id,state,revision,kind,title,source_event,"
                         "created_at,due_at,expires_at) VALUES(?1,?2,?3,0,1,?4,?5,?6,?7,?8,?9)");
    if (!insert.valid() || !bind_key(insert, key) ||
        sqlite3_bind_int(insert.get(), 4, static_cast<int>(definition.kind)) != SQLITE_OK ||
        !bind_text(insert, 5, definition.title) ||
        !bind_text(insert, 6, definition.source_event.to_string()) ||
        !bind_time(insert, 7, definition.created_at) || !bind_time(insert, 8, definition.due_at) ||
        !bind_time(insert, 9, definition.expires_at) || sqlite3_step(insert.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal creation failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    return GoalRecord{key, GoalState::Pending, 1, std::move(definition), {}};
}

Result<GoalRecord> settle_goal(sqlite3 *db, const GoalKey &key, std::uint64_t expected,
                               const GoalResolution &resolution) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal resolution transaction unavailable");
    auto prior = read_goal(db, key);
    if (!prior)
        return prior.error();
    if (!prior.value())
        return goal_error(ErrorCode::NotFound, "goal is unavailable for resolution");
    auto record = std::move(*prior.value());
    if (record.revision == expected + 1 && record.resolution == resolution)
        return record;
    if (record.state != GoalState::Pending || record.revision != expected || !record.definition)
        return goal_error(ErrorCode::InvalidState, "goal state or revision changed");
    if (!resolution_matches_definition(resolution, *record.definition))
        return goal_error(ErrorCode::InvalidArgument,
                          "goal resolution time conflicts with its definition");
    Statement update(
        db, "UPDATE goals SET state=?4, revision=revision+1, resolution_event=?5,resolved_at=?6"
            " WHERE tenant=?1 AND owner=?2 AND id=?3 AND state=0 AND revision=?7");
    if (!update.valid() || !bind_key(update, key) ||
        sqlite3_bind_int(update.get(), 4, static_cast<int>(resolution.state)) != SQLITE_OK ||
        !bind_text(update, 5, resolution.evidence.to_string()) ||
        !bind_time(update, 6, resolution.at) ||
        sqlite3_bind_int64(update.get(), 7, static_cast<std::int64_t>(expected)) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal resolution failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    ++record.revision;
    record.state = resolution.state;
    record.resolution = resolution;
    return record;
}

Result<std::vector<GoalRecord>> decode_rows(Statement &rows, const GoalOwner &owner) {
    std::vector<GoalRecord> records;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        auto record = decode_record(rows, owner);
        if (!record)
            return record.error();
        records.push_back(std::move(record).value());
    }
    if (step != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal query failed");
    return records;
}

Result<GoalDuePage> read_due(sqlite3 *db, const GoalDueQuery &query) {
    const auto sql = std::string("SELECT ") + kColumns +
                     " FROM goals WHERE tenant=?1 AND owner=?2 AND state=0 AND due_at<=?3"
                     " AND (expires_at IS NULL OR expires_at>?3) AND (due_at,id)>(?4,?5)"
                     " ORDER BY due_at,id LIMIT ?6";
    Statement rows(db, sql.c_str());
    if (!rows.valid() || !bind_owner(rows, query.owner) || !bind_time(rows, 3, query.cutoff) ||
        !bind_time(rows, 4, query.after ? query.after->due_at : GoalTime{}) ||
        !bind_text(rows, 5, query.after ? query.after->goal.to_string() : std::string{}) ||
        sqlite3_bind_int64(rows.get(), 6, static_cast<std::int64_t>(query.limit + 1)) != SQLITE_OK)
        return goal_error(ErrorCode::Internal, "due query preparation failed");
    auto records = decode_rows(rows, query.owner);
    if (!records)
        return records.error();
    GoalDuePage page{std::move(records).value(), {}};
    if (std::ranges::any_of(page.records, [](const GoalRecord &record) {
            return record.state != GoalState::Pending || !record.definition ||
                   !record.definition->due_at;
        }))
        return corrupt_row();
    if (page.records.size() > query.limit) {
        page.records.pop_back();
        const auto &last = page.records.back();
        page.next = GoalDueCursor{*last.definition->due_at, last.key.id};
    }
    return page;
}

Result<GoalListPage> list_goals(sqlite3 *db, const GoalListQuery &query) {
    auto sql =
        std::string("SELECT ") + kColumns + " FROM goals WHERE tenant=?1 AND owner=?2 AND id>?3";
    if (query.state)
        sql += " AND state=?5";
    sql += " ORDER BY id LIMIT ?4";
    Statement rows(db, sql.c_str());
    if (!rows.valid() || !bind_owner(rows, query.owner) ||
        !bind_text(rows, 3, query.after ? query.after->to_string() : std::string{}) ||
        sqlite3_bind_int64(rows.get(), 4, static_cast<std::int64_t>(query.limit + 1)) !=
            SQLITE_OK ||
        (query.state &&
         sqlite3_bind_int(rows.get(), 5, static_cast<int>(*query.state)) != SQLITE_OK))
        return goal_error(ErrorCode::Internal, "goal list preparation failed");
    auto records = decode_rows(rows, query.owner);
    if (!records)
        return records.error();
    GoalListPage page{std::move(records).value(), {}};
    if (page.records.size() > query.limit) {
        page.records.pop_back();
        page.next = page.records.back().key.id;
    }
    return page;
}

Result<void> erase_goal(sqlite3 *db, std::size_t capacity, const GoalKey &key) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal erasure transaction unavailable");
    Statement erase(db, "UPDATE goals SET state=4,revision=revision+1,kind=NULL,title=NULL,"
                        " source_event=NULL,created_at=NULL,due_at=NULL,expires_at=NULL,"
                        " resolution_event=NULL,resolved_at=NULL WHERE tenant=?1 AND owner=?2 AND "
                        "id=?3 AND state<>4");
    if (!erase.valid() || !bind_key(erase, key) || sqlite3_step(erase.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal erasure failed");
    if (sqlite3_changes(db) != 1) {
        auto prior = read_goal(db, key);
        if (!prior)
            return prior.error();
        if (prior.value()) {
            if (prior.value()->state != GoalState::Erased)
                return goal_error(ErrorCode::Internal, "goal erasure did not apply");
        } else {
            if (auto room = require_capacity(db, "SELECT count(*) FROM goals", capacity); !room)
                return room.error();
            Statement fence(
                db, "INSERT INTO goals(tenant,owner,id,state,revision) VALUES(?1,?2,?3,4,1)");
            if (!fence.valid() || !bind_key(fence, key) ||
                sqlite3_step(fence.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
                return goal_error(ErrorCode::Internal, "goal erasure barrier failed");
        }
    }
    for (const char *sql :
         {"DELETE FROM goal_corrections WHERE tenant=?1 AND owner=?2 AND goal_id=?3",
          "DELETE FROM goal_reminders WHERE tenant=?1 AND owner=?2 AND goal_id=?3"}) {
        Statement cleanup(db, sql);
        if (!cleanup.valid() || !bind_key(cleanup, key) ||
            sqlite3_step(cleanup.get()) != SQLITE_DONE)
            return goal_error(ErrorCode::Internal, "goal history erasure failed");
    }
    Statement remaining(db,
                        "SELECT 1 FROM goal_corrections WHERE tenant=?1 AND owner=?2 AND goal_id=?3"
                        " UNION ALL SELECT 1 FROM goal_reminders WHERE tenant=?1 AND owner=?2 AND "
                        "goal_id=?3 LIMIT 1");
    if (!remaining.valid() || !bind_key(remaining, key) ||
        sqlite3_step(remaining.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal history erasure did not complete");
    return transaction.commit();
}

Result<std::vector<GoalDefinitionVersion>> read_corrections(sqlite3 *db, const GoalKey &key) {
    Statement rows(db, "SELECT revision,kind,title,source_event,created_at,due_at,expires_at"
                       " FROM goal_corrections WHERE tenant=?1 AND owner=?2 AND goal_id=?3"
                       " ORDER BY revision LIMIT 129");
    if (!rows.valid() || !bind_key(rows, key))
        return goal_error(ErrorCode::Internal, "goal correction history preparation failed");
    std::vector<GoalDefinitionVersion> entries;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        auto definition = decode_definition(rows, 1);
        const auto revision = sqlite3_column_int64(rows.get(), 0);
        if (!definition || revision <= 0 || entries.size() >= 128)
            return corrupt_row();
        entries.push_back({static_cast<std::uint64_t>(revision), std::move(definition).value()});
    }
    if (step != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal correction history read failed");
    return entries;
}

Result<GoalRecord> correct_goal(sqlite3 *db, const GoalKey &key, std::uint64_t expected,
                                GoalDefinition replacement, std::size_t capacity) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal correction transaction unavailable");
    auto current = read_goal(db, key);
    if (!current)
        return current.error();
    if (!current.value() || current.value()->state != GoalState::Pending)
        return goal_error(ErrorCode::InvalidState, "goal is unavailable for correction");
    auto record = std::move(*current.value());
    const auto &original = *record.definition;
    if ((record.revision == expected || record.revision == expected + 1) && original == replacement)
        return record;
    if (record.revision != expected || original.kind != replacement.kind ||
        original.created_at != replacement.created_at ||
        original.source_event == replacement.source_event)
        return goal_error(ErrorCode::InvalidState,
                          "goal version, identity or correction source changed");
    if (record.revision > capacity)
        return goal_error(ErrorCode::ResourceExhausted, "goal correction quota reached");
    Statement archive(
        db,
        "INSERT INTO goal_corrections(tenant,owner,goal_id,revision,kind,title,source_event,"
        " created_at,due_at,expires_at) SELECT tenant,owner,id,revision,kind,title,source_event,"
        " created_at,due_at,expires_at FROM goals WHERE tenant=?1 AND owner=?2 AND id=?3");
    if (!archive.valid() || !bind_key(archive, key) || sqlite3_step(archive.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal correction history write failed");
    Statement update(
        db, "UPDATE goals SET revision=revision+1,title=?4,source_event=?5,due_at=?6,expires_at=?7"
            " WHERE tenant=?1 AND owner=?2 AND id=?3 AND state=0 AND revision=?8"
            " AND NOT EXISTS(SELECT 1 FROM goal_corrections WHERE tenant=?1 AND owner=?2"
            " AND goal_id=?3 AND source_event=?5)");
    if (!update.valid() || !bind_key(update, key) || !bind_text(update, 4, replacement.title) ||
        !bind_text(update, 5, replacement.source_event.to_string()) ||
        !bind_time(update, 6, replacement.due_at) ||
        !bind_time(update, 7, replacement.expires_at) ||
        sqlite3_bind_int64(update.get(), 8, static_cast<std::int64_t>(expected)) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal correction failed");
    if (sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::InvalidState, "goal correction source was already used");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    ++record.revision;
    record.definition = std::move(replacement);
    return record;
}

Result<GoalReminderReceipt> store_reminder(sqlite3 *db, const GoalKey &key, std::uint64_t expected,
                                           const GoalReminder &reminder, std::size_t capacity) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal reminder transaction unavailable");
    const auto evidence = reminder.evidence.to_string();
    Statement prior(db,
                    "SELECT revision,delivered_at FROM goal_reminders WHERE tenant=?1 AND owner=?2"
                    " AND goal_id=?3 AND evidence=?4");
    if (!prior.valid() || !bind_key(prior, key) || !bind_text(prior, 4, evidence))
        return goal_error(ErrorCode::Internal, "goal reminder receipt preparation failed");
    const auto step = sqlite3_step(prior.get());
    if (step == SQLITE_ROW) {
        if (sqlite3_column_int64(prior.get(), 0) == static_cast<std::int64_t>(expected) &&
            column_time(prior, 1) == reminder.delivered_at)
            return GoalReminderReceipt{expected, reminder, true};
        return goal_error(ErrorCode::InvalidState, "goal reminder evidence already recorded");
    }
    if (step != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "goal reminder receipt read failed");
    auto record = read_goal(db, key);
    if (!record)
        return record.error();
    if (!record.value() || record.value()->state != GoalState::Pending ||
        record.value()->revision != expected)
        return goal_error(ErrorCode::InvalidState, "goal is unavailable for reminders");
    const auto &definition = *record.value()->definition;
    if (reminder.delivered_at < definition.created_at ||
        (definition.expires_at && reminder.delivered_at >= *definition.expires_at))
        return goal_error(ErrorCode::InvalidArgument, "goal reminder time is outside its lifetime");
    if (record.value()->reminder_count >= capacity)
        return goal_error(ErrorCode::ResourceExhausted, "goal reminder quota reached");
    Statement insert(
        db, "INSERT INTO goal_reminders(tenant,owner,goal_id,evidence,revision,delivered_at)"
            " VALUES(?1,?2,?3,?4,?5,?6)");
    if (!insert.valid() || !bind_key(insert, key) || !bind_text(insert, 4, evidence) ||
        sqlite3_bind_int64(insert.get(), 5, static_cast<std::int64_t>(expected)) != SQLITE_OK ||
        !bind_time(insert, 6, reminder.delivered_at) || sqlite3_step(insert.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal reminder write failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    return GoalReminderReceipt{expected, reminder, false};
}

bool valid_plan(const RecurringPlanDefinition &plan) {
    return valid_title(plan.title) && !plan.source_event.is_nil() && valid_time(plan.first_at) &&
           plan.interval >= std::chrono::seconds{1} && plan.interval <= std::chrono::days{365} &&
           plan.first_at.time_since_epoch().count() <= kMaxSqlInteger - plan.interval.count();
}

Error invalid_plan_row() {
    return goal_error(ErrorCode::DataLoss, "recurring plan row is invalid");
}

Result<RecurringPlanRecord> decode_plan(Statement &row, const GoalOwner &owner) {
    const auto id = Id128::parse(column_text(row, 0));
    const auto state = sqlite3_column_int64(row.get(), 1);
    const auto revision = sqlite3_column_int64(row.get(), 2);
    if (!id || id->is_nil() || sqlite3_column_type(row.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(row.get(), 2) != SQLITE_INTEGER || state < 0 || state > 2 ||
        revision < 1)
        return invalid_plan_row();
    RecurringPlanRecord plan{{owner, *id},
                             static_cast<RecurringPlanState>(state),
                             static_cast<std::uint64_t>(revision),
                             {},
                             {},
                             {}};
    if (plan.state == RecurringPlanState::Erased) {
        for (int index = 3; index <= 9; ++index)
            if (!null_column(row, index))
                return invalid_plan_row();
        return plan;
    }
    const auto source = EventId::parse(column_text(row, 4));
    const auto first_at = column_time(row, 5);
    if (!source || !first_at || sqlite3_column_type(row.get(), 6) != SQLITE_INTEGER)
        return invalid_plan_row();
    plan.definition =
        RecurringPlanDefinition{column_text(row, 3), *source, *first_at,
                                std::chrono::milliseconds{sqlite3_column_int64(row.get(), 6)}};
    if (!valid_plan(*plan.definition))
        return invalid_plan_row();
    if (plan.state == RecurringPlanState::Active) {
        plan.next_at = column_time(row, 7);
        if (!plan.next_at || *plan.next_at < *first_at ||
            (*plan.next_at - *first_at).count() % plan.definition->interval.count() != 0 ||
            !null_column(row, 8) || !null_column(row, 9))
            return invalid_plan_row();
        return plan;
    }
    const auto event = EventId::parse(column_text(row, 8));
    const auto cancelled_at = column_time(row, 9);
    if (!null_column(row, 7) || !event || event->is_nil() || !cancelled_at ||
        !valid_time(*cancelled_at))
        return invalid_plan_row();
    plan.cancellation = RecurringPlanCancellation{*event, *cancelled_at};
    return plan;
}

Result<std::optional<RecurringPlanRecord>> read_plan_row(sqlite3 *db, const GoalKey &key) {
    const auto sql = std::string("SELECT ") + kPlanColumns +
                     " FROM recurring_plans WHERE tenant=?1 AND owner=?2 AND id=?3";
    Statement row(db, sql.c_str());
    if (!row.valid() || !bind_key(row, key))
        return goal_error(ErrorCode::Internal, "plan read preparation failed");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<RecurringPlanRecord>{};
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "plan read failed");
    auto plan = decode_plan(row, key.owner);
    if (!plan)
        return plan.error();
    return std::optional{std::move(plan).value()};
}

bool bind_revision(Statement &statement, int parameter, std::uint64_t revision) {
    return sqlite3_bind_int64(statement.get(), parameter, static_cast<std::int64_t>(revision)) ==
           SQLITE_OK;
}

bool bind_plan(Statement &statement, const RecurringPlanDefinition &definition) {
    return bind_text(statement, 4, definition.title) &&
           bind_text(statement, 5, definition.source_event.to_string()) &&
           bind_time(statement, 6, definition.first_at) &&
           sqlite3_bind_int64(statement.get(), 7, definition.interval.count()) == SQLITE_OK;
}

Result<RecurringPlanRecord> create_plan_row(sqlite3 *db, const SqliteGoalStoreOptions &options,
                                            const GoalKey &key,
                                            RecurringPlanDefinition definition) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "plan creation transaction unavailable");
    auto prior = read_plan_row(db, key);
    if (!prior)
        return prior.error();
    if (prior.value()) {
        const auto &plan = *prior.value();
        if (plan.state == RecurringPlanState::Active && plan.revision == 1 &&
            plan.definition == definition)
            return std::move(*prior.value());
        return goal_error(ErrorCode::InvalidState, "plan ID is already recorded or erased");
    }
    if (auto room = require_capacity(db, "SELECT count(*) FROM recurring_plans", options.max_plans);
        !room)
        return room.error();
    if (auto room = require_capacity(
            db, "SELECT count(*) FROM recurring_plans WHERE tenant=?1 AND owner=?2 AND state=0",
            options.max_active_plans_per_owner, &key.owner);
        !room)
        return room.error();
    Statement insert(
        db, "INSERT INTO recurring_plans(tenant,owner,id,state,revision,title,source_event,"
            "first_at,interval_ms,next_at) VALUES(?1,?2,?3,0,1,?4,?5,?6,?7,?6)");
    if (!insert.valid() || !bind_key(insert, key) || !bind_plan(insert, definition) ||
        sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "plan creation failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    const auto first_at = definition.first_at;
    return RecurringPlanRecord{key, RecurringPlanState::Active, 1, std::move(definition), first_at,
                               {}};
}

Result<RecurringPlanRecord> replace_plan_row(sqlite3 *db, const GoalKey &key,
                                             std::uint64_t expected,
                                             RecurringPlanDefinition definition) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "plan replacement transaction unavailable");
    auto prior = read_plan_row(db, key);
    if (!prior)
        return prior.error();
    if (!prior.value())
        return goal_error(ErrorCode::NotFound, "plan is unavailable for replacement");
    auto plan = std::move(*prior.value());
    if (plan.state != RecurringPlanState::Active)
        return goal_error(ErrorCode::InvalidState, "plan is inactive");
    if ((plan.revision == expected || plan.revision == expected + 1) &&
        plan.definition == definition)
        return plan;
    if (plan.revision != expected || plan.definition->source_event == definition.source_event)
        return goal_error(ErrorCode::InvalidState, "plan version or source changed");
    Statement update(db, "UPDATE recurring_plans SET revision=revision+1,title=?4,source_event=?5,"
                         " first_at=?6,interval_ms=?7,next_at=?6"
                         " WHERE tenant=?1 AND owner=?2 AND id=?3 AND revision=?8 AND state=0");
    if (!update.valid() || !bind_key(update, key) || !bind_plan(update, definition) ||
        !bind_revision(update, 8, expected) || sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "plan replacement failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    ++plan.revision;
    plan.next_at = definition.first_at;
    plan.definition = std::move(definition);
    return plan;
}

Result<RecurringPlanRecord> cancel_plan_row(sqlite3 *db, const GoalKey &key, std::uint64_t expected,
                                            const RecurringPlanCancellation &cancellation) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "plan cancellation transaction unavailable");
    auto prior = read_plan_row(db, key);
    if (!prior)
        return prior.error();
    if (!prior.value())
        return goal_error(ErrorCode::NotFound, "plan is unavailable for cancellation");
    auto plan = std::move(*prior.value());
    if (plan.state == RecurringPlanState::Cancelled && plan.revision == expected + 1 &&
        plan.cancellation == cancellation)
        return plan;
    if (plan.state != RecurringPlanState::Active || plan.revision != expected)
        return goal_error(ErrorCode::InvalidState, "plan state or version changed");
    Statement update(db, "UPDATE recurring_plans SET state=1,revision=revision+1,next_at=NULL,"
                         " cancellation_event=?4,cancelled_at=?5"
                         " WHERE tenant=?1 AND owner=?2 AND id=?3 AND revision=?6 AND state=0");
    if (!update.valid() || !bind_key(update, key) ||
        !bind_text(update, 4, cancellation.evidence.to_string()) ||
        !bind_time(update, 5, cancellation.at) || !bind_revision(update, 6, expected) ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "plan cancellation failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    plan.state = RecurringPlanState::Cancelled;
    ++plan.revision;
    plan.next_at.reset();
    plan.cancellation = cancellation;
    return plan;
}

Result<RecurringDuePage> read_due_plans(sqlite3 *db, const RecurringDueQuery &query) {
    const auto sql =
        std::string("SELECT ") + kPlanColumns +
        " FROM recurring_plans WHERE tenant=?1 AND owner=?2 AND state=0 AND next_at<=?3"
        " AND (next_at,id)>(?4,?5) ORDER BY next_at,id LIMIT ?6";
    Statement rows(db, sql.c_str());
    if (!rows.valid() || !bind_owner(rows, query.owner) || !bind_time(rows, 3, query.cutoff) ||
        !bind_time(rows, 4, query.after ? query.after->scheduled_at : GoalTime{}) ||
        !bind_text(rows, 5, query.after ? query.after->plan.to_string() : std::string{}) ||
        sqlite3_bind_int64(rows.get(), 6, static_cast<std::int64_t>(query.limit + 1)) != SQLITE_OK)
        return goal_error(ErrorCode::Internal, "due plan query preparation failed");
    RecurringDuePage page;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        auto plan = decode_plan(rows, query.owner);
        if (!plan)
            return plan.error();
        if (!plan.value().next_at)
            return invalid_plan_row();
        page.records.push_back(std::move(plan).value());
    }
    if (step != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "due plan query failed");
    if (page.records.size() > query.limit) {
        page.records.pop_back();
        const auto &last = page.records.back();
        page.next = RecurringDueCursor{*last.next_at, last.key.id};
    }
    return page;
}

bool bind_occurrence(Statement &statement, const GoalOccurrenceKey &key) {
    return bind_key(statement, key.plan) && bind_revision(statement, 4, key.plan_revision) &&
           bind_time(statement, 5, key.scheduled_at);
}

Result<std::optional<GoalOccurrenceReceipt>> read_occurrence_row(sqlite3 *db,
                                                                 const GoalOccurrenceKey &key) {
    Statement row(db, "SELECT recorded_at FROM recurring_occurrences WHERE tenant=?1 AND owner=?2"
                      " AND plan_id=?3 AND revision=?4 AND scheduled_at=?5");
    if (!row.valid() || !bind_occurrence(row, key))
        return goal_error(ErrorCode::Internal, "occurrence read preparation failed");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<GoalOccurrenceReceipt>{};
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "occurrence read failed");
    const auto recorded_at = column_time(row, 0);
    if (!recorded_at || *recorded_at < key.scheduled_at)
        return goal_error(ErrorCode::DataLoss, "occurrence receipt is invalid");
    return std::optional{GoalOccurrenceReceipt{key, *recorded_at, false}};
}

Result<GoalOccurrenceReceipt> record_occurrence_row(sqlite3 *db, std::size_t capacity,
                                                    const GoalOccurrenceKey &key,
                                                    GoalTime recorded_at) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "occurrence transaction unavailable");
    auto recorded = read_occurrence_row(db, key);
    if (!recorded)
        return recorded.error();
    if (recorded.value()) {
        recorded.value()->idempotent_replay = true;
        return *recorded.value();
    }
    const auto result = read_plan_row(db, key.plan);
    if (!result)
        return result.error();
    if (!result.value() || result.value()->state != RecurringPlanState::Active ||
        result.value()->revision != key.plan_revision ||
        result.value()->next_at != key.scheduled_at)
        return goal_error(ErrorCode::InvalidState,
                          "occurrence does not match the current due plan");
    if (auto room = require_capacity(db, "SELECT count(*) FROM recurring_occurrences", capacity);
        !room)
        return room.error();
    const auto interval = result.value()->definition->interval;
    if (key.scheduled_at.time_since_epoch().count() > kMaxSqlInteger - interval.count())
        return goal_error(ErrorCode::ResourceExhausted, "recurring plan time range reached");
    Statement insert(
        db,
        "INSERT INTO recurring_occurrences(tenant,owner,plan_id,revision,scheduled_at,recorded_at)"
        " VALUES(?1,?2,?3,?4,?5,?6)");
    if (!insert.valid() || !bind_occurrence(insert, key) || !bind_time(insert, 6, recorded_at) ||
        sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "occurrence write failed");
    Statement advance(db, "UPDATE recurring_plans SET next_at=?6 WHERE tenant=?1 AND owner=?2"
                          " AND id=?3 AND revision=?4 AND next_at=?5 AND state=0");
    if (!advance.valid() || !bind_occurrence(advance, key) ||
        !bind_time(advance, 6, key.scheduled_at + interval) ||
        sqlite3_step(advance.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "recurring plan advance failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    return GoalOccurrenceReceipt{key, recorded_at, false};
}

Result<void> erase_plan_row(sqlite3 *db, std::size_t capacity, const GoalKey &key) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "plan erasure transaction unavailable");
    Statement wipe(db,
                   "UPDATE recurring_plans SET state=2,title=NULL,source_event=NULL,first_at=NULL,"
                   " interval_ms=NULL,next_at=NULL,cancellation_event=NULL,cancelled_at=NULL"
                   " WHERE tenant=?1 AND owner=?2 AND id=?3 AND state<>2");
    if (!wipe.valid() || !bind_key(wipe, key) || sqlite3_step(wipe.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "plan erasure failed");
    if (sqlite3_changes(db) == 0) {
        auto prior = read_plan_row(db, key);
        if (!prior)
            return prior.error();
        if (prior.value() && prior.value()->state != RecurringPlanState::Erased)
            return goal_error(ErrorCode::Internal, "plan erasure did not apply");
        if (!prior.value()) {
            if (auto room = require_capacity(db, "SELECT count(*) FROM recurring_plans", capacity);
                !room)
                return room.error();
            Statement fence(
                db,
                "INSERT INTO recurring_plans(tenant,owner,id,state,revision) VALUES(?1,?2,?3,2,1)");
            if (!fence.valid() || !bind_key(fence, key) ||
                sqlite3_step(fence.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
                return goal_error(ErrorCode::Internal, "plan erasure barrier failed");
        }
    }
    Statement receipts(
        db, "DELETE FROM recurring_occurrences WHERE tenant=?1 AND owner=?2 AND plan_id=?3");
    if (!receipts.valid() || !bind_key(receipts, key) ||
        sqlite3_step(receipts.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "plan occurrence erasure failed");
    Statement remaining(
        db,
        "SELECT 1 FROM recurring_occurrences WHERE tenant=?1 AND owner=?2 AND plan_id=?3 LIMIT 1");
    if (!remaining.valid() || !bind_key(remaining, key) ||
        sqlite3_step(remaining.get()) != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "plan occurrence erasure did not complete");
    return transaction.commit();
}
} // namespace

class SqliteGoalStore::Impl final {
  public:
    Impl(kairo::Executor &executor, storage::DatabaseHandle handle, SqliteGoalStoreOptions config)
        : options(std::move(config)), database(std::move(handle)),
          channel(executor, database.get(),
                  {"mira-goal-store", options.max_pending_requests, options.operation_timeout}) {}

    const SqliteGoalStoreOptions options;
    storage::DatabaseHandle database;
    storage::StoreChannel channel;
};

SqliteGoalStore::SqliteGoalStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SqliteGoalStore::~SqliteGoalStore() { (void)close(); }

Result<std::unique_ptr<SqliteGoalStore>> SqliteGoalStore::open(kairo::Executor &executor,
                                                               SqliteGoalStoreOptions options) {
    if (options.path.empty() || options.max_goals == 0 || options.max_goals > 100'000 ||
        options.max_pending_per_owner == 0 || options.max_pending_per_owner > options.max_goals ||
        options.max_pending_requests == 0 || options.max_pending_requests > 256 ||
        options.operation_timeout.count() <= 0 ||
        options.operation_timeout > std::chrono::minutes{10} || options.max_plans == 0 ||
        options.max_plans > 100'000 || options.max_active_plans_per_owner == 0 ||
        options.max_active_plans_per_owner > options.max_plans || options.max_occurrences == 0 ||
        options.max_occurrences > 1'000'000 || options.max_corrections_per_goal == 0 ||
        options.max_corrections_per_goal > 128 || options.max_reminders_per_goal == 0 ||
        options.max_reminders_per_goal > 128)
        return goal_error(ErrorCode::InvalidArgument, "invalid goal store options");
    storage::DbOpenOptions db;
    db.path = options.path;
    db.exclusive_owner = true;
    db.busy_timeout = std::min(options.operation_timeout, std::chrono::milliseconds{5000});
    const std::string schema =
        std::string("CREATE TABLE store_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);") +
        kGoalTable + kGoalIndexes + kRecurringSchema + kGoalHistorySchema;
    auto opened = storage::open_reference_store(
        db, {"goal-store",
             {1, 2},
             schema.c_str(),
             goal_error,
             [](sqlite3 *database, SchemaVersion previous) {
                 if (previous.minor < 1) {
                     if (auto migrated = storage::exec_sql(database, kRecurringSchema); !migrated)
                         return migrated;
                 }
                 const auto migration =
                     std::string("ALTER TABLE goals RENAME TO goals_before_revision;") +
                     kGoalTable +
                     "INSERT INTO goals SELECT * FROM goals_before_revision;"
                     "DROP TABLE goals_before_revision;" +
                     kGoalIndexes + kGoalHistorySchema;
                 return storage::exec_sql(database, migration.c_str());
             }});
    if (!opened)
        return opened.error();
    if (auto secure = storage::exec_sql(opened.value().database.get(), "PRAGMA secure_delete=ON;");
        !secure)
        return secure.error();
    return std::unique_ptr<SqliteGoalStore>(new SqliteGoalStore(
        std::make_unique<Impl>(executor, std::move(opened.value().database), std::move(options))));
}

Result<GoalRecord> SqliteGoalStore::create(const GoalKey &key, GoalDefinition definition) {
    if (!valid_key(key) || !valid_definition(definition))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal definition");
    if (matches_content_marker(definition.title, default_forbidden_content_markers()))
        return goal_error(ErrorCode::SafetyRejected, "goal title is not retainable");
    return impl_->channel.run<GoalRecord>(
        [key, options = &impl_->options, definition = std::move(definition)](sqlite3 *db) mutable {
            return create_goal(db, *options, key, std::move(definition));
        });
}

Result<std::optional<GoalRecord>> SqliteGoalStore::read(const GoalKey &key) const {
    if (!valid_key(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal key");
    return impl_->channel.run<std::optional<GoalRecord>>(
        [key](sqlite3 *db) { return read_goal(db, key); });
}

Result<GoalRecord> SqliteGoalStore::settle(const GoalKey &key, std::uint64_t expected_revision,
                                           GoalResolution resolution) {
    if (!valid_key(key) || expected_revision == 0 || expected_revision >= kMaxSqlInteger ||
        !valid_resolution(resolution))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal resolution");
    return impl_->channel.run<GoalRecord>([key, expected_revision, resolution](sqlite3 *db) {
        return settle_goal(db, key, expected_revision, resolution);
    });
}

Result<GoalDuePage> SqliteGoalStore::due(const GoalDueQuery &query) const {
    if (!valid_owner(query.owner) || !valid_time(query.cutoff) || query.limit == 0 ||
        query.limit > 128 ||
        (query.after && (!valid_time(query.after->due_at) || query.after->due_at > query.cutoff ||
                         query.after->goal.is_nil())))
        return goal_error(ErrorCode::InvalidArgument, "invalid due goal query");
    return impl_->channel.run<GoalDuePage>([query](sqlite3 *db) { return read_due(db, query); });
}

Result<GoalListPage> SqliteGoalStore::list(const GoalListQuery &query) const {
    if (!valid_owner(query.owner) || query.limit == 0 || query.limit > 128 ||
        (query.after && query.after->is_nil()) || (query.state && *query.state > GoalState::Erased))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal list query");
    return impl_->channel.run<GoalListPage>([query](sqlite3 *db) { return list_goals(db, query); });
}

Result<void> SqliteGoalStore::erase(const GoalKey &key) {
    if (!valid_key(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal erasure key");
    return impl_->channel.run<void>([key, capacity = impl_->options.max_goals](sqlite3 *db) {
        return erase_goal(db, capacity, key);
    });
}

Result<GoalRecord> SqliteGoalStore::correct(const GoalKey &key, std::uint64_t expected_revision,
                                            GoalDefinition definition) {
    if (!valid_key(key) || expected_revision == 0 || expected_revision >= kMaxSqlInteger ||
        !valid_definition(definition))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal correction");
    if (matches_content_marker(definition.title, default_forbidden_content_markers()))
        return goal_error(ErrorCode::SafetyRejected, "goal title is not retainable");
    return impl_->channel.run<GoalRecord>(
        [key, expected_revision, definition = std::move(definition),
         capacity = impl_->options.max_corrections_per_goal](sqlite3 *db) mutable {
            return correct_goal(db, key, expected_revision, std::move(definition), capacity);
        });
}

Result<std::vector<GoalDefinitionVersion>>
SqliteGoalStore::correction_history(const GoalKey &key) const {
    if (!valid_key(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal key");
    return impl_->channel.run<std::vector<GoalDefinitionVersion>>(
        [key](sqlite3 *db) { return read_corrections(db, key); });
}

Result<GoalReminderReceipt> SqliteGoalStore::record_reminder(const GoalKey &key,
                                                             std::uint64_t expected_revision,
                                                             GoalReminder reminder) {
    if (!valid_key(key) || expected_revision == 0 || expected_revision >= kMaxSqlInteger ||
        reminder.evidence.is_nil() || !valid_time(reminder.delivered_at))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal reminder");
    return impl_->channel.run<GoalReminderReceipt>(
        [key, expected_revision, reminder,
         capacity = impl_->options.max_reminders_per_goal](sqlite3 *db) {
            return store_reminder(db, key, expected_revision, reminder, capacity);
        });
}

Result<RecurringPlanRecord> SqliteGoalStore::create_plan(const GoalKey &key,
                                                         RecurringPlanDefinition definition) {
    if (!valid_key(key) || !valid_plan(definition))
        return goal_error(ErrorCode::InvalidArgument, "invalid recurring plan definition");
    if (matches_content_marker(definition.title, default_forbidden_content_markers()))
        return goal_error(ErrorCode::SafetyRejected, "recurring plan title is not retainable");
    return impl_->channel.run<RecurringPlanRecord>(
        [key, options = &impl_->options, definition = std::move(definition)](sqlite3 *db) mutable {
            return create_plan_row(db, *options, key, std::move(definition));
        });
}

Result<RecurringPlanRecord> SqliteGoalStore::replace_plan(const GoalKey &key,
                                                          std::uint64_t expected_revision,
                                                          RecurringPlanDefinition definition) {
    if (!valid_key(key) || expected_revision == 0 || expected_revision >= kMaxSqlInteger - 1 ||
        !valid_plan(definition))
        return goal_error(ErrorCode::InvalidArgument, "invalid recurring plan replacement");
    if (matches_content_marker(definition.title, default_forbidden_content_markers()))
        return goal_error(ErrorCode::SafetyRejected, "recurring plan title is not retainable");
    return impl_->channel.run<RecurringPlanRecord>(
        [key, expected_revision, definition = std::move(definition)](sqlite3 *db) mutable {
            return replace_plan_row(db, key, expected_revision, std::move(definition));
        });
}

Result<RecurringPlanRecord> SqliteGoalStore::cancel_plan(const GoalKey &key,
                                                         std::uint64_t expected_revision,
                                                         RecurringPlanCancellation cancellation) {
    if (!valid_key(key) || expected_revision == 0 || expected_revision >= kMaxSqlInteger ||
        cancellation.evidence.is_nil() || !valid_time(cancellation.at))
        return goal_error(ErrorCode::InvalidArgument, "invalid recurring plan cancellation");
    return impl_->channel.run<RecurringPlanRecord>(
        [key, expected_revision, cancellation](sqlite3 *db) {
            return cancel_plan_row(db, key, expected_revision, cancellation);
        });
}

Result<std::optional<RecurringPlanRecord>> SqliteGoalStore::read_plan(const GoalKey &key) const {
    if (!valid_key(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid recurring plan key");
    return impl_->channel.run<std::optional<RecurringPlanRecord>>(
        [key](sqlite3 *db) { return read_plan_row(db, key); });
}

Result<RecurringDuePage> SqliteGoalStore::due_plans(const RecurringDueQuery &query) const {
    if (!valid_owner(query.owner) || !valid_time(query.cutoff) || query.limit == 0 ||
        query.limit > 128 ||
        (query.after && (!valid_time(query.after->scheduled_at) ||
                         query.after->scheduled_at > query.cutoff || query.after->plan.is_nil())))
        return goal_error(ErrorCode::InvalidArgument, "invalid due plan query");
    return impl_->channel.run<RecurringDuePage>(
        [query](sqlite3 *db) { return read_due_plans(db, query); });
}

Result<GoalOccurrenceReceipt> SqliteGoalStore::record_occurrence(const GoalOccurrenceKey &key,
                                                                 GoalTime recorded_at) {
    if (!valid_key(key.plan) || key.plan_revision == 0 || key.plan_revision >= kMaxSqlInteger ||
        !valid_time(key.scheduled_at) || recorded_at < key.scheduled_at)
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence recording");
    return impl_->channel.run<GoalOccurrenceReceipt>(
        [key, recorded_at, capacity = impl_->options.max_occurrences](sqlite3 *db) {
            return record_occurrence_row(db, capacity, key, recorded_at);
        });
}

Result<std::optional<GoalOccurrenceReceipt>>
SqliteGoalStore::read_occurrence(const GoalOccurrenceKey &key) const {
    if (!valid_key(key.plan) || key.plan_revision == 0 || key.plan_revision >= kMaxSqlInteger ||
        !valid_time(key.scheduled_at))
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence key");
    return impl_->channel.run<std::optional<GoalOccurrenceReceipt>>(
        [key](sqlite3 *db) { return read_occurrence_row(db, key); });
}

Result<void> SqliteGoalStore::erase_plan(const GoalKey &key) {
    if (!valid_key(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid recurring plan erasure key");
    return impl_->channel.run<void>([key, capacity = impl_->options.max_plans](sqlite3 *db) {
        return erase_plan_row(db, capacity, key);
    });
}

Result<void> SqliteGoalStore::close() noexcept { return impl_->channel.close(); }

} // namespace mira
