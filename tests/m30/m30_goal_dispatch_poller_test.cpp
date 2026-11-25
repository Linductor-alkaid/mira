#include "support/test.hpp"

#include <mira/environment.hpp>
#include <mira/goal_dispatch_poller.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>

namespace {
using namespace mira;
using namespace std::chrono_literals;

GoalTime clock_now() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

struct PollFixture final {
    PollFixture() {
        kairo::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        if (!executor.initialize(config))
            std::abort();
        root = std::filesystem::temp_directory_path() /
               ("mira-goal-poller-" + Id128::generate().to_string());
        std::filesystem::create_directories(root);
        options.path = root / "goals.db";
        if (!reopen())
            std::abort();
    }

    ~PollFixture() {
        store.reset();
        (void)executor.shutdown(true);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    bool reopen() {
        if (store && !store->close())
            return false;
        store.reset();
        auto opened = SqliteGoalStore::open(executor, options);
        if (!opened)
            return false;
        store = std::move(opened).value();
        return true;
    }

    bool create(const GoalKey &key, GoalTime due, std::string title) {
        return store
            ->create(key, {GoalKind::Commitment, std::move(title), EventId::generate(), due - 1h,
                           due, due + 4h})
            .has_value();
    }

    template <typename Predicate> bool wait_for(Predicate ready) {
        const auto until = std::chrono::steady_clock::now() + 4s;
        while (!ready() && std::chrono::steady_clock::now() < until) {
            auto pause = executor.submit_delayed(5, [] {});
            pause.future.get();
        }
        return ready();
    }

    kairo::Executor executor;
    const GoalOwner owner{TenantId::generate(), UserId::generate()};
    std::filesystem::path root;
    SqliteGoalStoreOptions options;
    std::shared_ptr<SqliteGoalStore> store;
};

GoalDispatchSubmission accepted() {
    return {SessionId::generate(), TaskId::generate(), clock_now()};
}

int scans_past_uncertainty_and_resumes_after_reopen() {
    PollFixture fixture;
    const GoalKey first{fixture.owner, Id128::generate()};
    const GoalKey second{fixture.owner, Id128::generate()};
    const GoalKey future{fixture.owner, Id128::generate()};
    const auto now = clock_now();
    MIRA_CHECK(fixture.create(first, now - 5min, "Review the changes"));
    MIRA_CHECK(fixture.create(second, now - 1min, "Review the results"));
    MIRA_CHECK(fixture.create(future, now + 2h, "Review the final report"));
    std::optional<GoalDispatchSubmission> first_accepted;
    std::optional<GoalDispatchKey> first_dispatch;
    std::atomic<std::size_t> new_tasks{0};
    auto started = GoalDispatchPoller::start(
        fixture.executor, fixture.store, fixture.owner,
        [&](const GoalRecord &goal, const GoalDispatchKey &key,
            const OperationContext &context) -> Result<GoalDispatchSubmission> {
            Error failure;
            failure.code = ErrorCode::Internal;
            if (key.goal != goal.key || key.revision != goal.revision ||
                context.operation != key.operation)
                return failure;
            if (goal.key == first) {
                if (first_dispatch && *first_dispatch != key)
                    return failure;
                if (!first_accepted) {
                    first_dispatch = key;
                    first_accepted = accepted();
                    ++new_tasks;
                }
                failure.code = ErrorCode::ExecutionUncertain;
                return failure;
            }
            if (goal.key != second)
                return failure;
            ++new_tasks;
            return accepted();
        },
        {10ms, 2s, 1});
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().submitted == 1; }));
    MIRA_CHECK(poller->close());
    const auto status = poller->status();
    MIRA_CHECK(!status.running && status.failures >= 1 && status.last_error &&
               status.last_error->code == ErrorCode::ExecutionUncertain);
    const auto pending = fixture.store->read_dispatch(first);
    MIRA_CHECK(pending && pending.value() && pending.value()->key == first_dispatch &&
               !pending.value()->submission);
    MIRA_CHECK(fixture.store->read_dispatch(second).value()->submission);
    MIRA_CHECK(!fixture.store->read_dispatch(future).value());
    MIRA_CHECK(new_tasks.load() == 2 && first_accepted);
    poller.reset();
    MIRA_CHECK(fixture.reopen());

    auto resumed = GoalDispatchPoller::start(
        fixture.executor, fixture.store, fixture.owner,
        [&](const GoalRecord &goal, const GoalDispatchKey &key,
            const OperationContext &context) -> Result<GoalDispatchSubmission> {
            if (goal.key == first && key == first_dispatch && context.operation == key.operation)
                return *first_accepted;
            Error failure;
            failure.code = ErrorCode::InvalidState;
            return failure;
        },
        {10ms, 2s, 1});
    MIRA_CHECK(resumed);
    poller = std::move(resumed).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().submitted == 1; }));
    MIRA_CHECK(poller->close() && new_tasks.load() == 2);
    MIRA_CHECK(fixture.store->read_dispatch(first).value()->submission == first_accepted);
    MIRA_CHECK(fixture.store->due_for_dispatch({fixture.owner, clock_now(), 8, {}})
                   .value()
                   .records.empty());
    MIRA_CHECK(fixture.store->read(first).value()->state == GoalState::Pending);
    return 0;
}

