#pragma once

#include <mira/context_contracts.hpp>
#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Task checkpoint contracts (M4-01/M4-05)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr SchemaVersion checkpoint_schema_current() noexcept { return {1, 0}; }

struct CheckpointConstraint final {
    std::string key;
    std::string requirement;
    bool safety = false;
    std::vector<EventId> provenance;
};

struct VerifiedFact final {
    std::string key;
    std::string value;
    std::vector<EventId> provenance;
    friend bool operator==(const VerifiedFact &, const VerifiedFact &) = default;
};

struct CompletedStep final {
    StepId step_id;
    std::string summary;
    std::vector<EventId> provenance;
};

struct PendingObjective final {
    std::string objective;
    std::vector<EventId> provenance;
};

struct UnresolvedIssue final {
    std::string issue;
    std::vector<EventId> provenance;
};

// Pinned projection field: recovery must Observe/Verify these actions before
// any new autonomous action may be dispatched.
struct UncertainSideEffect final {
    ActionId action_id;
    std::string action_kind;
    std::string reason;
    std::vector<EventId> provenance;
    friend bool operator==(const UncertainSideEffect &, const UncertainSideEffect &) = default;
};

struct CheckpointActionSummary final {
    ActionId action_id;
    std::string kind;
    std::string outcome;
    std::uint64_t sequence = 0;
};

struct CheckpointObservationRef final {
    ObservationId observation_id;
    Sha256Digest digest{};
    std::uint64_t sequence = 0;
};

// Deterministic recovery projection derived from the EventStore; never a
// second chat history (design Context/Memory §12).
struct TaskCheckpoint final {
    CheckpointId id;
    TaskId task_id;
    SessionId session_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    Timestamp created_at;
    std::string goal_statement;
    std::optional<std::string> success_criterion;
    std::vector<CheckpointConstraint> constraints;
    std::vector<VerifiedFact> verified_facts;
    std::vector<CompletedStep> completed_steps;
    std::vector<PendingObjective> pending_objectives;
    std::vector<UnresolvedIssue> unresolved_issues;
    std::vector<UncertainSideEffect> uncertain_side_effects;
    std::optional<CheckpointObservationRef> current_observation;
    std::vector<CheckpointActionSummary> recent_actions;
    // An action completed but no VerificationResult event followed it yet.
    bool verification_pending = false;
    std::optional<TaskState> terminal_state;
    // Optional model-assisted narrative; never authoritative for state,
    // constraints or side-effect facts and excluded from the projection digest.
    std::optional<std::string> narrative_summary;
    SchemaVersion schema_version = checkpoint_schema_current();

    [[nodiscard]] Result<void> validate() const;
    // Canonical digest over authoritative fields; excludes id and created_at so
    // incremental and full rebuilds of the same prefix compare equal.
    [[nodiscard]] Hash projection_digest() const;
};

[[nodiscard]] JsonValue checkpoint_to_json(const TaskCheckpoint &checkpoint);
[[nodiscard]] Result<TaskCheckpoint> checkpoint_from_json(const JsonValue &json);

// ---------------------------------------------------------------------------
// Deterministic checkpoint reducer
// ---------------------------------------------------------------------------

struct ReducerStats final {
    std::size_t applied = 0;
    std::size_t ignored = 0;       // payload types the reducer does not consume
    std::size_t malformed = 0;     // known types with undecodable payloads
    std::size_t stale_skipped = 0; // sequence <= base watermark or other task
};

struct CheckpointBuildResult final {
    TaskCheckpoint checkpoint;
    ReducerStats stats;
};

// Bounds applied while the reducer folds events; hoisted to namespace scope
// because nested-class default member initializers cannot back a default
// argument of the enclosing class constructor.
struct CheckpointReducerConfig final {
    std::size_t max_recent_actions = 16;
    std::size_t max_verified_facts = 256;
    std::size_t max_completed_steps = 512;
    std::size_t max_pending_objectives = 128;
    std::size_t max_unresolved_issues = 128;
};

// Event vocabulary consumed by the reducer. JSON payloads: TaskGoalSet,
// TaskConstraintAdded/Removed, TaskFactVerified, TaskStepCompleted,
// TaskObjectiveAdded/Completed, TaskIssueRaised/Resolved, TaskStateChanged,
// TaskEpochAdvanced, EnvironmentEpochChanged, ObservationRecorded,
// ActionDispatched, VerificationResult, LoopSettled. ActionJournal pipe
// payloads: ActionDispatchStarted, ActionReceipt, ActionExecutionUncertain.
class CheckpointBuilder final {
  public:
    using Config = CheckpointReducerConfig;

    explicit CheckpointBuilder(Config config = Config{});

