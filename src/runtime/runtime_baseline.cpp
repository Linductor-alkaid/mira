#include <mira/runtime_baseline.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mira {
namespace {

BaselineResult make_result(BaselineResultCode code, std::uint64_t command_id,
                           std::string message) {
    BaselineResult result;
    result.code = code;
    result.command_id = command_id;
    result.safe_message = std::move(message);
    return result;
}

BaselineResult consume_result(const std::shared_future<BaselineResult> &future,
                              std::uint64_t command_id,
                              std::chrono::milliseconds timeout) {
    if (future.wait_for(timeout) != std::future_status::ready) {
        return make_result(BaselineResultCode::TimedOut, command_id,
                           "timed out waiting for command result");
    }
    try {
        return future.get();
    } catch (const executor::TaskCancelled &error) {
        return make_result(BaselineResultCode::Cancelled, command_id, error.what());
    } catch (const executor::ExecutorStopping &error) {
        return make_result(BaselineResultCode::ContextStopped, command_id, error.what());
    } catch (const std::exception &error) {
        return make_result(BaselineResultCode::Failed, command_id, error.what());
    } catch (...) {
        return make_result(BaselineResultCode::Failed, command_id,
                           "command failed with an unknown exception");
    }
}

} // namespace

class RuntimeBaseline::Impl final {
public:
    explicit Impl(BaselineRuntimeConfig runtime_config) : config(runtime_config) {}

    struct CommandEntry final {
        executor::TaskHandle handle;
        std::shared_future<BaselineResult> future;
        bool observed = false;
    };

    struct TaskState final {
        std::uint64_t epoch = 0;
        bool terminal = false;
    };

    BaselineResult apply(BaselineCommand command) {
        const auto sequence = ++control_sequence;
        auto &task = tasks[command.task_id];
        BaselineResult result;
        result.command_id = command.command_id;
        result.control_sequence = sequence;

        if (command.kind == BaselineCommandKind::DiagnosticFailure) {
            throw std::runtime_error("injected control callback failure");
        }
        if (command.kind == BaselineCommandKind::Completion &&
            (task.terminal || command.epoch != task.epoch)) {
            result.code = BaselineResultCode::StaleCompletionIgnored;
            result.task_terminal = task.terminal;
            result.safe_message = "stale completion ignored";
            return result;
        }
        if (command.kind == BaselineCommandKind::CompleteTask) {
            task.epoch = command.epoch;
            task.terminal = true;
        } else if (!task.terminal) {
            task.epoch = command.epoch;
        }

        result.code = BaselineResultCode::Applied;
        result.task_terminal = task.terminal;
        result.safe_message = "command applied";
        return result;
    }

    bool mark_observed(std::uint64_t command_id) {
        std::lock_guard lock(entries_mutex);
        const auto found = entries.find(command_id);
        if (found != entries.end() && !found->second.observed) {
            found->second.observed = true;
            unobserved_results.fetch_sub(1, std::memory_order_relaxed);
            entries.erase(found);
            return true;
        }
        return false;
    }

    BaselineRuntimeConfig config;
    executor::Executor executor;
    executor::SerialExecutionContext control_context;

    mutable std::mutex entries_mutex;
    std::unordered_map<std::uint64_t, CommandEntry> entries;
    std::unordered_map<std::uint64_t, TaskState> tasks;
    std::shared_future<BaselineResult> shutdown_future;

    std::atomic<BaselineRuntimeState> state{BaselineRuntimeState::Constructed};
    std::atomic<std::size_t> admission_rejections{0};
    std::atomic<std::size_t> unobserved_results{0};
    std::atomic<std::uint64_t> control_sequence{0};
};

