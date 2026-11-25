#include <mira/sqlite_social_directory.hpp>

#include "social_message_source.hpp"
#include "sqlite_store_open.hpp"

#include <mira/content_markers.hpp>
#include <mira/environment.hpp>
#include <mira/social_profile.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <algorithm>
#include <atomic>
#include <iterator>
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
constexpr const char *kAccountsSchema =
    "CREATE TABLE social_accounts(scope TEXT NOT NULL,account_key TEXT NOT NULL,"
    " object TEXT,source_event TEXT,CHECK((object IS NULL)=(source_event IS NULL)),"
    " PRIMARY KEY(scope,account_key));"
    "CREATE INDEX social_accounts_by_object ON social_accounts(scope,object)"
    " WHERE object IS NOT NULL;";
constexpr const char *kMessagesSchema =
    "CREATE TABLE social_messages(scope TEXT NOT NULL,event_key TEXT NOT NULL,"
    " session TEXT,sequence INTEGER,event TEXT,object TEXT,source TEXT,account_key TEXT,integrity "
    "TEXT,"
    " CHECK((object IS NULL)=(session IS NULL) AND (object IS NULL)=(sequence IS NULL)"
    " AND (object IS NULL)=(event IS NULL) AND (object IS NULL)=(source IS NULL)"
    " AND (object IS NULL)=(account_key IS NULL) AND (object IS NULL)=(integrity IS NULL)"
    " AND (sequence IS NULL OR sequence>0)),"
    " PRIMARY KEY(scope,event_key),UNIQUE(scope,session,sequence));"
    "CREATE INDEX social_messages_by_object ON social_messages(scope,object)"
    " WHERE object IS NOT NULL;";
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

Error profile_write_busy() {
    auto error =
        directory_error(ErrorCode::ResourceExhausted, "social profile write boundary is busy");
    error.retryable = true;
    return error;
}

Result<void> check_profile_recording(const OperationContext &context) {
    if (context.cancelled())
        return directory_error(ErrorCode::Cancelled, "social profile recording was cancelled");
    if (context.expired(Timestamp::now()))
        return directory_error(ErrorCode::DeadlineExceeded, "social profile recording expired");
    return {};
}

bool valid_environment(const SocialEnvironmentKey &environment) {
    return !environment.tenant.is_nil() && !environment.environment.is_nil();
}

std::string scope_key(const SocialEnvironmentKey &environment) {
    return digest_string("mira.social.environment.v1/" + environment.tenant.to_string() + "/" +
                         environment.environment.to_string())
        .to_string();
}

Result<Sha256Digest> account_key(const SocialAccountKey &account, const std::string &scope) {
    if (account.account.is_nil() || account.source.empty() || account.source.size() > 64 ||
        !std::ranges::all_of(account.source, [](char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
                   byte == '-' || byte == '_';
        }))
        return directory_error(ErrorCode::InvalidArgument, "invalid social account key");
    return digest_string("mira.social.account.v1/" + scope + "/" + account.source + "/" +
                         account.account.to_string());
}

