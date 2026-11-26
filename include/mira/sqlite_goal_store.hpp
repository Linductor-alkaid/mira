#pragma once

#include <mira/goal_calendar.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kairo {
class Executor;
}

namespace mira {

class IEventStore;
struct OperationContext;

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
    std::size_t reminder_count = 0;
};

struct GoalCreationReceipt final {
    GoalRecord record;
    bool idempotent_replay = false;
};

struct GoalDefinitionVersion final {
    std::uint64_t revision = 0;
    GoalDefinition definition;
};

struct GoalReminder final {
    EventId evidence;
    GoalTime delivered_at{};
    friend bool operator==(const GoalReminder &, const GoalReminder &) = default;
};

struct GoalReminderReceipt final {
    std::uint64_t revision = 0;
    GoalReminder reminder;
    bool idempotent_replay = false;
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

struct GoalDispatchKey final {
    GoalKey goal;
    std::uint64_t revision = 0;
    OperationId operation;
    friend bool operator==(const GoalDispatchKey &, const GoalDispatchKey &) = default;
};

struct GoalDispatchSubmission final {
    SessionId session;
    TaskId task;
    GoalTime submitted_at{};
    friend bool operator==(const GoalDispatchSubmission &,
                           const GoalDispatchSubmission &) = default;
};

struct GoalDispatchReceipt final {
    GoalDispatchKey key;
    GoalTime prepared_at{};
    std::optional<GoalDispatchSubmission> submission;
    friend bool operator==(const GoalDispatchReceipt &, const GoalDispatchReceipt &) = default;
};

struct GoalDispatchPreparation final {
    GoalDispatchReceipt receipt;
    bool newly_prepared = false;
};

struct GoalPendingFulfillmentQuery final {
    GoalOwner owner;
    std::size_t limit = 32;
    std::optional<Id128> after = {};
};

struct GoalPendingFulfillmentPage final {
    std::vector<GoalDispatchKey> records;
    std::optional<Id128> next; // Reuse the same owner on the next page.
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

enum class RecurringPlanState : std::uint8_t { Active, Cancelled, Erased };

struct RecurringPlanDefinition final {
    std::string title;
    EventId source_event;
    GoalTime first_at{};
    std::chrono::milliseconds interval{};
    std::optional<GoalCalendarRule> calendar = std::nullopt;
    friend bool operator==(const RecurringPlanDefinition &,
                           const RecurringPlanDefinition &) = default;
};

struct RecurringPlanCancellation final {
    EventId evidence;
    GoalTime at{};
    friend bool operator==(const RecurringPlanCancellation &,
                           const RecurringPlanCancellation &) = default;
};

struct RecurringPlanRecord final {
    GoalKey key;
    RecurringPlanState state = RecurringPlanState::Active;
    std::uint64_t revision = 1;
    std::optional<RecurringPlanDefinition> definition;
    std::optional<GoalTime> next_at;
    std::optional<RecurringPlanCancellation> cancellation;
};

struct RecurringDueCursor final {
    GoalTime scheduled_at;
    Id128 plan;
};

struct RecurringDueQuery final {
    GoalOwner owner;
    GoalTime cutoff;
    std::size_t limit = 32;
    std::optional<RecurringDueCursor> after;
};

struct RecurringDuePage final {
    std::vector<RecurringPlanRecord> records;
    std::optional<RecurringDueCursor> next;
};

struct GoalOccurrenceKey final {
    GoalKey plan;
    std::uint64_t plan_revision = 0;
    GoalTime scheduled_at{};
    friend bool operator==(const GoalOccurrenceKey &, const GoalOccurrenceKey &) = default;
};

struct GoalOccurrenceReceipt final {
    GoalOccurrenceKey key;
    GoalTime recorded_at{};
    bool idempotent_replay = false;
};

struct GoalOccurrenceDispatchKey final {
    GoalOccurrenceKey occurrence;
    OperationId operation;
    friend bool operator==(const GoalOccurrenceDispatchKey &,
                           const GoalOccurrenceDispatchKey &) = default;
};

struct GoalOccurrenceDispatchReceipt final {
    GoalOccurrenceDispatchKey key;
    GoalTime prepared_at{};
    std::optional<GoalDispatchSubmission> submission;
    friend bool operator==(const GoalOccurrenceDispatchReceipt &,
                           const GoalOccurrenceDispatchReceipt &) = default;
};

struct GoalOccurrenceDispatchPreparation final {
    GoalOccurrenceDispatchReceipt receipt;
    bool newly_prepared = false;
};

enum class GoalStoreResource : std::uint8_t { Goal, RecurringPlan };

enum class GoalStoreOperation : std::uint8_t {
    Read,
    List,
    ReadDue,
    Create,
    Correct,
    Settle,
    Erase,
    ReadHistory,
    RecordReminder,
    Replace,
    Cancel,
    RecordOccurrence,
    ReadOccurrence,
    ReadDispatchDue,
    PrepareDispatch,
    ReadDispatch,
    RecordDispatchSubmission,
    ReadPendingFulfillments
};

struct GoalStoreAccess final {
    GoalOwner owner;
    GoalStoreResource resource;
    GoalStoreOperation operation;
    std::optional<Id128> id; // Absent for collection reads, including due queries.
};

// A bounded, thread-safe host policy query. Runs on the store worker and
// must not call back into the store. A false result or exception denies access.
using GoalStoreAuthorizer = std::function<bool(const GoalStoreAccess &)>;

struct SqliteGoalStoreOptions final {
    std::filesystem::path path;
    std::size_t max_goals = 4096; // Includes erased IDs.
    std::size_t max_pending_per_owner = 64;
    std::size_t max_pending_requests = 32;
    std::chrono::milliseconds operation_timeout{30'000};
    std::size_t max_plans = 1024;
    std::size_t max_active_plans_per_owner = 32;
    std::size_t max_occurrences = 8192;
    std::size_t max_corrections_per_goal = 16;
    std::size_t max_reminders_per_goal = 32;
};

// The host authenticates the exact owner. Due records describe work; their
// presence does not authorize its execution.
class SqliteGoalStore final {
  public:
    // Trusted callers enforce current authorization outside the store.
    [[nodiscard]] static Result<std::unique_ptr<SqliteGoalStore>>
    open(kairo::Executor &executor, SqliteGoalStoreOptions options);
    [[nodiscard]] static Result<std::unique_ptr<SqliteGoalStore>>
    open_authorized(kairo::Executor &executor, SqliteGoalStoreOptions options,
                    GoalStoreAuthorizer authorize);
    ~SqliteGoalStore();

