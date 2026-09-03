// Internal support for the SQLite-backed reference stores (M4-06/M4-09).
// Not installed and not part of the public API; sqlite3 types never leak past
// this boundary or the store implementations.

#pragma once

#include <mira/core_contracts.hpp>

#include <sqlite3.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <executor/blocking_io.hpp>
#include <executor/executor.hpp>

namespace mira::storage {

// ---------------------------------------------------------------------------
// Database handle
// ---------------------------------------------------------------------------

enum class DbOpenMode : std::uint8_t { ReadWrite, ReadOnlyDiagnostic };

struct DbOpenOptions final {
    std::filesystem::path path;
    DbOpenMode mode = DbOpenMode::ReadWrite;
    std::chrono::milliseconds busy_timeout{5'000};
};

[[nodiscard]] std::string sqlite_error_message(int code, sqlite3 *database);

// SQLITE_TRANSIENT expands to an old-style cast; wrap the constant once so
// Mira warning policy stays clean at every bind site.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
[[nodiscard]] inline sqlite3_destructor_type transient_copy() noexcept {
    return SQLITE_TRANSIENT;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Opens (never creates implicitly when read-only) and applies WAL pragmas.
// Fails closed on a non-database file; the caller decides how to surface it.
[[nodiscard]] Result<sqlite3 *> open_database(const DbOpenOptions &options);

// RAII closer so every failure path releases the handle exactly once.
class DatabaseHandle final {
  public:
    DatabaseHandle() noexcept = default;
    explicit DatabaseHandle(sqlite3 *database) noexcept : database_(database) {}
    ~DatabaseHandle();
    DatabaseHandle(const DatabaseHandle &) = delete;
    DatabaseHandle &operator=(const DatabaseHandle &) = delete;
    DatabaseHandle(DatabaseHandle &&other) noexcept;
    DatabaseHandle &operator=(DatabaseHandle &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return database_ != nullptr; }
    [[nodiscard]] sqlite3 *get() const noexcept { return database_; }

  private:
    sqlite3 *database_ = nullptr;
};

// Minimal statement guard; keeps SQL plumbing explicit at call sites.
class Statement final {
  public:
    Statement() noexcept = default;
    Statement(sqlite3 *database, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }
    [[nodiscard]] sqlite3_stmt *get() const noexcept { return statement_; }
    void reset() noexcept;

  private:
    sqlite3_stmt *statement_ = nullptr;
};

// RAII write transaction: BEGIN IMMEDIATE on the single writer connection;
// any early return rolls back, commit() closes it exactly once.
class Transaction final {
  public:
    Transaction() noexcept = default;
    explicit Transaction(sqlite3 *database);
    ~Transaction();

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    [[nodiscard]] bool valid() const noexcept { return database_ != nullptr; }
    [[nodiscard]] Result<void> commit();

  private:
    sqlite3 *database_ = nullptr;
    bool committed_ = false;
};

[[nodiscard]] Result<void> exec_sql(sqlite3 *database, const char *sql);
[[nodiscard]] Result<std::int64_t> last_insert_rowid(sqlite3 *database) noexcept;

// store_meta helpers: one row per key, values are plain text.
[[nodiscard]] Result<std::optional<std::string>> meta_read(sqlite3 *database, const std::string &key);
[[nodiscard]] Result<void> meta_write(sqlite3 *database, const std::string &key,
                                      const std::string &value);

// Lists the tables present in the main database (excluding SQLite internals).
[[nodiscard]] Result<std::vector<std::string>> list_tables(sqlite3 *database);

// ---------------------------------------------------------------------------
// Single-writer bounded channel
// ---------------------------------------------------------------------------

// Move-only type-erased store operation: queued work owns its promise and
// cannot rely on std::function's copyable-target requirement.
class ChannelWork final {
  public:
    template <typename F>
    ChannelWork(F fn) : fn_(std::make_unique<Model<F>>(std::move(fn))) {}
    ChannelWork(ChannelWork &&) noexcept = default;
    ChannelWork &operator=(ChannelWork &&) noexcept = default;
    void operator()(sqlite3 *db) { fn_->invoke(db); }

  private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke(sqlite3 *database) = 0;
    };
    template <typename F>
    struct Model final : Concept {
        explicit Model(F callable) : fn(std::move(callable)) {}
        void invoke(sqlite3 *database) override { fn(database); }
        F fn;
    };

    std::unique_ptr<Concept> fn_;
};

// State shared between the channel facade and the Executor-owned loop
// object; the shared_ptr keeps it alive for late wakeup() calls the Executor
// may issue after the facade is gone.
struct ChannelShared final {
    sqlite3 *database = nullptr;
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable done_cv;
    std::deque<ChannelWork> pending;
    bool accepting = true;
    bool closing = false;
    bool paused = false;
    bool worker_done = false;
    std::thread::id worker_thread{};
};

// Every SQLite reference store serializes its work through one dedicated
// Executor blocking-I/O worker (design Context/Memory §16/§17): the worker
// thread owns the only connection, admission is bounded, and accepted work
// always settles before the worker exits (critical writes are never dropped
// on close or Executor shutdown).
class StoreChannel final {
  public:
    struct Config final {
        std::string worker_name;
        std::size_t max_pending = 256;
        std::chrono::milliseconds operation_timeout{30'000};
    };

