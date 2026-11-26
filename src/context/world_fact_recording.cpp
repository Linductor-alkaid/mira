#include <mira/world_fact_recording.hpp>

#include <mira/verified_fact.hpp>

namespace mira {
namespace {

constexpr std::string_view kVerifiedFactEvent = "TaskFactVerified";

Error recording_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.world_fact.recording";
    error.safe_message = message;
    return error;
}

Result<void> check_operation(const OperationContext &context) {
    if (context.cancelled())
        return recording_error(ErrorCode::Cancelled, "world fact recording cancelled");
    if (context.expired(Timestamp::now()))
        return recording_error(ErrorCode::DeadlineExceeded, "world fact recording expired");
    return {};
}

Result<EventEnvelope> read_source(const IEventStore &events, const WorldFactSource &source,
                                  const OperationContext &context) {
    EventSliceQuery query;
    query.session_id = context.session;
    query.after_sequence = source.sequence - 1;
    query.through_sequence = source.sequence;
    query.limit = 1;
    query.scan_limit = 1;
    query.byte_limit = 8192 + kVerifiedFactEvent.size();
    auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (page.value().events.empty())
        return recording_error(ErrorCode::NotFound, "world fact source event is unavailable");
    if (page.value().events.size() != 1 || page.value().has_more ||
        page.value().snapshot_through != source.sequence ||
        page.value().scanned_through != source.sequence)
        return recording_error(ErrorCode::DataLoss, "world fact source page is inconsistent");
    auto &event = page.value().events.front();
    if (event.session_id != context.session || event.session_sequence != source.sequence ||
        event.event_id != source.event || event.task_id != context.task)
        return recording_error(ErrorCode::InvalidArgument, "world fact source identity disagrees");
    if (event.timestamp_origin == EventTimestampOrigin::UnavailableLegacy)
        return recording_error(ErrorCode::DataLoss, "world fact source time is unavailable");
    return std::move(event);
}

} // namespace

Result<ConsolidationReport>
record_verified_world_fact(IMemory &memory, const IEventStore &events,
                           const WorldFactRecordingBinding &binding, const WorldFactSource &source,
                           const OperationContext &context,
                           const WorldFactRecordingLeaseAcquirer &acquire) {
    if (!acquire || binding.runtime.is_nil() || binding.verified_key.empty() ||
        source.event.is_nil() || source.sequence == 0 || context.operation.is_nil() ||
        context.session.is_nil() || context.task.is_nil())
        return recording_error(ErrorCode::InvalidArgument, "world fact binding is incomplete");
    if (auto valid = WorldFactQuery{binding.world_scope, binding.property, {}, {}}.validate();
        !valid)
        return valid.error();
    if ((binding.verified_scope.kind != MemoryScopeKind::Environment &&
         binding.verified_scope.kind != MemoryScopeKind::Application) ||
        binding.verified_scope.tenant_id != binding.world_scope.tenant_id)
        return make_memory_error(MemoryDomainCode::ScopeDenied,
                                 "world fact source scope is incompatible");

    try {
        if (auto active = check_operation(context); !active)
            return active.error();
        auto acquired = acquire(binding, source, context);
        if (!acquired)
            return acquired.error();
        auto lease = std::move(acquired).value();
        if (!lease)
            return recording_error(ErrorCode::InvalidState, "world fact lease is unavailable");
        if (auto active = check_operation(context); !active)
            return active.error();

        const auto event = read_source(events, source, context);
        if (!event)
            return event.error();
        const auto fact = parse_published_task_fact(event.value());
        if (!fact)
            return fact.error();
        const auto &claim = fact.value().claim;
        if (fact.value().scope != binding.verified_scope)
            return make_memory_error(MemoryDomainCode::ScopeDenied, "world fact scope disagrees");
        if (fact.value().runtime != binding.runtime || claim.task_epoch != context.task_epoch ||
            claim.key != binding.verified_key ||
            !fact.value().valid_at(event.value().timestamp.wall))
            return recording_error(ErrorCode::InvalidArgument,
                                   "verified world fact disagrees with binding");

        using ClockDuration = std::chrono::system_clock::duration;
        auto record = make_world_observation(
            binding.world_scope, binding.property, claim.value, source.event,
            {std::chrono::time_point_cast<ClockDuration>(claim.valid_from),
             std::chrono::time_point_cast<ClockDuration>(claim.valid_until)},
            std::chrono::time_point_cast<ClockDuration>(event.value().timestamp.wall));
        if (!record)
            return record.error();
        MemoryCandidate candidate;
        candidate.proposed = std::move(record).value();
        candidate.evidence = candidate.proposed.provenance;

        if (auto active = check_operation(context); !active)
            return active.error();
        if (auto allowed = lease->validate(); !allowed)
            return allowed.error();
        if (auto active = check_operation(context); !active)
            return active.error();
        return MemoryConsolidator{}.consolidate_candidates(memory, {std::move(candidate)},
                                                           binding.world_scope, Timestamp::now());
    } catch (...) {
        return recording_error(ErrorCode::Internal, "world fact recording operation failed");
    }
}

} // namespace mira
