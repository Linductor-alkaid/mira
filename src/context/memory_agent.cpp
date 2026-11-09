#include "memory_agent_support.hpp"

#include <algorithm>

namespace mira {
using memory_agent_support::error;
namespace {} // namespace
SessionId memory_agent_support::control_partition() {
    return SessionId{*Id128::parse("c82afd01e69b47f4ac18a737d1c65036")};
}
namespace {
bool owns_scope(const MemoryWorkBinding &binding, const MemoryScope &scope) {
    return std::find(binding.scopes.begin(), binding.scopes.end(), scope) != binding.scopes.end();
}
} // namespace

std::string memory_agent_support::scope_key(const MemoryScope &scope) {
    return canonical_json_digest(JsonValue::Object{{"kind", memory_scope_kind_name(scope.kind)},
                                                   {"subject", scope.subject_id},
                                                   {"tenant", scope.tenant_id.value_or("")}})
        .to_string();
}

MemoryScope memory_user_scope(const PrincipalContext &principal) {
    return {MemoryScopeKind::User,
            digest_string("mira.memory.user.v1/" + principal.tenant_id.to_string() + "/" +
                          principal.user_id.to_string())
                .to_string(),
            principal.tenant_id.to_string()};
}
MemoryScope memory_session_scope(const PrincipalContext &principal, SessionId session) {
    return {MemoryScopeKind::Session,
            digest_string("mira.memory.session.v1/" + principal.tenant_id.to_string() + "/" +
                          session.to_string())
                .to_string(),
            principal.tenant_id.to_string()};
}

Result<void> MemoryAgentService::State::validate() const {
    if (!memory || !events || runtime.is_nil() || config.max_works == 0 || config.max_works > 256 ||
        config.max_jobs == 0 || config.max_jobs > 256 || config.max_records == 0 ||
        config.max_records > 64 || config.max_events == 0 || config.max_events > 1024 ||
        config.max_history_items == 0 || config.max_history_items > 254 ||
        config.max_source_bytes == 0 || config.max_source_bytes > 1024 * 1024 ||
        config.max_result_bytes == 0 || config.max_result_bytes > 64 * 1024 ||
        config.timeout.count() <= 0 || config.timeout > std::chrono::minutes(10) ||
        config.cache_ttl.count() <= 0 || config.cache_ttl > std::chrono::minutes(10) ||
        config.max_projection_artifacts == 0 || config.max_projection_artifacts > 4096 ||
        !config.consolidation.validate())
        return error(ErrorCode::InvalidArgument, "invalid memory agent configuration");
    return {};
}

MemoryAgentService::MemoryAgentService(kairo::Executor &executor, std::shared_ptr<IMemory> memory,
                                       std::shared_ptr<IEventStore> events, RuntimeId runtime,
                                       MemoryAgentProcessor processor, MemoryAgentConfig config)
    : state_(std::make_shared<State>(executor, std::move(memory), std::move(events), runtime,
                                     std::move(processor), std::move(config))) {}
MemoryAgentService::~MemoryAgentService() { (void)close(); }

Result<void> MemoryAgentService::initialize() {
    auto &s = *state_;
    if (auto valid = s.validate(); !valid)
        return valid;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    if (s.closing)
        return error(ErrorCode::InvalidState, "memory agent is closing");
    if (s.initialized)
        return {};
    EventQuery query{memory_agent_support::control_partition(), std::nullopt, 256};
    do {
        auto page = s.events->read(query);
        if (!page)
            return page.error();
        for (const auto &event : page.value().events) {
            query.after_sequence = event.session_sequence;
            if (event.payload.type == "mira.memory-agent.projection-stored.v1") {
                auto payload = parse_json(event.payload.data);
                if (!payload)
                    return error(ErrorCode::DataLoss,
                                 "memory projection registration is malformed");
                if (auto restored = s.restore_projection(payload.value()); !restored)
                    return restored;
                continue;
            }
            if (event.payload.type != "mira.memory-agent.scope-erased.v1")
                continue;
            auto payload = parse_json(event.payload.data);
            const auto *key = payload ? payload.value().find("scope_digest") : nullptr;
            if (!key || !key->is_string() || !digest_from_hex(*key->as_string()))
                return error(ErrorCode::DataLoss, "memory erasure barrier is malformed");
            s.blocked_scopes.insert(*key->as_string());
            if (auto erased = s.erase_projections(*key->as_string()); !erased)
                return erased;
            if (s.blocked_scopes.size() > s.config.max_works * 16)
                return error(ErrorCode::ResourceExhausted,
                             "memory erasure barrier capacity exceeded");
        }
        if (!page.value().has_more)
            break;
        if (page.value().events.empty())
            return error(ErrorCode::DataLoss, "memory erasure barrier scan made no progress");
    } while (true);
    for (const auto &scope : s.blocked_scopes)
        if (auto erased = s.erase_projections(scope); !erased)
            return erased;
    s.initialized = true;
    return {};
}

Result<void> MemoryAgentService::bind(MemoryWorkBinding binding) {
    auto &s = *state_;
    if (auto valid = s.validate(); !valid)
        return valid;
    if (binding.work.task_id.is_nil() || binding.work.session_id.is_nil() ||
        binding.work.goal.empty() || binding.work.goal.size() > 16 * 1024 ||
        binding.principal.tenant_id.is_nil() || binding.principal.user_id.is_nil() ||
        binding.scopes.empty() || binding.scopes.size() > 16)
        return error(ErrorCode::InvalidArgument, "invalid memory work binding");
    for (const auto &scope : binding.scopes)
        if (scope.subject_id.empty() || scope.subject_id.size() > 256 ||
            scope.tenant_id != binding.principal.tenant_id.to_string())
            return error(ErrorCode::PermissionDenied, "memory scope is outside the work tenant");
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    if (!s.initialized || s.closing)
        return error(ErrorCode::InvalidState, "memory agent is not accepting bindings");
    for (const auto &scope : binding.scopes)
        if (s.blocked_scopes.contains(memory_agent_support::scope_key(scope)))
            return error(ErrorCode::PermissionDenied, "erased memory scope cannot be rebound");
    if (s.works.contains(binding.work.task_id))
        return error(ErrorCode::AlreadyExists, "memory work is already bound");
    if (s.works.size() >= s.config.max_works)
        return error(ErrorCode::ResourceExhausted, "memory work capacity exceeded");
    State::Work work;
    work.binding = std::move(binding);
    s.works.emplace(work.binding.work.task_id, std::move(work));
    return {};
}

void MemoryAgentService::State::invalidate(Work &work) {
    ++work.revision;
    work.candidates.clear();
    work.prefix.clear();
    work.compacted.clear();
    work.captured.clear();
    for (const auto &[id, job] : jobs) {
        (void)id;
        if (!job->done && job->request.task == work.binding.work.task_id)
            job->cancelled.store(true);
    }
}

Result<void> MemoryAgentService::revise(TaskId task, std::uint64_t epoch,
                                        std::uint64_t environment_epoch) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    auto found = s.works.find(task);
    if (s.closing || found == s.works.end() || !found->second.active || found->second.revoked ||
        epoch < found->second.binding.work.task_epoch)
        return error(ErrorCode::InvalidState, "memory work cannot be revised");
    s.invalidate(found->second);
    found->second.binding.work.task_epoch = epoch;
    found->second.binding.environment_epoch = environment_epoch;
    return {};
}

