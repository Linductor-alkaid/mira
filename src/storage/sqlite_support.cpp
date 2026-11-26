#include "sqlite_support.hpp"

#include <mira/text_segmentation.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mira::storage {

// ---------------------------------------------------------------------------
// Database helpers
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string quote_fts_term(std::string_view term) {
    std::string result = "\"";
    for (const char character : term) {
        if (character == '"') {
            result += "\"\"";
        }
        result += character;
    }
    result += '"';
    return result;
}

// A unit as an FTS5 operand. A bigram is the phrase of its two characters,
// which a character index matches by position.
[[nodiscard]] std::string quoted_unit(std::string_view unit, LexicalUnitKind kind) {
    if (kind != LexicalUnitKind::Bigram) {
        return quote_fts_term(unit);
    }
    const auto lead = static_cast<unsigned char>(unit.front());
    const std::size_t first = (lead & 0xF0U) == 0xE0U   ? 3
                              : (lead & 0xF8U) == 0xF0U ? 4
                              : (lead & 0xE0U) == 0xC0U ? 2
                                                        : 1;
    std::string phrase(unit);
    phrase.insert(first, 1, ' ');
    return quote_fts_term(phrase);
}

} // namespace

FtsMatch fts_match_expression(const std::string &text, bool conversation) {
    const auto units = segment_lexically(text, LexicalUnits::MatchUnits);
    // Unique units per kind in text order. A full-text index gives a
    // character that many rows share no weight, so a query too long to match
    // completely gives characters up first and words last.
    std::vector<std::string_view> words;
    std::vector<std::string_view> characters;
    std::vector<std::string_view> bigrams;
    std::set<std::string_view> seen;
    for (const auto &unit : units) {
        if (conversation && (unit.text.find(kConversationFtsBoundary) != std::string::npos ||
                             unit.text.find_first_not_of('_') == std::string::npos))
            continue;
        if (!seen.insert(unit.text).second) {
            continue;
        }
        switch (unit.kind) {
        case LexicalUnitKind::Word:
            words.push_back(unit.text);
            break;
        case LexicalUnitKind::Character:
            characters.push_back(unit.text);
            break;
        case LexicalUnitKind::Bigram:
            bigrams.push_back(unit.text);
            break;
        }
    }
    if (conversation && (!words.empty() || !bigrams.empty()))
        characters.clear();
    FtsMatch match;
    std::size_t budget = kMaxFtsMatchUnits;
    const std::pair<const std::vector<std::string_view> *, LexicalUnitKind> groups[] = {
        {&words, LexicalUnitKind::Word},
        {&bigrams, LexicalUnitKind::Bigram},
        {&characters, LexicalUnitKind::Character},
    };
    for (const auto &[group, kind] : groups) {
        const std::size_t taken = std::min(budget, group->size());
        match.truncated = match.truncated || taken < group->size();
        // Units that do not all fit are spread over the whole text instead of
        // being taken from its beginning.
        const std::size_t stride = taken == 0 ? 1 : (group->size() + taken - 1) / taken;
        for (std::size_t index = 0; index < group->size() && taken != 0; index += stride) {
            if (!match.expression.empty()) {
                match.expression += " OR ";
            }
            match.expression += quoted_unit((*group)[index], kind);
            if (conversation)
                match.terms.emplace_back((*group)[index]);
        }
        budget -= taken;
    }
    return match;
}

std::string sqlite_error_message(int code, sqlite3 *database) {
    std::string message = "sqlite error ";
    message += std::to_string(code);
    if (database != nullptr) {
        const char *text = sqlite3_errmsg(database);
        if (text != nullptr) {
            message += ": ";
            message += text;
        }
    }
    return message;
}

namespace {

[[nodiscard]] Error db_error(const char *stage, int code, sqlite3 *database) {
    Error error;
    error.code = ErrorCode::Internal;
    error.domain = "mira.state_store";
    error.safe_message = std::string(stage) + " failed: " + sqlite_error_message(code, database);
    return error;
}

} // namespace

