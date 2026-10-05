#include <mira/conversation_loop.hpp>

#include <mira/model_digest.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "loop_support.hpp"

namespace mira {
namespace {

[[nodiscard]] Error conversation_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.conversation_loop";
    error.safe_message = std::move(message);
    return error;
}

// The concatenated assistant text of one response; empty when the response
// carries no message text at all.
[[nodiscard]] std::string assistant_text(const ModelResponse &response) {
    std::string text;
    for (const auto &item : response.output) {
        if (const auto *message = std::get_if<MessageOutput>(&item)) {
            for (const auto &part : message->content) {
                if (const auto *output = std::get_if<OutputTextPart>(&part)) {
                    if (!output->text.empty()) {
                        if (!text.empty()) {
                            text += '\n';
                        }
                        text += output->text;
                    }
                }
            }
        }
    }
    return text;
}

[[nodiscard]] bool carries_tool_calls(const ModelResponse &response) {
    return std::any_of(
        response.output.begin(), response.output.end(),
        [](const ModelOutputItem &item) { return std::get_if<ToolCallOutput>(&item) != nullptr; });
}

} // namespace

std::string conversation_outcome_name(ConversationOutcome outcome) {
    switch (outcome) {
    case ConversationOutcome::Answered:
        return "Answered";
    case ConversationOutcome::Failed:
        return "Failed";
    case ConversationOutcome::Cancelled:
        return "Cancelled";
    case ConversationOutcome::MaxTurns:
        return "MaxTurns";
    }
    return "Failed";
}

ConversationLoop::ConversationLoop(ModelGateway &gateway, ConversationLoopConfig config)
    : gateway_(gateway), config_(config) {}

void ConversationLoop::set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                                       SessionId session) {
    events_ = std::move(events);
    runtime_ = runtime;
    session_ = session;
}

void ConversationLoop::set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools) {
    tools_ = std::move(tools);
}

void ConversationLoop::emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
                            EventClass classification) const {
    if (events_ == nullptr) {
        return;
    }
    JsonValue::Object envelope;
    envelope.emplace_back("task_id", spec.task_id.to_string());
    envelope.emplace_back("task_epoch", static_cast<std::int64_t>(spec.task_epoch));
    envelope.emplace_back("detail", std::move(summary));
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_;
    append.session_id = session_;
    append.task_id = spec.task_id;
    append.payload = EventPayload{std::move(type), to_json_string(JsonValue(std::move(envelope))),
                                  classification};
    (void)events_->append(append);
}

Result<ModelRequest> ConversationLoop::build_request(const AgentLoopSpec &spec,
                                                     const std::vector<ModelInputItem> &history,
                                                     const std::string &feedback) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = spec.task_id;
    request.task_epoch = spec.task_epoch;
    request.profile_id = spec.profile_id;

    if (tools_ != nullptr) {
        request.tools = tools_->exposed_tools();
    }

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = "mira.conversation-loop.system.v1";
    system_item.authority = Sensitivity::Internal;
    TextPart system_text;
    system_text.text =
        "You are Mira, a conversational agent. Answer the user's request directly with "
        "a final text answer. You may call the exposed tools when they help; tool results "
        "arrive on your next turn. A text reply without a tool call ends the conversation.";
    if (!request.tools.empty()) {
        system_text.text += " Keep using tools until you can answer, then reply with text only.";
    }
    system_text.sensitivity = Sensitivity::Internal;
    system_item.content.emplace_back(std::move(system_text));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = "mira.conversation-loop.request.v1";
    user_item.authority = Sensitivity::Internal;
    TextPart goal_text;
    goal_text.text = "Request: " + spec.goal;
    goal_text.sensitivity = Sensitivity::Internal;
    user_item.content.emplace_back(std::move(goal_text));
    if (!feedback.empty()) {
        TextPart feedback_text;
        feedback_text.text = feedback;
        feedback_text.sensitivity = Sensitivity::Internal;
        user_item.content.emplace_back(std::move(feedback_text));
    }

    request.input.push_back(std::move(system_item));
    request.input.push_back(std::move(user_item));
    // Canonical tool round-trip history rides after the request block, in
    // execution order (call echo, then its result).
    request.input.insert(request.input.end(), history.begin(), history.end());

    request.output_contract.mode = OutputMode::Text;
    request.generation.max_output_tokens = config_.max_output_tokens_per_turn;
    request.budget.max_output_tokens =
        config_.max_output_tokens_per_turn *
        (static_cast<std::uint64_t>(config_.max_turns) + config_.max_recoveries + 1);
    request.budget.max_requests = config_.max_turns + config_.max_recoveries + 1;
    request.data_policy.store = false;
    request.prompt_provenance.system_template_digest =
        digest_string("mira.conversation-loop.system.v1");
    return request;
}

