#include <mira/goal_dispatch_poller.hpp>

#include <mira/environment.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <mutex>

namespace mira {
namespace {
Error poll_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.goal_dispatch_poller";
    error.safe_message = message;
    return error;
}

GoalTime dispatch_now() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

std::optional<Error> consume_timer(std::future<void> &future, bool closing = false) {
    if (!future.valid())
        return {};
    try {
        future.get();
    } catch (const kairo::TaskCancelled &) {
        if (!closing)
            return poll_error(ErrorCode::Cancelled, "goal dispatch timer was cancelled");
    } catch (...) {
        return poll_error(ErrorCode::Unavailable, "goal dispatch timer did not complete");
    }
    return {};
}

thread_local const void *executing_poller = nullptr;
} // namespace

struct GoalDispatchPoller::State final : std::enable_shared_from_this<State> {
    State(kairo::Executor &owner_executor, std::shared_ptr<SqliteGoalStore> goal_store,
          GoalOwner goal_owner, GoalDispatchSubmitter task_submitter,
          GoalDispatchPollerOptions limits, RecurringGoalDispatchSubmitter recurring_submitter)
        : executor(owner_executor), store(std::move(goal_store)), owner(goal_owner),
          submit(std::move(task_submitter)), submit_recurring(std::move(recurring_submitter)),
          options(limits) {}

    Result<kairo::TimerSubmission<void>> arm(std::chrono::milliseconds delay) {
        try {
            auto scheduled = executor.submit_delayed_cancellable(
                delay.count(), [weak = weak_from_this()](kairo::StopToken stop) {
                    if (const auto state = weak.lock())
                        state->tick(stop);
                });
            if (scheduled.handle.valid() && scheduled.future.valid())
                return scheduled;
            if (auto error = consume_timer(scheduled.future))
                return *error;
        } catch (...) {
            return poll_error(ErrorCode::ResourceExhausted,
                              "Executor rejected goal dispatch timer");
        }
        return poll_error(ErrorCode::Unavailable, "Executor did not arm the goal dispatch timer");
    }

    static void report_failure(GoalDispatchPollerStatus &report, Error error) {
        ++report.failures;
        report.last_error = std::move(error);
    }

    static bool ready(const OperationContext &context, GoalDispatchPollerStatus &report) {
        if (context.cancelled())
            return false;
        if (!context.expired(Timestamp::now()))
            return true;
        report_failure(report,
                       poll_error(ErrorCode::DeadlineExceeded, "goal dispatch scan expired"));
        return false;
    }

    template <typename Cursor, typename Query, typename GetPosition, typename Dispatch>
    void scan_page(std::optional<Cursor> &position, const OperationContext &context,
                   GoalDispatchPollerStatus &report, Query query, GetPosition get_position,
                   Dispatch dispatch) {
        if (!ready(context, report))
            return;
        const auto page = query();
        if (!page) {
            report_failure(report, page.error());
            return;
        }
        for (const auto &record : page.value().records) {
            if (!ready(context, report))
                return;
            // Keep this position if the attempted item consumes the rest of the scan.
            position = get_position(record);
            dispatch(record);
        }
        position = page.value().next;
    }

    template <typename Prepared, typename Admit, typename Record>
    void handoff(const Prepared &prepared, const OperationContext &context,
                 GoalDispatchPollerStatus &report, Admit admit, Record record) {
        if (!prepared) {
            report_failure(report, prepared.error());
            return;
        }
        if (prepared.value().submission || !ready(context, report))
            return;
        auto dispatch_context = context;
        const auto &key = prepared.value().key;
        dispatch_context.operation = key.operation;
        try {
            const auto submitted = admit(key, dispatch_context);
            if (!submitted) {
                report_failure(report, submitted.error());
                return;
            }
            // A confirmed submission must be recorded even if this scan is stopping.
            const auto recorded = record(key, submitted.value());
            if (!recorded) {
                report_failure(report, recorded.error());
                return;
            }
            ++report.submitted;
        } catch (...) {
            report_failure(report, poll_error(ErrorCode::Internal, "goal dispatch handoff failed"));
        }
    }

