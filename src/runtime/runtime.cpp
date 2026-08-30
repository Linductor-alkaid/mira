#include <mira/runtime.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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

template <typename T>
Result<T> wait_result(const std::shared_future<T> &future, std::chrono::milliseconds timeout) {
    if (!future.valid()) return make_error(ErrorCode::InvalidState, "command handle is invalid");
    if (future.wait_for(timeout) != std::future_status::ready) {
        return make_error(ErrorCode::DeadlineExceeded, "command handle wait timed out", true);
    }
    return future.get();
}

} // namespace

struct CommandHandle::State final {
    explicit State(CommandId command_id) : id(command_id), receipt_future(receipt.get_future().share()),
                                           outcome_future(outcome.get_future().share()) {}
    CommandId id;
    std::promise<CommandReceipt> receipt;
    std::promise<CommandOutcome> outcome;
    std::shared_future<CommandReceipt> receipt_future;
    std::shared_future<CommandOutcome> outcome_future;
    std::shared_future<void> submission_future;
};

CommandId CommandHandle::id() const noexcept { return state_ ? state_->id : CommandId{}; }

Result<CommandReceipt> CommandHandle::receipt(std::chrono::milliseconds timeout) const {
    if (!state_) return make_error(ErrorCode::InvalidState, "command handle is invalid");
    return wait_result(state_->receipt_future, timeout);
}