Result<void> MemoryAgentService::retire(TaskId task) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    auto found = s.works.find(task);
    if (found == s.works.end())
        return error(ErrorCode::NotFound, "memory work is not bound");
    s.invalidate(found->second);
    found->second.active = false;
    return {};
}
Result<void> MemoryAgentService::revoke(TaskId task) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    auto found = s.works.find(task);
    if (found == s.works.end())
        return error(ErrorCode::NotFound, "memory work is not bound");
    s.invalidate(found->second);
    found->second.active = false;
    found->second.revoked = true;
    return {};
}

Result<void> MemoryAgentService::invalidate_scope(const MemoryScope &scope) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    const std::lock_guard lock(s.mutex);
    for (auto &[id, work] : s.works) {
        (void)id;
        if (owns_scope(work.binding, scope))
            s.invalidate(work);
    }
    return {};
}

Result<ErasureResult> MemoryAgentService::erase_scope(const MemoryScope &scope) {
    auto &s = *state_;
    const std::lock_guard serialize(s.commit_mutex);
    {
        const std::lock_guard lock(s.mutex);
        if (!s.initialized || s.closing)
            return error(ErrorCode::InvalidState, "memory agent is not initialized");
        if (std::none_of(s.works.begin(), s.works.end(), [&](const auto &entry) {
                return owns_scope(entry.second.binding, scope);
            }))
            return error(ErrorCode::PermissionDenied, "erasure scope has no host binding");
        const auto key = memory_agent_support::scope_key(scope);
        if (!s.blocked_scopes.contains(key) && s.blocked_scopes.size() >= s.config.max_works * 16)
            return error(ErrorCode::ResourceExhausted, "memory erasure barrier capacity exceeded");
        s.blocked_scopes.insert(key);
        for (auto &[id, work] : s.works) {
            (void)id;
            if (owns_scope(work.binding, scope)) {
                s.invalidate(work);
                work.revoked = true;
            }
        }
    }
    AppendRequest barrier;
    barrier.event_id = EventId::generate();
    barrier.runtime_id = s.runtime;
    barrier.session_id = memory_agent_support::control_partition();
    barrier.payload = {
        "mira.memory-agent.scope-erased.v1",
        to_json_string(JsonValue::Object{{"scope_digest", memory_agent_support::scope_key(scope)}}),
        EventClass::Critical};
    barrier.required = Durability::ProcessCrash;
    auto stored = s.events->append(barrier);
    if (!stored || stored.value().achieved < Durability::ProcessCrash)
        return error(ErrorCode::DataLoss, "memory erasure barrier was not durably stored");
    if (auto erased = s.erase_projections(memory_agent_support::scope_key(scope)); !erased)
        return erased.error();
    ErasureRequest request;
    request.scope = scope;
    request.reason = "memory-agent-host-erasure";
    return s.memory->erase(request);
}

