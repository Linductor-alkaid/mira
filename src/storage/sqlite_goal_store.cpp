#include <mira/sqlite_goal_store.hpp>

#include "sqlite_store_open.hpp"

#include <mira/content_markers.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <utility>

namespace mira {
namespace {
using storage::Statement;
using storage::Transaction;

constexpr const char *kColumns = "id,state,revision,kind,title,source_event,created_at,due_at,"
                                 "expires_at,resolution_event,resolved_at";
constexpr const char *kSchema =
    "CREATE TABLE store_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
    "CREATE TABLE goals(tenant TEXT NOT NULL,owner TEXT NOT NULL,id TEXT NOT NULL,"
    " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
    " revision INTEGER NOT NULL CHECK(revision BETWEEN 1 AND 3),"
    " kind INTEGER,title TEXT,source_event TEXT,created_at INTEGER,due_at INTEGER,expires_at "
    "INTEGER,"
    " resolution_event TEXT,resolved_at INTEGER,PRIMARY KEY(tenant,owner,id));"
    "CREATE INDEX goals_due ON goals(tenant,owner,due_at,id) WHERE state=0;"
    "CREATE INDEX goals_by_owner_state ON goals(tenant,owner,state,id);";

Error goal_error(ErrorCode code, std::string message) {
    return storage::store_error(code, "mira.goal_store", 0, std::move(message));
}

bool valid_owner(const GoalOwner &owner) { return !owner.tenant.is_nil() && !owner.user.is_nil(); }

bool valid_key(const GoalKey &key) { return valid_owner(key.owner) && !key.id.is_nil(); }

bool valid_time(GoalTime time) { return time.time_since_epoch().count() > 0; }

bool valid_definition(const GoalDefinition &value) {
    return (value.kind == GoalKind::Task || (value.kind == GoalKind::Commitment && value.due_at)) &&
           !value.source_event.is_nil() && valid_time(value.created_at) &&
           (!value.due_at || *value.due_at >= value.created_at) &&
           (!value.expires_at || (*value.expires_at > value.created_at &&
                                  (!value.due_at || *value.expires_at > *value.due_at))) &&
           value.title.size() <= 512 &&
           value.title.find_first_not_of(" \t\r\n") != std::string::npos &&
           value.title.find('\0') == std::string::npos &&
           parse_json(canonical_json_string(JsonValue{value.title}));
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

Result<GoalRecord> decode_record(Statement &s, const GoalOwner &owner) {
    const auto id = Id128::parse(column_text(s, 0));
    const int state = sqlite3_column_int(s.get(), 1);
    const auto revision = sqlite3_column_int64(s.get(), 2);
    if (!id || id->is_nil() || sqlite3_column_type(s.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(s.get(), 2) != SQLITE_INTEGER || state < 0 || state > 4 ||
        revision < 1 || revision > 3)
        return corrupt_row();
    GoalRecord record{
        {owner, *id}, static_cast<GoalState>(state), static_cast<std::uint64_t>(revision), {}, {}};
    if (record.state == GoalState::Erased) {
        for (int column = 3; column <= 10; ++column)
            if (!null_column(s, column))
                return corrupt_row();
        return record;
    }
    const int kind = sqlite3_column_int(s.get(), 3);
    const auto source = EventId::parse(column_text(s, 5));
    const auto created = column_time(s, 6);
    const auto due_at = column_time(s, 7);
    const auto expires_at = column_time(s, 8);
    if (sqlite3_column_type(s.get(), 3) != SQLITE_INTEGER || kind < 0 || kind > 1 || !source ||
        !created || (!due_at && !null_column(s, 7)) || (!expires_at && !null_column(s, 8)))
        return corrupt_row();
    record.definition = GoalDefinition{
        static_cast<GoalKind>(kind), column_text(s, 4), *source, *created, due_at, expires_at};
    if (!valid_definition(*record.definition))
        return corrupt_row();
    if (record.state == GoalState::Pending) {
        if (revision != 1 || !null_column(s, 9) || !null_column(s, 10))
            return corrupt_row();
        return record;
    }
    const auto evidence = EventId::parse(column_text(s, 9));
    const auto resolved_at = column_time(s, 10);
    if (revision != 2 || !evidence || !resolved_at)
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

Result<void> require_capacity(sqlite3 *db, std::size_t limit, const GoalOwner *owner = nullptr) {
    Statement count(db, owner
                            ? "SELECT count(*) FROM goals WHERE tenant=?1 AND owner=?2 AND state=0"
                            : "SELECT count(*) FROM goals");
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
        if (prior.value()->state == GoalState::Pending && prior.value()->definition == definition)
            return std::move(*prior.value());
        return goal_error(ErrorCode::InvalidState, "goal ID is already recorded or erased");
    }
    if (auto room = require_capacity(db, options.max_goals); !room)
        return room.error();
    if (auto room = require_capacity(db, options.max_pending_per_owner, &key.owner); !room)
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
    if (expected == 1 && record.revision == 2 && record.resolution == resolution)
        return record;
    if (record.state != GoalState::Pending || record.revision != expected || !record.definition)
        return goal_error(ErrorCode::InvalidState, "goal state or revision changed");
    if (!resolution_matches_definition(resolution, *record.definition))
        return goal_error(ErrorCode::InvalidArgument,
                          "goal resolution time conflicts with its definition");
    Statement update(db, "UPDATE goals SET state=?4, revision=2, resolution_event=?5,resolved_at=?6"
                         " WHERE tenant=?1 AND owner=?2 AND id=?3 AND state=0 AND revision=1");
    if (!update.valid() || !bind_key(update, key) ||
        sqlite3_bind_int(update.get(), 4, static_cast<int>(resolution.state)) != SQLITE_OK ||
        !bind_text(update, 5, resolution.evidence.to_string()) ||
        !bind_time(update, 6, resolution.at) || sqlite3_step(update.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal resolution failed");
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    record.revision = 2;
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
    if (sqlite3_changes(db) == 1)
        return transaction.commit();
    auto prior = read_goal(db, key);
    if (!prior)
        return prior.error();
    if (prior.value()) {
        if (prior.value()->state != GoalState::Erased)
            return goal_error(ErrorCode::Internal, "goal erasure did not apply");
        return {};
    }
    if (auto room = require_capacity(db, capacity); !room)
        return room.error();
    Statement fence(db, "INSERT INTO goals(tenant,owner,id,state,revision) VALUES(?1,?2,?3,4,1)");
    if (!fence.valid() || !bind_key(fence, key) || sqlite3_step(fence.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal erasure barrier failed");
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
        options.operation_timeout > std::chrono::minutes{10})
        return goal_error(ErrorCode::InvalidArgument, "invalid goal store options");
    storage::DbOpenOptions db;
    db.path = options.path;
    db.exclusive_owner = true;
    db.busy_timeout = std::min(options.operation_timeout, std::chrono::milliseconds{5000});
    auto opened = storage::open_reference_store(db, {"goal-store", {1, 0}, kSchema, goal_error});
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
    if (!valid_key(key) || expected_revision == 0 || !valid_resolution(resolution))
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

Result<void> SqliteGoalStore::close() noexcept { return impl_->channel.close(); }

} // namespace mira