bool valid_message_sequence(SessionSequence sequence) {
    return sequence > 0 &&
           sequence <= static_cast<SessionSequence>(std::numeric_limits<std::int64_t>::max());
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

Result<SocialAliasResult> find_alias(sqlite3 *db, const SocialEnvironmentKey &environment,
                                     const std::string &scope, const std::string &name,
                                     std::size_t limit) {
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

Result<std::optional<SocialAccountBinding>>
read_account_binding(sqlite3 *db, const SocialEnvironmentKey &environment, const std::string &scope,
                     const std::string &account) {
    Statement lookup(db, "SELECT a.object,a.source_event,o.revision FROM social_accounts a"
                         " LEFT JOIN social_objects o ON o.scope=a.scope AND o.object=a.object"
                         " WHERE a.scope=?1 AND a.account_key=?2");
    if (!lookup.valid() || !bind_text(lookup, 1, scope) || !bind_text(lookup, 2, account))
        return directory_error(ErrorCode::Internal, "social account lookup preparation failed");
    const auto step = sqlite3_step(lookup.get());
    if (step == SQLITE_DONE)
        return std::optional<SocialAccountBinding>{};
    if (step != SQLITE_ROW)
        return directory_error(ErrorCode::Internal, "social account lookup failed");
    if (sqlite3_column_type(lookup.get(), 0) == SQLITE_NULL)
        return directory_error(ErrorCode::PermissionDenied, "social account has been erased");
    const auto object = Id128::parse(column(lookup, 0));
    const auto source_event = EventId::parse(column(lookup, 1));
    const auto version = sqlite3_column_int64(lookup.get(), 2);
    if (!object || object->is_nil() || !source_event || source_event->is_nil() || version <= 0)
        return directory_error(ErrorCode::DataLoss, "social account binding is malformed");
    return std::optional{SocialAccountBinding{
        {environment, *object}, static_cast<std::uint64_t>(version), *source_event}};
}

Result<SocialAccountBinding> admit_account(sqlite3 *db, const SqliteSocialDirectoryOptions &options,
                                           const SocialEnvironmentKey &environment,
                                           const std::string &scope, const std::string &account,
                                           EventId source_event,
                                           const std::optional<SocialObjectKey> &known_object) {
    const auto active = ensure_environment(db, scope, options.max_environments);
    if (!active)
        return active.error();
    if (!active.value())
        return directory_error(ErrorCode::PermissionDenied, "social environment has been erased");
    const auto existing = read_account_binding(db, environment, scope, account);
    if (!existing)
        return existing.error();
    if (existing.value()) {
        if (known_object && existing.value()->object != *known_object)
            return directory_error(ErrorCode::AlreadyExists,
                                   "social account belongs to a different object");
        return *existing.value();
    }
    if (auto room =
            require_capacity(db, "SELECT count(*) FROM social_accounts", options.max_accounts);
        !room)
        return room.error();
    const auto object = known_object ? known_object->object : Id128::generate();
    const auto object_text = object.to_string();
    const auto current = revision(db, scope, object_text);
    if (!current)
        return current.error();
    std::uint64_t version = 1;
    if (known_object) {
        if (current.value() == 0)
            return directory_error(ErrorCode::PermissionDenied, "social object has been erased");
        if (!current.value())
            return directory_error(ErrorCode::NotFound, "social object is missing");
        if (*current.value() < 0)
            return directory_error(ErrorCode::DataLoss, "social object revision is malformed");
        version = static_cast<std::uint64_t>(*current.value());
    } else {
        if (current.value())
            return directory_error(ErrorCode::AlreadyExists,
                                   "generated social object already exists");
        if (auto room =
                require_capacity(db, "SELECT count(*) FROM social_objects", options.max_objects);
            !room)
            return room.error();
        if (auto created = set_revision(db, scope, object_text, 1); !created)
            return created.error();
    }
    Statement insert(db, "INSERT INTO social_accounts(scope,account_key,object,source_event)"
                         " VALUES(?1,?2,?3,?4)");
    if (!insert.valid() || !bind_text(insert, 1, scope) || !bind_text(insert, 2, account) ||
        !bind_text(insert, 3, object_text) || !bind_text(insert, 4, source_event.to_string()) ||
        sqlite3_step(insert.get()) != SQLITE_DONE)
        return directory_error(ErrorCode::Internal, "social account binding write failed");
    return SocialAccountBinding{{environment, object}, version, source_event};
}

Result<SocialMessageEvidence> decode_message(Statement &row,
                                             const SocialEnvironmentKey &environment) {
    const auto session = SessionId::parse(column(row, 0));
    const auto sequence = sqlite3_column_int64(row.get(), 1);
    const auto event = EventId::parse(column(row, 2));
    const auto object = Id128::parse(column(row, 3));
    auto source = column(row, 4);
    const auto account = digest_from_hex(column(row, 5));
    const auto integrity = digest_from_hex(column(row, 6));
    if (!session || session->is_nil() || sequence <= 0 || !event || event->is_nil() || !object ||
        object->is_nil() || source.empty() || !account || !integrity)
        return directory_error(ErrorCode::DataLoss, "social message evidence is malformed");
    return SocialMessageEvidence{{*session, static_cast<SessionSequence>(sequence), *event},
                                 {environment, *object},
                                 std::move(source),
                                 *account,
                                 *integrity};
}

std::string message_event_key(const std::string &scope, EventId event) {
    return digest_string("mira.social.message.v1/" + scope + "/" + event.to_string()).to_string();
}

Result<std::optional<SocialMessageEvidence>> find_message(sqlite3 *db,
                                                          const SocialEnvironmentKey &environment,
                                                          const std::string &scope,
                                                          const std::string &event_key) {
    Statement lookup(db, "SELECT session,sequence,event,object,source,account_key,integrity"
                         " FROM social_messages WHERE scope=?1 AND event_key=?2");
    if (!lookup.valid() || !bind_text(lookup, 1, scope) || !bind_text(lookup, 2, event_key))
        return directory_error(ErrorCode::Internal, "social message lookup preparation failed");
    const auto step = sqlite3_step(lookup.get());
    if (step == SQLITE_DONE)
        return std::optional<SocialMessageEvidence>{};
    if (step != SQLITE_ROW)
        return directory_error(ErrorCode::Internal, "social message lookup failed");
    if (sqlite3_column_type(lookup.get(), 3) == SQLITE_NULL)
        return directory_error(ErrorCode::PermissionDenied, "social message has been erased");
    auto evidence = decode_message(lookup, environment);
    if (!evidence)
        return evidence.error();
    return std::optional{std::move(evidence).value()};
}

Result<std::optional<SocialMessageEvidence>>
find_message_at(sqlite3 *db, const SocialEnvironmentKey &environment, const std::string &scope,
                const SocialMessageLocator &message) {
    auto found = find_message(db, environment, scope, message_event_key(scope, message.event));
    if (found && found.value() && found.value()->message != message)
        return directory_error(ErrorCode::InvalidArgument,
                               "social message reference position does not match");
    return found;
}

using PreparedMention = std::variant<Sha256Digest, SocialMessageLocator, std::string>;

Result<SocialMentionResult> find_mention(sqlite3 *db, const SocialEnvironmentKey &environment,
                                         const std::string &scope, const PreparedMention &mention,
                                         std::size_t alias_limit) {
    if (const auto *account = std::get_if<Sha256Digest>(&mention)) {
        auto found = read_account_binding(db, environment, scope, account->to_string());
        if (!found && found.error().code != ErrorCode::PermissionDenied)
            return found.error();
        if (!found || !found.value())
            return SocialMentionResult{};
        return SocialMentionResult{*found.value()};
    }
    if (const auto *quoted = std::get_if<SocialMessageLocator>(&mention)) {
        auto found = find_message_at(db, environment, scope, *quoted);
        if (!found && found.error().code != ErrorCode::PermissionDenied)
            return found.error();
        if (!found || !found.value())
            return SocialMentionResult{};
        return SocialMentionResult{std::move(*found.value())};
    }
    auto aliases = find_alias(db, environment, scope, std::get<std::string>(mention), alias_limit);
    if (!aliases)
        return aliases.error();
    return SocialMentionResult{std::move(aliases).value()};
}

Result<SocialMessageEvidence>
index_message_evidence(sqlite3 *db, const SqliteSocialDirectoryOptions &options,
                       const SocialEnvironmentKey &environment, const std::string &scope,
                       const Sha256Digest &account_digest, const std::string &source,
                       const SocialMessageLocator &message, const Sha256Digest &integrity) {
    Transaction transaction(db);
    if (!transaction.valid())
        return directory_error(ErrorCode::Unavailable, "social message transaction unavailable");
    const auto event_key = message_event_key(scope, message.event);
    const auto account = account_digest.to_string();
    auto previous = find_message(db, environment, scope, event_key);
    if (!previous)
        return previous.error();
    if (previous.value()) {
        if (previous.value()->message != message || previous.value()->source != source ||
            previous.value()->account_digest != account_digest ||
            previous.value()->event_integrity != integrity)
            return directory_error(ErrorCode::AlreadyExists, "social message evidence conflicts");
        return std::move(*previous.value());
    }
    const auto speaker =
        admit_account(db, options, environment, scope, account, message.event, std::nullopt);
    if (!speaker)
        return speaker.error();
    if (auto room =
            require_capacity(db, "SELECT count(*) FROM social_messages", options.max_messages);
        !room)
        return room.error();
    Statement insert(db,
                     "INSERT INTO social_messages(scope,event_key,session,sequence,event,object,"
                     "source,account_key,integrity) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    if (!insert.valid() || !bind_text(insert, 1, scope) || !bind_text(insert, 2, event_key) ||
        !bind_text(insert, 3, message.session.to_string()) ||
        sqlite3_bind_int64(insert.get(), 4, static_cast<std::int64_t>(message.sequence)) !=
            SQLITE_OK ||
        !bind_text(insert, 5, message.event.to_string()) ||
        !bind_text(insert, 6, speaker.value().object.object.to_string()) ||
        !bind_text(insert, 7, source) || !bind_text(insert, 8, account) ||
        !bind_text(insert, 9, integrity.to_string()))
        return directory_error(ErrorCode::Internal, "social message insert preparation failed");
    if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE)
            return directory_error(ErrorCode::AlreadyExists, "social message sequence conflicts");
        return directory_error(ErrorCode::Internal, "social message insert failed");
    }
    if (auto committed = transaction.commit(); !committed)
        return committed.error();
    return SocialMessageEvidence{message, speaker.value().object, source, account_digest,
                                 integrity};
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
        if (auto removed = delete_rows(db,
                                       "UPDATE social_accounts SET object=NULL,source_event=NULL"
                                       " WHERE scope=?1 AND object=?2 AND object IS NOT NULL",
                                       scope, &object);
            !removed)
            return removed;
        if (auto removed =
                delete_rows(db,
                            "UPDATE social_messages SET session=NULL,sequence=NULL,event=NULL,"
                            " object=NULL,source=NULL,account_key=NULL,integrity=NULL"
                            " WHERE scope=?1 AND object=?2 AND object IS NOT NULL",
                            scope, &object);
            !removed)
            return removed;
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
        if (auto removed = delete_rows(db, "DELETE FROM social_accounts WHERE scope=?1", scope);
            !removed)
            return removed;
        if (auto removed = delete_rows(db, "DELETE FROM social_messages WHERE scope=?1", scope);
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
        options.max_objects == 0 || options.max_objects > 100000 || options.max_accounts == 0 ||
        options.max_accounts > 1000000 || options.max_messages == 0 ||
        options.max_messages > 1000000 || options.max_pending_requests == 0 ||
        options.max_pending_requests > 256 || options.operation_timeout.count() <= 0 ||
        options.operation_timeout > std::chrono::minutes(10))
        return directory_error(ErrorCode::InvalidArgument, "invalid social directory options");

    storage::DbOpenOptions db_options;
    db_options.path = options.path;
    db_options.exclusive_owner = true;
    db_options.busy_timeout = std::min(options.operation_timeout, std::chrono::milliseconds{5000});
    const std::string schema =
        std::string(kSchema) + kErasureSchema + kAccountsSchema + kMessagesSchema;
    auto opened = storage::open_reference_store(
        db_options, {"social-directory",
                     {1, 3},
                     schema.c_str(),
                     directory_error,
                     [](sqlite3 *db, SchemaVersion previous) -> Result<void> {
                         if (previous.minor < 1) {
                             if (auto installed = storage::exec_sql(db, kErasureSchema); !installed)
                                 return installed.error();
                         }
                         if (previous.minor < 2) {
                             if (auto installed = storage::exec_sql(db, kAccountsSchema);
                                 !installed)
                                 return installed.error();
                         }
                         return storage::exec_sql(db, kMessagesSchema);
                     }});
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
    const std::unique_lock serialize(impl_->erasure_mutex, std::try_to_lock);
    if (!serialize.owns_lock())
        return profile_write_busy();
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
    return impl_->run<SocialAliasResult>(
        [environment, scope = scope_key(environment), name = std::move(normalized).value(),
         limit](sqlite3 *db) { return find_alias(db, environment, scope, name, limit); });
}