Result<sqlite3 *> open_database(const DbOpenOptions &options) {
    const int flags = options.mode == DbOpenMode::ReadWrite
                          ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                          : SQLITE_OPEN_READONLY;
    sqlite3 *database = nullptr;
    const int open_result =
        sqlite3_open_v2(options.path.string().c_str(), &database, flags, options.vfs_name);
    if (open_result != SQLITE_OK) {
        const std::string message =
            open_result == SQLITE_CANTOPEN
                ? "store database cannot be opened (path unreadable or not a database)"
                : "store database open failed";
        Error error;
        error.code = open_result == SQLITE_CANTOPEN ? ErrorCode::NotFound : ErrorCode::Internal;
        error.domain = "mira.state_store";
        error.safe_message = message + ": " + sqlite_error_message(open_result, database);
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
        return error;
    }
    // A garbage file opens lazily; the first pragma read fails immediately and
    // the file is left untouched (initialization never wipes or rewrites).
    const char *probe =
        options.mode == DbOpenMode::ReadWrite
            ? (options.exclusive_owner ? "PRAGMA locking_mode=EXCLUSIVE;PRAGMA temp_store=MEMORY;"
                                         "PRAGMA mmap_size=0;PRAGMA journal_mode=WAL;"
                                       : "PRAGMA journal_mode=WAL;")
            : "PRAGMA query_only=ON;";
    char *probe_error = nullptr;
    const int probe_result = sqlite3_exec(database, probe, nullptr, nullptr, &probe_error);
    if (probe_result != SQLITE_OK) {
        std::string message = "store database is not usable: ";
        message +=
            probe_error != nullptr ? probe_error : sqlite_error_message(probe_result, database);
        sqlite3_free(probe_error);
        sqlite3_close_v2(database);
        Error error;
        error.code =
            probe_result == SQLITE_NOTADB ? ErrorCode::InvalidArgument : ErrorCode::Internal;
        error.domain = "mira.state_store";
        error.safe_message = std::move(message);
        return error;
    }
    sqlite3_busy_timeout(database, static_cast<int>(options.busy_timeout.count()));
    return database;
}

