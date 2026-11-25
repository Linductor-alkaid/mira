#include "interaction_coordinator_support.hpp"

#include <mira/json.hpp>

#include <algorithm>

namespace mira {
namespace {
EventId completion_event_id(RuntimeId runtime, SessionId session, TaskId task) {
    const auto digest =
        digest_string(std::string(kVerifiedTaskCompletionEvent) + ':' + runtime.to_string() + ':' +
                      session.to_string() + ':' + task.to_string());
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return EventId{Id128{bytes}};
}

Error invalid_completion_event() {
    return interaction_error(ErrorCode::DataLoss, InteractionDomainCode::RuntimeCommandFailed,
                             "verified task completion evidence is invalid");
}
} // namespace

Result<VerifiedTaskCompletion> parse_verified_task_completion(const EventEnvelope &event) {
    if (event.payload.type != kVerifiedTaskCompletionEvent ||
        event.payload.classification != EventClass::Critical || event.payload.data.size() > 512 ||
        event.runtime_id.is_nil() || event.session_id.is_nil() || !event.task_id ||
        event.task_id->is_nil() || event.cause_control_sequence == 0 || event.event_id.is_nil() ||
        event.session_sequence == 0 ||
        event.event_id != completion_event_id(event.runtime_id, event.session_id, *event.task_id))
        return invalid_completion_event();
    const auto payload = parse_json(event.payload.data);
    if (!payload || !payload.value().is_object())
        return invalid_completion_event();
    const auto *tenant = payload.value().find("tenant");
    const auto *user = payload.value().find("user");
    const auto *completed = payload.value().find("completed_at_ns");
    if (!tenant || !tenant->is_string() || !user || !user->is_string() || !completed ||
        !completed->is_integer())
        return invalid_completion_event();
    const auto tenant_id = TenantId::parse(*tenant->as_string());
    const auto user_id = UserId::parse(*user->as_string());
    const auto completion_ns = completed->as_integer().value();
    if (!tenant_id || tenant_id->is_nil() || !user_id || user_id->is_nil() || completion_ns <= 0)
        return invalid_completion_event();
    return VerifiedTaskCompletion{*tenant_id, *user_id,
                                  WallTimePoint{std::chrono::nanoseconds{completion_ns}}};
}

Result<AppendReceipt>
InteractionCoordinator::record_verified_task_completion(const PrincipalContext &principal,
                                                        TaskId task) {
    const std::lock_guard lock(impl_->submit_mutex);
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire) || !impl_->events ||
        impl_->runtime_id.is_nil())
        return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::LedgerUnavailable,
                                 "the task completion event store is unavailable");
    if (!interaction_support::permitted(principal, impl_->config))
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the principal cannot record task completion");
    const auto snapshot = impl_->runtime.task_snapshot(task);
    if (!snapshot)
        return snapshot.error();
    const auto session = impl_->runtime.session_snapshot(snapshot.value().session_id);
    if (!session)
        return session.error();
    if (session.value().principal.tenant_id != principal.tenant_id ||
        session.value().principal.user_id != principal.user_id)
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the task is outside the principal's session ownership");
    const auto &state = snapshot.value();
    if (state.state != TaskState::Completed || !state.terminal_outcome ||
        state.terminal_outcome->terminal_state != TaskState::Completed ||
        state.terminal_outcome->error || state.control_sequence == 0 ||
        state.updated_at.wall.time_since_epoch().count() <= 0)
        return interaction_error(ErrorCode::InvalidState,
                                 InteractionDomainCode::RuntimeCommandFailed,
                                 "the task has no successful Runtime completion");

    const auto payload = to_json_string(JsonValue::Object{
        {"tenant", principal.tenant_id.to_string()},
        {"user", principal.user_id.to_string()},
        {"completed_at_ns", state.updated_at.wall.time_since_epoch().count()},
    });
    const AppendRequest request{
        completion_event_id(impl_->runtime_id, state.session_id, task),
        impl_->runtime_id,
        state.session_id,
        task,
        {std::string(kVerifiedTaskCompletionEvent), payload, EventClass::Critical},
        state.control_sequence,
        Durability::ProcessCrash};
    auto result = impl_->events->append(request);
    if (!result)
        return result.error();
    if (result.value().achieved < Durability::ProcessCrash ||
        result.value().event_id != request.event_id || result.value().session_sequence == 0 ||
        !result.value().task_sequence || *result.value().task_sequence == 0)
        return interaction_error(ErrorCode::DataLoss, InteractionDomainCode::LedgerUnavailable,
                                 "task completion evidence was not durably acknowledged");
    return result;
}
} // namespace mira
