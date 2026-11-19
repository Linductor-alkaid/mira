#include <mira/content_markers.hpp>
#include <mira/workflow_trajectory_capture.hpp>

#include <algorithm>
#include <unordered_set>

namespace mira {
namespace {
Error trace_error(ErrorCode code, const char *message) {
    Error error;
    error.domain = "mira.workflow.trajectory";
    error.code = code;
    error.safe_message = message;
    return error;
}

const std::string *text_member(const JsonValue &json, const char *key) {
    const auto *member = json.find(key);
    return member ? member->as_string() : nullptr;
}
} // namespace

Result<ConversationTrajectoryCapture>
capture_conversation_trajectory(const IEventStore &events, const OperationContext &context,
                                std::span<const VerifiedTrajectoryTool> verified,
                                std::size_t max_session_events) {
    if (context.session.is_nil() || context.task.is_nil() || verified.empty() ||
        verified.size() > 64 || max_session_events == 0 || max_session_events > 4096)
        return trace_error(ErrorCode::InvalidArgument, "invalid conversation capture bounds");

    const auto check_current = [&]() -> Result<void> {
        if (context.cancelled())
            return trace_error(ErrorCode::Cancelled, "conversation capture cancelled");
        if (context.expired(Timestamp::now()))
            return trace_error(ErrorCode::DeadlineExceeded, "conversation capture expired");
        return {};
    };
    if (auto current = check_current(); !current)
        return current.error();
    auto page = events.read({context.session, {}, max_session_events});
    if (!page)
        return page.error();
    if (page.value().has_more || page.value().events.size() > max_session_events)
        return trace_error(ErrorCode::ResourceExhausted,
                           "conversation source exceeded event budget");

    ConversationTrajectoryCapture captured;
    std::vector<SessionSequence> execution_sequences;
    std::unordered_set<OperationId, StrongIdHash<OperationId>> operations;
    std::unordered_set<EventId, StrongIdHash<EventId>> proofs;
    const auto forbidden = default_forbidden_content_markers();
    SessionSequence previous_sequence = 0;
    TaskSequence previous_task_sequence = 0;
    SessionSequence settlement_sequence = 0;
    for (const auto &event : page.value().events) {
        if (auto current = check_current(); !current)
            return current.error();
        if (event.session_id != context.session || event.session_sequence <= previous_sequence)
            return trace_error(ErrorCode::DataLoss,
                               "conversation source sequence or scope is invalid");
        previous_sequence = event.session_sequence;
        if (event.task_id != context.task)
            continue;
        if (!event.task_sequence || *event.task_sequence != ++previous_task_sequence)
            return trace_error(ErrorCode::DataLoss, "conversation task event range is incomplete");
        const auto &type = event.payload.type;
        if (type != "ToolExecuted" && type != "ToolSkipped" && type != "ConversationSettled" &&
            type != "LoopSettled")
            continue;
        if (!captured.settlement_event.is_nil())
            return trace_error(ErrorCode::DataLoss, "execution evidence follows task settlement");
        if (type == "ToolSkipped" || type == "LoopSettled")
            return trace_error(ErrorCode::InvalidState,
                               "conversation contains unsupported execution");
        if (event.event_id.is_nil() || event.payload.data.size() > 16 * 1024)
            return trace_error(ErrorCode::DataLoss, "conversation execution event is invalid");

        const auto payload = parse_json(event.payload.data);
        const auto *detail = payload ? payload.value().find("detail") : nullptr;
        const auto *task = payload ? text_member(payload.value(), "task_id") : nullptr;
        const auto *epoch = payload ? payload.value().find("task_epoch") : nullptr;
        if (!task || *task != context.task.to_string() || !epoch || !epoch->as_integer() ||
            *epoch->as_integer() < 0 ||
            static_cast<std::uint64_t>(*epoch->as_integer()) != context.task_epoch || !detail)
            return trace_error(ErrorCode::DataLoss, "execution event identity is incomplete");

        if (type == "ConversationSettled") {
            const auto *outcome = text_member(*detail, "outcome");
            const auto *count = detail->find("tool_executions");
            if (!outcome || *outcome != "Answered")
                return trace_error(ErrorCode::InvalidState,
                                   "conversation did not settle successfully");
            if (!count || count->as_integer() != static_cast<std::int64_t>(verified.size()) ||
                captured.trajectory.steps.size() != verified.size())
                return trace_error(ErrorCode::DataLoss, "conversation tool history is incomplete");
            captured.settlement_event = event.event_id;
            settlement_sequence = event.session_sequence;
            continue;
        }

        const auto *failed = detail->find("failed");
        const auto *wire_name = text_member(*detail, "wire_name");
        const auto *operation = text_member(*detail, "operation_id");
        const auto *digest = text_member(*detail, "arguments_digest");
        const auto id = operation ? OperationId::parse(*operation) : std::nullopt;
        if (!failed || failed->as_boolean() != false || !wire_name || wire_name->empty() ||
            wire_name->size() > 128 || !id || id->is_nil() || !operations.insert(*id).second ||
            !digest)
            return trace_error(ErrorCode::InvalidState, "tool execution is not a unique success");
        const auto proof = std::find_if(verified.begin(), verified.end(),
                                        [&](const auto &entry) { return entry.operation == *id; });
        if (proof == verified.end() || proof->verification_event.is_nil() ||
            !proofs.insert(proof->verification_event).second || !proof->arguments.is_object() ||
            proof->arguments.find("tool"))
            return trace_error(ErrorCode::PermissionDenied,
                               "verified tool arguments do not match execution");
        auto serialized = to_json_string(proof->arguments);
        if (serialized.size() > 8192)
            return trace_error(ErrorCode::ResourceExhausted,
                               "verified tool arguments exceed byte budget");
        if (*digest != digest_string(serialized).to_string())
            return trace_error(ErrorCode::PermissionDenied,
                               "verified tool arguments do not match execution");
        std::transform(serialized.begin(), serialized.end(), serialized.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        });
        if (std::any_of(forbidden.begin(), forbidden.end(), [&](const auto &marker) {
                return serialized.find(marker) != std::string::npos;
            }))
            return trace_error(ErrorCode::SafetyRejected,
                               "verified arguments contain a retention marker");

        WorkflowTrajectoryStep step;
        step.source_step_id = StepId{event.event_id.value};
        step.name = *wire_name;
        step.arguments = proof->arguments;
        step.arguments.set("tool", JsonValue{*wire_name});
        step.raw_arguments = step.arguments;
        if (captured.trajectory.steps.size() >= verified.size())
            return trace_error(ErrorCode::ResourceExhausted,
                               "tool history exceeded verified capacity");
        captured.trajectory.steps.push_back(std::move(step));
        captured.execution_events.push_back(event.event_id);
        captured.verification_events.push_back(proof->verification_event);
        execution_sequences.push_back(event.session_sequence);
    }
    if (captured.settlement_event.is_nil())
        return trace_error(ErrorCode::InvalidState, "conversation has no settled outcome");

    for (std::size_t i = 0; i < captured.verification_events.size(); ++i) {
        const auto proof = std::find_if(
            page.value().events.begin(), page.value().events.end(), [&](const auto &event) {
                // A host may verify inside the tool callback, before the loop
                // emits ToolExecuted, or afterwards before the final answer.
                return event.event_id == captured.verification_events[i] &&
                       event.task_id == context.task &&
                       (i == 0 || event.session_sequence > execution_sequences[i - 1]) &&
                       event.session_sequence < settlement_sequence &&
                       event.payload.type != "ToolExecuted" && event.payload.type != "ToolSkipped";
            });
        if (proof == page.value().events.end())
            return trace_error(ErrorCode::PermissionDenied,
                               "verification event is missing from the execution range");
    }
    if (auto current = check_current(); !current)
        return current.error();
    return captured;
}

} // namespace mira
