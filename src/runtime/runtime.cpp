#include <mira/runtime.hpp>

#include <kairo/executor.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mira {
namespace {

Error make_error(ErrorCode code, std::string message, bool retryable = false) {
    Error result;
    result.code = code;
    result.domain = "mira.runtime";
    result.safe_message = std::move(message);
    result.retryable = retryable;
    return result;
}

// No caller can bypass the frozen transition table with an unchecked store.
class RuntimeStateCell final {
  public:
    [[nodiscard]] RuntimeState load() const noexcept {
        return value_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool transition(RuntimeState from, RuntimeState to) noexcept {
        return valid_runtime_transition(from, to) &&
               value_.compare_exchange_strong(from, to, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

  private:
    std::atomic<RuntimeState> value_{RuntimeState::Constructed};
};

template <typename T>
Result<T> wait_result(const std::shared_future<T> &future, std::chrono::milliseconds timeout) {
    if (!future.valid())
        return make_error(ErrorCode::InvalidState, "command handle is invalid");
    if (future.wait_for(timeout) != std::future_status::ready) {
        return make_error(ErrorCode::DeadlineExceeded, "command handle wait timed out", true);
    }
    return future.get();
}

// Reachability over the frozen task transition table. Completion is admitted
// only when a legal path exists; the intermediate phases are traversed
// atomically inside one control-plane command, so no observer ever sees them
// as distinct states (e.g. a cancelling task has no path except Cancelled).
[[nodiscard]] bool task_state_path_exists(TaskState from, TaskState to) {
    if (from == to) {
        return true;
    }
    const std::array<TaskState, 15> kStates{
        TaskState::Idle,
        TaskState::Observing,
        TaskState::Reasoning,
        TaskState::Planning,
        TaskState::Acting,
        TaskState::Verifying,
        TaskState::Recovering,
        TaskState::Pausing,
        TaskState::Paused,
        TaskState::TakeoverSettling,
        TaskState::SuspendedForTakeover,
        TaskState::Cancelling,
        TaskState::Completed,
        TaskState::Failed,
        TaskState::Cancelled,
    };
    std::array<bool, 15> visited{};
    std::vector<TaskState> frontier{from};
    const auto index_of = [](TaskState state) { return static_cast<std::size_t>(state); };
    visited[index_of(from)] = true;
    while (!frontier.empty()) {
        const auto current = frontier.back();
        frontier.pop_back();
        for (const auto candidate : kStates) {
            const auto index = index_of(candidate);
            if (visited[index] || !valid_task_transition(current, candidate)) {
                continue;
            }
            if (candidate == to) {
                return true;
            }
            visited[index] = true;
            frontier.push_back(candidate);
        }
    }
    return false;
}

} // namespace

struct CommandHandle::State final {
    explicit State(CommandId command_id)
        : id(command_id), receipt_future(receipt.get_future().share()),
          outcome_future(outcome.get_future().share()) {}
    CommandId id;
    CommandKind kind = CommandKind::OpenSession;
    std::promise<CommandReceipt> receipt;
    std::promise<CommandOutcome> outcome;
    std::shared_future<CommandReceipt> receipt_future;
    std::shared_future<CommandOutcome> outcome_future;
    std::shared_future<void> submission_future;
};

CommandId CommandHandle::id() const noexcept { return state_ ? state_->id : CommandId{}; }

Result<CommandReceipt> CommandHandle::receipt(std::chrono::milliseconds timeout) const {
    if (!state_)
        return make_error(ErrorCode::InvalidState, "command handle is invalid");
    return wait_result(state_->receipt_future, timeout);
}

Result<CommandOutcome> CommandHandle::outcome(std::chrono::milliseconds timeout) const {
    if (!state_)
        return make_error(ErrorCode::InvalidState, "command handle is invalid");
    return wait_result(state_->outcome_future, timeout);
}

class MiraRuntime::Impl final {
  public:
    struct SessionRecord final {
        SessionId id;
        SessionState state = SessionState::Opening;
        std::shared_ptr<IEnvironment> environment;
        PrincipalContext principal;
        std::uint64_t environment_epoch = 0;
    };

    struct TaskRecord final {
        TaskSnapshot snapshot;
        std::string goal;
        SessionId session_id;
    };

    struct OperationRecord final {
        OperationKey key;
        OperationState state = OperationState::Created;
    };

    explicit Impl(RuntimeConfig runtime_config)
        : config(runtime_config), runtime_id(RuntimeId::generate()) {}

    RuntimeConfig config;
    RuntimeId runtime_id;
    kairo::Executor executor;
    kairo::SerialExecutionContext control_context;
    RuntimeStateCell runtime_state;
    std::mutex admission_mutex;
    std::mutex finalizer_mutex;
    bool executor_initialized = false;             // Guarded by admission_mutex.
    std::optional<CommandHandle> shutdown_command; // Guarded by admission_mutex.
    std::optional<ShutdownReport> final_report;    // Guarded by finalizer_mutex.
    inline static thread_local const Impl *active_control = nullptr;
    std::atomic<std::uint64_t> control_sequence{0};

    mutable std::mutex records_mutex;
    std::unordered_map<SessionId, SessionRecord, StrongIdHash<SessionId>> sessions;
    std::unordered_map<TaskId, TaskRecord, StrongIdHash<TaskId>> tasks;
    std::unordered_map<OperationId, OperationRecord, StrongIdHash<OperationId>> operations;
    std::mutex commands_mutex;
    std::vector<std::shared_ptr<CommandHandle::State>> commands;

    std::shared_ptr<CommandHandle::State> make_handle() {
        return std::make_shared<CommandHandle::State>(CommandId::generate());
    }

    static void set_receipt(const std::shared_ptr<CommandHandle::State> &state, CommandKind kind,
                            ReceiptStatus status, std::uint64_t sequence,
                            std::optional<Error> error = std::nullopt) noexcept {
        try {
            state->receipt.set_value(
                CommandReceipt{state->id, kind, status, sequence, std::move(error)});
        } catch (...) {
        }
    }

    static void set_outcome(const std::shared_ptr<CommandHandle::State> &state,
                            SettlementStatus status, std::optional<Error> error = std::nullopt,
                            std::optional<TaskSnapshot> task = std::nullopt) noexcept {
        try {
            state->outcome.set_value(
                CommandOutcome{state->id, status, std::move(error), std::move(task)});
        } catch (...) {
        }
    }

    template <typename Callback>
    Result<CommandHandle> enqueue(CommandKind kind, Callback callback) {
        std::lock_guard admission(admission_mutex);
        return enqueue_admitted(kind, std::move(callback));
    }

    // The admission lock spans dispatch AND future registration, so a finalizer
    // cannot miss an accepted command. Callbacks never hold that lock.
    template <typename Callback>
    Result<CommandHandle> enqueue_admitted(CommandKind kind, Callback callback) {
        const auto current = runtime_state.load();
        if (current != RuntimeState::Running && kind != CommandKind::ShutdownRuntime) {
            return make_error(ErrorCode::InvalidState, "runtime is not accepting this command");
        }
        auto handle_state = make_handle();
        handle_state->kind = kind;
        try {
            auto submission = executor.submit_on_with_handle(
                control_context, [this, kind, callback = std::move(callback), handle_state] {
                    struct ControlScope final {
                        const Impl *previous = active_control;
                        explicit ControlScope(const Impl *owner) { active_control = owner; }
                        ~ControlScope() { active_control = previous; }
                    } scope(this);
                    const auto sequence = ++control_sequence;
                    try {
                        callback(sequence, *handle_state);
                    } catch (const std::exception &exception) {
                        const auto failure = make_error(ErrorCode::Internal, exception.what());
                        set_receipt(handle_state, kind, ReceiptStatus::Accepted, sequence, failure);
                        set_outcome(handle_state, SettlementStatus::Failed, failure);
                    } catch (...) {
                        const auto failure =
                            make_error(ErrorCode::Internal, "command callback failed");
                        set_receipt(handle_state, kind, ReceiptStatus::Accepted, sequence, failure);
                        set_outcome(handle_state, SettlementStatus::Failed, failure);
                    }
                });
            auto submission_future = submission.future.share();
            if (!submission_future.valid()) {
                return make_error(ErrorCode::ResourceExhausted,
                                  "runtime command submission rejected", true);
            }
            if (submission_future.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                try {
                    submission_future.get();
                } catch (const kairo::CapacityExhaustedException &exception) {
                    auto failure = make_error(ErrorCode::ResourceExhausted, exception.what(), true);
                    set_receipt(handle_state, kind, ReceiptStatus::Rejected, 0, failure);
                    set_outcome(handle_state, SettlementStatus::Failed, failure);
                    return failure;
                } catch (const kairo::ExecutorStopping &exception) {
                    auto failure = make_error(ErrorCode::Unavailable, exception.what(), true);
                    set_receipt(handle_state, kind, ReceiptStatus::Rejected, 0, failure);
                    set_outcome(handle_state, SettlementStatus::Failed, failure);
                    return failure;
                } catch (const std::exception &) {
                    // The callback itself reports business failures through
                    // the Mira outcome; only facade admission/stopping is a
                    // submission error here.
                }
            }
            handle_state->submission_future = std::move(submission_future);
            {
                std::lock_guard lock(commands_mutex);
                commands.push_back(handle_state);
            }
            return CommandHandle(std::move(handle_state));
        } catch (const std::exception &exception) {
            return make_error(ErrorCode::ResourceExhausted, exception.what(), true);
        } catch (...) {
            return make_error(ErrorCode::ResourceExhausted, "runtime command submission rejected",
                              true);
        }
    }

    Result<CommandHandle> submit_shutdown_admitted() {
        auto command =
            enqueue_admitted(CommandKind::ShutdownRuntime, [this](std::uint64_t sequence,
                                                                  CommandHandle::State &state) {
                std::lock_guard lock(records_mutex);
                const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
                for (auto &[task_id, task] : tasks) {
                    if (!is_terminal(task.snapshot.state)) {
                        task.snapshot.state = TaskState::Cancelled;
                        ++task.snapshot.epoch;
                        task.snapshot.terminal_outcome =
                            TaskOutcome{TaskState::Cancelled, std::nullopt};
                        task.snapshot.control_sequence = sequence;
                        task.snapshot.updated_at = Timestamp::now();
                    }
                }
                std::optional<Error> failure;
                for (auto &[session_id, session] : sessions) {
                    if (is_terminal(session.state))
                        continue;
                    try {
                        auto released = session.environment->interrupt(make_control_context());
                        if (!released) {
                            if (!failure)
                                failure = released.error();
                            session.state = SessionState::Failed;
                        } else {
                            session.state = SessionState::Closed;
                        }
                    } catch (...) {
                        if (!failure)
                            failure = make_error(ErrorCode::Internal,
                                                 "environment interrupt threw during shutdown");
                        session.state = SessionState::Failed;
                    }
                }
                const auto target = failure ? RuntimeState::Failed : RuntimeState::Quiesced;
                if (!runtime_state.transition(RuntimeState::Stopping, target)) {
                    failure = make_error(ErrorCode::InvalidState,
                                         "runtime shutdown state transition rejected");
                }
                set_receipt(handle, CommandKind::ShutdownRuntime, ReceiptStatus::Accepted,
                            sequence);
                set_outcome(handle, failure ? SettlementStatus::Failed : SettlementStatus::Applied,
                            std::move(failure));
            });
        if (command)
            shutdown_command = command.value();
        return command;
    }

    Result<CommandHandle> request_shutdown_admitted() {
        const auto current = runtime_state.load();
        if (shutdown_command || current == RuntimeState::Stopped ||
            current == RuntimeState::Quiesced || current == RuntimeState::Stopping) {
            return make_error(ErrorCode::InvalidState, "runtime shutdown is already in progress");
        }
        if (!runtime_state.transition(current, RuntimeState::Stopping)) {
            return make_error(ErrorCode::InvalidState,
                              "runtime shutdown state transition rejected");
        }
        if (!executor_initialized) {
            return make_error(ErrorCode::InvalidState, "runtime was not initialized");
        }
        return submit_shutdown_admitted();
    }

    // Called only by the external finalizer after business admission closed.
    // A late facade rejection must settle handles whose callback never ran.
    void consume_commands() {
        std::vector<std::shared_ptr<CommandHandle::State>> pending;
        {
            std::lock_guard lock(commands_mutex);
            pending.swap(commands);
        }
        for (const auto &command : pending) {
            std::optional<Error> failure;
            try {
                if (!command->submission_future.valid()) {
                    failure =
                        make_error(ErrorCode::Internal, "command submission future is invalid");
                } else {
                    command->submission_future.get();
                }
            } catch (const kairo::CapacityExhaustedException &) {
                failure = make_error(ErrorCode::ResourceExhausted,
                                     "command submission was rejected", true);
            } catch (const kairo::ExecutorStopping &) {
                failure = make_error(ErrorCode::Unavailable, "command executor stopped", true);
            } catch (...) {
                failure = make_error(ErrorCode::Internal, "command submission failed");
            }
            if (failure) {
                set_receipt(command, command->kind, ReceiptStatus::Rejected, 0, failure);
                set_outcome(command, SettlementStatus::Failed, failure);
            }
        }
    }

    Result<CommandHandle> simple_task_command(CommandKind kind, TaskId task_id, TaskState target) {
        return enqueue(kind, [this, kind, task_id, target](std::uint64_t sequence,
                                                           CommandHandle::State &state) {
            std::lock_guard lock(records_mutex);
            const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
            const auto found = tasks.find(task_id);
            if (found == tasks.end()) {
                const auto reason = make_error(ErrorCode::NotFound, "task was not found");
                set_receipt(handle, kind, ReceiptStatus::Rejected, sequence, reason);
                set_outcome(handle, SettlementStatus::Failed, reason);
                return;
            }
            auto &snapshot = found->second.snapshot;
            if (is_terminal(snapshot.state) && snapshot.state == target) {
                set_receipt(handle, kind, ReceiptStatus::Accepted, sequence);
                set_outcome(handle, SettlementStatus::NoOp, std::nullopt, snapshot);
                return;
            }
            auto transition_via = [&](TaskState intermediate) {
                if (!valid_task_transition(snapshot.state, intermediate) ||
                    !valid_task_transition(intermediate, target)) {
                    return false;
                }
                snapshot.state = intermediate;
                return true;
            };
            const bool direct_transition = valid_task_transition(snapshot.state, target);
            const bool pause_transition = target == TaskState::Paused &&
                                          snapshot.state != TaskState::Pausing &&
                                          transition_via(TaskState::Pausing);
            const bool cancel_transition = target == TaskState::Cancelled &&
                                           snapshot.state != TaskState::Cancelling &&
                                           transition_via(TaskState::Cancelling);
            const bool allowed = direct_transition || pause_transition || cancel_transition;
            if (!allowed) {
                const auto reason =
                    make_error(ErrorCode::InvalidState, "task state transition is not allowed");
                set_receipt(handle, kind, ReceiptStatus::Rejected, sequence, reason);
                set_outcome(handle, SettlementStatus::Failed, reason, snapshot);
                return;
            }
            const auto previous_state = snapshot.state;
            snapshot.state = target;
            if (target == TaskState::Observing &&
                (previous_state == TaskState::Paused ||
                 previous_state == TaskState::SuspendedForTakeover ||
                 previous_state == TaskState::Recovering)) {
                ++snapshot.epoch;
            }
            if (target == TaskState::Paused || target == TaskState::SuspendedForTakeover ||
                target == TaskState::Cancelled || target == TaskState::Recovering) {
                ++snapshot.epoch;
            }
            if (target == TaskState::Cancelled) {
                snapshot.terminal_outcome = TaskOutcome{TaskState::Cancelled, std::nullopt};
            }
            snapshot.control_sequence = sequence;
            snapshot.updated_at = Timestamp::now();
            set_receipt(handle, kind, ReceiptStatus::Accepted, sequence);
            set_outcome(handle, SettlementStatus::Applied, std::nullopt, snapshot);
        });
    }

    Result<CommandHandle> complete_task(TaskId task_id, TaskOutcome outcome) {
        return enqueue(CommandKind::CompleteTask, [this, task_id,
                                                   outcome](std::uint64_t sequence,
                                                            CommandHandle::State &state) {
            std::lock_guard lock(records_mutex);
            const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
            const auto found = tasks.find(task_id);
            if (found == tasks.end()) {
                const auto reason = make_error(ErrorCode::NotFound, "task was not found");
                set_receipt(handle, CommandKind::CompleteTask, ReceiptStatus::Rejected, sequence,
                            reason);
                set_outcome(handle, SettlementStatus::Failed, reason);
                return;
            }
            auto &snapshot = found->second.snapshot;
            if (outcome.terminal_state != TaskState::Completed &&
                outcome.terminal_state != TaskState::Failed) {
                const auto reason =
                    make_error(ErrorCode::InvalidArgument,
                               "task completion requires the Completed or Failed terminal state");
                set_receipt(handle, CommandKind::CompleteTask, ReceiptStatus::Rejected, sequence,
                            reason);
                set_outcome(handle, SettlementStatus::Failed, reason, snapshot);
                return;
            }
            if (is_terminal(snapshot.state)) {
                // Terminal states are idempotent per state and never revive:
                // repeating the same settlement is a NoOp, a conflicting one
                // is rejected (RULE-03).
                const auto reason =
                    make_error(ErrorCode::InvalidState,
                               snapshot.state == outcome.terminal_state
                                   ? "task already settled in the requested terminal state"
                                   : "task already settled in a different terminal state");
                set_receipt(handle, CommandKind::CompleteTask,
                            snapshot.state == outcome.terminal_state ? ReceiptStatus::Accepted
                                                                     : ReceiptStatus::Rejected,
                            sequence, reason);
                set_outcome(handle,
                            snapshot.state == outcome.terminal_state ? SettlementStatus::NoOp
                                                                     : SettlementStatus::Failed,
                            reason, snapshot);
                return;
            }
            if (!task_state_path_exists(snapshot.state, outcome.terminal_state)) {
                const auto reason =
                    make_error(ErrorCode::InvalidState,
                               "no legal task transition path to the requested terminal state");
                set_receipt(handle, CommandKind::CompleteTask, ReceiptStatus::Rejected, sequence,
                            reason);
                set_outcome(handle, SettlementStatus::Failed, reason, snapshot);
                return;
            }
            snapshot.state = outcome.terminal_state;
            ++snapshot.epoch;
            snapshot.terminal_outcome = outcome;
            snapshot.control_sequence = sequence;
            snapshot.updated_at = Timestamp::now();
            set_receipt(handle, CommandKind::CompleteTask, ReceiptStatus::Accepted, sequence);
            set_outcome(handle, SettlementStatus::Applied, std::nullopt, snapshot);
        });
    }
};

MiraRuntime::MiraRuntime(RuntimeConfig config) : impl_(std::make_unique<Impl>(config)) {}

MiraRuntime::~MiraRuntime() {
    try {
        (void)finish_shutdown();
    } catch (...) {
    }
}

Result<void> MiraRuntime::initialize() {
    std::lock_guard admission(impl_->admission_mutex);
    if (!impl_->runtime_state.transition(RuntimeState::Constructed, RuntimeState::Initializing)) {
        return make_error(ErrorCode::InvalidState, "runtime was already initialized or stopped");
    }
    std::optional<Error> failure;
    if (impl_->config.worker_threads == 0 || impl_->config.executor_queue_capacity == 0 ||
        impl_->config.max_in_flight == 0) {
        failure = make_error(ErrorCode::InvalidArgument, "runtime limits must be positive");
    } else {
        kairo::ExecutorConfig executor_config;
        executor_config.min_threads = impl_->config.worker_threads;
        executor_config.max_threads = impl_->config.worker_threads;
        executor_config.queue_capacity = impl_->config.executor_queue_capacity;
        executor_config.task_graph_retention_capacity = impl_->config.max_in_flight;
        executor_config.max_in_flight_tasks = impl_->config.max_in_flight;
        try {
            impl_->executor_initialized =
                static_cast<bool>(impl_->executor.initialize(executor_config));
            if (!impl_->executor_initialized) {
                failure =
                    make_error(ErrorCode::Unavailable, "executor initialization failed", true);
            }
        } catch (...) {
            failure = make_error(ErrorCode::Unavailable, "executor initialization threw", true);
        }
    }
    const auto target = failure ? RuntimeState::Failed : RuntimeState::Running;
    if (!impl_->runtime_state.transition(RuntimeState::Initializing, target)) {
        return make_error(ErrorCode::InvalidState,
                          "runtime initialization state transition rejected");
    }
    if (failure)
        return *failure;
    return Result<void>{};
}

Result<SessionSubmission> MiraRuntime::open_session(std::shared_ptr<IEnvironment> environment,
                                                    SessionConfig config) {
    if (!environment)
        return make_error(ErrorCode::InvalidArgument, "environment must not be null");
    const auto session_id = SessionId::generate();
    auto command = impl_->enqueue(
        CommandKind::OpenSession, [this, session_id, environment = std::move(environment),
                                   config](std::uint64_t sequence, CommandHandle::State &state) {
            std::lock_guard lock(impl_->records_mutex);
            if (impl_->sessions.contains(session_id)) {
                const auto reason = make_error(ErrorCode::AlreadyExists, "session ID collision");
                Impl::set_receipt(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                                  CommandKind::OpenSession, ReceiptStatus::Rejected, sequence,
                                  reason);
                Impl::set_outcome(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                                  SettlementStatus::Failed, reason);
                return;
            }
            impl_->sessions.emplace(session_id,
                                    Impl::SessionRecord{session_id, SessionState::Autonomous,
                                                        environment, config.principal, 0});
            Impl::set_receipt(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                              CommandKind::OpenSession, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                              SettlementStatus::Applied);
        });
    if (!command)
        return command.error();
    return SessionSubmission{session_id, std::move(command).value()};
}

Result<TaskSubmission> MiraRuntime::submit_task(SessionId session_id, TaskSpec task) {
    if (task.goal.empty())
        return make_error(ErrorCode::InvalidArgument, "task goal must not be empty");
    const auto task_id = TaskId::generate();
    auto command = impl_->enqueue(CommandKind::SubmitTask, [this, session_id, task_id,
                                                            task = std::move(task)](
                                                               std::uint64_t sequence,
                                                               CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto session = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (session == impl_->sessions.end() || session->second.state != SessionState::Autonomous) {
            const auto reason = make_error(ErrorCode::InvalidState, "session is not autonomous");
            Impl::set_receipt(handle, CommandKind::SubmitTask, ReceiptStatus::Rejected, sequence,
                              reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        TaskSnapshot snapshot;
        snapshot.id = task_id;
        snapshot.session_id = session_id;
        snapshot.state = TaskState::Idle;
        snapshot.updated_at = Timestamp::now();
        snapshot.control_sequence = sequence;
        // The user constraints of the task are the ones it was created with
        // (M28/DEC-068 §4.5); later constraints travel through the loop input
        // seam.
        snapshot.user_constraints = task.user_constraints;
        impl_->tasks.emplace(task_id, Impl::TaskRecord{snapshot, task.goal, session_id});
        Impl::set_receipt(handle, CommandKind::SubmitTask, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied, std::nullopt, snapshot);
    });
    if (!command)
        return command.error();
    return TaskSubmission{task_id, std::move(command).value()};
}

Result<CommandHandle> MiraRuntime::pause_task(TaskId task_id) {
    return impl_->simple_task_command(CommandKind::PauseTask, task_id, TaskState::Paused);
}

Result<CommandHandle> MiraRuntime::resume_task(TaskId task_id) {
    return impl_->simple_task_command(CommandKind::ResumeTask, task_id, TaskState::Observing);
}

Result<CommandHandle> MiraRuntime::cancel_task(TaskId task_id) {
    return impl_->simple_task_command(CommandKind::CancelTask, task_id, TaskState::Cancelled);
}

Result<CommandHandle> MiraRuntime::begin_task_recovery(TaskId task_id) {
    return impl_->simple_task_command(CommandKind::BeginTaskRecovery, task_id,
                                      TaskState::Recovering);
}

Result<CommandHandle> MiraRuntime::complete_task(TaskId task_id, TaskOutcome outcome) {
    return impl_->complete_task(task_id, outcome);
}

Result<CommandHandle> MiraRuntime::request_human_takeover(SessionId session_id) {
    return impl_->enqueue(CommandKind::RequestTakeover, [this,
                                                         session_id](std::uint64_t sequence,
                                                                     CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Rejected,
                              sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        auto &session = found->second;
        if (session.state == SessionState::HumanControlled ||
            session.state == SessionState::TakeoverPending) {
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Accepted,
                              sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp);
            return;
        }
        if (!valid_session_transition(session.state, SessionState::TakeoverPending)) {
            const auto reason =
                make_error(ErrorCode::InvalidState, "session cannot enter takeover");
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Rejected,
                              sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::TakeoverPending;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id || is_terminal(task.snapshot.state))
                continue;
            if (valid_task_transition(task.snapshot.state, TaskState::TakeoverSettling)) {
                task.snapshot.state = TaskState::TakeoverSettling;
                task.snapshot.epoch++;
                task.snapshot.state = TaskState::SuspendedForTakeover;
                task.snapshot.control_sequence = sequence;
                task.snapshot.updated_at = Timestamp::now();
            }
        }
        session.state = SessionState::HumanControlled;
        // Takeover must converge autonomous activity, not just block new
        // actions: the best-effort platform release drops in-flight input and
        // unblocks environment waits (AGENTS.md Human Takeover contract).
        // Idempotent by the IEnvironment::interrupt contract, so a NoOp
        // environment is unaffected.
        static_cast<void>(session.environment->interrupt(make_control_context()));
        Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied);
    });
}

Result<CommandHandle> MiraRuntime::release_human_takeover(SessionId session_id) {
    return impl_->enqueue(CommandKind::ReleaseTakeover, [this,
                                                         session_id](std::uint64_t sequence,
                                                                     CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Rejected,
                              sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        auto &session = found->second;
        if (session.state == SessionState::Autonomous) {
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Accepted,
                              sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp);
            return;
        }
        if (session.state != SessionState::HumanControlled ||
            !valid_session_transition(session.state, SessionState::Resuming)) {
            const auto reason =
                make_error(ErrorCode::InvalidState, "session is not human controlled");
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Rejected,
                              sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::Resuming;
        ++session.environment_epoch;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id ||
                task.snapshot.state != TaskState::SuspendedForTakeover)
                continue;
            task.snapshot.state = TaskState::Observing;
            task.snapshot.epoch++;
            task.snapshot.environment_epoch = session.environment_epoch;
            task.snapshot.control_sequence = sequence;
            task.snapshot.updated_at = Timestamp::now();
        }
        session.state = SessionState::Autonomous;
        Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied);
    });
}

