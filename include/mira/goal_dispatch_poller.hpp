#pragma once

#include <mira/sqlite_goal_store.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace mira {

struct OperationContext;

// Publishes the live task's verified completion into completion_events under
// current host authorization. Returns false if the task has no new evidence.
using GoalCompletionPublisher = std::function<Result<bool>(
    const GoalDispatchKey &, const GoalDispatchSubmission &, const OperationContext &)>;

// Read-only lookup by the existing operation ID. An empty result leaves the
// handoff unresolved. The callback must never start work under this operation.
using GoalDispatchReconciler = std::function<Result<std::optional<GoalDispatchSubmission>>(
    const GoalDispatchKey &, const OperationContext &)>;
using RecurringGoalDispatchReconciler = std::function<Result<std::optional<GoalDispatchSubmission>>(
    const GoalOccurrenceDispatchKey &, const OperationContext &)>;

struct GoalDispatchPollerOptions final {
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds scan_timeout{5000};
    std::size_t page_size = 32;
    // Opt in to fulfillment from the host's trusted task event store.
    std::shared_ptr<IEventStore> completion_events = {};
    GoalCompletionPublisher publish_completion = {};
    GoalDispatchReconciler reconcile_submission = {};
    RecurringGoalDispatchReconciler reconcile_recurring_submission = {};
};

struct GoalDispatchPollerStatus final {
    bool running = false;
    std::size_t scans = 0;
    std::size_t submitted = 0;
    std::size_t failures = 0;
    std::optional<Error> last_error;
    std::size_t fulfilled = 0;
    std::size_t unresolved_reconciliations = 0;
};

// Host task admission. Reconcile by key.operation before retrying, and
// return the original receipt for an operation already admitted. Cooperate
// with the context deadline and cancellation without awaiting a pool peer.
using GoalDispatchSubmitter = std::function<Result<GoalDispatchSubmission>(
    const GoalRecord &, const GoalDispatchKey &, const OperationContext &)>;
// The recurring channel uses the same admission and reconciliation contract.
using RecurringGoalDispatchSubmitter = std::function<Result<GoalDispatchSubmission>(
    const RecurringPlanRecord &, const GoalOccurrenceDispatchKey &, const OperationContext &)>;

class GoalDispatchPoller final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<GoalDispatchPoller>>
    start(kairo::Executor &, std::shared_ptr<SqliteGoalStore>, GoalOwner, GoalDispatchSubmitter,
          GoalDispatchPollerOptions = {}, RecurringGoalDispatchSubmitter = {});
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