Result<std::optional<SocialAccountBinding>>
SqliteSocialDirectory::resolve_account(const SocialEnvironmentKey &environment,
                                       const SocialAccountKey &account) const {
    if (!valid_environment(environment))
        return directory_error(ErrorCode::InvalidArgument, "invalid social account environment");
    auto scope = scope_key(environment);
    auto hashed = account_key(account, scope);
    if (!hashed)
        return hashed.error();
    return impl_->run<std::optional<SocialAccountBinding>>(
        [environment, scope = std::move(scope), account = hashed.value().to_string()](sqlite3 *db) {
            return read_account_binding(db, environment, scope, account);
        });
}

Result<SocialAccountBinding>
SqliteSocialDirectory::observe_account(const SocialEnvironmentKey &environment,
                                       const SocialAccountKey &account, EventId source_event,
                                       std::optional<SocialObjectKey> known_object) {
    if (!valid_environment(environment) || source_event.is_nil() ||
        (known_object && known_object->object.is_nil()))
        return directory_error(ErrorCode::InvalidArgument, "invalid social account observation");
    if (known_object && known_object->environment != environment)
        return directory_error(ErrorCode::PermissionDenied,
                               "known object is outside the social environment");
    auto scope = scope_key(environment);
    auto hashed = account_key(account, scope);
    if (!hashed)
        return hashed.error();
    return impl_->run<SocialAccountBinding>(
        [state = impl_.get(), environment, scope = std::move(scope),
         account = hashed.value().to_string(), source_event,
         known_object](sqlite3 *db) -> Result<SocialAccountBinding> {
            Transaction transaction(db);
            if (!transaction.valid())
                return directory_error(ErrorCode::Unavailable,
                                       "social account transaction unavailable");
            auto admitted = admit_account(db, state->options, environment, scope, account,
                                          source_event, known_object);
            if (!admitted)
                return admitted.error();
            if (auto committed = transaction.commit(); !committed)
                return committed.error();
            return admitted;
        });
}