Result<ConversationLoopResult> ConversationLoop::run(const AgentLoopSpec &spec,
                                                     const OperationContext &context) {
    ConversationLoopResult result;
    if (spec.goal.empty()) {
        return conversation_error(ErrorCode::InvalidArgument,
                                  "conversation goal must not be empty");
    }
    // Exhausting the turn budget is the default terminal; every early exit
    // assigns a specific outcome before leaving the loop.
    result.outcome = ConversationOutcome::MaxTurns;

    // Canonical tool round-trip items of the whole run, replayed into every
    // request so providers see native multi-turn tool semantics.
    std::vector<ModelInputItem> history;
    std::string feedback;
    std::uint32_t tool_executions = 0;

    for (std::uint32_t turn = 1; turn <= config_.max_turns; ++turn) {
        if (context.cancelled()) {
            result.outcome = ConversationOutcome::Cancelled;
            result.safe_summary = "cancellation requested";
            emit(spec, "ConversationSettled",
                 JsonValue::Object{{"outcome", conversation_outcome_name(result.outcome)}},
                 EventClass::State);
            return result;
        }

        ConversationTurnRecord record;
        record.turn = turn;

        auto request = build_request(spec, history, feedback);
        if (!request) {
            result.outcome = ConversationOutcome::Failed;
            result.safe_summary = "request assembly failed";
            break;
        }
        feedback.clear();
        record.model_request = request.value().request_id;

        OperationContext model_context = context;
        model_context.operation = request.value().operation_id;
        model_context.deadline =
            context.deadline.has_value()
                ? std::min(context.deadline.value(),
                           std::chrono::steady_clock::now() + config_.model_call_deadline)
                : std::chrono::steady_clock::now() + config_.model_call_deadline;

        auto call = gateway_.infer(request.value(), model_context);
        if (!call) {
            if (call.error().code == ErrorCode::Cancelled) {
                result.outcome = ConversationOutcome::Cancelled;
                result.safe_summary = "model call was cancelled";
                break;
            }
            if (loop_support::recoverable_model_failure(call.error()) &&
                result.recoveries < config_.max_recoveries) {
                ++result.recoveries;
                record.note = "model failure; recovering: " + call.error().safe_message;
                result.turns.push_back(std::move(record));
                continue;
            }
            result.outcome = ConversationOutcome::Failed;
            result.safe_summary = "model call failed: " + call.error().safe_message;
            break;
        }
        auto outcome = std::move(call).value();
        if (!outcome.admitted) {
            result.outcome = ConversationOutcome::Cancelled;
            result.safe_summary = outcome.rejection_reason;
            break;
        }

        const auto parse_outcome = outcome.parse.outcome;
        if (parse_outcome == DecisionParseOutcome::Refused ||
            parse_outcome == DecisionParseOutcome::ContentFiltered) {
            result.outcome = ConversationOutcome::Failed;
            result.safe_summary = "model refused or was filtered; no policy bypass";
            result.turns.push_back(std::move(record));
            break;
        }
        if (parse_outcome == DecisionParseOutcome::Ambiguous) {
            // Tool-bridge rejections (calls against tools that were not
            // exposed, conflicting duplicates) surface here with a precise
            // summary: the gateway downgrades unresolved ToolProposals to
            // Ambiguous. There is no policy bypass — the loop fails closed.
            if (tools_ == nullptr && carries_tool_calls(outcome.response)) {
                result.outcome = ConversationOutcome::Failed;
                result.safe_summary = "tool proposals require a tool registry; none is attached";
            } else {
                result.outcome = ConversationOutcome::Failed;
                result.safe_summary = "model output was rejected: " + outcome.parse.safe_summary;
            }
            result.turns.push_back(std::move(record));
            break;
        }
        if (parse_outcome == DecisionParseOutcome::ToolProposals) {
            if (tools_ == nullptr) {
                result.outcome = ConversationOutcome::Failed;
                result.safe_summary = "tool proposals require a tool registry; none is attached";
                result.turns.push_back(std::move(record));
                break;
            }
            if (!outcome.tool_proposals.has_value() || outcome.tool_proposals->proposals.empty()) {
                result.outcome = ConversationOutcome::Failed;
                result.safe_summary = "tool proposal batch carried no executable proposal";
                result.turns.push_back(std::move(record));
                break;
            }
            if (tool_executions +
                    static_cast<std::uint32_t>(outcome.tool_proposals->proposals.size()) >
                config_.max_tool_executions) {
                result.outcome = ConversationOutcome::Failed;
                result.safe_summary = "tool execution budget exhausted";
                result.turns.push_back(std::move(record));
                break;
            }
            std::optional<ConversationOutcome> abort;
            std::string abort_summary;
            for (const auto &proposal : outcome.tool_proposals->proposals) {
                if (context.cancelled()) {
                    abort = ConversationOutcome::Cancelled;
                    abort_summary = "cancellation requested during tool execution";
                    break;
                }
                auto executed = tools_->execute(proposal, context);
                if (!executed) {
                    abort = executed.error().code == ErrorCode::Cancelled
                                ? ConversationOutcome::Cancelled
                                : ConversationOutcome::Failed;
                    abort_summary =
                        executed.error().code == ErrorCode::Cancelled
                            ? "tool execution was cancelled"
                            : "tool execution rejected: " + executed.error().safe_message;
                    break;
                }
                ++tool_executions;
                emit(spec, "ToolExecuted",
                     JsonValue::Object{{"wire_name", proposal.wire_name},
                                       {"operation_id", proposal.operation_id.to_string()},
                                       {"failed", executed.value().failed},
                                       {"arguments_digest", proposal.arguments_digest.to_string()}},
                     EventClass::State);
                if (!record.summary.empty()) {
                    record.summary += ";";
                }
                record.summary += "tool:" + proposal.wire_name;
                // The canonical round trip: echo the call, then carry the
                // result — the provider pairs them by call id.
                history.push_back(make_tool_call_item(proposal));
                history.push_back(make_tool_result_item(executed.value()));
            }
            if (abort.has_value()) {
                result.outcome = *abort;
                result.safe_summary = std::move(abort_summary);
                result.turns.push_back(std::move(record));
                break;
            }
            record.note = "executed tool round; awaiting model continuation";
            result.turns.push_back(std::move(record));
            continue;
        }

        // Text modes: a plain text answer is the terminal state; there is no
        // device verification and no forced decision JSON (DEC-047).
        auto answer = assistant_text(outcome.response);
        if (!answer.empty()) {
            record.summary = "answer";
            result.turns.push_back(std::move(record));
            result.outcome = ConversationOutcome::Answered;
            result.answer = std::move(answer);
            result.safe_summary = "model returned a terminal text answer";
            break;
        }
        // No usable output (empty text, incomplete or unusable shapes):
        // recover once with explicit feedback, then fail closed.
        if (result.recoveries < config_.max_recoveries) {
            ++result.recoveries;
            record.note = "no usable answer text; recovering";
            result.turns.push_back(std::move(record));
            feedback = "The previous reply carried no answer text. Answer the request "
                       "directly with text.";
            continue;
        }
        result.outcome = ConversationOutcome::Failed;
        result.safe_summary = parse_outcome == DecisionParseOutcome::Incomplete
                                  ? "model output remained incomplete after recovery budget"
                                  : "model output carried no answer text";
        result.turns.push_back(std::move(record));
        break;
    }

    if (result.outcome == ConversationOutcome::MaxTurns && result.safe_summary.empty()) {
        result.safe_summary = "turn budget exhausted before a final answer";
    }
    emit(spec, "ConversationSettled",
         JsonValue::Object{{"outcome", conversation_outcome_name(result.outcome)},
                           {"turns", static_cast<std::int64_t>(result.turns.size())},
                           {"recoveries", static_cast<std::int64_t>(result.recoveries)}},
         EventClass::State);
    return result;
}

} // namespace mira