    void poll_goals(const OperationContext &context, GoalDispatchPollerStatus &report) {
        if (!submit)
            return;
        const auto cutoff = dispatch_now();
        if (cursor && cursor->due_at > cutoff)
            cursor.reset();
        scan_page(
            cursor, context, report,
            [&] { return store->due_for_dispatch({owner, cutoff, options.page_size, cursor}); },
            [](const GoalRecord &goal) {
                return GoalDueCursor{*goal.definition->due_at, goal.key.id};
            },
            [&](const GoalRecord &goal) {
                handoff(
                    store->prepare_dispatch(goal.key, goal.revision, dispatch_now()), context,
                    report,
                    [&](const GoalDispatchKey &key, const OperationContext &dispatch_context) {
                        return submit(goal, key, dispatch_context);
                    },
                    [&](const GoalDispatchKey &key, const GoalDispatchSubmission &submission) {
                        return store->record_dispatch_submission(key, submission);
                    });
            });
    }

    void poll_recurring(const OperationContext &context, GoalDispatchPollerStatus &report) {
        if (!submit_recurring)
            return;
        const auto cutoff = dispatch_now();
        if (recurring_cursor && recurring_cursor->scheduled_at > cutoff)
            recurring_cursor.reset();
        scan_page(
            recurring_cursor, context, report,
            [&] { return store->due_plans({owner, cutoff, options.page_size, recurring_cursor}); },
            [](const RecurringPlanRecord &plan) {
                return RecurringDueCursor{*plan.next_at, plan.key.id};
            },
            [&](const RecurringPlanRecord &plan) {
                const GoalOccurrenceKey occurrence{plan.key, plan.revision, *plan.next_at};
                handoff(
                    store->prepare_occurrence_dispatch(occurrence, dispatch_now()), context, report,
                    [&](const GoalOccurrenceDispatchKey &key,
                        const OperationContext &dispatch_context) {
                        return submit_recurring(plan, key, dispatch_context);
                    },
                    [&](const GoalOccurrenceDispatchKey &key,
                        const GoalDispatchSubmission &submission) {
                        return store->record_occurrence_submission(key, submission);
                    });
            });
    }

    void poll(const OperationContext &context, GoalDispatchPollerStatus &report) {
        if (recurring_first) {
            poll_recurring(context, report);
            poll_goals(context, report);
        } else {
            poll_goals(context, report);
            poll_recurring(context, report);
        }
        recurring_first = !recurring_first;
    }

    void tick(kairo::StopToken stop) {
        struct ActiveTick final {
            const void *prior;
            ~ActiveTick() { executing_poller = prior; }
        } active_tick{executing_poller};
        executing_poller = this;
        std::future<void> previous;
        {
            const std::lock_guard lock(mutex);
            if (stopping.load())
                return;
            previous = std::move(previous_tick);
        }
        GoalDispatchPollerStatus report;
        if (auto error = consume_timer(previous)) {
            ++report.failures;
            report.last_error = std::move(error);
        }
        auto context = make_control_context();
        context.deadline = context.started_at.monotonic + options.scan_timeout;
        context.cancellation_requested = [this, stop] {
            return stopping.load() || stop.stop_requested();
        };
        try {
            if (!context.cancelled())
                poll(context, report);
        } catch (...) {
            ++report.failures;
            report.last_error = poll_error(ErrorCode::Internal, "goal dispatch scan failed");
        }

        const std::lock_guard lock(mutex);
        ++diagnostics.scans;
        diagnostics.submitted += report.submitted;
        diagnostics.failures += report.failures;
        if (report.last_error)
            diagnostics.last_error = std::move(report.last_error);
        if (context.cancelled()) {
            diagnostics.running = false;
            return;
        }
        auto next = arm(options.interval);
        if (!next) {
            diagnostics.running = false;
            diagnostics.last_error = next.error();
            ++diagnostics.failures;
            return;
        }
        // The next tick's entry consumes this future after the current callable returns.
        previous_tick = std::move(timer.future);
        timer = std::move(next).value();
    }