    // The Executor must outlive the channel; the database must outlive every
    // accepted operation (close it after close()).
    StoreChannel(executor::Executor &executor, sqlite3 *database, Config config);
    ~StoreChannel();

    StoreChannel(const StoreChannel &) = delete;
    StoreChannel &operator=(const StoreChannel &) = delete;

    // Runs `op` on the writer thread and waits for its result. Rejects with
    // Unavailable after close, ResourceExhausted when the bounded queue is
    // full, InvalidState on worker self-call, DeadlineExceeded on timeout.
    template <typename T>
    [[nodiscard]] Result<T> run(std::function<Result<T>(sqlite3 *)> op);

    // Maintenance seam: the worker holds queued work until unpaused. Admission
    // keeps working, so pausing is how tests and maintenance observe the
    // bounded queue instead of racing on timing.
    void set_paused(bool paused);

    // Stops admission, wakes the worker, drains accepted work and joins it.
    // Idempotent; must be called from a non-worker thread.
    Result<void> close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    // Queued-but-not-executed request count (backpressure observability).
    [[nodiscard]] std::size_t pending_count() const;

  private:
    [[nodiscard]] Error reject(const char *message, ErrorCode code) const;

    std::shared_ptr<ChannelShared> shared_;
    Config config_;
    std::optional<executor::WorkerHandle> handle_;
};

[[nodiscard]] inline Error channel_error(const char *message, ErrorCode code) {
    Error error;
    error.code = code;
    error.domain = "mira.state_store";
    error.safe_message = message;
    return error;
}

// Runs one operation with exception isolation; store failures surface as
// Result errors, never as unhandled exceptions on the worker thread.
template <typename T>
[[nodiscard]] Result<T> invoke_guarded(const std::function<Result<T>(sqlite3 *)> &op,
                                       sqlite3 *db) noexcept {
    try {
        return op(db);
    } catch (const std::bad_alloc &) {
        return channel_error("store operation ran out of memory", ErrorCode::ResourceExhausted);
    } catch (...) {
        return channel_error("store operation failed", ErrorCode::Internal);
    }
}

template <typename T>
Result<T> StoreChannel::run(std::function<Result<T>(sqlite3 *)> op) {
    std::promise<Result<T>> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lock(shared_->mutex);
        if (std::this_thread::get_id() == shared_->worker_thread) {
            return channel_error("store operation issued from the store worker itself",
                                 ErrorCode::InvalidState);
        }
        if (!shared_->accepting || shared_->worker_done) {
            return channel_error("store channel no longer accepts work", ErrorCode::Unavailable);
        }
        if (shared_->pending.size() >= config_.max_pending) {
            return channel_error("store request queue is full", ErrorCode::ResourceExhausted);
        }
        shared_->pending.push_back(
            ChannelWork([promise = std::move(promise), op = std::move(op)](sqlite3 *db) mutable {
                promise.set_value(invoke_guarded(op, db));
            }));
    }
    shared_->work_cv.notify_one();
    if (future.wait_for(config_.operation_timeout) != std::future_status::ready) {
        return channel_error("store operation timed out", ErrorCode::DeadlineExceeded);
    }
    return future.get();
}

// Shared error helper for store implementations.
[[nodiscard]] Error store_error(enum ErrorCode code, std::string domain,
                                std::int32_t domain_code, std::string message,
                                bool retryable = false);

} // namespace mira::storage