Result<std::optional<SocialAccountAttributeRecall>>
SqliteSocialDirectory::recall_account_attribute(const SocialEnvironmentKey &environment,
                                                const SocialAccountKey &account, std::string field,
                                                std::size_t limit) const {
    if (!is_social_attribute_field(field) || limit == 0 || limit > 64)
        return directory_error(ErrorCode::InvalidArgument,
                               "invalid social account attribute query");
    if (!impl_->memory)
        return directory_error(ErrorCode::InvalidState,
                               "social profile recall needs a paired store");
    const auto binding = resolve_account(environment, account);
    if (!binding)
        return binding.error();
    if (!binding.value())
        return std::optional<SocialAccountAttributeRecall>{};
    auto scope = social_object_scope(binding.value()->object);
    if (!scope)
        return scope.error();
    MemoryQuery query;
    query.scopes = {std::move(scope).value()};
    query.social_attribute_field = std::move(field);
    query.max_results = limit;
    auto recalled = impl_->memory->query(query);
    if (!recalled)
        return recalled.error();
    const auto current = resolve_account(environment, account);
    if (!current)
        return current.error();
    if (current.value() != binding.value())
        return directory_error(ErrorCode::InvalidState,
                               "social account binding changed during recall");
    return std::optional{
        SocialAccountAttributeRecall{*current.value(), std::move(recalled).value()}};
}