Result<CommandHandle> MiraRuntime::close_session(SessionId session_id) {
    return impl_->enqueue(CommandKind::CloseSession, [this,
                                                      session_id](std::uint64_t sequence,
                                                                  CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Rejected, sequence,
                              reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        auto &session = found->second;
        if (session.state == SessionState::Closed) {
            Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp);
            return;
        }
        if (!valid_session_transition(session.state, SessionState::Closing)) {
            const auto reason = make_error(ErrorCode::InvalidState, "session cannot be closed");
            Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Rejected, sequence,
                              reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::Closing;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id || is_terminal(task.snapshot.state))
                continue;
            task.snapshot.state = TaskState::Cancelled;
            task.snapshot.epoch++;
            task.snapshot.terminal_outcome = TaskOutcome{TaskState::Cancelled, std::nullopt};
            task.snapshot.control_sequence = sequence;
            task.snapshot.updated_at = Timestamp::now();
        }
        // Interrupt is best effort at close; an already stopped environment
        // is expected and must not fail the close command.
        static_cast<void>(session.environment->interrupt(make_control_context()));
        session.state = SessionState::Closed;
        Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied);
    });
}

Result<CommandHandle> MiraRuntime::request_shutdown() {
    std::lock_guard admission(impl_->admission_mutex);
    return impl_->request_shutdown_admitted();
}

