#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/task_checkpoint.hpp>

#include <cstdint>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Crash recovery planning (M4-08)
// ---------------------------------------------------------------------------

enum class RecoveryAction : std::uint8_t {
    ResumeObserving, // Default non-terminal resume state.
    ResumeVerifying, // An action completed but verification never ran.
    AlreadyTerminal, // The durable log already settled the task; never revive.
    NoState,         // No checkpoint and no events for the task.
};

[[nodiscard]] std::string recovery_action_name(RecoveryAction action);

struct RecoveryOutcome final {
    RecoveryAction action = RecoveryAction::NoState;
    // Effective projection: newest compatible checkpoint plus replayed
    // increments up to the durable sequence.
    std::optional<TaskCheckpoint> checkpoint;
    std::uint64_t durable_sequence = 0;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    // Pinned side effects that must be Observe/Verify-resolved before any new
    // autonomous action may be dispatched.
    std::vector<UncertainSideEffect> pending_side_effects;
    ReducerStats stats;
};

struct RecoveryPolicy final {
    // Newest compatible schema the planner accepts (current major or older).
    SchemaVersion max_supported_schema = checkpoint_schema_current();
    std::uint64_t max_events_scanned = 1'000'000;
};

// Plans recovery for one task from the durable EventStore and CheckpointStore.
// Pure and synchronous: hosts invoke it from an Executor-managed operation at
// process start (design Context/Memory §12.3).
class RecoveryPlanner final {
  public:
    RecoveryPlanner(IEventStore &events, ICheckpointStore &checkpoints,
                    RecoveryPolicy policy = RecoveryPolicy{});

    [[nodiscard]] Result<RecoveryOutcome> plan(TaskId task, SessionId session,
                                               const Timestamp &now) const;

  private:
    [[nodiscard]] Result<std::vector<EventEnvelope>> read_task_events(TaskId task,
                                                                      SessionId session) const;

    IEventStore &events_;
    ICheckpointStore &checkpoints_;
    RecoveryPolicy policy_;
    CheckpointBuilder builder_;
};

} // namespace mira