Result<SocialMessageEvidence> SqliteSocialDirectory::index_message(
    const SocialEnvironmentKey &environment, const SocialAccountKey &sender,
    const SocialMessageLocator &message, const IEventStore &events) {
    if (!valid_environment(environment) || message.session.is_nil() || message.event.is_nil() ||
        !valid_message_sequence(message.sequence))
        return directory_error(ErrorCode::InvalidArgument, "invalid social message source");
    auto scope = scope_key(environment);
    const auto account = account_key(sender, scope);
    if (!account)
        return account.error();
    EventSliceQuery query;
    query.session_id = message.session;
    query.after_sequence = message.sequence - 1;
    query.through_sequence = message.sequence;
    query.limit = 1;
    query.scan_limit = 1;
    const auto found = events.read_slice(query);
    if (!found)
        return found.error();
    const auto &page = found.value();
    if (page.events.size() != 1 || page.snapshot_through != message.sequence ||
        page.scanned_through != message.sequence || page.has_more)
        return directory_error(ErrorCode::DataLoss, "social message event proof is incomplete");
    const auto &event = page.events.front();
    if (event.session_id != message.session || event.session_sequence != message.sequence ||
        event.event_id != message.event || event.integrity == Sha256Digest{})
        return directory_error(ErrorCode::DataLoss,
                               "social message event does not match its source");
    return impl_->run<SocialMessageEvidence>(
        [state = impl_.get(), environment, scope = std::move(scope), account = account.value(),
         source = sender.source, message, integrity = event.integrity](sqlite3 *db) {
            return index_message_evidence(db, state->options, environment, scope, account, source,
                                          message, integrity);
        });
}