Result<OperationKey> MiraRuntime::begin_operation(TaskId task_id, StepId step_id) {
    std::lock_guard admission(impl_->admission_mutex);
    if (impl_->runtime_state.load() != RuntimeState::Running) {
        return make_error(ErrorCode::InvalidState, "runtime is not accepting operations");
    }
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->tasks.find(task_id);
    if (found == impl_->tasks.end())
        return make_error(ErrorCode::NotFound, "task was not found");
    if (is_terminal(found->second.snapshot.state))
        return make_error(ErrorCode::InvalidState, "task is terminal");
    const auto state = found->second.snapshot.state;
    if (state == TaskState::Paused || state == TaskState::SuspendedForTakeover) {
        // Paused and taken-over tasks admit no new autonomous work: the user
        // has withdrawn the environment from the agent, so an accepted
        // operation here would dispatch new actions behind their back.
        return make_error(ErrorCode::InvalidState,
                          "task is paused or under human takeover; no new operations are admitted");
    }
    OperationKey key{task_id, found->second.snapshot.epoch, step_id, OperationId::generate()};
    impl_->operations.emplace(key.operation_id,
                              Impl::OperationRecord{key, OperationState::Submitted});
    return key;
}

Result<CommandHandle> MiraRuntime::admit_operation_completion(const OperationKey &key) {
    return impl_->enqueue(CommandKind::OperationCompletion, [this,
                                                             key](std::uint64_t sequence,
                                                                  CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        const auto operation = impl_->operations.find(key.operation_id);
        const auto task = impl_->tasks.find(key.task_id);
        if (operation == impl_->operations.end() || task == impl_->tasks.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "operation or task was not found");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Rejected,
                              sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        if (operation->second.state == OperationState::Settled) {
            const auto reason =
                make_error(ErrorCode::InvalidState, "operation completion is duplicate");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted,
                              sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp, reason, task->second.snapshot);
            return;
        }
        if (task->second.snapshot.epoch != key.task_epoch ||
            task->second.snapshot.state == TaskState::Cancelled ||
            task->second.snapshot.state == TaskState::Completed ||
            task->second.snapshot.state == TaskState::Failed) {
            operation->second.state = OperationState::Settled;
            const auto reason =
                make_error(ErrorCode::StaleObservation, "stale operation completion ignored");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted,
                              sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp, reason, task->second.snapshot);
            return;
        }
        operation->second.state = OperationState::Settled;
        task->second.snapshot.control_sequence = sequence;
        task->second.snapshot.updated_at = Timestamp::now();
        Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted,
                          sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied, std::nullopt, task->second.snapshot);
    });
}

