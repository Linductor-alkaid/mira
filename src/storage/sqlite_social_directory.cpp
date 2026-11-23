#include <mira/sqlite_social_directory.hpp>

#include "sqlite_store_open.hpp"

#include <mira/content_markers.hpp>
#include <mira/social_profile.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>

namespace mira {
namespace {
using storage::Statement;
using storage::Transaction;
using Aliases = std::map<std::string, std::string>;

constexpr std::size_t kMaxAliases = 16;
constexpr const char *kErasureSchema =
    "CREATE TABLE social_erasure_pending(slot INTEGER PRIMARY KEY CHECK(slot=1),"
    " tenant TEXT NOT NULL, environment TEXT NOT NULL, object TEXT);";
constexpr const char *kSchema =
    "CREATE TABLE store_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
    "CREATE TABLE social_environments(scope TEXT PRIMARY KEY,erased INTEGER NOT NULL"
    " CHECK(erased IN (0,1)));"
    "CREATE TABLE social_objects(scope TEXT NOT NULL,object TEXT NOT NULL,"
    " revision INTEGER NOT NULL CHECK(revision>=0),PRIMARY KEY(scope,object));"
    "CREATE TABLE social_aliases(scope TEXT NOT NULL,name_key TEXT NOT NULL,"
    " object TEXT NOT NULL,display TEXT NOT NULL,PRIMARY KEY(scope,name_key,object));"
    "CREATE INDEX social_aliases_by_object ON social_aliases(scope,object,name_key);";

Error directory_error(ErrorCode code, std::string message) {
    return storage::store_error(code, "mira.social_directory", 0, std::move(message));
}

bool valid_environment(const SocialEnvironmentKey &environment) {
    return !environment.tenant.is_nil() && !environment.environment.is_nil();
}

std::string scope_key(const SocialEnvironmentKey &environment) {
    return digest_string("mira.social.environment.v1/" + environment.tenant.to_string() + "/" +
                         environment.environment.to_string())
        .to_string();
}

struct ErasureTarget final {
    SocialEnvironmentKey environment;
    std::optional<Id128> object;
};

Result<std::string> alias_key(const std::string &alias, const std::vector<std::string> &forbidden) {
    if (alias.size() > 256 || !parse_json(canonical_json_string(JsonValue{alias})))
        return directory_error(ErrorCode::InvalidArgument, "social alias is invalid");
    if (matches_content_marker(alias, forbidden))
        return directory_error(ErrorCode::SafetyRejected, "social alias is not indexable");
    std::string key;
    key.reserve(alias.size());
    bool space = false;
    for (char byte : alias) {
        if (std::string_view(" \t\r\n\f\v").find(byte) != std::string_view::npos) {
            space = !key.empty();
            continue;
        }
        const auto value = static_cast<unsigned char>(byte);
        if (value < 32 || value == 127)
            return directory_error(ErrorCode::InvalidArgument, "social alias has control bytes");
        if (space) {
            key.push_back(' ');
            space = false;
        }
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
        key.push_back(byte);
    }
    if (key.empty() || matches_content_marker(key, forbidden))
        return directory_error(ErrorCode::InvalidArgument,
                               "social alias is invalid after normalization");
    return key;
}

bool bind_text(Statement &statement, int index, const std::string &value) {
    return sqlite3_bind_text(statement.get(), index, value.data(), static_cast<int>(value.size()),
                             storage::transient_copy()) == SQLITE_OK;
}

std::string column(Statement &statement, int index) {
    const auto *text = sqlite3_column_text(statement.get(), index);
    const auto size = sqlite3_column_bytes(statement.get(), index);
    return text ? std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(size))
                : std::string{};
}

Result<std::optional<ErasureTarget>> pending_erasure(sqlite3 *db);

Result<void> pair_memory_store(sqlite3 *db, std::optional<Id128> memory_id) {
    Transaction transaction(db);
    if (!transaction.valid())
        return directory_error(ErrorCode::Unavailable, "social store pairing unavailable");
    const auto paired = storage::meta_read(db, "social_erasure_store_id");
    if (!paired)
        return paired.error();
    if (paired.value()) {
        if (!memory_id || memory_id->to_string() != *paired.value())
            return directory_error(ErrorCode::InvalidState,
                                   "social directory requires its paired memory store");
        return {};
    }
    if (!memory_id)
        return {};
    const auto pending = pending_erasure(db);
    if (!pending)
        return pending.error();
    if (pending.value())
        return directory_error(ErrorCode::DataLoss,
                               "social erasure intent is missing its store binding");
    if (auto stored = storage::meta_write(db, "social_erasure_store_id", memory_id->to_string());
        !stored)
        return stored.error();
    return transaction.commit();
}

Result<void> journal_erasure(sqlite3 *db, const ErasureTarget &target) {
    Statement insert(
        db,
        "INSERT INTO social_erasure_pending(slot,tenant,environment,object) VALUES(1,?1,?2,?3)");
    if (!insert.valid() || !bind_text(insert, 1, target.environment.tenant.to_string()) ||
        !bind_text(insert, 2, target.environment.environment.to_string()))
        return directory_error(ErrorCode::Internal, "social erasure intent preparation failed");
    const bool bound = target.object ? bind_text(insert, 3, target.object->to_string())
                                     : sqlite3_bind_null(insert.get(), 3) == SQLITE_OK;
    if (!bound || sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return directory_error(ErrorCode::Internal, "social erasure intent write failed");
    return {};
}

Result<std::optional<ErasureTarget>> pending_erasure(sqlite3 *db) {
    Statement row(db, "SELECT tenant,environment,object FROM social_erasure_pending WHERE slot=1");
    if (!row.valid())
        return directory_error(ErrorCode::Internal,
                               "social erasure recovery read failed to prepare");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<ErasureTarget>{};
    if (step != SQLITE_ROW)
        return directory_error(ErrorCode::Internal, "social erasure recovery read failed");
    const auto tenant = TenantId::parse(column(row, 0));
    const auto environment = Id128::parse(column(row, 1));
    std::optional<Id128> object;
    if (sqlite3_column_type(row.get(), 2) != SQLITE_NULL) {
        object = Id128::parse(column(row, 2));
        if (!object || object->is_nil())
            return directory_error(ErrorCode::DataLoss, "social erasure journal object is invalid");
    }
    if (!tenant || tenant->is_nil() || !environment || environment->is_nil())
        return directory_error(ErrorCode::DataLoss,
                               "social erasure journal environment is invalid");
    return std::optional{ErasureTarget{{*tenant, *environment}, object}};
}

Result<std::optional<std::int64_t>> read_integer(sqlite3 *db, const char *sql,
                                                 const std::string &scope,
                                                 const std::string *object = nullptr) {
    Statement statement(db, sql);
    if (!statement.valid() || !bind_text(statement, 1, scope) ||
        (object && !bind_text(statement, 2, *object)))
        return directory_error(ErrorCode::Internal, "social directory lookup preparation failed");
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return std::optional<std::int64_t>{};
    if (step != SQLITE_ROW)
        return directory_error(ErrorCode::Internal, "social directory lookup failed");
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER)
        return directory_error(ErrorCode::DataLoss, "social directory state is invalid");
    return std::optional<std::int64_t>{sqlite3_column_int64(statement.get(), 0)};
}

Result<void> require_capacity(sqlite3 *db, const char *sql, std::size_t capacity) {
    Statement statement(db, sql);
    if (!statement.valid() || sqlite3_step(statement.get()) != SQLITE_ROW)
        return directory_error(ErrorCode::Internal, "social directory capacity check failed");
    if (sqlite3_column_int64(statement.get(), 0) >= static_cast<std::int64_t>(capacity))
        return directory_error(ErrorCode::ResourceExhausted, "social directory capacity reached");
    return {};
}

Result<bool> ensure_environment(sqlite3 *db, const std::string &scope, std::size_t capacity) {
    const auto current =
        read_integer(db, "SELECT erased FROM social_environments WHERE scope=?1", scope);
    if (!current)
        return current.error();
    if (current.value())
        return *current.value() == 0;
    if (auto room = require_capacity(db, "SELECT count(*) FROM social_environments", capacity);
        !room)
        return room.error();
    Statement insert(db, "INSERT INTO social_environments(scope,erased) VALUES(?1,0)");
    if (!insert.valid() || !bind_text(insert, 1, scope) ||
        sqlite3_step(insert.get()) != SQLITE_DONE)
        return directory_error(ErrorCode::Internal, "social environment creation failed");
    return true;
}

Result<std::optional<std::int64_t>> revision(sqlite3 *db, const std::string &scope,
                                             const std::string &object) {
    return read_integer(db, "SELECT revision FROM social_objects WHERE scope=?1 AND object=?2",
                        scope, &object);
}

Result<std::optional<ErasureTarget>> recoverable_erasure(sqlite3 *db) {
    auto pending = pending_erasure(db);
    if (!pending || !pending.value())
        return pending;
    const auto &target = *pending.value();
    const auto scope = scope_key(target.environment);
    const auto environment =
        read_integer(db, "SELECT erased FROM social_environments WHERE scope=?1", scope);
    if (!environment)
        return environment.error();
    if (environment.value() == 1)
        return pending;
    if (environment.value() == 0 && target.object) {
        const auto object = revision(db, scope, target.object->to_string());
        if (!object)
            return object.error();
        if (object.value() == 0)
            return pending;
    }
    return directory_error(ErrorCode::DataLoss,
                           "social erasure intent is missing its directory fence");
}

Result<void> set_revision(sqlite3 *db, const std::string &scope, const std::string &object,
                          std::int64_t version) {
    Statement update(db, "INSERT INTO social_objects(scope,object,revision) VALUES(?1,?2,?3)"
                         " ON CONFLICT(scope,object) DO UPDATE SET revision=excluded.revision");
    if (!update.valid() || !bind_text(update, 1, scope) || !bind_text(update, 2, object) ||
        sqlite3_bind_int64(update.get(), 3, version) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return directory_error(ErrorCode::Internal, "social object revision write failed");
    return {};
}

Result<Aliases> read_aliases(sqlite3 *db, const std::string &scope, const std::string &object) {
    Statement select(db, "SELECT name_key,display FROM social_aliases"
                         " WHERE scope=?1 AND object=?2 ORDER BY name_key LIMIT 17");
    if (!select.valid() || !bind_text(select, 1, scope) || !bind_text(select, 2, object))
        return directory_error(ErrorCode::Internal, "social alias read preparation failed");
    Aliases aliases;
    const auto forbidden = default_forbidden_content_markers();
    int step = SQLITE_OK;
    while ((step = sqlite3_step(select.get())) == SQLITE_ROW) {
        auto name = column(select, 0);
        auto display = column(select, 1);
        const auto normalized = alias_key(display, forbidden);
        if (aliases.size() >= kMaxAliases || !normalized || normalized.value() != name)
            return directory_error(ErrorCode::DataLoss, "social alias row is malformed");
        aliases.emplace(std::move(name), std::move(display));
    }
    if (step != SQLITE_DONE)
        return directory_error(ErrorCode::Internal, "social alias read failed");
    return aliases;
}

Result<void> delete_rows(sqlite3 *db, const char *sql, const std::string &scope,
                         const std::string *object = nullptr) {
    Statement remove(db, sql);
    if (!remove.valid() || !bind_text(remove, 1, scope) ||
        (object && !bind_text(remove, 2, *object)) || sqlite3_step(remove.get()) != SQLITE_DONE)
        return directory_error(ErrorCode::Internal, "social directory deletion failed");
    return {};
}

Result<void> activate_identity(sqlite3 *db, const SqliteSocialDirectoryOptions &options,
                               const std::string &scope, const std::string &object,
                               std::int64_t version, const Aliases &aliases) {
    Transaction transaction(db);
    if (!transaction.valid())
        return directory_error(ErrorCode::Unavailable, "social directory transaction unavailable");
    const auto environment = ensure_environment(db, scope, options.max_environments);
    if (!environment)
        return environment.error();
    if (!environment.value())
        return directory_error(ErrorCode::PermissionDenied, "social environment has been erased");
    const auto stored = revision(db, scope, object);
    if (!stored)
        return stored.error();
    if (stored.value()) {
        if (*stored.value() == 0)
            return directory_error(ErrorCode::PermissionDenied, "social object has been erased");
        if (*stored.value() > version)
            return directory_error(ErrorCode::InvalidState, "social object revision is stale");
        if (*stored.value() == version) {
            const auto previous = read_aliases(db, scope, object);
            if (!previous)
                return previous.error();
            if (previous.value() != aliases)
                return directory_error(ErrorCode::DataLoss,
                                       "social aliases changed within one revision");
            return {};
        }
    } else if (auto room =
                   require_capacity(db, "SELECT count(*) FROM social_objects", options.max_objects);
               !room) {
        return room;
    }
    if (auto changed = set_revision(db, scope, object, version); !changed)
        return changed;
    if (auto removed = delete_rows(db, "DELETE FROM social_aliases WHERE scope=?1 AND object=?2",
                                   scope, &object);
        !removed)
        return removed;
    Statement insert(
        db, "INSERT INTO social_aliases(scope,name_key,object,display) VALUES(?1,?2,?3,?4)");
    for (const auto &[key, display] : aliases) {
        if (!insert.valid() || !bind_text(insert, 1, scope) || !bind_text(insert, 2, key) ||
            !bind_text(insert, 3, object) || !bind_text(insert, 4, display) ||
            sqlite3_step(insert.get()) != SQLITE_DONE)
            return directory_error(ErrorCode::Internal, "social alias write failed");
        insert.reset();
    }
    return transaction.commit();
}

Result<void> erase_identity(sqlite3 *db, const SqliteSocialDirectoryOptions &options,
                            const ErasureTarget &target, bool paired) {
    Transaction transaction(db);
    if (!transaction.valid())
        return directory_error(ErrorCode::Unavailable, "social erasure transaction unavailable");
    const auto scope = scope_key(target.environment);
    const auto environment = ensure_environment(db, scope, options.max_environments);
    if (!environment)
        return environment.error();
    if (paired) {
        if (auto recorded = journal_erasure(db, target); !recorded)
            return recorded.error();
    }
    if (!environment.value())
        return transaction.commit();
    if (target.object) {
        const auto object = target.object->to_string();
        const auto stored = revision(db, scope, object);
        if (!stored)
            return stored.error();
        if (!stored.value()) {
            if (auto room = require_capacity(db, "SELECT count(*) FROM social_objects",
                                             options.max_objects);
                !room)
                return room;
        }
        if (auto fenced = set_revision(db, scope, object, 0); !fenced)
            return fenced;
        if (auto removed = delete_rows(
                db, "DELETE FROM social_aliases WHERE scope=?1 AND object=?2", scope, &object);
            !removed)
            return removed;
    } else {
        Statement fence(db, "UPDATE social_environments SET erased=1 WHERE scope=?1");
        if (!fence.valid() || !bind_text(fence, 1, scope) ||
            sqlite3_step(fence.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
            return directory_error(ErrorCode::Internal, "social environment erasure fence failed");
        if (auto removed = delete_rows(db, "DELETE FROM social_aliases WHERE scope=?1", scope);
            !removed)
            return removed;
        if (auto removed = delete_rows(db, "DELETE FROM social_objects WHERE scope=?1", scope);
            !removed)
            return removed;
    }
    return transaction.commit();
}
} // namespace

class SqliteSocialDirectory::Impl final {
  public:
    Impl(kairo::Executor &executor, storage::DatabaseHandle handle,
         SqliteSocialDirectoryOptions configuration, std::shared_ptr<SqliteMemoryStore> knowledge)
        : options(std::move(configuration)), database(std::move(handle)),
          channel(
              executor, database.get(),
              {"mira-social-directory", options.max_pending_requests, options.operation_timeout}),
          memory(std::move(knowledge)) {}

    template <typename T, typename Op> Result<T> run(Op operation) {
        const auto epoch = erasure_epoch.load();
        if ((epoch & 1U) != 0U)
            return directory_error(ErrorCode::Unavailable,
                                   "social directory erasure fence is active");
        auto result = channel.run<T>(
            [this, epoch, operation = std::move(operation)](sqlite3 *db) mutable -> Result<T> {
                if (erasure_epoch.load() != epoch)
                    return directory_error(ErrorCode::InvalidState,
                                           "social directory operation invalidated");
                return operation(db);
            });
        if (erasure_epoch.load() != epoch)
            return directory_error(ErrorCode::InvalidState, "social directory result invalidated");
        return result;
    }

    Result<void> recover() {
        auto pending = channel.run<std::optional<ErasureTarget>>(recoverable_erasure);
        if (!pending)
            return pending.error();
        if (!pending.value())
            return {};
        if (!memory)
            return directory_error(ErrorCode::InvalidState,
                                   "social erasure recovery needs its memory store");
        return finish_erasure(*pending.value());
    }

    Result<void> erase(const ErasureTarget &target) {
        const std::lock_guard lock(erasure_mutex);
        if ((erasure_epoch.load() & 1U) != 0U)
            return directory_error(ErrorCode::Unavailable,
                                   "social directory requires erasure recovery");
        erasure_epoch.fetch_add(1);
        auto result = channel.run<void>([this, target](sqlite3 *db) {
            return erase_identity(db, options, target, memory != nullptr);
        });
        if (result && memory)
            result = finish_erasure(target);
        if (result)
            erasure_epoch.fetch_add(1);
        return result;
    }

    Result<void> finish_erasure(const ErasureTarget &target) {
        auto scope = target.object ? social_object_scope({target.environment, *target.object})
                                   : social_environment_scope(target.environment);
        if (!scope)
            return scope.error();
        ErasureRequest request;
        request.scope = std::move(scope).value();
        request.reason = "social-profile-erasure";
        auto erased = memory->erase(request);
        if (!erased)
            return erased.error();
        if (erased.value().status != ErasureStatus::Complete)
            return make_memory_error(MemoryDomainCode::ErasurePending,
                                     "social memory erasure needs recovery");
        return channel.run<void>([](sqlite3 *db) -> Result<void> {
            Statement ack(db, "DELETE FROM social_erasure_pending WHERE slot=1");
            if (!ack.valid() || sqlite3_step(ack.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
                return directory_error(ErrorCode::Internal,
                                       "social erasure acknowledgement failed");
            return {};
        });
    }

    const SqliteSocialDirectoryOptions options;
    storage::DatabaseHandle database;
    storage::StoreChannel channel;
    const std::shared_ptr<SqliteMemoryStore> memory;
    std::mutex erasure_mutex;
    std::atomic<std::uint64_t> erasure_epoch{0};
};

SqliteSocialDirectory::SqliteSocialDirectory(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SqliteSocialDirectory::~SqliteSocialDirectory() { (void)close(); }

Result<std::unique_ptr<SqliteSocialDirectory>>
SqliteSocialDirectory::open(kairo::Executor &executor, SqliteSocialDirectoryOptions options,
                            std::shared_ptr<SqliteMemoryStore> memory) {
    if (options.path.empty() || options.max_environments == 0 || options.max_environments > 4096 ||
        options.max_objects == 0 || options.max_objects > 100000 ||
        options.max_pending_requests == 0 || options.max_pending_requests > 256 ||
        options.operation_timeout.count() <= 0 ||
        options.operation_timeout > std::chrono::minutes(10))
        return directory_error(ErrorCode::InvalidArgument, "invalid social directory options");

    storage::DbOpenOptions db_options;
    db_options.path = options.path;
    db_options.exclusive_owner = true;
    db_options.busy_timeout = std::min(options.operation_timeout, std::chrono::milliseconds{5000});
    const std::string schema = std::string(kSchema) + kErasureSchema;
    auto opened = storage::open_reference_store(
        db_options,
        {"social-directory",
         {1, 1},
         schema.c_str(),
         directory_error,
         [](sqlite3 *db, SchemaVersion) { return storage::exec_sql(db, kErasureSchema); }});
    if (!opened)
        return opened.error();
    if (opened.value().diagnostics.read_only)
        return directory_error(ErrorCode::UnsupportedVersion,
                               "unsupported social directory schema");
    if (auto secure = storage::exec_sql(opened.value().database.get(), "PRAGMA secure_delete=ON;");
        !secure)
        return secure.error();
    std::optional<Id128> memory_id;
    if (memory) {
        auto identified = memory->social_erasure_store_id();
        if (!identified)
            return identified.error();
        memory_id = identified.value();
    }
    if (auto paired = pair_memory_store(opened.value().database.get(), memory_id); !paired)
        return paired.error();
    auto instance =
        std::unique_ptr<SqliteSocialDirectory>(new SqliteSocialDirectory(std::make_unique<Impl>(
            executor, std::move(opened.value().database), std::move(options), std::move(memory))));
    if (auto recovered = instance->impl_->recover(); !recovered)
        return recovered.error();
    return instance;
}

Result<void> SqliteSocialDirectory::activate(const SocialObjectKey &key,
                                             SocialObjectIdentity value) {
    if (!valid_environment(key.environment) || key.object.is_nil() || value.revision == 0 ||
        value.revision > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        value.aliases.size() > kMaxAliases)
        return directory_error(ErrorCode::InvalidArgument, "invalid social object identity");
    Aliases aliases;
    const auto forbidden = default_forbidden_content_markers();
    for (auto &display : value.aliases) {
        auto normalized = alias_key(display, forbidden);
        if (!normalized)
            return normalized.error();
        if (!aliases.emplace(std::move(normalized).value(), std::move(display)).second)
            return directory_error(ErrorCode::InvalidArgument,
                                   "social object has duplicate aliases");
    }
    return impl_->run<void>([state = impl_.get(), scope = scope_key(key.environment),
                             object = key.object.to_string(),
                             version = static_cast<std::int64_t>(value.revision),
                             aliases = std::move(aliases)](sqlite3 *db) {
        return activate_identity(db, state->options, scope, object, version, aliases);
    });
}

Result<std::optional<SocialObjectIdentity>>
SqliteSocialDirectory::read(const SocialObjectKey &key) const {
    if (!valid_environment(key.environment) || key.object.is_nil())
        return directory_error(ErrorCode::InvalidArgument, "invalid social object key");
    return impl_->run<std::optional<SocialObjectIdentity>>(
        [scope = scope_key(key.environment), object = key.object.to_string()](
            sqlite3 *db) -> Result<std::optional<SocialObjectIdentity>> {
            const auto stored = revision(db, scope, object);
            if (!stored)
                return stored.error();
            if (!stored.value() || *stored.value() == 0)
                return std::optional<SocialObjectIdentity>{};
            if (*stored.value() < 0)
                return directory_error(ErrorCode::DataLoss, "social object revision is malformed");
            auto aliases = read_aliases(db, scope, object);
            if (!aliases)
                return aliases.error();
            SocialObjectIdentity identity;
            identity.revision = static_cast<std::uint64_t>(*stored.value());
            for (auto &[name, display] : aliases.value()) {
                (void)name;
                identity.aliases.push_back(std::move(display));
            }
            return std::optional{std::move(identity)};
        });
}

Result<SocialAliasResult> SqliteSocialDirectory::resolve(const SocialEnvironmentKey &environment,
                                                         std::string alias,
                                                         std::size_t limit) const {
    if (!valid_environment(environment) || limit == 0 || limit > 64)
        return directory_error(ErrorCode::InvalidArgument, "invalid social alias lookup");
    auto normalized = alias_key(alias, default_forbidden_content_markers());
    if (!normalized)
        return normalized.error();
    return impl_->run<SocialAliasResult>([environment, scope = scope_key(environment),
                                          name = std::move(normalized).value(),
                                          limit](sqlite3 *db) -> Result<SocialAliasResult> {
        Statement rows(db, "SELECT a.object,o.revision FROM social_aliases a"
                           " JOIN social_objects o ON o.scope=a.scope AND o.object=a.object"
                           " WHERE a.scope=?1 AND a.name_key=?2 AND o.revision>0"
                           " ORDER BY a.object LIMIT ?3");
        if (!rows.valid() || !bind_text(rows, 1, scope) || !bind_text(rows, 2, name) ||
            sqlite3_bind_int64(rows.get(), 3, static_cast<std::int64_t>(limit + 1)) != SQLITE_OK)
            return directory_error(ErrorCode::Internal, "social alias lookup preparation failed");
        SocialAliasResult result;
        int step = SQLITE_OK;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
            const auto object = Id128::parse(column(rows, 0));
            const auto version = sqlite3_column_int64(rows.get(), 1);
            if (!object || object->is_nil() || version <= 0)
                return directory_error(ErrorCode::DataLoss, "social alias match is malformed");
            result.matches.push_back({{environment, *object}, static_cast<std::uint64_t>(version)});
        }
        if (step != SQLITE_DONE)
            return directory_error(ErrorCode::Internal, "social alias lookup failed");
        if (result.matches.size() == 1)
            result.resolution = SocialAliasResolution::Unique;
        else if (result.matches.size() > 1)
            result.resolution = SocialAliasResolution::Ambiguous;
        if (result.matches.size() > limit) {
            result.truncated = true;
            result.matches.resize(limit);
        }
        return result;
    });
}

Result<void> SqliteSocialDirectory::erase_object(const SocialObjectKey &key) {
    if (!valid_environment(key.environment) || key.object.is_nil())
        return directory_error(ErrorCode::InvalidArgument, "invalid social object erasure target");
    return impl_->erase({key.environment, key.object});
}

Result<void> SqliteSocialDirectory::erase_environment(const SocialEnvironmentKey &key) {
    if (!valid_environment(key))
        return directory_error(ErrorCode::InvalidArgument,
                               "invalid social environment erasure target");
    return impl_->erase({key, std::nullopt});
}

Result<void> SqliteSocialDirectory::close() noexcept { return impl_->channel.close(); }

} // namespace mira
