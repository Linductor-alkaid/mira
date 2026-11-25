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

bool successful_completion(const TaskSnapshot &state) {
    return state.state == TaskState::Completed && state.terminal_outcome &&
           state.terminal_outcome->terminal_state == TaskState::Completed &&
           !state.terminal_outcome->error && state.control_sequence != 0 &&
           state.updated_at.wall.time_since_epoch().count() > 0;
}

Result<void> check_publication_context(const OperationContext &context) {
    if (context.cancelled())
        return interaction_error(ErrorCode::Cancelled, InteractionDomainCode::RuntimeCommandFailed,
                                 "task completion publication was cancelled");
    if (context.expired(Timestamp::now()))
        return interaction_error(ErrorCode::DeadlineExceeded,
                                 InteractionDomainCode::RuntimeCommandTimeout,
                                 "task completion publication expired", true);
    return {};
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

Result<TaskSnapshot>
InteractionCoordinator::Impl::owned_completion_task(const PrincipalContext &principal, TaskId task,
                                                    std::optional<SessionId> expected) const {
    if (!initialized || stopping.load(std::memory_order_acquire) || !events || runtime_id.is_nil())
        return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::LedgerUnavailable,
                                 "the task completion event store is unavailable");
    if (!interaction_support::permitted(principal, config))
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the principal cannot record task completion");
    auto snapshot = runtime.task_snapshot(task);
    if (!snapshot)
        return snapshot.error();
    if (expected && snapshot.value().session_id != *expected)
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the task does not belong to the dispatched session");
    const auto session = runtime.session_snapshot(snapshot.value().session_id);
    if (!session)
        return session.error();
    if (session.value().principal.tenant_id != principal.tenant_id ||
        session.value().principal.user_id != principal.user_id)
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the task is outside the principal's session ownership");
    return snapshot;
}

Result<AppendReceipt>
InteractionCoordinator::Impl::append_verified_completion(const PrincipalContext &principal,
                                                         const TaskSnapshot &state) {
    const auto payload = to_json_string(JsonValue::Object{
        {"tenant", principal.tenant_id.to_string()},
        {"user", principal.user_id.to_string()},
        {"completed_at_ns", state.updated_at.wall.time_since_epoch().count()},
    });
    const AppendRequest request{
        completion_event_id(runtime_id, state.session_id, state.id),
        runtime_id,
        state.session_id,
        state.id,
        {std::string(kVerifiedTaskCompletionEvent), payload, EventClass::Critical},
        state.control_sequence,
        Durability::ProcessCrash};
    auto result = events->append(request);
    if (!result)
        return result.error();
    if (result.value().achieved < Durability::ProcessCrash ||
        result.value().event_id != request.event_id || result.value().session_sequence == 0 ||
        !result.value().task_sequence || *result.value().task_sequence == 0)
        return interaction_error(ErrorCode::DataLoss, InteractionDomainCode::LedgerUnavailable,
                                 "task completion evidence was not durably acknowledged");
    return result;
}

Result<AppendReceipt>
InteractionCoordinator::record_verified_task_completion(const PrincipalContext &principal,
                                                        TaskId task) {
    const std::lock_guard lock(impl_->submit_mutex);
    const auto state = impl_->owned_completion_task(principal, task, {});
    if (!state)
        return state.error();
    if (!successful_completion(state.value()))
        return interaction_error(ErrorCode::InvalidState,
                                 InteractionDomainCode::RuntimeCommandFailed,
                                 "the task has no successful Runtime completion");
    return impl_->append_verified_completion(principal, state.value());
}

Result<bool>
InteractionCoordinator::publish_verified_task_completion_if_ready(const PrincipalContext &principal,
                                                                  SessionId session, TaskId task,
                                                                  const OperationContext &context) {
    if (session.is_nil() || task.is_nil())
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "completion publication requires a concrete task and session");
    if (const auto ready = check_publication_context(context); !ready)
        return ready.error();
    // An input submission may hold this lock while awaiting work on this Executor.
    const std::unique_lock lock(impl_->submit_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return interaction_error(
            ErrorCode::ResourceExhausted, InteractionDomainCode::ExecutorRejected,
            "the coordinator is busy during task completion publication", true);
    const auto state = impl_->owned_completion_task(principal, task, session);
    if (!state) {
        if (state.error().code == ErrorCode::NotFound)
            return false;
        return state.error();
    }
    if (!successful_completion(state.value()))
        return false;
    if (const auto ready = check_publication_context(context); !ready)
        return ready.error();
    if (const auto published = impl_->append_verified_completion(principal, state.value());
        !published)
        return published.error();
    return true;
}
} // namespace mira