Result<TaskSnapshot> MiraRuntime::task_snapshot(TaskId task_id) const {
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->tasks.find(task_id);
    if (found == impl_->tasks.end())
        return make_error(ErrorCode::NotFound, "task was not found");
    return found->second.snapshot;
}

Result<SessionSnapshot> MiraRuntime::session_snapshot(SessionId session_id) const {
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->sessions.find(session_id);
    if (found == impl_->sessions.end())
        return make_error(ErrorCode::NotFound, "session was not found");
    return SessionSnapshot{found->second.id, found->second.state, found->second.environment_epoch,
                           found->second.principal};
}

RuntimeState MiraRuntime::state() const noexcept { return impl_->runtime_state.load(); }

SchedulingMetricsSnapshot MiraRuntime::scheduling_metrics() const {
    const auto metrics = impl_->executor.get_scheduling_metrics();
    return SchedulingMetricsSnapshot{
        .accepted = metrics.accepted_count,
        .accepted_degraded = metrics.accepted_degraded_count,
        .rejected = metrics.rejected_count,
        .deadline_rejected = metrics.deadline_rejected_count,
        .backend_unavailable_rejected = metrics.backend_unavailable_rejected_count,
        .capacity_rejected = metrics.capacity_rejected_count,
        .resource_rejected = metrics.resource_rejected_count,
        .affinity_mismatch = metrics.affinity_mismatch_count,
        .deadline_missed = metrics.deadline_missed_count,
        .feedback_reported = metrics.feedback_reported_count,
        .adaptive_history = metrics.adaptive_history_count,
        .load_shedding_rejected = metrics.load_shedding_rejected_count,
        .priority_promoted = metrics.priority_promoted_count,
    };
}