bool MemoryAgentService::State::current(const Job &job) const {
    const auto found = works.find(job.request.task);
    return !closing && found != works.end() && !found->second.revoked &&
           !found->second.input_pending && found->second.revision == job.revision &&
           (job.request.mode == MemoryAgentMode::Record || found->second.active);
}

Result<MemoryAgentHandle> MemoryAgentService::submit(MemoryAgentRequest request,
                                                     OperationContext context) {
    return state_->submit(std::move(request), std::move(context));
}
Result<std::optional<MemoryAgentResult>> MemoryAgentService::result(OperationId id) const {
    return state_->result(id);
}
Result<std::optional<MemoryAgentResult>> MemoryAgentService::State::result(OperationId id) const {
    std::shared_ptr<Job> job;
    {
        const std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end())
            return error(ErrorCode::NotFound, "memory job is not owned by this service");
        job = found->second;
        if (!current(*job))
            return error(ErrorCode::InvalidState, "memory job result is stale");
    }
    if (job->future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
        return std::optional<MemoryAgentResult>{};
    const auto &value = job->future.get();
    if (!value)
        return value.error();
    return std::optional<MemoryAgentResult>{value.value()};
}
Result<void> MemoryAgentService::cancel(OperationId id) {
    const std::lock_guard lock(state_->mutex);
    const auto found = state_->jobs.find(id);
    if (found == state_->jobs.end())
        return error(ErrorCode::NotFound, "memory job is not owned by this service");
    found->second->cancelled.store(true);
    return {};
}

Result<std::size_t> MemoryAgentService::reap() {
    auto &s = *state_;
    const std::lock_guard serialize(s.close_mutex);
    std::vector<std::shared_ptr<State::Job>> done;
    {
        const std::lock_guard lock(s.mutex);
        for (const auto &[id, job] : s.jobs) {
            (void)id;
            if (job->worker_id == std::this_thread::get_id())
                return error(ErrorCode::InvalidState, "memory reap requires an external owner");
            if (job->done)
                done.push_back(job);
        }
    }
    for (const auto &job : done) {
        if (job->worker && job->worker->started())
            job->worker->stop();
        (void)job->future.get();
        const std::lock_guard lock(s.mutex);
        s.jobs.erase(job->id);
    }
    return done.size();
}

Result<void> MemoryAgentService::close(std::chrono::milliseconds timeout) {
    auto &s = *state_;
    {
        const std::lock_guard lock(s.mutex);
        for (const auto &[id, job] : s.jobs) {
            (void)id;
            if (job->worker_id == std::this_thread::get_id())
                return error(ErrorCode::InvalidState, "memory close requires an external owner");
        }
        s.closing = true;
        for (const auto &[id, job] : s.jobs) {
            (void)id;
            job->cancelled.store(true);
        }
    }
    {
        std::unique_lock lock(s.mutex);
        if (!s.cv.wait_for(lock, std::max(timeout, std::chrono::milliseconds{0}), [&] {
                return std::all_of(s.jobs.begin(), s.jobs.end(),
                                   [](const auto &entry) { return entry.second->done; });
            }))
            return error(ErrorCode::DeadlineExceeded, "memory close awaits cooperative cleanup");
    }
    auto reaped = reap();
    if (!reaped)
        return reaped.error();
    const std::lock_guard lock(s.mutex);
    s.works.clear();
    if (s.audit_error)
        return *s.audit_error;
    return {};
}
} // namespace mira