RuntimeBaseline::RuntimeBaseline(BaselineRuntimeConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

RuntimeBaseline::~RuntimeBaseline() {
    try {
        finish_shutdown();
    } catch (...) {
        // Destructors cannot surface teardown failures; explicit
        // finish_shutdown() remains the observable owner boundary.
        static_cast<void>(0);
    }
}

bool RuntimeBaseline::initialize() {
    auto expected = BaselineRuntimeState::Constructed;
    if (!impl_->state.compare_exchange_strong(expected, BaselineRuntimeState::Running)) {
        return false;
    }
    if (impl_->config.worker_threads == 0 || impl_->config.executor_queue_capacity == 0 ||
        impl_->config.max_in_flight == 0) {
        impl_->state.store(BaselineRuntimeState::Failed);
        return false;
    }

    executor::ExecutorConfig executor_config;
    executor_config.min_threads = impl_->config.worker_threads;
    executor_config.max_threads = impl_->config.worker_threads;
    executor_config.queue_capacity = impl_->config.executor_queue_capacity;
    executor_config.enable_work_stealing = impl_->config.worker_threads > 1;
    executor_config.task_graph_retention_capacity = impl_->config.max_in_flight;
    executor_config.max_in_flight_tasks = impl_->config.max_in_flight;
    impl_->executor.set_cancellation_registry_capacity(impl_->config.max_in_flight + 1);
    const auto initialized = impl_->executor.initialize_ex(executor_config);
    if (!initialized.ok) {
        impl_->state.store(BaselineRuntimeState::Failed);
        return false;
    }
    return true;
}

BaselineSubmission RuntimeBaseline::submit(BaselineCommand command) {
    BaselineSubmission submission;
    submission.command_id = command.command_id;
    if (impl_->state.load(std::memory_order_acquire) != BaselineRuntimeState::Running) {
        submission.rejection = make_result(BaselineResultCode::Rejected, command.command_id,
                                           "runtime is not accepting commands");
        impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
        return submission;
    }
    if (command.command_id == 0 || command.task_id == 0) {
        submission.rejection = make_result(BaselineResultCode::Rejected, command.command_id,
                                           "command_id and task_id must be non-zero");
        impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
        return submission;
    }

    {
        std::lock_guard lock(impl_->entries_mutex);
        if (impl_->entries.contains(command.command_id)) {
            submission.rejection = make_result(BaselineResultCode::Rejected, command.command_id,
                                               "duplicate command_id");
            impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
            return submission;
        }
    }

    try {
        auto tracked = impl_->executor.submit_on_with_handle(
            impl_->control_context, [this, command] { return impl_->apply(command); });
        auto result_future = tracked.future.share();
        if (result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                static_cast<void>(result_future.get());
            } catch (const executor::CapacityExhaustedException &error) {
                submission.rejection = make_result(BaselineResultCode::Rejected,
                                                   command.command_id, error.what());
                impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
                return submission;
            } catch (const executor::ExecutorStopping &error) {
                submission.rejection = make_result(BaselineResultCode::Rejected,
                                                   command.command_id, error.what());
                impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
                return submission;
            } catch (const std::exception &) {
                // A callback exception is an admitted command failure and is
                // converted by wait(), not a submission rejection.
            }
        }
        Impl::CommandEntry entry;
        entry.handle = tracked.handle;
        entry.future = std::move(result_future);
        {
            std::lock_guard lock(impl_->entries_mutex);
            impl_->entries.emplace(command.command_id, std::move(entry));
        }
        impl_->unobserved_results.fetch_add(1, std::memory_order_relaxed);
        submission.admitted = true;
        return submission;
    } catch (const std::exception &error) {
        impl_->admission_rejections.fetch_add(1, std::memory_order_relaxed);
        submission.rejection =
            make_result(BaselineResultCode::Rejected, command.command_id, error.what());
        return submission;
    }
}

BaselineResult RuntimeBaseline::wait(std::uint64_t command_id,
                                     std::chrono::milliseconds timeout) {
    std::shared_future<BaselineResult> future;
    {
        std::lock_guard lock(impl_->entries_mutex);
        const auto found = impl_->entries.find(command_id);
        if (found == impl_->entries.end()) {
            return make_result(BaselineResultCode::NotFound, command_id, "command was not found");
        }
        future = found->second.future;
    }
    auto result = consume_result(future, command_id, timeout);
    if (result.code != BaselineResultCode::TimedOut) {
        static_cast<void>(impl_->mark_observed(command_id));
    }
    return result;
}

