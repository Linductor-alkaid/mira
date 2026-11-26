#include "interaction_coordinator_support.hpp"

#include <mira/json.hpp>

#include <algorithm>
#include <limits>

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

bool valid_dispatch_source(const VerifiedDispatchCompletionSource &source) {
    return !source.operation.is_nil() && !source.event.is_nil() && source.sequence > 0 &&
           source.sequence <=
               static_cast<SessionSequence>(std::numeric_limits<std::int64_t>::max());
}

std::optional<VerifiedDispatchCompletionSource> parse_dispatch_source(const JsonValue &json) {
    const auto *operation = json.find("operation");
    const auto *event = json.find("event");
    const auto *sequence = json.find("sequence");
    if (!json.is_object() || json.as_object()->size() != 3 || !operation ||
        !operation->is_string() || !event || !event->is_string() || !sequence ||
        !sequence->is_integer() || *sequence->as_integer() <= 0)
        return std::nullopt;
    const VerifiedDispatchCompletionSource source{
        OperationId::parse(*operation->as_string()).value_or(OperationId{}),
        EventId::parse(*event->as_string()).value_or(EventId{}),
        static_cast<SessionSequence>(*sequence->as_integer())};
    if (!valid_dispatch_source(source))
        return std::nullopt;
    return source;
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
        event.task_id->is_nil() || event.event_id.is_nil() || event.session_sequence == 0 ||
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
    VerifiedTaskCompletion completion{*tenant_id, *user_id,
                                      WallTimePoint{std::chrono::nanoseconds{completion_ns}}};
    if (const auto *host = payload.value().find("dispatch_source")) {
        completion.dispatch_source = parse_dispatch_source(*host);
        if (event.cause_control_sequence != 0 || !completion.dispatch_source ||
            completion.dispatch_source->event == event.event_id ||
            completion.dispatch_source->sequence >= event.session_sequence)
            return invalid_completion_event();
    } else if (event.cause_control_sequence == 0) {
        return invalid_completion_event();
    }
    return completion;
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

Result<AppendReceipt> append_verified_task_completion(IEventStore &events, RuntimeId runtime,
                                                      SessionId session, TaskId task,
                                                      const VerifiedTaskCompletion &completion,
                                                      ControlSequence cause) {
    const auto id = completion_event_id(runtime, session, task);
    if (runtime.is_nil() || session.is_nil() || task.is_nil() || completion.tenant.is_nil() ||
        completion.user.is_nil() || completion.completed_at.time_since_epoch().count() <= 0 ||
        (completion.dispatch_source
             ? cause != 0 || !valid_dispatch_source(*completion.dispatch_source) ||
                   completion.dispatch_source->event == id
             : cause == 0))
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "task completion attestation is invalid");
    JsonValue::Object payload{
        {"tenant", completion.tenant.to_string()},
        {"user", completion.user.to_string()},
        {"completed_at_ns", completion.completed_at.time_since_epoch().count()},
    };
    if (const auto &source = completion.dispatch_source)
        payload.emplace_back(
            "dispatch_source",
            JsonValue::Object{{"operation", source->operation.to_string()},
                              {"event", source->event.to_string()},
                              {"sequence", static_cast<std::int64_t>(source->sequence)}});
    const AppendRequest request{
        id,
        runtime,
        session,
        task,
        {std::string(kVerifiedTaskCompletionEvent), to_json_string(payload), EventClass::Critical},
        cause,
        Durability::ProcessCrash};
    auto result = events.append(request);
    if (!result)
        return result.error();
    if (result.value().achieved < Durability::ProcessCrash ||
        result.value().event_id != request.event_id || result.value().session_sequence == 0 ||
        !result.value().task_sequence || *result.value().task_sequence == 0 ||
        (completion.dispatch_source &&
         completion.dispatch_source->sequence >= result.value().session_sequence))
        return interaction_error(ErrorCode::DataLoss, InteractionDomainCode::LedgerUnavailable,
                                 "task completion evidence was not durably acknowledged");
    return result;
}

namespace {
Result<AppendReceipt> append_completion(IEventStore &events, RuntimeId runtime, TenantId tenant,
                                        UserId user, const TaskSnapshot &state) {
    const auto evidence = state.completion_evidence.value_or(
        TaskCompletionEvidence{state.updated_at.wall, state.control_sequence});
    return append_verified_task_completion(events, runtime, state.session_id, state.id,
                                           {tenant, user, evidence.completed_at},
                                           evidence.control_sequence);
}
} // namespace

Result<AppendReceipt>
InteractionCoordinator::Impl::append_verified_completion(const PrincipalContext &principal,
                                                         const TaskSnapshot &state) {
    return append_completion(*events, runtime_id, principal.tenant_id, principal.user_id, state);
}

Result<TaskCompletionRecorder> InteractionCoordinator::make_task_completion_recorder(
    const PrincipalContext &principal, SessionId session, OperationId operation) const {
    if (session.is_nil() || operation.is_nil())
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "completion recording requires a concrete session and operation");
    const std::unique_lock lock(impl_->submit_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return interaction_error(ErrorCode::ResourceExhausted,
                                 InteractionDomainCode::ExecutorRejected,
                                 "the coordinator is busy during task completion binding", true);
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire) || !impl_->events ||
        impl_->runtime_id.is_nil())
        return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::LedgerUnavailable,
                                 "the task completion event store is unavailable");
    if (!interaction_support::permitted(principal, impl_->config))
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the principal cannot bind task completion recording");
    return TaskCompletionRecorder{[events = impl_->events, runtime = impl_->runtime_id,
                                   tenant = principal.tenant_id, user = principal.user_id, session,
                                   operation](const TaskSnapshot &state, TenantId owner_tenant,
                                              UserId owner_user) -> Result<void> {
        if (tenant != owner_tenant || user != owner_user || state.session_id != session)
            return interaction_error(ErrorCode::PermissionDenied,
                                     InteractionDomainCode::PermissionDenied,
                                     "task completion is outside the authorized session");
        if (state.admission_operation != operation || !successful_completion(state) ||
            !state.completion_evidence || state.completion_evidence->control_sequence == 0 ||
            state.completion_evidence->completed_at.time_since_epoch().count() <= 0)
            return interaction_error(ErrorCode::InvalidArgument,
                                     InteractionDomainCode::InputInvalid,
                                     "task completion does not match its binding or evidence");
        const auto result = append_completion(*events, runtime, tenant, user, state);
        if (!result)
            return result.error();
        return {};
    }};
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
