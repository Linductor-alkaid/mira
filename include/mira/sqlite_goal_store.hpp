#pragma once

#include <mira/core_contracts.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kairo {
class Executor;
}

namespace mira {

using GoalTime = std::chrono::sys_time<std::chrono::milliseconds>;

struct GoalOwner final {
    TenantId tenant;
    UserId user;
    friend bool operator==(const GoalOwner &, const GoalOwner &) = default;
};

struct GoalKey final {
    GoalOwner owner;
    Id128 id;
    friend bool operator==(const GoalKey &, const GoalKey &) = default;
};

enum class GoalKind : std::uint8_t { Task, Commitment };
enum class GoalState : std::uint8_t { Pending, Fulfilled, Cancelled, Expired, Erased };

struct GoalDefinition final {
    GoalKind kind = GoalKind::Task;
    std::string title;
    EventId source_event;
    GoalTime created_at{};
    std::optional<GoalTime> due_at;
    std::optional<GoalTime> expires_at;
    friend bool operator==(const GoalDefinition &, const GoalDefinition &) = default;
};

struct GoalResolution final {
    GoalState state = GoalState::Fulfilled;
    EventId evidence;
    GoalTime at{};
    friend bool operator==(const GoalResolution &, const GoalResolution &) = default;
};

struct GoalRecord final {
    GoalKey key;
    GoalState state = GoalState::Pending;
    std::uint64_t revision = 1;
    std::optional<GoalDefinition> definition; // Absent after erasure.
    std::optional<GoalResolution> resolution;
};

struct GoalDueCursor final {
    GoalTime due_at;
    Id128 goal;
};

struct GoalDueQuery final {
    GoalOwner owner;
    GoalTime cutoff;
    std::size_t limit = 32;
    std::optional<GoalDueCursor> after;
};

struct GoalDuePage final {
    std::vector<GoalRecord> records;
    std::optional<GoalDueCursor> next; // Reuse the owner and cutoff on the next page.
};

struct GoalListQuery final {
    GoalOwner owner;
    std::optional<GoalState> state;
    std::size_t limit = 32;
    std::optional<Id128> after;
};

struct GoalListPage final {
    std::vector<GoalRecord> records;
    std::optional<Id128> next;
};

struct SqliteGoalStoreOptions final {
    std::filesystem::path path;
    std::size_t max_goals = 4096; // Includes erased IDs.
    std::size_t max_pending_per_owner = 64;
    std::size_t max_pending_requests = 32;
    std::chrono::milliseconds operation_timeout{30'000};
};

// The host authenticates the exact owner and authorizes each operation. Due
// records describe work; their presence does not authorize its execution.
class SqliteGoalStore final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteGoalStore>>
    open(kairo::Executor &executor, SqliteGoalStoreOptions options);
    ~SqliteGoalStore();

    SqliteGoalStore(const SqliteGoalStore &) = delete;
    SqliteGoalStore &operator=(const SqliteGoalStore &) = delete;

    [[nodiscard]] Result<GoalRecord> create(const GoalKey &key, GoalDefinition definition);
    [[nodiscard]] Result<std::optional<GoalRecord>> read(const GoalKey &key) const;
    [[nodiscard]] Result<GoalRecord> settle(const GoalKey &key, std::uint64_t expected_revision,
                                            GoalResolution resolution);
    [[nodiscard]] Result<GoalDuePage> due(const GoalDueQuery &query) const;
    [[nodiscard]] Result<GoalListPage> list(const GoalListQuery &query) const;
    [[nodiscard]] Result<void> erase(const GoalKey &key);
    [[nodiscard]] Result<void> close() noexcept;

  private:
    class Impl;
    explicit SqliteGoalStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
