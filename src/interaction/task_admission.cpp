#include "interaction_coordinator_support.hpp"

#include <mira/json.hpp>

#include <algorithm>

namespace mira {
namespace {
constexpr std::string_view kTaskAdmissionEvent = "mira.task.admission.v1";

struct AdmissionLocation final {
    SessionId partition;
    EventId event;
};

Id128 admission_id(std::string_view prefix, const std::string &identity) {
    const auto digest = digest_string(std::string(prefix) + identity);
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id128{bytes};
}

AdmissionLocation admission_location(const PrincipalContext &principal, OperationId operation) {
    const auto identity = principal.tenant_id.to_string() + ':' + principal.user_id.to_string() +
                          ':' + operation.to_string();
    return {SessionId{admission_id("mira.task.admission.partition.v1:", identity)},
            EventId{admission_id("mira.task.admission.event.v1:", identity)}};
}

Error bad_admission_evidence() {
    return interaction_error(ErrorCode::DataLoss, InteractionDomainCode::LedgerUnavailable,
                             "the durable task admission record is invalid");
}

Result<void> check_admission_context(const OperationContext &context) {
    if (context.cancelled())
        return interaction_error(ErrorCode::Cancelled, InteractionDomainCode::RuntimeCommandFailed,
                                 "task admission reconciliation was cancelled");
    if (context.expired(Timestamp::now()))
        return interaction_error(ErrorCode::DeadlineExceeded,
                                 InteractionDomainCode::RuntimeCommandTimeout,
                                 "task admission reconciliation expired", true);
    return {};
}

Result<std::optional<TaskAdmissionReceipt>>
read_admission(IEventStore &events, const PrincipalContext &principal, OperationId operation,
               AdmissionLocation location, const OperationContext &context) {
    EventSliceQuery query;
    query.session_id = location.partition;
    query.limit = 2;
    query.scan_limit = 2;
    query.byte_limit = 1024;
    const auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (const auto ready = check_admission_context(context); !ready)
        return ready.error();
    if (page.value().has_more || page.value().events.size() > 1 ||
        page.value().scanned_through != page.value().snapshot_through)
        return bad_admission_evidence();
    if (page.value().events.empty()) {
        if (page.value().snapshot_through != 0)
            return bad_admission_evidence();
        return std::optional<TaskAdmissionReceipt>{};
    }
    const auto &event = page.value().events.front();
    if (event.event_id != location.event || event.session_id != location.partition ||
        event.runtime_id.is_nil() || event.session_sequence != 1 || event.task_id ||
        event.task_sequence || event.cause_control_sequence == 0 ||
        page.value().snapshot_through != 1 || event.payload.type != kTaskAdmissionEvent ||
        event.payload.classification != EventClass::Critical || event.payload.data.size() > 512)
        return bad_admission_evidence();

    const auto payload = parse_json(event.payload.data);
    if (!payload || !payload.value().is_object())
        return bad_admission_evidence();
    const auto field = [&](std::string_view name) -> std::string_view {
        const auto *value = payload.value().find(name);
        return value && value->is_string() ? std::string_view(*value->as_string())
                                           : std::string_view{};
    };
    const auto session = SessionId::parse(field("session"));
    const auto task = TaskId::parse(field("task"));
    const auto *admitted = payload.value().find("admitted_at_ns");
    if (field("tenant") != principal.tenant_id.to_string() ||
        field("user") != principal.user_id.to_string() ||
        field("operation") != operation.to_string() || !session || session->is_nil() || !task ||
        task->is_nil() || !admitted || !admitted->is_integer() ||
        admitted->as_integer().value() <= 0)
        return bad_admission_evidence();
    if (const auto durable = events.flush(Durability::ProcessCrash); !durable)
        return durable.error();
    if (const auto ready = check_admission_context(context); !ready)
        return ready.error();
    return std::optional{TaskAdmissionReceipt{
        operation, *session, *task,
        WallTimePoint{std::chrono::nanoseconds{admitted->as_integer().value()}}, location.event}};
}

Result<TaskAdmissionReceipt> persist_admission(IEventStore &events, RuntimeId runtime,
                                               const PrincipalContext &principal,
                                               OperationId operation, const TaskSnapshot &snapshot,
                                               const OperationContext &context) {
    const auto location = admission_location(principal, operation);
    const TaskAdmissionReceipt admission{operation, snapshot.session_id, snapshot.id,
                                         snapshot.updated_at.wall, location.event};
    const auto stored = read_admission(events, principal, operation, location, context);
    if (!stored)
        return stored.error();
    if (stored.value()) {
        if (*stored.value() != admission)
            return interaction_error(ErrorCode::AlreadyExists, InteractionDomainCode::InputConflict,
                                     "the operation is bound to another task admission");
        return admission;
    }
    const AppendRequest request{
        location.event,
        runtime,
        location.partition,
        {},
        {std::string(kTaskAdmissionEvent),
         to_json_string(JsonValue::Object{
             {"tenant", principal.tenant_id.to_string()},
             {"user", principal.user_id.to_string()},
             {"operation", operation.to_string()},
             {"session", admission.session.to_string()},
             {"task", admission.task.to_string()},
             {"admitted_at_ns", admission.admitted_at.time_since_epoch().count()}}),
         EventClass::Critical},
        snapshot.control_sequence,
        Durability::ProcessCrash};
    const auto recorded = events.append(request);
    if (!recorded)
        return recorded.error();
    if (recorded.value().event_id != location.event || recorded.value().session_sequence != 1 ||
        recorded.value().task_sequence || recorded.value().achieved < Durability::ProcessCrash)
        return bad_admission_evidence();
    return admission;
}

Error admission_busy() {
    return interaction_error(ErrorCode::ResourceExhausted, InteractionDomainCode::ExecutorRejected,
                             "the coordinator is busy during task admission reconciliation", true);
}
} // namespace

Result<void>
InteractionCoordinator::Impl::check_task_admission_access(const PrincipalContext &principal,
                                                          OperationId operation) const {
    if (!initialized || stopping.load(std::memory_order_acquire) || !events || runtime_id.is_nil())
        return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::LedgerUnavailable,
                                 "the durable task admission ledger is unavailable");
    if (!interaction_support::permitted(principal, config))
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the principal cannot access task admission");
    if (operation.is_nil())
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "task admission needs a stable operation ID");
    return {};
}