int close_drains_confirmed_submission_and_stops_following_goals() {
    PollFixture fixture;
    const GoalKey first{fixture.owner, Id128::generate()};
    const GoalKey second{fixture.owner, Id128::generate()};
    const auto now = clock_now();
    MIRA_CHECK(fixture.create(first, now - 3min, "Check the draft"));
    MIRA_CHECK(fixture.create(second, now - 1min, "Check the appendices"));
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::atomic<std::size_t> calls{0};
    std::atomic<bool> saw_stop{false};
    auto started = GoalDispatchPoller::start(
        fixture.executor, fixture.store, fixture.owner,
        [&](const GoalRecord &, const GoalDispatchKey &,
            const OperationContext &context) -> Result<GoalDispatchSubmission> {
            if (++calls == 1)
                entered.set_value();
            std::mutex mutex;
            std::condition_variable wait;
            std::unique_lock lock(mutex);
            while (!context.cancelled() && !context.expired(Timestamp::now()))
                wait.wait_for(lock, 2ms);
            saw_stop.store(context.cancelled());
            return accepted();
        },
        {10ms, 2s, 8});
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(entered_future.wait_for(3s) == std::future_status::ready);
    entered_future.get();
    MIRA_CHECK(poller->close() && poller->close());
    MIRA_CHECK(!poller->status().running && saw_stop.load() && calls.load() == 1);
    MIRA_CHECK(fixture.store->read_dispatch(first).value()->submission);
    MIRA_CHECK(!fixture.store->read_dispatch(second).value());
    return 0;
}