    SqliteGoalStore(const SqliteGoalStore &) = delete;
    SqliteGoalStore &operator=(const SqliteGoalStore &) = delete;

    [[nodiscard]] Result<GoalRecord> create(const GoalKey &key, GoalDefinition definition);
    [[nodiscard]] Result<GoalCreationReceipt> create_with_receipt(const GoalKey &key,
                                                                  GoalDefinition definition);
    [[nodiscard]] Result<std::optional<GoalRecord>> read(const GoalKey &key) const;
    [[nodiscard]] Result<GoalRecord> settle(const GoalKey &key, std::uint64_t expected_revision,
                                            GoalResolution resolution);
    [[nodiscard]] Result<GoalDuePage> due(const GoalDueQuery &query) const;
    [[nodiscard]] Result<GoalDuePage> due_for_dispatch(const GoalDueQuery &query) const;
    // Persists the stable dispatch identity. The host uses it on the task submission channel.
    [[nodiscard]] Result<GoalDispatchReceipt>
    prepare_dispatch(const GoalKey &key, std::uint64_t expected_revision, GoalTime cutoff);
    // Only the transaction inserting the durable receipt reports newly_prepared.
    [[nodiscard]] Result<GoalDispatchPreparation>
    prepare_dispatch_attempt(const GoalKey &key, std::uint64_t expected_revision, GoalTime cutoff);
    [[nodiscard]] Result<std::optional<GoalDispatchReceipt>>
    read_dispatch(const GoalKey &key) const;
    [[nodiscard]] Result<GoalDispatchReceipt>
    record_dispatch_submission(const GoalDispatchKey &key, GoalDispatchSubmission submission);
    // Reads completion evidence from the trusted task event store for this dispatch.
    [[nodiscard]] Result<GoalRecord> fulfill_dispatch(const GoalDispatchKey &, IEventStore &);
    [[nodiscard]] Result<GoalPendingFulfillmentPage>
    pending_fulfillments(const GoalPendingFulfillmentQuery &) const;
    // An empty result means the dispatch or its completion evidence is absent.
    [[nodiscard]] Result<std::optional<GoalRecord>>
    reconcile_dispatch_completion(const GoalDispatchKey &, IEventStore &, const OperationContext &);
    [[nodiscard]] Result<GoalListPage> list(const GoalListQuery &query) const;
    [[nodiscard]] Result<void> erase(const GoalKey &key);
    [[nodiscard]] Result<GoalRecord> correct(const GoalKey &key, std::uint64_t expected_revision,
                                             GoalDefinition definition);
    [[nodiscard]] Result<std::vector<GoalDefinitionVersion>>
    correction_history(const GoalKey &key) const;
    [[nodiscard]] Result<GoalReminderReceipt>
    record_reminder(const GoalKey &key, std::uint64_t expected_revision, GoalReminder reminder);