BaselineResult RuntimeBaseline::cancel(std::uint64_t command_id) {
    executor::TaskHandle handle;
    {
        std::lock_guard lock(impl_->entries_mutex);
        const auto found = impl_->entries.find(command_id);
        if (found == impl_->entries.end()) {
            return make_result(BaselineResultCode::NotFound, command_id, "command was not found");
        }
        handle = found->second.handle;
    }
    const auto response = impl_->executor.request_task_cancel(handle);
    if (response.result == executor::TaskCancellationResult::RequestedBeforeStart ||
        response.result == executor::TaskCancellationResult::RequestedRunning ||
        response.result == executor::TaskCancellationResult::AlreadyRequested) {
        return make_result(BaselineResultCode::Cancelled, command_id,
                           executor::to_string(response.result));
    }
    if (response.result == executor::TaskCancellationResult::AlreadyCompleted) {
        return make_result(BaselineResultCode::Applied, command_id,
                           executor::to_string(response.result));
    }
    return make_result(BaselineResultCode::Rejected, command_id,
                       executor::to_string(response.result));
}

bool RuntimeBaseline::request_shutdown() {
    auto expected = BaselineRuntimeState::Running;
    if (!impl_->state.compare_exchange_strong(expected, BaselineRuntimeState::Stopping)) {
        return expected == BaselineRuntimeState::Stopping ||
               expected == BaselineRuntimeState::Quiesced ||
               expected == BaselineRuntimeState::Stopped;
    }
    try {
        auto submission = impl_->executor.submit_on_with_handle(
            impl_->control_context, [this] {
                BaselineResult value;
                value.code = BaselineResultCode::Applied;
                value.control_sequence = ++impl_->control_sequence;
                value.safe_message = "runtime quiesced";
                impl_->state.store(BaselineRuntimeState::Quiesced,
                                   std::memory_order_release);
                return value;
            });
        impl_->shutdown_future = submission.future.share();
        if (impl_->shutdown_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                static_cast<void>(impl_->shutdown_future.get());
            } catch (...) {
                impl_->state.store(BaselineRuntimeState::Failed, std::memory_order_release);
                return false;
            }
        }
        return true;
    } catch (...) {
        impl_->state.store(BaselineRuntimeState::Failed, std::memory_order_release);
        return false;
    }
}

void RuntimeBaseline::finish_shutdown() {
    auto current = impl_->state.load(std::memory_order_acquire);
    if (current == BaselineRuntimeState::Stopped) {
        return;
    }
    if (current == BaselineRuntimeState::Constructed) {
        impl_->control_context.shutdown();
        static_cast<void>(impl_->executor.shutdown(true));
        impl_->state.store(BaselineRuntimeState::Stopped, std::memory_order_release);
        return;
    }
    if (current == BaselineRuntimeState::Running) {
        static_cast<void>(request_shutdown());
    }
    impl_->control_context.shutdown();
    if (impl_->shutdown_future.valid()) {
        try {
            static_cast<void>(impl_->shutdown_future.get());
        } catch (...) {
        }
    }

    struct PendingResult final {
        std::uint64_t command_id;
        std::shared_future<BaselineResult> result;
    };
    std::vector<PendingResult> pending;
    {
        std::lock_guard lock(impl_->entries_mutex);
        pending.reserve(impl_->entries.size());
        for (const auto &[command_id, entry] : impl_->entries) {
            if (!entry.observed) {
                pending.push_back({command_id, entry.future});
            }
        }
    }
    for (const auto &entry : pending) {
        static_cast<void>(consume_result(entry.result, entry.command_id, std::chrono::seconds(30)));
        static_cast<void>(impl_->mark_observed(entry.command_id));
    }
    static_cast<void>(impl_->executor.shutdown(true));
    impl_->state.store(BaselineRuntimeState::Stopped, std::memory_order_release);
}

BaselineRuntimeStatus RuntimeBaseline::status() const {
    BaselineRuntimeStatus result;
    result.state = impl_->state.load(std::memory_order_acquire);
    result.in_flight = impl_->executor.get_in_flight_submissions();
    result.admission_rejections = impl_->admission_rejections.load(std::memory_order_relaxed);
    result.unobserved_results = impl_->unobserved_results.load(std::memory_order_relaxed);
    result.last_control_sequence = impl_->control_sequence.load(std::memory_order_acquire);
    return result;
}

} // namespace mira
