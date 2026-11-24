#pragma once

#include <mira/sqlite_goal_store.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace mira {

struct OperationContext;

struct GoalDispatchPollerOptions final {
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds scan_timeout{5000};
    std::size_t page_size = 32;
};

struct GoalDispatchPollerStatus final {
    bool running = false;
    std::size_t scans = 0;
    std::size_t submitted = 0;
    std::size_t failures = 0;
    std::optional<Error> last_error;
};

// Host task admission. Reconcile by key.operation before retrying, and
// return the original receipt for an operation already admitted. Cooperate
// with the context deadline and cancellation without awaiting a pool peer.
using GoalDispatchSubmitter = std::function<Result<GoalDispatchSubmission>(
    const GoalRecord &, const GoalDispatchKey &, const OperationContext &)>;

class GoalDispatchPoller final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<GoalDispatchPoller>>
    start(kairo::Executor &, std::shared_ptr<SqliteGoalStore>, GoalOwner, GoalDispatchSubmitter,
          GoalDispatchPollerOptions = {});
    ~GoalDispatchPoller();
    GoalDispatchPoller(const GoalDispatchPoller &) = delete;
    GoalDispatchPoller &operator=(const GoalDispatchPoller &) = delete;

    [[nodiscard]] GoalDispatchPollerStatus status() const;
    // The external owner drains the poller before closing the store or Executor.
    [[nodiscard]] Result<void> close();

  private:
    struct State;
    explicit GoalDispatchPoller(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace mira