Result<SocialMessagePage>
SqliteSocialDirectory::read_message_evidence(const SocialEnvironmentKey &environment,
                                             SessionId session, SessionSequence after,
                                             SessionSequence through, std::size_t limit) const {
    if (!valid_environment(environment) || session.is_nil() || !valid_message_sequence(through) ||
        after > through || limit == 0 || limit > 64)
        return directory_error(ErrorCode::InvalidArgument, "invalid social message evidence query");
    return impl_->run<SocialMessagePage>([environment, scope = scope_key(environment),
                                          session_key = session.to_string(), after, through,
                                          limit](sqlite3 *db) -> Result<SocialMessagePage> {
        Statement rows(db, "SELECT session,sequence,event,object,source,account_key,integrity"
                           " FROM social_messages WHERE scope=?1 AND session=?2"
                           " AND sequence>?3 AND sequence<=?4 ORDER BY sequence LIMIT ?5");
        if (!rows.valid() || !bind_text(rows, 1, scope) || !bind_text(rows, 2, session_key) ||
            sqlite3_bind_int64(rows.get(), 3, static_cast<std::int64_t>(after)) != SQLITE_OK ||
            sqlite3_bind_int64(rows.get(), 4, static_cast<std::int64_t>(through)) != SQLITE_OK ||
            sqlite3_bind_int64(rows.get(), 5, static_cast<std::int64_t>(limit + 1)) != SQLITE_OK)
            return directory_error(ErrorCode::Internal, "social message query preparation failed");
        SocialMessagePage result;
        int step = SQLITE_OK;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
            if (result.evidence.size() == limit) {
                result.has_more = true;
                return result;
            }
            auto decoded = decode_message(rows, environment);
            if (!decoded)
                return decoded.error();
            result.evidence.push_back(std::move(decoded).value());
        }
        if (step != SQLITE_DONE)
            return directory_error(ErrorCode::Internal, "social message query failed");
        return result;
    });
}

Result<SocialMessageParticipants> SqliteSocialDirectory::resolve_message_participants(
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    std::span<const SocialMentionReference> mentions, std::size_t alias_limit) const {
    if (!valid_environment(environment) || message.session.is_nil() || message.event.is_nil() ||
        !valid_message_sequence(message.sequence) || mentions.size() > 16 || alias_limit == 0 ||
        alias_limit > 64)
        return directory_error(ErrorCode::InvalidArgument,
                               "invalid social message participants query");
    auto scope = scope_key(environment);
    const auto forbidden = default_forbidden_content_markers();
    std::vector<PreparedMention> prepared;
    prepared.reserve(mentions.size());
    for (const auto &mention : mentions) {
        if (const auto *account = std::get_if<SocialAccountKey>(&mention)) {
            const auto hashed = account_key(*account, scope);
            if (!hashed)
                return hashed.error();
            prepared.emplace_back(hashed.value());
        } else if (const auto *quoted = std::get_if<SocialMessageLocator>(&mention)) {
            if (quoted->session != message.session || quoted->sequence >= message.sequence ||
                quoted->sequence == 0 || quoted->event.is_nil() || quoted->event == message.event)
                return directory_error(
                    ErrorCode::InvalidArgument,
                    "quoted message must precede the current one in the same session");
            prepared.emplace_back(*quoted);
        } else {
            auto normalized = alias_key(std::get<std::string>(mention), forbidden);
            if (!normalized)
                return normalized.error();
            prepared.emplace_back(std::move(normalized).value());
        }
    }
    return impl_->run<SocialMessageParticipants>(
        [environment, message, scope = std::move(scope), prepared = std::move(prepared),
         alias_limit](sqlite3 *db) -> Result<SocialMessageParticipants> {
            auto speaker = find_message_at(db, environment, scope, message);
            if (!speaker)
                return speaker.error();
            if (!speaker.value())
                return directory_error(ErrorCode::NotFound,
                                       "social message speaker is not indexed");
            SocialMessageParticipants resolved{std::move(*speaker.value()), {}};
            resolved.mentions.reserve(prepared.size());
            for (const auto &mention : prepared) {
                auto found = find_mention(db, environment, scope, mention, alias_limit);
                if (!found)
                    return found.error();
                resolved.mentions.push_back(std::move(found).value());
            }
            return resolved;
        });
}

