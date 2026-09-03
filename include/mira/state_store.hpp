#pragma once

#include <mira/core_contracts.hpp>
#include <mira/json.hpp>
#include <mira/task_checkpoint.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// ---------------------------------------------------------------------------
// Durable SQLite/WAL checkpoint store (M4-06)
// ---------------------------------------------------------------------------

enum class StoreSchemaDisposition : std::uint8_t {
    Created,          // fresh database, schema installed at the current version
    UpToDate,         // existing database already at the current schema
    Migrated,         // explicit migration ran from an older compatible schema
    ReadOnlyDiagnostic, // schema newer than this reader: reads only, writes fail
};

[[nodiscard]] std::string store_schema_disposition_name(StoreSchemaDisposition disposition);

struct SqliteStoreOptions final {
    std::filesystem::path path;
    // Read-only diagnostic mode: queries and diagnostics work, every write is
    // rejected. Used for incompatible or quarantined databases.
    bool read_only_diagnostic = false;
    // Bounded single-writer channel capacity; submissions beyond it fail with
    // ResourceExhausted instead of queueing without bound.
    std::size_t max_pending_requests = 256;
    std::size_t max_checkpoints_per_task = 64;
    std::chrono::milliseconds busy_timeout{5'000};
    std::chrono::milliseconds operation_timeout{30'000};

    [[nodiscard]] Result<void> validate() const;
};

// Readable store state for operators: what schema the file carries, what this
// reader supports and how the connection was opened. Never includes task data.
struct StoreDiagnostics final {
    StoreSchemaDisposition disposition = StoreSchemaDisposition::Created;
    SchemaVersion file_schema{};
    SchemaVersion reader_schema{};
    bool read_only = false;
    std::string journal_mode;
    std::int64_t page_count = 0;
    std::string note;
};

JsonValue store_diagnostics_to_json(const StoreDiagnostics &diagnostics);

// ICheckpointStore over SQLite with WAL journaling. All work runs on one
// dedicated Executor blocking-I/O worker (single writer, bounded admission);
// initialization and migration happen in open(), never implicitly on a store
// request, and a failed initialization never wipes or rewrites the file.
class SqliteCheckpointStore final : public ICheckpointStore {
  public:
    // Factory so open/migration failures surface as Result errors. The
    // Executor must outlive the store; close it from a host (non-worker)
    // thread before Executor shutdown or store destruction.
    [[nodiscard]] static Result<std::unique_ptr<SqliteCheckpointStore>>
    open(executor::Executor &executor, SqliteStoreOptions options);
    ~SqliteCheckpointStore() override;

    SqliteCheckpointStore(const SqliteCheckpointStore &) = delete;
    SqliteCheckpointStore &operator=(const SqliteCheckpointStore &) = delete;

    Result<void> put(const TaskCheckpoint &checkpoint) override;
    [[nodiscard]] Result<std::optional<TaskCheckpoint>> latest(TaskId task) const override;
    [[nodiscard]] Result<std::optional<TaskCheckpoint>>
    latest_at_or_before(TaskId task, std::uint64_t max_sequence) const override;
    [[nodiscard]] Result<std::size_t> count(TaskId task) const override;
    Result<std::size_t> erase_task(TaskId task, std::string reason) override;

    [[nodiscard]] const StoreDiagnostics &diagnostics() const noexcept;
    // Queued-but-not-executed request count (bounded-queue observability).
    [[nodiscard]] std::size_t pending_requests() const;
    // Maintenance seam: the writer holds queued work while paused; used to
    // observe bounded-queue rejection deterministically.
    void set_worker_paused(bool paused);
    // Drains accepted work and stops the writer thread. Idempotent.
    Result<void> close() noexcept;

  private:
    class Impl;
    explicit SqliteCheckpointStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