ShutdownReport MiraRuntime::finish_shutdown() {
    // Check before either mutex: callbacks may already own records_mutex and
    // an external finalizer may be waiting for this very callback.
    if (Impl::active_control == impl_.get()) {
        return {false, state(), 0,
                "finish_shutdown requires an external owner; pending commands not scanned"};
    }
    std::lock_guard finalizer(impl_->finalizer_mutex);
    if (impl_->final_report)
        return *impl_->final_report;
    std::optional<Error> failure;
    {
        std::lock_guard admission(impl_->admission_mutex);
        const auto current = impl_->runtime_state.load();
        if (current == RuntimeState::Constructed || current == RuntimeState::Running ||
            (current == RuntimeState::Failed && !impl_->shutdown_command)) {
            auto requested = impl_->request_shutdown_admitted();
            // No executor means no command is needed and no lazy submission.
            if (!requested && impl_->executor_initialized)
                failure = requested.error();
        }
    }
    impl_->consume_commands();
    bool resubmitted = false;
    {
        std::lock_guard admission(impl_->admission_mutex);
        if (impl_->executor_initialized && !impl_->shutdown_command &&
            impl_->runtime_state.load() == RuntimeState::Stopping) {
            resubmitted = true;
            auto barrier = impl_->submit_shutdown_admitted();
            if (!barrier)
                failure = barrier.error();
            else
                failure.reset();
        }
    }
    impl_->consume_commands();
    if (impl_->shutdown_command) {
        auto outcome = impl_->shutdown_command->outcome(std::chrono::milliseconds(0));
        if (!outcome)
            failure = outcome.error();
        else if (outcome.value().status == SettlementStatus::Failed) {
            failure = outcome.value().error.value_or(
                make_error(ErrorCode::Internal, "runtime shutdown command failed"));
        }
    }
    impl_->control_context.shutdown();
    auto current = impl_->runtime_state.load();
    if (current == RuntimeState::Stopping) {
        const auto target = failure ? RuntimeState::Failed : RuntimeState::Quiesced;
        if (!impl_->runtime_state.transition(current, target)) {
            failure = make_error(ErrorCode::InvalidState,
                                 "runtime finalization state transition rejected");
        }
    }
    const auto shutdown_result = impl_->executor.shutdown(true);
    if (shutdown_result != kairo::ShutdownResult::Completed) {
        return {false, state(), 0, "executor requires final shutdown from an external owner"};
    }
    current = impl_->runtime_state.load();
    if (!failure && current == RuntimeState::Quiesced) {
        if (!impl_->runtime_state.transition(current, RuntimeState::Stopped)) {
            failure = make_error(ErrorCode::InvalidState, "runtime stopped transition rejected");
        }
    }
    const bool clean = !failure && state() == RuntimeState::Stopped;
    impl_->final_report = ShutdownReport{
        clean, state(), 0,
        failure ? (resubmitted ? "shutdown admission retry failed: " + failure->safe_message
                               : failure->safe_message)
                : (resubmitted ? "runtime stopped after shutdown admission retry"
                               : (clean ? "runtime stopped" : "runtime shutdown failed"))};
    return *impl_->final_report;
}

} // namespace mira