    kairo::Executor &executor;
    const std::shared_ptr<SqliteGoalStore> store;
    const GoalOwner owner;
    const GoalDispatchSubmitter submit;
    const RecurringGoalDispatchSubmitter submit_recurring;
    const GoalDispatchPollerOptions options;
    std::mutex mutex;
    std::mutex close_mutex;
    std::atomic<bool> stopping{false};
    kairo::TimerSubmission<void> timer;
    std::future<void> previous_tick;
    GoalDispatchPollerStatus diagnostics;
    std::optional<GoalDueCursor> cursor;
    std::optional<RecurringDueCursor> recurring_cursor;
    bool recurring_first = false;
};

GoalDispatchPoller::GoalDispatchPoller(std::shared_ptr<State> state) : state_(std::move(state)) {}

GoalDispatchPoller::~GoalDispatchPoller() { (void)close(); }

Result<std::unique_ptr<GoalDispatchPoller>>
GoalDispatchPoller::start(kairo::Executor &executor, std::shared_ptr<SqliteGoalStore> store,
                          GoalOwner owner, GoalDispatchSubmitter submit,
                          GoalDispatchPollerOptions options,
                          RecurringGoalDispatchSubmitter submit_recurring) {
    if (!store || (!submit && !submit_recurring) || owner.tenant.is_nil() || owner.user.is_nil() ||
        options.page_size == 0 || options.page_size > 128 || options.interval.count() <= 0 ||
        options.interval > std::chrono::hours{1} || options.scan_timeout.count() <= 0 ||
        options.scan_timeout > std::chrono::minutes{1})
        return poll_error(ErrorCode::InvalidArgument, "invalid goal dispatch poller configuration");
    auto state = std::make_shared<State>(executor, std::move(store), owner, std::move(submit),
                                         options, std::move(submit_recurring));
    auto poller = std::unique_ptr<GoalDispatchPoller>(new GoalDispatchPoller(state));
    const std::lock_guard lock(state->mutex);
    auto first = state->arm(std::chrono::milliseconds{1});
    if (!first)
        return first.error();
    state->timer = std::move(first).value();
    state->diagnostics.running = true;
    return poller;
}

GoalDispatchPollerStatus GoalDispatchPoller::status() const {
    const std::lock_guard lock(state_->mutex);
    auto &future = state_->timer.future;
    if (future.valid() && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        if (auto stalled = consume_timer(future)) {
            ++state_->diagnostics.failures;
            state_->diagnostics.last_error = std::move(stalled);
        }
        state_->diagnostics.running = false;
    }
    return state_->diagnostics;
}

Result<void> GoalDispatchPoller::close() {
    if (executing_poller == state_.get()) {
        state_->stopping.store(true);
        return poll_error(ErrorCode::InvalidState,
                          "goal dispatch close must run outside its callback");
    }
    const std::lock_guard close_lock(state_->close_mutex);
    kairo::TimerSubmission<void> timer;
    std::future<void> previous;
    {
        const std::lock_guard lock(state_->mutex);
        state_->stopping.store(true);
        state_->diagnostics.running = false;
        timer = std::move(state_->timer);
        previous = std::move(state_->previous_tick);
    }
    std::optional<Error> failure;
    if (timer.handle.valid() && timer.handle.cancel() == kairo::TimerOperationResult::InternalError)
        failure = poll_error(ErrorCode::Internal, "goal dispatch timer cancellation failed");
    const auto current_result = consume_timer(timer.future, true);
    const auto previous_result = consume_timer(previous, true);
    if (current_result)
        return *current_result;
    if (previous_result)
        return *previous_result;
    if (failure)
        return *failure;
    return {};
}

} // namespace mira
