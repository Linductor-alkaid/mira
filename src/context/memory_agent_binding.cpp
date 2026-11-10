#include "memory_agent_support.hpp"

namespace mira {
using memory_agent_support::error;

Result<void> MemoryAgentService::State::validate_binding(const MemoryWorkBinding &binding) const {
    if (auto valid = validate(); !valid)
        return valid;
    if (binding.work.task_id.is_nil() || binding.work.session_id.is_nil() ||
        binding.work.goal.empty() || binding.work.goal.size() > 16 * 1024 ||
        binding.principal.tenant_id.is_nil() || binding.principal.user_id.is_nil() ||
        binding.scopes.empty() || binding.scopes.size() > 16)
        return error(ErrorCode::InvalidArgument, "invalid memory work binding");
    if (binding.user_constraints.size() > 64)
        return error(ErrorCode::ResourceExhausted, "memory work has too many constraints");
    std::size_t bytes = 0;
    for (const auto &constraint : binding.user_constraints) {
        if (constraint.size() > config.max_source_bytes - bytes)
            return error(ErrorCode::ResourceExhausted, "memory constraints exceed byte capacity");
        bytes += constraint.size();
    }
    for (const auto &scope : binding.scopes)
        if (scope.subject_id.empty() || scope.subject_id.size() > 256 ||
            scope.tenant_id != binding.principal.tenant_id.to_string())
            return error(ErrorCode::PermissionDenied, "memory scope is outside the work tenant");
    return {};
}

Result<std::uint64_t> MemoryAgentService::stage_input(MemoryWorkBinding binding) {
    auto &s = *state_;
    if (auto valid = s.validate_binding(binding); !valid)
        return valid.error();
    std::size_t bytes = binding.work.goal.size();
    if (binding.user_constraints.size() > 64)
        return error(ErrorCode::ResourceExhausted, "memory work has too many constraints");
    for (const auto &constraint : binding.user_constraints) {
        if (constraint.size() > state_->config.max_source_bytes ||
            bytes > state_->config.max_source_bytes - constraint.size())
            return error(ErrorCode::ResourceExhausted, "memory work frame exceeds byte capacity");
        bytes += constraint.size();
    }
    if (bytes > state_->config.max_source_bytes)
        return error(ErrorCode::ResourceExhausted, "memory work frame exceeds byte capacity");
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    if (!s.initialized || s.closing)
        return error(ErrorCode::InvalidState, "memory agent is not accepting bindings");
    for (const auto &scope : binding.scopes)
        if (s.blocked_scopes.contains(memory_agent_support::scope_key(scope)))
            return error(ErrorCode::PermissionDenied, "erased memory scope cannot be rebound");
    const auto found = s.works.find(binding.work.task_id);
    if (found == s.works.end()) {
        if (s.works.size() >= s.config.max_works)
            return error(ErrorCode::ResourceExhausted, "memory work capacity exceeded");
        State::Work work;
        work.binding = std::move(binding);
        work.input_pending = true;
        const auto task = work.binding.work.task_id;
        return s.works.emplace(task, std::move(work)).first->second.revision;
    }
    if (!found->second.active || found->second.revoked)
        return error(ErrorCode::InvalidState, "memory input work is not active");
    auto &work = found->second;
    if (work.binding.principal.tenant_id != binding.principal.tenant_id ||
        work.binding.principal.user_id != binding.principal.user_id ||
        work.binding.principal.identity_version > binding.principal.identity_version ||
        work.binding.work.session_id != binding.work.session_id ||
        work.binding.scopes != binding.scopes ||
        work.binding.work.task_epoch > binding.work.task_epoch ||
        work.binding.environment_epoch > binding.environment_epoch)
        return error(ErrorCode::PermissionDenied, "memory input binding cannot change ownership");
    s.invalidate(work);
    work.binding = std::move(binding);
    work.input_pending = true;
    return work.revision;
}

Result<void> MemoryAgentService::acknowledge_input(TaskId task, std::uint64_t revision) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    const auto found = s.works.find(task);
    if (s.closing || found == s.works.end() || !found->second.active || found->second.revoked ||
        found->second.revision != revision)
        return error(ErrorCode::InvalidState, "memory input acknowledgement is stale");
    found->second.input_pending = false;
    return {};
}
} // namespace mira