    // Folds events strictly after base.through_event_sequence (or from scratch
    // when base is empty) for one task. Events of other tasks and stale
    // sequences are skipped; unknown types are ignored, never fatal.
    [[nodiscard]] CheckpointBuildResult build(const std::optional<TaskCheckpoint> &base,
                                              std::span<const EventEnvelope> events,
                                              const Timestamp &now) const;

    [[nodiscard]] const Config &config() const noexcept { return config_; }

  private:
    Config config_;
};

// ---------------------------------------------------------------------------
// Checkpoint store
// ---------------------------------------------------------------------------

class ICheckpointStore {
  public:
    virtual ~ICheckpointStore() = default;
    virtual Result<void> put(const TaskCheckpoint &checkpoint) = 0;
    [[nodiscard]] virtual Result<std::optional<TaskCheckpoint>> latest(TaskId task) const = 0;
    // Latest checkpoint whose through_event_sequence does not exceed the
    // durable sequence visible to a recovering process.
    [[nodiscard]] virtual Result<std::optional<TaskCheckpoint>>
    latest_at_or_before(TaskId task, std::uint64_t max_sequence) const = 0;
    [[nodiscard]] virtual Result<std::size_t> count(TaskId task) const = 0;
    virtual Result<std::size_t> erase_task(TaskId task, std::string reason) = 0;
};

// In-memory reference store (design phase CM0); the SQLite/WAL reference
// backend is M4-06 and implements the same contract.
class MemoryCheckpointStore final : public ICheckpointStore {
  public:
    explicit MemoryCheckpointStore(std::size_t max_checkpoints_per_task = 64);
    ~MemoryCheckpointStore() override;

    Result<void> put(const TaskCheckpoint &checkpoint) override;
    [[nodiscard]] Result<std::optional<TaskCheckpoint>> latest(TaskId task) const override;
    [[nodiscard]] Result<std::optional<TaskCheckpoint>>
    latest_at_or_before(TaskId task, std::uint64_t max_sequence) const override;
    [[nodiscard]] Result<std::size_t> count(TaskId task) const override;
    Result<std::size_t> erase_task(TaskId task, std::string reason) override;
    [[nodiscard]] std::size_t erasures() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Checkpoint scheduling (M4-07)
// ---------------------------------------------------------------------------

enum class CheckpointTrigger : std::uint8_t {
    Watermark,         // context crossed checkpoint_watermark
    Pause,             // task entering Paused
    Takeover,          // session entering human control
    Shutdown,          // runtime shutdown with a recoverable active task
    ProjectionMissing, // stored projection absent or behind policy
    PeriodicRebuild,   // periodic full rebuild from the EventStore
};

[[nodiscard]] std::string checkpoint_trigger_name(CheckpointTrigger trigger);

struct CheckpointSchedulePolicy final {
    // Fewer new critical/state events than this keeps the trigger a no-op.
    std::uint32_t min_event_increment = 4;
    std::uint32_t rebuild_interval_events = 256;
    CheckpointBuilder::Config builder{};
};

struct ProjectionReport final {
    bool consistent = false;
    std::uint64_t stored_sequence = 0;
    std::uint64_t rebuilt_sequence = 0;
    Hash stored_digest{};
    Hash rebuilt_digest{};
    bool repaired = false;
    std::string note;
};

// Schedules checkpoints on behalf of the control plane. The coordinator is a
// synchronous component: hosts invoke it from Executor-managed operations and
// own the resulting futures (M4-16 adds the dedicated supervisor).
class CheckpointCoordinator final {
  public:
    CheckpointCoordinator(IEventStore &events, ICheckpointStore &store,
                          CheckpointSchedulePolicy policy = CheckpointSchedulePolicy{});

    // Reads the session log after the stored projection and writes a new
    // checkpoint when the trigger policy admits it. Returns nullopt (with a
    // success Result) when the increment is below policy.
    [[nodiscard]] Result<std::optional<TaskCheckpoint>>
    checkpoint(TaskId task, SessionId session, CheckpointTrigger trigger, const Timestamp &now);

    // Full deterministic rebuild from the session log; with repair the rebuilt
    // projection replaces the stored one, without repair it only reports.
    [[nodiscard]] Result<ProjectionReport> rebuild(TaskId task, SessionId session,
                                                   const Timestamp &now, bool repair);

    [[nodiscard]] Result<std::uint64_t> latest_stored_sequence(TaskId task) const;

  private:
    [[nodiscard]] Result<std::vector<EventEnvelope>> read_task_events(TaskId task,
                                                                      SessionId session) const;

    IEventStore &events_;
    ICheckpointStore &store_;
    CheckpointSchedulePolicy policy_;
    CheckpointBuilder builder_;
};

} // namespace mira
