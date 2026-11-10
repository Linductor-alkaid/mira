#include "interaction_coordinator_support.hpp"

namespace mira {
namespace {
Error memory_error(ErrorCode code, const char *text) {
    Error error;
    error.code = code;
    error.domain = "mira.memory_agent";
    error.safe_message = text;
    return error;
}
OperationContext work_context(const MemoryWorkBinding &binding) {
    auto context = make_control_context();
    context.task = binding.work.task_id;
    context.session = binding.work.session_id;
    context.task_epoch = binding.work.task_epoch;
    return context;
}
} // namespace

Result<void>
InteractionCoordinator::set_memory_service(std::shared_ptr<MemoryAgentService> memory) {
    const std::lock_guard serialize(impl_->submit_mutex);
    if (impl_->initialized || impl_->stopping.load() || !memory)
        return memory_error(ErrorCode::InvalidState,
                            "configure memory service before coordinator initialization");
    impl_->memory = std::move(memory);
    return {};
}

void InteractionCoordinator::Impl::stage_memory(const InputEnvelope &input,
                                                const PrincipalTaskEntry &task) {
    if (!memory)
        return;
    auto &work = memory_works[task.task_id];
    work.status.suspended = true;
    work.status.job.reset();
    work.status.error.reset();
    const auto snapshot = runtime.task_snapshot(task.task_id);
    if (!snapshot || task.historical || is_terminal(snapshot.value().state)) {
        revoke_memory(task.task_id);
        work.status.error = memory_error(ErrorCode::InvalidState, "memory work is not live");
        return;
    }
    MemoryWorkBinding binding;
    binding.principal = input.principal;
    binding.work.task_id = task.task_id;
    binding.work.session_id = task.session_id;
    binding.work.task_epoch = snapshot.value().epoch;
    binding.work.goal = task.goal;
    binding.user_constraints = task.constraints;
    binding.environment_epoch = snapshot.value().environment_epoch;
    binding.scopes = {memory_user_scope(input.principal),
                      memory_session_scope(input.principal, task.session_id)};
    binding.request_current = [alive = memory_alive, owner = &runtime, task = task.task_id,
                               epoch = binding.work.task_epoch,
                               environment = binding.environment_epoch,
                               principal = binding.principal, authorization = config] {
        if (!alive->load(std::memory_order_acquire) ||
            !interaction_support::permitted(principal, authorization))
            return false;
        const auto current = owner->task_snapshot(task);
        return current && !is_terminal(current.value().state) && current.value().epoch == epoch &&
               current.value().environment_epoch == environment;
    };
    work.binding = std::move(binding);
    try {
        auto staged = memory->stage_input(work.binding);
        if (staged) {
            work.revision = staged.value();
            return;
        }
        work.status.error = staged.error();
    } catch (...) {
        work.status.error = memory_error(ErrorCode::Unavailable, "memory input staging failed");
    }
    // A failed refresh cannot leave the previous request candidates usable.
    const auto failure = work.status.error;
    revoke_memory(task.task_id);
    work.status.error = failure;
}

void InteractionCoordinator::Impl::activate_memory(TaskId task) {
    const auto found = memory_works.find(task);
    if (!memory || found == memory_works.end() || found->second.status.error ||
        found->second.status.terminal || !found->second.status.suspended)
        return;
    auto &work = found->second;
    try {
        if (auto acknowledged = memory->acknowledge_input(task, work.revision); !acknowledged) {
            work.status.error = acknowledged.error();
            return;
        }
        work.status.suspended = false;
        if (auto reaped = memory->reap(); !reaped) {
            work.status.error = reaped.error();
            return;
        }
        MemoryAgentRequest request;
        request.task = task;
        request.mode = MemoryAgentMode::Recall;
        const auto latest = work.binding.user_constraints.empty()
                                ? std::string{}
                                : work.binding.user_constraints.back() + "\n";
        request.query = interaction_support::bound_text(latest + work.binding.work.goal, 4096);
        auto submitted = memory->submit(std::move(request), work_context(work.binding));
        if (submitted)
            work.status.job = std::move(submitted).value();
        else
            work.status.error = submitted.error();
    } catch (...) {
        work.status.error = memory_error(ErrorCode::Unavailable, "memory recall submission failed");
    }
}

void InteractionCoordinator::Impl::settle_memory(TaskId task) {
    const auto found = memory_works.find(task);
    if (!memory || found == memory_works.end() || found->second.status.terminal)
        return;
    auto &work = found->second;
    work.status.terminal = true;
    work.status.suspended = true;
    work.status.job.reset();
    work.status.error.reset();
    try {
        if (auto retired = memory->retire(task); !retired) {
            work.status.error = retired.error();
            return;
        }
        // Retiring invalidates a staged input too. Never extract while an
        // input's delivery is unresolved; that requires host reconciliation.
        if (pending.contains(task) || uncertain.contains(task)) {
            work.status.error = memory_error(ErrorCode::InvalidState,
                                             "memory recording awaits input reconciliation");
            return;
        }
        if (auto reaped = memory->reap(); !reaped) {
            work.status.error = reaped.error();
            return;
        }
        MemoryAgentRequest request;
        request.task = task;
        request.mode = MemoryAgentMode::Record;
        request.write_scope =
            memory_session_scope(work.binding.principal, work.binding.work.session_id);
        auto submitted = memory->submit(std::move(request), work_context(work.binding));
        if (submitted)
            work.status.job = std::move(submitted).value();
        else
            work.status.error = submitted.error();
    } catch (...) {
        work.status.error = memory_error(ErrorCode::Unavailable, "memory record submission failed");
    }
}

void InteractionCoordinator::Impl::revoke_memory(TaskId task) {
    const auto found = memory_works.find(task);
    if (!memory || found == memory_works.end())
        return;
    auto &status = found->second.status;
    status.suspended = true;
    status.terminal = true;
    try {
        if (auto revoked = memory->revoke(task); !revoked)
            status.error = revoked.error();
    } catch (...) {
        status.error = memory_error(ErrorCode::Unavailable, "memory revocation failed");
    }
}

Result<MemoryLifecycleStatus>
InteractionCoordinator::memory_status(const PrincipalContext &principal, TaskId task) const {
    const std::lock_guard serialize(impl_->submit_mutex);
    const auto found = impl_->memory_works.find(task);
    if (!interaction_support::permitted(principal, impl_->config) ||
        found == impl_->memory_works.end() ||
        found->second.binding.principal.tenant_id != principal.tenant_id ||
        found->second.binding.principal.user_id != principal.user_id ||
        found->second.binding.principal.identity_version != principal.identity_version)
        return memory_error(ErrorCode::PermissionDenied, "memory lifecycle receipt is unavailable");
    auto status = found->second.status;
    if (status.job) {
        // Revalidate before sharing a captured future: erase/revoke and
        // external reap must not turn this facade into an old-data backdoor.
        const auto current = impl_->memory->result(status.job->operation);
        if (!current) {
            status.error = current.error();
            status.job.reset();
        }
    }
    return status;
}
} // namespace mira