    [[nodiscard]] Result<RecurringPlanRecord> create_plan(const GoalKey &key,
                                                          RecurringPlanDefinition definition);
    [[nodiscard]] Result<RecurringPlanRecord> replace_plan(const GoalKey &key,
                                                           std::uint64_t expected_revision,
                                                           RecurringPlanDefinition definition);
    [[nodiscard]] Result<RecurringPlanRecord> cancel_plan(const GoalKey &key,
                                                          std::uint64_t expected_revision,
                                                          RecurringPlanCancellation cancellation);
    [[nodiscard]] Result<std::optional<RecurringPlanRecord>> read_plan(const GoalKey &key) const;
    [[nodiscard]] Result<RecurringDuePage> due_plans(const RecurringDueQuery &query) const;
    [[nodiscard]] Result<GoalOccurrenceReceipt> record_occurrence(const GoalOccurrenceKey &key,
                                                                  GoalTime recorded_at);
    [[nodiscard]] Result<std::optional<GoalOccurrenceReceipt>>
    read_occurrence(const GoalOccurrenceKey &key) const;
    // Preparation reserves the occurrence without advancing the plan.
    [[nodiscard]] Result<GoalOccurrenceDispatchReceipt>
    prepare_occurrence_dispatch(const GoalOccurrenceKey &key, GoalTime cutoff);
    [[nodiscard]] Result<GoalOccurrenceDispatchPreparation>
    prepare_occurrence_dispatch_attempt(const GoalOccurrenceKey &key, GoalTime cutoff);
    [[nodiscard]] Result<std::optional<GoalOccurrenceDispatchReceipt>>
    read_occurrence_dispatch(const GoalOccurrenceKey &key) const;
    // Records admission and advances the plan if the occurrence is still current.
    [[nodiscard]] Result<GoalOccurrenceDispatchReceipt>
    record_occurrence_submission(const GoalOccurrenceDispatchKey &key,
                                 GoalDispatchSubmission submission);
    [[nodiscard]] Result<void> erase_plan(const GoalKey &key);

    [[nodiscard]] Result<void> close() noexcept;

  private:
    class Impl;
    [[nodiscard]] static Result<std::unique_ptr<SqliteGoalStore>>
    open_impl(kairo::Executor &executor, SqliteGoalStoreOptions options,
              GoalStoreAuthorizer authorize);
    explicit SqliteGoalStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