Result<SocialMessageAttributeReport> SqliteSocialDirectory::record_inferred_message_attributes(
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    const IEventStore &events, std::span<const SocialMessageAttributeCandidate> attributes,
    const OperationContext &context) {
    if (context.session.is_nil() || context.session != message.session)
        return directory_error(ErrorCode::PermissionDenied,
                               "social attribute recording needs its authorized session");
    if (!impl_->memory)
        return directory_error(ErrorCode::InvalidState,
                               "social attribute recording needs a paired store");
    if (auto active = check_profile_recording(context); !active)
        return active.error();
    const auto indexed = resolve_message_participants(environment, message, {});
    if (!indexed)
        return indexed.error();
    const auto &speaker = indexed.value().speaker;
    const auto event = storage::read_social_message_source(events, speaker);
    if (!event)
        return event.error();
    auto prepared = storage::prepare_social_message_attributes(speaker, event.value(), attributes);
    if (!prepared)
        return prepared.error();
    auto report = commit_inferred_message_candidates(environment, indexed.value(), {},
                                                     std::move(prepared).value(), context);
    if (!report)
        return report.error();
    return SocialMessageAttributeReport{speaker, std::move(report).value()};
}

Result<ConsolidationReport> SqliteSocialDirectory::commit_inferred_message_candidates(
    const SocialEnvironmentKey &environment, const SocialMessageParticipants &indexed,
    std::span<const SocialMentionReference> references, std::vector<MemoryCandidate> prepared,
    const OperationContext &context) {
    if (!impl_->memory)
        return directory_error(ErrorCode::InvalidState,
                               "social profile recording needs a paired store");
    const std::unique_lock serialize(impl_->erasure_mutex, std::try_to_lock);
    if (!serialize.owns_lock())
        return profile_write_busy();
    const auto current =
        resolve_message_participants(environment, indexed.speaker.message, references);
    if (!current)
        return current.error();
    if (current.value() != indexed)
        return directory_error(ErrorCode::InvalidState, "social profile source was invalidated");
    if (auto active = check_profile_recording(context); !active)
        return active.error();
    ConsolidationReport report;
    const MemoryConsolidator consolidator;
    for (auto &candidate : prepared) {
        if (auto active = check_profile_recording(context); !active)
            return active.error();
        const auto scope = candidate.proposed.scope;
        std::vector<MemoryCandidate> batch;
        batch.push_back(std::move(candidate));
        auto consolidated = consolidator.consolidate_candidates(*impl_->memory, std::move(batch),
                                                                scope, Timestamp::now());
        if (!consolidated)
            return consolidated.error();
        auto &completed = consolidated.value();
        report.candidates_examined += completed.candidates_examined;
        report.candidates_truncated += completed.candidates_truncated;
        std::ranges::move(completed.entries, std::back_inserter(report.entries));
        std::ranges::move(completed.pending_approval, std::back_inserter(report.pending_approval));
    }
    return report;
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

Result<void> SqliteSocialDirectory::close() noexcept {
    const std::lock_guard serialize(impl_->erasure_mutex);
    return impl_->channel.close();
}

} // namespace mira
