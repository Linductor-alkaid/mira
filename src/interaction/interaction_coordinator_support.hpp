#pragma once

#include "interaction_routing.hpp"

#include <kairo/executor.hpp>

#include <atomic>
#include <future>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace mira {

class InteractionCoordinator::Impl final {
  public:
    struct PrincipalEntry final {
        std::vector<SessionId> sessions;
        std::vector<PrincipalTaskEntry> tasks;
    };

    Impl(CoordinatorConfig coordinator_config, MiraRuntime &runtime_ref,
         kairo::Executor &executor_ref)
        : config(std::move(coordinator_config)), runtime(runtime_ref), executor(executor_ref) {}

    [[nodiscard]] PrincipalKey key_of(const PrincipalContext &principal) const {
        return PrincipalKey{principal.tenant_id, principal.user_id};
    }

    [[nodiscard]] PrincipalState snapshot(const PrincipalContext &principal) const {
        PrincipalState state;
        const auto found = registry.find(key_of(principal));
        if (found == registry.end()) {
            return state;
        }
        state.sessions = found->second.sessions;
        state.tasks = found->second.tasks;
        return state;
    }

    // The runtime owns the task state; the registry only caches it. The
    // cached state is refreshed from the runtime before it is read, so a
    // terminal task is never routed to again (DEC-068 R2).
    void refresh_task_states() {
        std::vector<TaskId> ids;
        {
            std::lock_guard lock(mutex);
            for (const auto &entry : registry) {
                for (const auto &task : entry.second.tasks) {
                    ids.push_back(task.task_id);
                }
            }
        }
        std::vector<std::pair<TaskId, TaskState>> states;
        states.reserve(ids.size());
        for (const auto &id : ids) {
            const auto snapshot = runtime.task_snapshot(id);
            if (snapshot) {
                states.emplace_back(id, snapshot.value().state);
            }
        }
        std::lock_guard lock(mutex);
        for (auto &entry : registry) {
            for (auto &task : entry.second.tasks) {
                for (const auto &item : states) {
                    if (task.task_id == item.first) {
                        task.state = item.second;
                    }
                }
            }
        }
    }

    [[nodiscard]] PrincipalTaskEntry *task_of(PrincipalEntry &entry, TaskId task_id) {
        for (auto &task : entry.tasks) {
            if (task.task_id == task_id) {
                return &task;
            }
        }
        return nullptr;
    }

    // Appends the input to the task's constraint list. The list is the
    // latest user constraints of this work item only: an unrelated input
    // never touches another task's constraints (DEC-068 §6).
    static void append_constraint(PrincipalTaskEntry &task, std::string constraint) {
        task.constraints.push_back(std::move(constraint));
        while (task.constraints.size() > interaction_support::kMaxConstraintsPerTask) {
            task.constraints.erase(task.constraints.begin());
        }
    }

    // Pure finite work captures values only: a timeout cannot write a stale
    // context or dereference a destroyed coordinator (DEC-102).
    [[nodiscard]] Result<std::string> prepare_context(PrincipalTaskEntry task) {
        drain_retired();
        if (late_failure)
            return *late_failure;
        if (stopping.load(std::memory_order_acquire) ||
            retired_futures.size() >= config.max_retired_preparations) {
            return interaction_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::ExecutorRejected,
                                     "context preparation is closed or at capacity");
        }
        std::future<std::string> future;
        try {
            future =
                executor.submit_auto([task = std::move(task)] { return assemble_context(task); });
        } catch (...) {
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::ExecutorRejected,
                                     "the executor refused context preparation", true);
        }
        if (future.wait_for(config.command_timeout) != std::future_status::ready) {
            retired_futures.push_back(std::move(future));
            return interaction_error(ErrorCode::DeadlineExceeded,
                                     InteractionDomainCode::RuntimeCommandTimeout,
                                     "context preparation did not settle in time", true);
        }
        try {
            return future.get();
        } catch (...) {
            return interaction_error(ErrorCode::Internal, InteractionDomainCode::ExecutorRejected,
                                     "context preparation failed");
        }
    }

    // The context block of one work item: the goal, the latest user
    // constraints and the task state. It is bounded and belongs to this work
    // item only.
    [[nodiscard]] static std::string assemble_context(const PrincipalTaskEntry &task) {
        std::string block = "goal: ";
        block += interaction_support::bound_text(task.goal, interaction_support::kMaxGoalBytes);
        block += "\nstate: ";
        block += task_state_name(task.state);
        if (!task.constraints.empty()) {
            block += "\nconstraints:";
            for (const auto &constraint : task.constraints) {
                block += "\n- ";
                block += constraint;
            }
        }
        return block;
    }

    void consume(std::future<std::string> &future) {
        try {
            (void)future.get();
        } catch (...) {
            late_failure =
                interaction_error(ErrorCode::Internal, InteractionDomainCode::ExecutorRejected,
                                  "a retired context preparation failed");
        }
    }

    void drain_retired() {
        auto it = retired_futures.begin();
        while (it != retired_futures.end()) {
            if (it->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            consume(*it);
            it = retired_futures.erase(it);
        }
    }

    // Called under submit_mutex. Only successful prefixes leave the queue.
    [[nodiscard]] Result<void> flush_pending(TaskId task,
                                             const std::shared_ptr<IUserInputSink> &sink);

    CoordinatorConfig config;
    MiraRuntime &runtime;
    kairo::Executor &executor;
    std::shared_ptr<IEventStore> events;
    RuntimeId runtime_id;
    std::shared_ptr<IInputLedger> ledger;
    RouteResolver resolver;
    EnvironmentProvider environment_provider;

    mutable std::mutex mutex;
    // Serializes the coordinator's own submissions: the registry, the pending
    // deliveries and the retired futures have one writer.
    std::mutex submit_mutex;
    std::unordered_map<PrincipalKey, PrincipalEntry, PrincipalKeyHash> registry;
    std::unordered_map<TaskId, std::shared_ptr<IUserInputSink>, StrongIdHash<TaskId>> sinks;
    std::unordered_map<TaskId, std::vector<InputEnvelope>, StrongIdHash<TaskId>> pending;
    std::vector<std::future<std::string>> retired_futures;
    std::optional<Error> late_failure;
    std::unordered_map<TaskId, Error, StrongIdHash<TaskId>> uncertain;
    std::atomic<bool> stopping{false};
    std::atomic<bool> initialized{false};
};

} // namespace mira
