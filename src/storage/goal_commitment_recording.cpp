#include <mira/goal_commitment_recording.hpp>

#include <mira/content_markers.hpp>
#include <mira/json.hpp>

#include <algorithm>

namespace mira {
namespace {

Error commitment_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.goal_commitment.recording";
    error.safe_message = message;
    return error;
}

Result<void> check_operation(const OperationContext &operation) {
    if (operation.cancelled())
        return commitment_error(ErrorCode::Cancelled, "commitment recording cancelled");
    if (operation.expired(Timestamp::now()))
        return commitment_error(ErrorCode::DeadlineExceeded, "commitment recording expired");
    return {};
}

bool source_authorized(const GoalCommitmentBinding &binding, const OperationContext &operation,
                       const GoalCommitmentAuthorizer &authorize) {
    try {
        return authorize && authorize(binding, operation);
    } catch (...) {
        return false;
    }
}

Result<EventEnvelope> read_evidence(const IEventStore &events, const GoalCommitmentBinding &binding,
                                    const OperationContext &operation, SessionSequence sequence,
                                    EventId id, const char *type) {
    EventSliceQuery query;
    query.session_id = operation.session;
    query.after_sequence = sequence - 1;
    query.through_sequence = sequence;
    query.limit = 1;
    query.scan_limit = 1;
    query.byte_limit = 32 * 1024; // Includes JSON escaping of the bounded answer text.
    auto page = events.read_slice(query);
    if (!page)
        return page.error();
    auto &value = page.value();
    if (value.events.size() != 1 || value.has_more || value.snapshot_through != sequence ||
        value.scanned_through != sequence)
        return commitment_error(ErrorCode::DataLoss, "commitment event page is incomplete");
    auto &event = value.events.front();
    if (event.runtime_id != binding.runtime || event.session_id != operation.session ||
        event.task_id != operation.task || event.session_sequence != sequence ||
        event.payload.type != type || event.payload.classification != EventClass::State ||
        event.event_id != id)
        return commitment_error(ErrorCode::InvalidArgument, "commitment event identity disagrees");
    return std::move(event);
}

Result<JsonValue> read_detail(const EventEnvelope &event, const OperationContext &operation) {
    auto body = parse_json(event.payload.data);
    if (!body)
        return commitment_error(ErrorCode::DataLoss, "commitment event payload is unreadable");
    const auto *task = body.value().find("task_id");
    const auto *epoch = body.value().find("task_epoch");
    auto *detail = body.value().find("detail");
    if (!task || !task->is_string() || TaskId::parse(*task->as_string()) != operation.task ||
        !epoch || !epoch->is_integer() || *epoch->as_integer() < 0 ||
        static_cast<std::uint64_t>(*epoch->as_integer()) != operation.task_epoch || !detail ||
        !detail->is_object())
        return commitment_error(ErrorCode::InvalidArgument, "commitment event frame disagrees");
    return std::move(*detail);
}

GoalKey commitment_key(const GoalOwner &owner, EventId answer) {
    const auto digest =
        digest_string("mira.conversation.commitment.v1/" + owner.tenant.to_string() + "/" +
                      owner.user.to_string() + "/" + answer.to_string());
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return {owner, Id128{bytes}};
}

} // namespace

Result<GoalCreationReceipt> record_conversation_commitment(
    SqliteGoalStore &goals, const IEventStore &events, const GoalCommitmentBinding &binding,
    const GoalCommitmentSource &source, const GoalCommitmentCandidate &candidate,
    const OperationContext &operation, const GoalCommitmentAuthorizer &authorize) {
    if (binding.runtime.is_nil() || binding.owner.tenant.is_nil() || binding.owner.user.is_nil() ||
        source.settlement_sequence == 0 || source.settlement_event.is_nil() ||
        operation.session.is_nil() || operation.task.is_nil() || operation.operation.is_nil() ||
        candidate.quote.empty() || candidate.quote.size() > 512)
        return commitment_error(ErrorCode::InvalidArgument, "commitment candidate is incomplete");
    if (auto active = check_operation(operation); !active)
        return active.error();
    if (!source_authorized(binding, operation, authorize))
        return commitment_error(ErrorCode::PermissionDenied, "commitment source access denied");

    try {
        const auto settlement =
            read_evidence(events, binding, operation, source.settlement_sequence,
                          source.settlement_event, "ConversationSettled");
        if (!settlement)
            return settlement.error();
        const auto settled = read_detail(settlement.value(), operation);
        if (!settled)
            return settled.error();
        const auto *outcome = settled.value().find("outcome");
        const auto *answer = settled.value().find("answer_event");
        const auto *position = settled.value().find("answer_sequence");
        const auto *total = settled.value().find("total_turns");
        const auto id =
            answer && answer->is_string() ? EventId::parse(*answer->as_string()) : std::nullopt;
        if (!outcome || !outcome->is_string() || *outcome->as_string() != "Answered" ||
            settled.value().find("incomplete_reason") || !id || id->is_nil() || !position ||
            !position->is_integer() || *position->as_integer() <= 0 ||
            static_cast<SessionSequence>(*position->as_integer()) >= source.settlement_sequence ||
            !total || !total->is_integer() || *total->as_integer() <= 0)
            return commitment_error(ErrorCode::InvalidArgument,
                                    "settlement has no complete terminal answer");

        if (auto active = check_operation(operation); !active)
            return active.error();
        const auto speech = read_evidence(events, binding, operation,
                                          static_cast<SessionSequence>(*position->as_integer()),
                                          *id, "AssistantSpeech");
        if (!speech)
            return speech.error();
        const auto detail = read_detail(speech.value(), operation);
        if (!detail)
            return detail.error();
        const auto *turn = detail.value().find("turn");
        const auto *text = detail.value().find("text");
        if (!turn || turn->as_integer() != total->as_integer() || !text || !text->is_string() ||
            detail.value().find("text_truncated") || detail.value().find("tool_calls") ||
            text->as_string()->find(candidate.quote) == std::string::npos)
            return commitment_error(ErrorCode::InvalidArgument,
                                    "commitment quote is outside the complete answer");
        if (speech.value().timestamp_origin == EventTimestampOrigin::UnavailableLegacy ||
            speech.value().timestamp.wall > settlement.value().timestamp.wall)
            return commitment_error(ErrorCode::DataLoss, "commitment evidence timestamps disagree");
        if (matches_content_marker(candidate.quote, default_injection_content_markers()))
            return commitment_error(ErrorCode::SafetyRejected,
                                    "commitment quote is not retainable");

        if (!source_authorized(binding, operation, authorize))
            return commitment_error(ErrorCode::PermissionDenied, "commitment source access denied");
        if (auto active = check_operation(operation); !active)
            return active.error();
        return goals.create_with_receipt(
            commitment_key(binding.owner, *id),
            {GoalKind::Commitment, candidate.quote, *id,
             std::chrono::time_point_cast<std::chrono::milliseconds>(speech.value().timestamp.wall),
             candidate.due_at, candidate.expires_at});
    } catch (...) {
        return commitment_error(ErrorCode::Internal, "commitment recording operation failed");
    }
}

} // namespace mira
