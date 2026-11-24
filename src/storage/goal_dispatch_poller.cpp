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
          GoalDispatchPollerOptions limits)
        : executor(owner_executor), store(std::move(goal_store)), owner(goal_owner),
          submit(std::move(task_submitter)), options(limits) {}

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

    void poll(const OperationContext &context, GoalDispatchPollerStatus &report) {
        const auto cutoff = dispatch_now();
        if (cursor && cursor->due_at > cutoff)
            cursor.reset();
        const auto page = store->due_for_dispatch({owner, cutoff, options.page_size, cursor});
        const auto report_failure = [&](Error error) {
            ++report.failures;
            report.last_error = std::move(error);
        };
        if (!page) {
            report_failure(page.error());
            return;
        }
        for (const auto &goal : page.value().records) {
            if (context.cancelled())
                return;
            if (context.expired(Timestamp::now())) {
                report_failure(
                    poll_error(ErrorCode::DeadlineExceeded, "goal dispatch scan expired"));
                return;
            }
            // Keep the page position if one attempted goal consumes the scan deadline.
            cursor = GoalDueCursor{*goal.definition->due_at, goal.key.id};
            const auto prepared = store->prepare_dispatch(goal.key, goal.revision, dispatch_now());
            if (!prepared) {
                report_failure(prepared.error());
                continue;
            }
            if (prepared.value().submission)
                continue;
            if (context.cancelled())
                return;
            if (context.expired(Timestamp::now())) {
                report_failure(
                    poll_error(ErrorCode::DeadlineExceeded, "goal dispatch scan expired"));
                return;
            }
            auto dispatch_context = context;
            dispatch_context.operation = prepared.value().key.operation;
            try {
                const auto submitted = submit(goal, prepared.value().key, dispatch_context);
                if (!submitted) {
                    report_failure(submitted.error());
                    continue;
                }
                // Once the host confirmed acceptance, preserve the receipt even if stopping.
                const auto recorded =
                    store->record_dispatch_submission(prepared.value().key, submitted.value());
                if (!recorded) {
                    report_failure(recorded.error());
                    continue;
                }
                ++report.submitted;
            } catch (...) {
                report_failure(poll_error(ErrorCode::Internal, "goal dispatch handoff failed"));
            }
        }
        cursor = page.value().next;
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
    const GoalDispatchPollerOptions options;
    std::mutex mutex;
    std::mutex close_mutex;
    std::atomic<bool> stopping{false};
    kairo::TimerSubmission<void> timer;
    std::future<void> previous_tick;
    GoalDispatchPollerStatus diagnostics;
    std::optional<GoalDueCursor> cursor;
};

GoalDispatchPoller::GoalDispatchPoller(std::shared_ptr<State> state) : state_(std::move(state)) {}

GoalDispatchPoller::~GoalDispatchPoller() { (void)close(); }

Result<std::unique_ptr<GoalDispatchPoller>>
GoalDispatchPoller::start(kairo::Executor &executor, std::shared_ptr<SqliteGoalStore> store,
                          GoalOwner owner, GoalDispatchSubmitter submit,
                          GoalDispatchPollerOptions options) {
    if (!store || !submit || owner.tenant.is_nil() || owner.user.is_nil() ||
        options.page_size == 0 || options.page_size > 128 || options.interval.count() <= 0 ||
        options.interval > std::chrono::hours{1} || options.scan_timeout.count() <= 0 ||
        options.scan_timeout > std::chrono::minutes{1})
        return poll_error(ErrorCode::InvalidArgument, "invalid goal dispatch poller configuration");
    auto state =
        std::make_shared<State>(executor, std::move(store), owner, std::move(submit), options);
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