int recurring_pagination_and_recovery_share_the_goal_poller() {
    PollFixture fixture;
    const auto now = clock_now();
    const GoalKey goal{fixture.owner, Id128::generate()};
    const GoalKey first{fixture.owner, Id128::generate()};
    const GoalKey second{fixture.owner, Id128::generate()};
    const GoalKey future{fixture.owner, Id128::generate()};
    const GoalOccurrenceKey missed{first, 1, now - 8min};
    const GoalOccurrenceKey catchup{first, 1, now - 3min};
    const GoalOccurrenceKey independent{second, 1, now - 2min};
    const GoalOccurrenceKey upcoming{future, 1, now + 1h};
    MIRA_CHECK(fixture.create(goal, now - 1min, "Send the latest report"));
    MIRA_CHECK(fixture.store->create_plan(
        first, {"Review the queue", EventId::generate(), missed.scheduled_at, 5min}));
    MIRA_CHECK(fixture.store->create_plan(
        second, {"Check the report", EventId::generate(), independent.scheduled_at, 1h}));
    MIRA_CHECK(fixture.store->create_plan(
        future, {"Check next report", EventId::generate(), upcoming.scheduled_at, 1h}));
    std::optional<GoalDispatchSubmission> first_acceptance;
    std::optional<GoalOccurrenceDispatchKey> first_dispatch;
    std::atomic<std::size_t> real_admissions{0};
    auto started = GoalDispatchPoller::start(
        fixture.executor, fixture.store, fixture.owner,
        [&](const GoalRecord &, const GoalDispatchKey &, const OperationContext &) {
            ++real_admissions;
            return accepted();
        },
        {10ms, 2s, 1},
        [&](const RecurringPlanRecord &plan, const GoalOccurrenceDispatchKey &key,
            const OperationContext &context) -> Result<GoalDispatchSubmission> {
            Error failure;
            failure.code = ErrorCode::InvalidState;
            if (key.operation != context.operation || plan.key != key.occurrence.plan ||
                plan.revision != key.occurrence.plan_revision ||
                plan.next_at != key.occurrence.scheduled_at)
                return failure;
            if (key.occurrence == missed) {
                if (!first_dispatch) {
                    first_dispatch = key;
                    first_acceptance = accepted();
                    ++real_admissions;
                }
                if (*first_dispatch != key)
                    return failure;
                failure.code = ErrorCode::ExecutionUncertain;
                return failure;
            }
            if (key.occurrence != independent)
                return failure;
            ++real_admissions;
            return accepted();
        });
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().submitted == 2; }));
    MIRA_CHECK(poller->close());
    const auto status = poller->status();
    MIRA_CHECK(status.failures >= 1 && status.last_error &&
               status.last_error->code == ErrorCode::ExecutionUncertain);
    MIRA_CHECK(real_admissions == 3 && first_dispatch && first_acceptance);
    MIRA_CHECK(fixture.store->read_dispatch(goal).value()->submission);
    MIRA_CHECK(fixture.store->read_occurrence_dispatch(independent).value()->submission);
    MIRA_CHECK(!fixture.store->read_occurrence_dispatch(missed).value()->submission);
    MIRA_CHECK(!fixture.store->read_occurrence_dispatch(catchup).value());
    MIRA_CHECK(!fixture.store->read_occurrence_dispatch(upcoming).value());
    poller.reset();
    MIRA_CHECK(fixture.reopen());

    auto resumed = GoalDispatchPoller::start(
        fixture.executor, fixture.store, fixture.owner, {}, {10ms, 2s, 1},
        [&](const RecurringPlanRecord &, const GoalOccurrenceDispatchKey &key,
            const OperationContext &context) -> Result<GoalDispatchSubmission> {
            if (context.operation == key.operation) {
                if (key == *first_dispatch)
                    return *first_acceptance;
                if (key.occurrence == catchup && key.operation != first_dispatch->operation) {
                    ++real_admissions;
                    return accepted();
                }
            }
            Error failure;
            failure.code = ErrorCode::InvalidState;
            return failure;
        });
    MIRA_CHECK(resumed);
    poller = std::move(resumed).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().submitted == 2; }));
    MIRA_CHECK(poller->close() && real_admissions == 4);
    MIRA_CHECK(fixture.store->read_occurrence_dispatch(missed).value()->submission ==
               first_acceptance);
    MIRA_CHECK(fixture.store->read_occurrence_dispatch(catchup).value()->submission);
    MIRA_CHECK(fixture.store->read_plan(first).value()->next_at == now + 2min);
    return 0;
}

int observes_store_failure_and_timer_shutdown() {
    PollFixture fixture;
    const GoalDispatchSubmitter callback = [](const GoalRecord &, const GoalDispatchKey &,
                                              const OperationContext &) { return accepted(); };
    MIRA_CHECK(!GoalDispatchPoller::start(fixture.executor, fixture.store, fixture.owner, callback,
                                          {0ms, 2s, 1}));
    MIRA_CHECK(fixture.store->close());
    auto started = GoalDispatchPoller::start(fixture.executor, fixture.store, fixture.owner,
                                             callback, {20s, 2s, 1});
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().failures == 1; }));
    MIRA_CHECK(poller->status().last_error->code == ErrorCode::Unavailable);
    MIRA_CHECK(fixture.executor.shutdown(true) == kairo::ShutdownResult::Completed);
    const auto stopped = poller->status();
    MIRA_CHECK(!stopped.running && stopped.failures == 2 && stopped.last_error &&
               stopped.last_error->code == ErrorCode::Cancelled);
    MIRA_CHECK(poller->close());
    return 0;
}
} // namespace

int main() {
    if (const int result = scans_past_uncertainty_and_resumes_after_reopen(); result != 0)
        return result;
    if (const int result = close_drains_confirmed_submission_and_stops_following_goals();
        result != 0)
        return result;
    if (const int result = recurring_pagination_and_recovery_share_the_goal_poller(); result != 0)
        return result;
    return observes_store_failure_and_timer_shutdown();
}