Result<TaskAdmissionReceipt> InteractionCoordinator::record_task_admission(
    const PrincipalContext &principal, OperationId operation, const TaskSubmission &submission,
    const OperationContext &context) {
    if (const auto ready = check_admission_context(context); !ready)
        return ready.error();
    const std::unique_lock lock(impl_->submit_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return admission_busy();
    if (const auto access = impl_->check_task_admission_access(principal, operation); !access)
        return access.error();
    if (submission.id.is_nil() || !submission.command.valid())
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "task admission needs a concrete Runtime submission");

    const auto receipt = submission.command.receipt(std::chrono::milliseconds{0});
    if (!receipt)
        return receipt.error();
    const auto outcome = submission.command.outcome(std::chrono::milliseconds{0});
    if (!outcome)
        return outcome.error();
    const auto &applied = outcome.value();
    if (receipt.value().kind != CommandKind::SubmitTask ||
        receipt.value().status != ReceiptStatus::Accepted ||
        applied.status != SettlementStatus::Applied || !applied.task ||
        applied.task->id != submission.id || applied.task->admission_operation != operation ||
        receipt.value().control_sequence == 0 ||
        applied.task->control_sequence != receipt.value().control_sequence ||
        applied.task->updated_at.wall.time_since_epoch().count() <= 0)
        return interaction_error(ErrorCode::InvalidState,
                                 InteractionDomainCode::RuntimeCommandFailed,
                                 "the command does not confirm admission of the operation's task");
    const auto live = impl_->runtime.task_snapshot(submission.id);
    if (!live)
        return live.error();
    const auto session = impl_->runtime.session_snapshot(live.value().session_id);
    if (!session)
        return session.error();
    if (live.value().session_id != applied.task->session_id ||
        live.value().admission_operation != operation ||
        session.value().principal.tenant_id != principal.tenant_id ||
        session.value().principal.user_id != principal.user_id)
        return interaction_error(ErrorCode::PermissionDenied,
                                 InteractionDomainCode::PermissionDenied,
                                 "the admitted task is outside the principal's session ownership");

    return persist_admission(*impl_->events, impl_->runtime_id, principal, operation, *applied.task,
                             context);
}

Result<std::optional<TaskAdmissionReceipt>>
InteractionCoordinator::lookup_task_admission(const PrincipalContext &principal,
                                              OperationId operation,
                                              const OperationContext &context) const {
    if (const auto ready = check_admission_context(context); !ready)
        return ready.error();
    const std::unique_lock lock(impl_->submit_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return admission_busy();
    if (const auto access = impl_->check_task_admission_access(principal, operation); !access)
        return access.error();
    return read_admission(*impl_->events, principal, operation,
                          admission_location(principal, operation), context);
}

Result<TaskAdmissionRecorder>
InteractionCoordinator::make_task_admission_recorder(const PrincipalContext &principal,
                                                     SessionId session, OperationId operation,
                                                     const OperationContext &context) const {
    if (const auto ready = check_admission_context(context); !ready)
        return ready.error();
    const std::unique_lock lock(impl_->submit_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return admission_busy();
    if (const auto access = impl_->check_task_admission_access(principal, operation); !access)
        return access.error();
    if (session.is_nil())
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "durable task admission needs a concrete session");
    OperationContext write_context;
    write_context.deadline = context.deadline;
    return TaskAdmissionRecorder{[events = impl_->events, runtime = impl_->runtime_id,
                                  config = impl_->config, principal, session, operation,
                                  write_context](const TaskSnapshot &snapshot,
                                                 TenantId owner_tenant,
                                                 UserId owner_user) -> Result<void> {
        if (const auto ready = check_admission_context(write_context); !ready)
            return ready.error();
        if (!interaction_support::permitted(principal, config) || snapshot.session_id != session ||
            principal.tenant_id != owner_tenant || principal.user_id != owner_user)
            return interaction_error(ErrorCode::PermissionDenied,
                                     InteractionDomainCode::PermissionDenied,
                                     "durable task admission is outside the authorized session");
        if (snapshot.admission_operation != operation)
            return interaction_error(ErrorCode::InvalidArgument,
                                     InteractionDomainCode::InputConflict,
                                     "durable task admission uses a different operation");
        const auto persisted =
            persist_admission(*events, runtime, principal, operation, snapshot, write_context);
        if (!persisted)
            return persisted.error();
        return {};
    }};
}
} // namespace mira