Result<CommandOutcome> CommandHandle::outcome(std::chrono::milliseconds timeout) const {
    if (!state_) return make_error(ErrorCode::InvalidState, "command handle is invalid");
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

    explicit Impl(RuntimeConfig runtime_config) : config(runtime_config), runtime_id(RuntimeId::generate()) {}

    RuntimeConfig config;
    RuntimeId runtime_id;
    executor::Executor executor;
    executor::SerialExecutionContext control_context;
    std::atomic<RuntimeState> runtime_state{RuntimeState::Constructed};
    std::atomic<std::uint64_t> control_sequence{0};

    mutable std::mutex records_mutex;
    std::unordered_map<SessionId, SessionRecord, StrongIdHash<SessionId>> sessions;
    std::unordered_map<TaskId, TaskRecord, StrongIdHash<TaskId>> tasks;
    std::unordered_map<OperationId, OperationRecord, StrongIdHash<OperationId>> operations;
    std::vector<std::shared_ptr<CommandHandle::State>> commands;

    std::shared_ptr<CommandHandle::State> make_handle() {
        return std::make_shared<CommandHandle::State>(CommandId::generate());
    }

    static void set_receipt(const std::shared_ptr<CommandHandle::State> &state,
                            CommandKind kind, ReceiptStatus status, std::uint64_t sequence,
                            std::optional<Error> error = std::nullopt) noexcept {
        try {
            state->receipt.set_value(CommandReceipt{state->id, kind, status, sequence, std::move(error)});
        } catch (...) {
        }
    }

    static void set_outcome(const std::shared_ptr<CommandHandle::State> &state,
                            SettlementStatus status, std::optional<Error> error = std::nullopt,
                            std::optional<TaskSnapshot> task = std::nullopt) noexcept {
        try {
            state->outcome.set_value(CommandOutcome{state->id, status, std::move(error), std::move(task)});
        } catch (...) {
        }
    }

    template <typename Callback>
    Result<CommandHandle> enqueue(CommandKind kind, Callback callback) {
        const auto current = runtime_state.load(std::memory_order_acquire);
        if (current != RuntimeState::Running && kind != CommandKind::ShutdownRuntime) {
            return make_error(ErrorCode::InvalidState, "runtime is not accepting this command");
        }
        auto handle_state = make_handle();
        try {
            auto submission = executor.submit_on_with_handle(
                control_context, [this, kind, callback = std::move(callback), handle_state] {
                    const auto sequence = ++control_sequence;
                    try {
                        callback(sequence, *handle_state);
                    } catch (const std::exception &exception) {
                        const auto failure = make_error(ErrorCode::Internal, exception.what());
                        set_receipt(handle_state, kind, ReceiptStatus::Accepted, sequence, failure);
                        set_outcome(handle_state, SettlementStatus::Failed, failure);
                    } catch (...) {
                        const auto failure = make_error(ErrorCode::Internal, "command callback failed");
                        set_receipt(handle_state, kind, ReceiptStatus::Accepted, sequence, failure);
                        set_outcome(handle_state, SettlementStatus::Failed, failure);
                    }
                });
            auto submission_future = submission.future.share();
            if (!submission_future.valid()) {
                return make_error(ErrorCode::ResourceExhausted,
                                  "runtime command submission rejected", true);
            }
            if (submission_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                try {
                    submission_future.get();
                } catch (const executor::CapacityExhaustedException &exception) {
                    auto failure = make_error(ErrorCode::ResourceExhausted, exception.what(), true);
                    set_receipt(handle_state, kind, ReceiptStatus::Rejected, 0, failure);
                    set_outcome(handle_state, SettlementStatus::Failed, failure);
                    return failure;
                } catch (const executor::ExecutorStopping &exception) {
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
                std::lock_guard lock(records_mutex);
                commands.push_back(handle_state);
            }
            return CommandHandle(std::move(handle_state));
        } catch (const std::exception &exception) {
            return make_error(ErrorCode::ResourceExhausted, exception.what(), true);
        } catch (...) {
            return make_error(ErrorCode::ResourceExhausted, "runtime command submission rejected", true);
        }
    }

    Result<CommandHandle> simple_task_command(CommandKind kind, TaskId task_id, TaskState target) {
        return enqueue(kind, [this, kind, task_id, target](std::uint64_t sequence, CommandHandle::State &state) {
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
                const auto reason = make_error(ErrorCode::InvalidState, "task state transition is not allowed");
                set_receipt(handle, kind, ReceiptStatus::Rejected, sequence, reason);
                set_outcome(handle, SettlementStatus::Failed, reason, snapshot);
                return;
            }
            const auto previous_state = snapshot.state;
            snapshot.state = target;
            if (target == TaskState::Observing &&
                (previous_state == TaskState::Paused || previous_state == TaskState::SuspendedForTakeover)) {
                ++snapshot.epoch;
            }
            if (target == TaskState::Paused || target == TaskState::SuspendedForTakeover ||
                target == TaskState::Cancelled) {
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
};

MiraRuntime::MiraRuntime(RuntimeConfig config) : impl_(std::make_unique<Impl>(config)) {}

MiraRuntime::~MiraRuntime() {
    try {
        (void)finish_shutdown();
    } catch (...) {
    }
}

Result<void> MiraRuntime::initialize() {
    auto expected = RuntimeState::Constructed;
    if (!impl_->runtime_state.compare_exchange_strong(expected, RuntimeState::Initializing)) {
        return make_error(ErrorCode::InvalidState, "runtime was already initialized");
    }
    if (impl_->config.worker_threads == 0 || impl_->config.executor_queue_capacity == 0 ||
        impl_->config.max_in_flight == 0) {
        impl_->runtime_state.store(RuntimeState::Failed, std::memory_order_release);
        return make_error(ErrorCode::InvalidArgument, "runtime limits must be positive");
    }
    executor::ExecutorConfig executor_config;
    executor_config.min_threads = impl_->config.worker_threads;
    executor_config.max_threads = impl_->config.worker_threads;
    executor_config.queue_capacity = impl_->config.executor_queue_capacity;
    executor_config.task_graph_retention_capacity = impl_->config.max_in_flight;
    executor_config.max_in_flight_tasks = impl_->config.max_in_flight;
    if (!impl_->executor.initialize(executor_config)) {
        impl_->runtime_state.store(RuntimeState::Failed, std::memory_order_release);
        return make_error(ErrorCode::Unavailable, "executor initialization failed", true);
    }
    impl_->runtime_state.store(RuntimeState::Running, std::memory_order_release);
    return Result<void>{};
}

Result<SessionSubmission> MiraRuntime::open_session(std::shared_ptr<IEnvironment> environment,
                                                    SessionConfig config) {
    if (!environment) return make_error(ErrorCode::InvalidArgument, "environment must not be null");
    const auto session_id = SessionId::generate();
    auto command = impl_->enqueue(CommandKind::OpenSession,
                                  [this, session_id, environment = std::move(environment), config]
                                  (std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        if (impl_->sessions.contains(session_id)) {
            const auto reason = make_error(ErrorCode::AlreadyExists, "session ID collision");
            Impl::set_receipt(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                              CommandKind::OpenSession, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                              SettlementStatus::Failed, reason);
            return;
        }
        impl_->sessions.emplace(session_id, Impl::SessionRecord{session_id, SessionState::Autonomous,
                                                                  environment, config.principal, 0});
        Impl::set_receipt(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                          CommandKind::OpenSession, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                          SettlementStatus::Applied);
    });
    if (!command) return command.error();
    return SessionSubmission{session_id, std::move(command).value()};
}

Result<TaskSubmission> MiraRuntime::submit_task(SessionId session_id, TaskSpec task) {
    if (task.goal.empty()) return make_error(ErrorCode::InvalidArgument, "task goal must not be empty");
    const auto task_id = TaskId::generate();
    auto command = impl_->enqueue(CommandKind::SubmitTask,
                                  [this, session_id, task_id, task = std::move(task)]
                                  (std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto session = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (session == impl_->sessions.end() || session->second.state != SessionState::Autonomous) {
            const auto reason = make_error(ErrorCode::InvalidState, "session is not autonomous");
            Impl::set_receipt(handle, CommandKind::SubmitTask, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        TaskSnapshot snapshot;
        snapshot.id = task_id;
        snapshot.session_id = session_id;
        snapshot.state = TaskState::Idle;
        snapshot.updated_at = Timestamp::now();
        snapshot.control_sequence = sequence;
        impl_->tasks.emplace(task_id, Impl::TaskRecord{snapshot, task.goal, session_id});
        Impl::set_receipt(handle, CommandKind::SubmitTask, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied, std::nullopt, snapshot);
    });
    if (!command) return command.error();
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

Result<CommandHandle> MiraRuntime::request_human_takeover(SessionId session_id) {
    return impl_->enqueue(CommandKind::RequestTakeover,
                          [this, session_id](std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        auto &session = found->second;
        if (session.state == SessionState::HumanControlled || session.state == SessionState::TakeoverPending) {
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp);
            return;
        }
        if (!valid_session_transition(session.state, SessionState::TakeoverPending)) {
            const auto reason = make_error(ErrorCode::InvalidState, "session cannot enter takeover");
            Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::TakeoverPending;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id || is_terminal(task.snapshot.state)) continue;
            if (valid_task_transition(task.snapshot.state, TaskState::TakeoverSettling)) {
                task.snapshot.state = TaskState::TakeoverSettling;
                task.snapshot.epoch++;
                task.snapshot.state = TaskState::SuspendedForTakeover;
                task.snapshot.control_sequence = sequence;
                task.snapshot.updated_at = Timestamp::now();
            }
        }
        session.state = SessionState::HumanControlled;
        Impl::set_receipt(handle, CommandKind::RequestTakeover, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied);
    });
}

Result<CommandHandle> MiraRuntime::release_human_takeover(SessionId session_id) {
    return impl_->enqueue(CommandKind::ReleaseTakeover,
                          [this, session_id](std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        auto &session = found->second;
        if (session.state == SessionState::Autonomous) {
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp);
            return;
        }
        if (session.state != SessionState::HumanControlled ||
            !valid_session_transition(session.state, SessionState::Resuming)) {
            const auto reason = make_error(ErrorCode::InvalidState, "session is not human controlled");
            Impl::set_receipt(handle, CommandKind::ReleaseTakeover, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::Resuming;
        ++session.environment_epoch;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id || task.snapshot.state != TaskState::SuspendedForTakeover) continue;
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
    return impl_->enqueue(CommandKind::CloseSession,
                          [this, session_id](std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto found = impl_->sessions.find(session_id);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        if (found == impl_->sessions.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "session was not found");
            Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Rejected, sequence, reason);
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
            Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        session.state = SessionState::Closing;
        for (auto &[task_id, task] : impl_->tasks) {
            if (task.session_id != session_id || is_terminal(task.snapshot.state)) continue;
            task.snapshot.state = TaskState::Cancelled;
            task.snapshot.epoch++;
            task.snapshot.terminal_outcome = TaskOutcome{TaskState::Cancelled, std::nullopt};
            task.snapshot.control_sequence = sequence;
            task.snapshot.updated_at = Timestamp::now();
        }
        session.environment->interrupt();
        session.state = SessionState::Closed;
        Impl::set_receipt(handle, CommandKind::CloseSession, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied);
    });
}

Result<CommandHandle> MiraRuntime::request_shutdown() {
    const auto current = impl_->runtime_state.load(std::memory_order_acquire);
    if (current == RuntimeState::Stopped || current == RuntimeState::Quiesced || current == RuntimeState::Stopping) {
        return make_error(ErrorCode::InvalidState, "runtime shutdown is already in progress");
    }
    if (current == RuntimeState::Constructed) {
        impl_->runtime_state.store(RuntimeState::Stopping, std::memory_order_release);
        return make_error(ErrorCode::InvalidState, "runtime was not initialized");
    }
    impl_->runtime_state.store(RuntimeState::Stopping, std::memory_order_release);
    return impl_->enqueue(CommandKind::ShutdownRuntime,
                          [this](std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        for (auto &[task_id, task] : impl_->tasks) {
            if (!is_terminal(task.snapshot.state)) {
                task.snapshot.state = TaskState::Cancelled;
                task.snapshot.epoch++;
                task.snapshot.terminal_outcome = TaskOutcome{TaskState::Cancelled, std::nullopt};
                task.snapshot.control_sequence = sequence;
                task.snapshot.updated_at = Timestamp::now();
            }
        }
        for (auto &[session_id, session] : impl_->sessions) {
            if (!is_terminal(session.state)) {
                session.environment->interrupt();
                session.state = SessionState::Closed;
            }
        }
        impl_->runtime_state.store(RuntimeState::Quiesced, std::memory_order_release);
        Impl::set_receipt(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}),
                          CommandKind::ShutdownRuntime, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(std::shared_ptr<CommandHandle::State>(&state, [](auto *) {}), SettlementStatus::Applied);
    });
}

Result<OperationKey> MiraRuntime::begin_operation(TaskId task_id, StepId step_id) {
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->tasks.find(task_id);
    if (found == impl_->tasks.end()) return make_error(ErrorCode::NotFound, "task was not found");
    if (is_terminal(found->second.snapshot.state)) return make_error(ErrorCode::InvalidState, "task is terminal");
    OperationKey key{task_id, found->second.snapshot.epoch, step_id, OperationId::generate()};
    impl_->operations.emplace(key.operation_id, Impl::OperationRecord{key, OperationState::Submitted});
    return key;
}

Result<CommandHandle> MiraRuntime::admit_operation_completion(const OperationKey &key) {
    return impl_->enqueue(CommandKind::OperationCompletion,
                          [this, key](std::uint64_t sequence, CommandHandle::State &state) {
        std::lock_guard lock(impl_->records_mutex);
        const auto handle = std::shared_ptr<CommandHandle::State>(&state, [](auto *) {});
        const auto operation = impl_->operations.find(key.operation_id);
        const auto task = impl_->tasks.find(key.task_id);
        if (operation == impl_->operations.end() || task == impl_->tasks.end()) {
            const auto reason = make_error(ErrorCode::NotFound, "operation or task was not found");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Rejected, sequence, reason);
            Impl::set_outcome(handle, SettlementStatus::Failed, reason);
            return;
        }
        if (operation->second.state == OperationState::Settled) {
            const auto reason = make_error(ErrorCode::InvalidState, "operation completion is duplicate");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp, reason, task->second.snapshot);
            return;
        }
        if (task->second.snapshot.epoch != key.task_epoch ||
            task->second.snapshot.state == TaskState::Cancelled ||
            task->second.snapshot.state == TaskState::Completed ||
            task->second.snapshot.state == TaskState::Failed) {
            operation->second.state = OperationState::Settled;
            const auto reason = make_error(ErrorCode::StaleObservation, "stale operation completion ignored");
            Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted, sequence);
            Impl::set_outcome(handle, SettlementStatus::NoOp, reason, task->second.snapshot);
            return;
        }
        operation->second.state = OperationState::Settled;
        task->second.snapshot.control_sequence = sequence;
        task->second.snapshot.updated_at = Timestamp::now();
        Impl::set_receipt(handle, CommandKind::OperationCompletion, ReceiptStatus::Accepted, sequence);
        Impl::set_outcome(handle, SettlementStatus::Applied, std::nullopt, task->second.snapshot);
    });
}

Result<TaskSnapshot> MiraRuntime::task_snapshot(TaskId task_id) const {
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->tasks.find(task_id);
    if (found == impl_->tasks.end()) return make_error(ErrorCode::NotFound, "task was not found");
    return found->second.snapshot;
}

Result<SessionSnapshot> MiraRuntime::session_snapshot(SessionId session_id) const {
    std::lock_guard lock(impl_->records_mutex);
    const auto found = impl_->sessions.find(session_id);
    if (found == impl_->sessions.end()) return make_error(ErrorCode::NotFound, "session was not found");
    return SessionSnapshot{found->second.id, found->second.state, found->second.environment_epoch,
                           found->second.principal};
}

RuntimeState MiraRuntime::state() const noexcept { return impl_->runtime_state.load(std::memory_order_acquire); }

ShutdownReport MiraRuntime::finish_shutdown() {
    auto current = impl_->runtime_state.load(std::memory_order_acquire);
    if (current == RuntimeState::Constructed) {
        impl_->control_context.shutdown();
        impl_->runtime_state.store(RuntimeState::Stopped, std::memory_order_release);
        return {true, RuntimeState::Stopped, 0, "runtime was never initialized"};
    }
    if (current == RuntimeState::Running) {
        auto requested = request_shutdown();
        if (requested) {
            auto outcome = requested.value().outcome(std::chrono::seconds(30));
            static_cast<void>(outcome);
        }
    }
    impl_->control_context.shutdown();
    std::vector<std::shared_future<void>> command_futures;
    {
        std::lock_guard lock(impl_->records_mutex);
        command_futures.reserve(impl_->commands.size());
        for (const auto &command : impl_->commands) {
            if (command->submission_future.valid()) command_futures.push_back(command->submission_future);
        }
        impl_->commands.clear();
    }
    for (const auto &future : command_futures) {
        try {
            future.get();
        } catch (...) {
        }
    }
    const auto shutdown_result = impl_->executor.shutdown(true);
    const auto clean = shutdown_result == executor::ShutdownResult::Completed;
    impl_->runtime_state.store(RuntimeState::Stopped, std::memory_order_release);
    return {clean, RuntimeState::Stopped, 0, clean ? "runtime stopped" : "executor shutdown requested from worker"};
}

} // namespace mira
