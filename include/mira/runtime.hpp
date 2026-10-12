#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/security.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace mira {

struct RuntimeConfig final {
    std::size_t worker_threads = 2;
    std::size_t executor_queue_capacity = 64;
    std::size_t max_in_flight = 1024;
};

struct SessionConfig final {
    PrincipalContext principal;
};

struct TaskSpec final {
    std::string goal;
};

struct ShutdownReport final {
    bool clean = false;
    RuntimeState state = RuntimeState::Constructed;
    std::size_t pending_commands = 0;
    std::string diagnostic;
};

// Mirror of the Executor's cumulative scheduling counters (kairo 0.7.0
// `get_scheduling_metrics()`): routing decisions by status/reason, execution
// deadline misses and scheduler feedback counts. Counters are monotonic from
// Executor construction; the read is lock-free and safe before initialize()
// and after shutdown (zeros when nothing was routed). Surfaces queue
// rejections that must not stay silent (project completion criteria).
struct SchedulingMetricsSnapshot final {
    std::uint64_t accepted = 0;
    std::uint64_t accepted_degraded = 0;
    std::uint64_t rejected = 0;
    std::uint64_t deadline_rejected = 0;
    std::uint64_t backend_unavailable_rejected = 0;
    std::uint64_t capacity_rejected = 0;
    std::uint64_t resource_rejected = 0;
    std::uint64_t affinity_mismatch = 0;
    std::uint64_t deadline_missed = 0;
    std::uint64_t feedback_reported = 0;
    std::uint64_t adaptive_history = 0;
    std::uint64_t load_shedding_rejected = 0;
    std::uint64_t priority_promoted = 0;
};

class CommandHandle final {
  public:
    CommandHandle() = default;
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }
    [[nodiscard]] CommandId id() const noexcept;
    [[nodiscard]] Result<CommandReceipt> receipt(std::chrono::milliseconds timeout) const;
    [[nodiscard]] Result<CommandOutcome> outcome(std::chrono::milliseconds timeout) const;

  private:
    struct State;
    explicit CommandHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}
    friend class MiraRuntime;
    std::shared_ptr<State> state_;
};

struct SessionSubmission final {
    SessionId id;
    CommandHandle command;
};

struct TaskSubmission final {
    TaskId id;
    CommandHandle command;
};

struct SessionSnapshot final {
    SessionId id;
    SessionState state = SessionState::Opening;
    std::uint64_t environment_epoch = 0;
    PrincipalContext principal;
};

class MiraRuntime final {
  public:
    explicit MiraRuntime(RuntimeConfig config = {});
    ~MiraRuntime();
    MiraRuntime(const MiraRuntime &) = delete;
    MiraRuntime &operator=(const MiraRuntime &) = delete;

    Result<void> initialize();
    Result<SessionSubmission> open_session(std::shared_ptr<IEnvironment> environment,
                                           SessionConfig config = {});
    Result<TaskSubmission> submit_task(SessionId session_id, TaskSpec task);
    Result<CommandHandle> pause_task(TaskId task_id);
    Result<CommandHandle> resume_task(TaskId task_id);
    Result<CommandHandle> cancel_task(TaskId task_id);
    // Moves one active task into Recovering (DEC-023): the carrier state for a
    // WorkflowRun escalated to WaitingAgent. Entering Recovering withdraws the
    // task from autonomous action like the pause family; resuming from it
    // returns to Observing with an epoch advance (late operation completions
    // settle as stale). No path out of a terminal state exists, as everywhere.
    Result<CommandHandle> begin_task_recovery(TaskId task_id);
    // Settles one task as Completed or Failed. Terminal states are idempotent
    // per state and never revive; the epoch advances so late completions of
    // in-flight operations settle as stale. Completion is only admitted when
    // the frozen transition table has a legal path from the current state
    // (e.g. a cancelling task can only end Cancelled).
    Result<CommandHandle> complete_task(TaskId task_id, TaskOutcome outcome);
    Result<CommandHandle> request_human_takeover(SessionId session_id);
    Result<CommandHandle> release_human_takeover(SessionId session_id);
    Result<CommandHandle> close_session(SessionId session_id);
    Result<CommandHandle> request_shutdown();
    Result<OperationKey> begin_operation(TaskId task_id, StepId step_id);
    Result<CommandHandle> admit_operation_completion(const OperationKey &key);
    [[nodiscard]] Result<TaskSnapshot> task_snapshot(TaskId task_id) const;
    [[nodiscard]] Result<SessionSnapshot> session_snapshot(SessionId session_id) const;
    [[nodiscard]] RuntimeState state() const noexcept;
    [[nodiscard]] SchedulingMetricsSnapshot scheduling_metrics() const;
    ShutdownReport finish_shutdown();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