DatabaseHandle::~DatabaseHandle() {
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

DatabaseHandle::DatabaseHandle(DatabaseHandle &&other) noexcept
    : database_(std::exchange(other.database_, nullptr)) {}

DatabaseHandle &DatabaseHandle::operator=(DatabaseHandle &&other) noexcept {
    if (this != &other) {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

Statement::Statement(sqlite3 *database, const char *sql) {
    const int result = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    if (result != SQLITE_OK) {
        statement_ = nullptr; // prepare failure leaves no statement to free
    }
}

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

void Statement::reset() noexcept {
    if (statement_ != nullptr) {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }
}

Transaction::Transaction(sqlite3 *database) : database_(database) {
    if (database_ != nullptr &&
        sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        database_ = nullptr; // leave valid() false; caller reports the failure
    }
}

Transaction::~Transaction() {
    if (database_ != nullptr && !committed_) {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

Result<void> Transaction::commit() {
    if (database_ == nullptr || committed_) {
        return db_error("transaction commit", SQLITE_ERROR, database_);
    }
    char *message = nullptr;
    const int result = sqlite3_exec(database_, "COMMIT", nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string text = message != nullptr ? message : sqlite_error_message(result, database_);
        sqlite3_free(message);
        Error error;
        error.code = ErrorCode::Internal;
        error.domain = "mira.state_store";
        error.safe_message = "transaction commit failed: " + text;
        return error;
    }
    committed_ = true;
    return Result<void>{};
}

Result<void> exec_sql(sqlite3 *database, const char *sql) {
    char *message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string text = message != nullptr ? message : sqlite_error_message(result, database);
        sqlite3_free(message);
        Error error;
        error.code = ErrorCode::Internal;
        error.domain = "mira.state_store";
        error.safe_message = "sql execution failed: " + text;
        return error;
    }
    return Result<void>{};
}

Result<std::int64_t> last_insert_rowid(sqlite3 *database) noexcept {
    return sqlite3_last_insert_rowid(database);
}

Result<std::optional<std::string>> meta_read(sqlite3 *database, const std::string &key) {
    Statement statement(database, "SELECT value FROM store_meta WHERE key = ?1");
    if (!statement.valid()) {
        return db_error("meta read prepare", SQLITE_ERROR, database);
    }
    sqlite3_bind_text(statement.get(), 1, key.c_str(), -1, storage::transient_copy());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_ROW) {
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        return std::optional<std::string>(text != nullptr ? text : "");
    }
    if (step == SQLITE_DONE) {
        return std::optional<std::string>{};
    }
    return db_error("meta read", step, database);
}

Result<void> meta_write(sqlite3 *database, const std::string &key, const std::string &value) {
    Statement statement(database, "INSERT INTO store_meta(key, value) VALUES(?1, ?2) "
                                  "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!statement.valid()) {
        return db_error("meta write prepare", SQLITE_ERROR, database);
    }
    sqlite3_bind_text(statement.get(), 1, key.c_str(), -1, storage::transient_copy());
    sqlite3_bind_text(statement.get(), 2, value.c_str(), -1, storage::transient_copy());
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_DONE) {
        return db_error("meta write", step, database);
    }
    return Result<void>{};
}

Result<std::vector<std::string>> list_tables(sqlite3 *database) {
    Statement statement(
        database,
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
    if (!statement.valid()) {
        return db_error("table list prepare", SQLITE_ERROR, database);
    }
    std::vector<std::string> tables;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        if (text != nullptr) {
            tables.emplace_back(text);
        }
    }
    if (step != SQLITE_DONE) {
        return db_error("table list", step, database);
    }
    std::sort(tables.begin(), tables.end());
    return tables;
}

// ---------------------------------------------------------------------------
// StoreChannel
// ---------------------------------------------------------------------------

// The Executor owns and deletes this object; it only references ChannelShared.
namespace {

class LoopWorker final : public kairo::IBlockingIoWorker {
  public:
    explicit LoopWorker(std::shared_ptr<ChannelShared> shared) : shared_(std::move(shared)) {}

    void run(kairo::StopToken stop) override {
        auto &state = *shared_;
        {
            std::lock_guard lock(state.mutex);
            state.worker_thread = std::this_thread::get_id();
        }
        std::unique_lock lock(state.mutex);
        while (true) {
            state.work_cv.wait(lock, [&state, &stop] {
                return !state.pending.empty() || state.closing || stop.stop_requested();
            });
            while (!state.pending.empty()) {
                // Maintenance pause holds queued work; close and stop win.
                while (state.paused && !state.closing && !stop.stop_requested()) {
                    state.work_cv.wait_for(lock, std::chrono::milliseconds(50));
                }
                auto work = std::move(state.pending.front());
                state.pending.pop_front();
                lock.unlock();
                work(state.database);
                lock.lock();
            }
            if (state.closing || stop.stop_requested()) {
                break;
            }
        }
        state.worker_done = true; // still under lock
        lock.unlock();
        state.done_cv.notify_all();
        state.work_cv.notify_all();
    }

    void wakeup() noexcept override { shared_->work_cv.notify_all(); }

  private:
    std::shared_ptr<ChannelShared> shared_;
};

} // namespace

StoreChannel::StoreChannel(kairo::Executor &executor, sqlite3 *database, Config config)
    : shared_(std::make_shared<ChannelShared>()), config_(std::move(config)) {
    static std::atomic<std::uint64_t> channel_counter{0};
    // Executor worker registration is keyed by name; every channel instance
    // (including reopen cycles over one file) needs a distinct identity.
    const std::string unique_name =
        config_.worker_name + "-" + std::to_string(channel_counter.fetch_add(1));
    config_.worker_name = unique_name;
    shared_->database = database;
    kairo::BlockingWorkerSpec spec;
    spec.name = unique_name;
    spec.config.thread_name = unique_name;
    spec.worker = std::make_unique<LoopWorker>(shared_);
    handle_ = executor.start_worker(std::move(spec));
    if (!handle_->started()) {
        std::lock_guard lock(shared_->mutex);
        shared_->accepting = false;
        shared_->worker_done = true;
    }
}

StoreChannel::~StoreChannel() { (void)close(); }

Error StoreChannel::reject(const char *message, ErrorCode code) const {
    Error error;
    error.code = code;
    error.domain = "mira.state_store";
    error.safe_message = message;
    return error;
}

void StoreChannel::set_paused(bool paused) {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->paused = paused;
    }
    shared_->work_cv.notify_all();
}

Result<void> StoreChannel::close() noexcept {
    try {
        {
            std::lock_guard lock(shared_->mutex);
            shared_->accepting = false;
            shared_->closing = true;
        }
        shared_->work_cv.notify_all();
        if (handle_.has_value() && handle_->started()) {
            handle_->request_stop();
        }
        std::unique_lock lock(shared_->mutex);
        const bool drained = shared_->done_cv.wait_for(lock, std::chrono::seconds(10),
                                                       [this] { return shared_->worker_done; });
        if (!drained) {
            return reject("store worker did not settle during close", ErrorCode::DeadlineExceeded);
        }
        return Result<void>{};
    } catch (...) {
        return reject("store close failed", ErrorCode::Internal);
    }
}

bool StoreChannel::closed() const noexcept {
    std::lock_guard lock(shared_->mutex);
    return !shared_->accepting;
}

std::size_t StoreChannel::pending_count() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->pending.size();
}

Error store_error(ErrorCode code, std::string domain, std::int32_t domain_code, std::string message,
                  bool retryable) {
    Error error;
    error.code = code;
    error.domain = std::move(domain);
    error.domain_code = domain_code;
    error.retryable = retryable;
    error.safe_message = std::move(message);
    return error;
}

} // namespace mira::storage
