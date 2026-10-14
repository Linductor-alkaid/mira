#include <mira/conversation_loop.hpp>

#include <mira/model_dialect.hpp>
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

Result<void> ModelInputSupplyOptions::validate() const {
    if (max_items == 0 || max_image_bytes == 0) {
        return conversation_error(ErrorCode::InvalidArgument, "supply bounds must be positive");
    }
    return Result<void>{};
}

void ConversationLoop::set_model_input_supplier(ModelInputSupplier supplier,
                                                ModelInputSupplyOptions options) {
    input_supplier_ = std::move(supplier);
    supply_options_ = options;
}

void ConversationLoop::set_model_input_identity(ModelInputIdentityProvider identity) {
    identity_provider_ = std::move(identity);
}

void ConversationLoop::set_history_transformer(HistoryTransformer transformer) {
    history_transformer_ = std::move(transformer);
}

void ConversationLoop::emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
                            EventClass classification) const {
    loop_support::emit_loop_event(events_, runtime_, session_, spec, std::move(type),
                                  std::move(summary), classification);
}

std::optional<std::vector<ModelInputItem>>
ConversationLoop::apply_history_transform(const AgentLoopSpec &spec,
                                          const std::vector<ModelInputItem> &history,
                                          ConversationLoopResult &result) {
    if (history_transformer_ == nullptr || history.empty()) {
        // The first model turn carries no executed work to compact; the seam
        // contract resolves only over a non-empty history.
        return std::nullopt;
    }
    Result<std::vector<ModelInputItem>> transformed{std::vector<ModelInputItem>{}};
    bool exception = false;
    try {
        transformed = history_transformer_(history);
    } catch (...) {
        // Host callback discipline (DEC-045): an exception is isolated into a
        // diagnostic and never escapes into request assembly; exception text
        // stays off the event surface.
        exception = true;
    }
    std::string reason;
    if (exception) {
        reason = "transformer-exception";
    } else if (!transformed.has_value()) {
        reason = "transformer-error";
    } else if (transformed.value().empty()) {
        // The canonical round trip is the model's only memory of executed
        // work: an empty replay would strip it silently, so it degrades.
        reason = "empty-result";
    } else {
        for (const auto &item : transformed.value()) {
            if (item.role == ModelRole::System || item.content.empty()) {
                reason = "invalid-item";
                break;
            }
        }
    }
    if (!reason.empty()) {
        ++result.history_transform_degradations;
        JsonValue::Object detail{{"reason", JsonValue{reason}}};
        if (!exception && !transformed.has_value()) {
            detail.emplace_back("code",
                                JsonValue{static_cast<std::int64_t>(transformed.error().code)});
        }
        emit(spec, "HistoryTransformDegraded", JsonValue{std::move(detail)}, EventClass::State);
        return std::nullopt;
    }
    ++result.history_transforms;
    emit(spec, "HistoryTransformApplied",
         JsonValue::Object{
             {"items_in", JsonValue{static_cast<std::int64_t>(history.size())}},
             {"items_out", JsonValue{static_cast<std::int64_t>(transformed.value().size())}}},
         EventClass::State);
    return std::move(transformed).value();
}

ConversationLoop::SuppliedInput
ConversationLoop::resolve_supplied_input(const AgentLoopSpec &spec) {
    SuppliedInput supplied;
    if (input_supplier_ == nullptr) {
        return supplied;
    }
    if (!supply_options_.validate().has_value()) {
        // Misconfigured bounds are a wiring error: degrade visibly instead of
        // folding unbounded content into requests (RULE-08).
        supplied.degraded = true;
        emit(spec, "ModelInputSupplyDegraded",
             JsonValue::Object{{"reason", std::string("invalid-supply-options")}},
             EventClass::State);
        return supplied;
    }
    // Exactly one supply call per assembled request; the loop never caches
    // offered content across turns — each request re-resolves the seam, so
    // retries re-ask and nothing accumulates in the tool-history replay.
    Result<std::vector<ModelInputItem>> offered{std::vector<ModelInputItem>{}};
    try {
        offered = input_supplier_(spec);
    } catch (...) {
        // Host callback discipline (DEC-045): an exception is isolated into a
        // diagnostic and never escapes into request assembly; exception text
        // stays off the event surface.
        supplied.degraded = true;
        emit(spec, "ModelInputSupplyDegraded",
             JsonValue::Object{{"reason", std::string("supplier-exception")}}, EventClass::State);
        return supplied;
    }
    if (!offered.has_value()) {
        supplied.degraded = true;
        emit(spec, "ModelInputSupplyDegraded",
             JsonValue::Object{{"reason", std::string("supplier-error")},
                               {"code", static_cast<std::int64_t>(offered.error().code)}},
             EventClass::State);
        return supplied;
    }

    // Untrusted-data discipline (DEC-053): offered items are user-side content.
    // The loop enforces the envelope at assembly, accepts only the host
    // content vocabulary and drops everything else with a counted reason —
    // no silent path. ToolCallPart/ToolResultPart/ThinkingPart belong to the
    // loop's canonical round trip and provider reasoning, never to host input.
    std::uint64_t image_bytes = 0;
    for (auto &item : offered.value()) {
        ++supplied.offered;
        std::string drop_reason;
        if (item.role != ModelRole::User) {
            drop_reason = "role";
        } else if (item.content.empty()) {
            drop_reason = "empty";
        } else {
            for (const auto &part : item.content) {
                if (std::holds_alternative<ToolCallPart>(part) ||
                    std::holds_alternative<ToolResultPart>(part) ||
                    std::holds_alternative<ThinkingPart>(part)) {
                    drop_reason = "vocabulary";
                    break;
                }
                if (const auto *image = std::get_if<ImagePart>(&part)) {
                    if (image->source.id.is_nil()) {
                        drop_reason = "artifact";
                        break;
                    }
                    if (image->media_type.empty() || image->media_type.rfind("image/", 0) != 0) {
                        drop_reason = "media-type";
                        break;
                    }
                    if (image->source.sensitivity == Sensitivity::Secret) {
                        drop_reason = "sensitivity";
                        break;
                    }
                }
            }
        }
        if (drop_reason.empty() && supplied.accepted >= supply_options_.max_items) {
            drop_reason = "item-budget";
        }
        std::uint64_t item_image_bytes = 0;
        if (drop_reason.empty()) {
            for (const auto &part : item.content) {
                if (const auto *image = std::get_if<ImagePart>(&part)) {
                    item_image_bytes += image->source.byte_size;
                }
            }
            if (image_bytes + item_image_bytes > supply_options_.max_image_bytes) {
                drop_reason = "image-budget";
            }
        }
        if (!drop_reason.empty()) {
            ++supplied.dropped;
            ++supplied.drop_reasons[drop_reason];
            continue;
        }
        image_bytes += item_image_bytes;
        supplied.image_bytes = image_bytes;
        ++supplied.accepted;
        supplied.parts.insert(supplied.parts.end(), item.content.begin(), item.content.end());
    }

    JsonValue::Object reasons;
    for (const auto &[reason, count] : supplied.drop_reasons) {
        reasons.emplace_back(reason, JsonValue{count});
    }

    // DEC-088 input-reuse identity: resolved exactly once per assembled
    // request after the supply closed, with the same exception discipline.
    // An empty offer carries no identity, because the note annotates content
    // that is actually present. The annotation qualifies from the third
    // consecutive identical supply (reuse_count >= 2) on an image-bearing
    // supply: text re-offers are cheap and stay unannotated.
    if (identity_provider_ != nullptr && !supplied.parts.empty()) {
        try {
            supplied.reuse = identity_provider_(spec);
        } catch (...) {
            supplied.reuse.reset();
            supplied.degraded = true;
            emit(spec, "ModelInputSupplyDegraded",
                 JsonValue::Object{{"reason", std::string("identity-exception")}},
                 EventClass::State);
        }
        if (supplied.reuse.has_value()) {
            supplied.annotate_reuse =
                supplied.reuse->reuse_count >= 2 &&
                std::any_of(supplied.parts.begin(), supplied.parts.end(),
                            [](const ModelContentPart &part) {
                                return std::holds_alternative<ImagePart>(part);
                            });
        }
    }

    JsonValue::Object event{
        {"offered_items", static_cast<std::int64_t>(supplied.offered)},
        {"accepted_items", static_cast<std::int64_t>(supplied.accepted)},
        {"dropped_items", static_cast<std::int64_t>(supplied.dropped)},
        {"image_bytes", JsonValue{static_cast<std::int64_t>(supplied.image_bytes)}},
        {"drop_reasons", JsonValue{std::move(reasons)}}};
    if (supplied.reuse.has_value()) {
        constexpr std::size_t kMaxDigestChars = 128;
        auto digest = supplied.reuse->content_digest;
        if (digest.size() > kMaxDigestChars) {
            digest.resize(kMaxDigestChars);
        }
        event.emplace_back("content_digest", JsonValue{std::move(digest)});
        event.emplace_back("reuse_count",
                           JsonValue{static_cast<std::int64_t>(supplied.reuse->reuse_count)});
        event.emplace_back("reuse_annotated", JsonValue{supplied.annotate_reuse});
    }
    emit(spec, "ModelInputSupplied", JsonValue{std::move(event)}, EventClass::State);
    return supplied;
}

Result<ModelRequest> ConversationLoop::build_request(const AgentLoopSpec &spec,
                                                     const std::vector<ModelInputItem> &history,
                                                     const std::string &feedback,
                                                     const SuppliedInput &supplied) {
    ModelRequest request = loop_support::begin_model_request(spec, tools_.get());

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

    // One user item carries the goal, any recovery feedback and the supplied
    // content (DEC-053): the request shape stays identical to an ordinary
    // image-bearing user message in every dialect, and the loop-controlled
    // envelope (role/provenance/authority) is never overridden by host input.
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
    user_item.content.insert(user_item.content.end(), supplied.parts.begin(), supplied.parts.end());
    // DEC-088 input-reuse note: the last part of the user envelope, under the
    // loop's own Internal envelope (never the system role, never elevated).
    // It renders from the third consecutive identical image supply so the
    // model can skip re-analysis instead of re-reading the same pixels.
    if (supplied.annotate_reuse) {
        TextPart note;
        note.text = "[System note: The " +
                    (supplied.reuse->semantic_label.empty() ? std::string("supplied content")
                                                            : supplied.reuse->semantic_label) +
                    " above is the same as in the previous " +
                    std::to_string(supplied.reuse->reuse_count) +
                    " turns. No need to re-analyze it unless the task explicitly requires it.]";
        note.sensitivity = Sensitivity::Internal;
        user_item.content.emplace_back(std::move(note));
    }

    request.input.push_back(std::move(system_item));
    request.input.push_back(std::move(user_item));
    // Canonical tool round-trip history rides after the request block, in
    // execution order (call echo, then its result).
    request.input.insert(request.input.end(), history.begin(), history.end());

    request.output_contract.mode = OutputMode::Text;
    request.generation.max_output_tokens = config_.max_output_tokens_per_turn;
    request.generation.reasoning_effort = config_.reasoning_effort;
    request.generation.thinking = config_.thinking;
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
    std::uint32_t output_continuations = 0;

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

        // Supplied-input seam (DEC-053): resolved exactly once per assembled
        // request, after tool rounds complete and before the model call.
        const SuppliedInput supplied = resolve_supplied_input(spec);
        result.supplied_input_items += static_cast<std::uint32_t>(supplied.accepted);
        result.supplied_input_dropped += static_cast<std::uint32_t>(supplied.dropped);
        result.supply_degradations += supplied.degraded ? 1U : 0U;
        result.supplied_input_reuse_annotations += supplied.annotate_reuse ? 1U : 0U;

        // History compaction seam (DEC-088 follow-up): the replay the host
        // returns replaces the canonical history for this request only; the
        // loop-owned history itself is never mutated, so later turns and
        // continuations rebuild from the full record.
        std::vector<ModelInputItem> replay = history;
        if (auto transformed = apply_history_transform(spec, history, result)) {
            replay = std::move(*transformed);
        }
        auto request = build_request(spec, replay, feedback, supplied);
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

        auto call = gateway_.infer(request.value(), model_context, config_.inference);
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

        result.last_usage = outcome.response.usage;
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
        // DEC follow-up to MIRA-20261009-002: a tolerated max_tokens cutoff
        // is settled by bounded continuation, not regeneration. The partial
        // protocol-allowed text is kept in the history so the model continues
        // from the cut; a reply cut off mid tool call is discarded unexecuted
        // (an Incomplete response never yields proposals, so nothing can
        // execute from it). Independent of the recovery budget; after it is
        // spent the run settles with an explicit output-limit summary.
        if (parse_outcome == DecisionParseOutcome::Incomplete &&
            outcome.response.incomplete_reason == IncompleteReason::MaxOutputTokens) {
            const auto partial_text = assistant_text(outcome.response);
            if (output_continuations < config_.max_output_continuations) {

                ++output_continuations;
                result.output_continuations = output_continuations;
                if (!partial_text.empty()) {
                    ModelInputItem assistant;
                    assistant.role = ModelRole::Assistant;
                    TextPart kept;
                    kept.text = partial_text;
                    assistant.content.emplace_back(std::move(kept));
                    history.push_back(std::move(assistant));
                    record.note = "output truncated by the token budget; continuing";
                    result.turns.push_back(std::move(record));
                    // DEC-088: anchor the continuation at the cut so the model
                    // resumes the exact tail instead of re-deriving it.
                    constexpr std::size_t kAnchorChars = 50;
                    const std::string anchor =
                        partial_text.size() > kAnchorChars
                            ? partial_text.substr(partial_text.size() - kAnchorChars)
                            : partial_text;
                    feedback =
                        "Your previous reply was cut off by the output-token limit. Continue "
                        "from: \"" +
                        anchor +
                        "\" — do not repeat what you already wrote, just continue and finish.";
                } else if (carries_tool_calls(outcome.response)) {
                    record.note = "tool call cut off by the token budget; discarded unexecuted";
                    result.turns.push_back(std::move(record));
                    feedback = "Your previous reply was cut off by the output-token limit while "
                               "writing a tool call. The incomplete call was discarded and NOT "
                               "executed. Either re-issue that tool call with compact arguments, "
                               "or reply with concise plain text stating the current result or "
                               "the blocking limitation.";
                } else {
                    record.note = "deliberation-only output truncated; asking for short text";
                    result.turns.push_back(std::move(record));
                    feedback = "Your previous reply was cut off by the output-token limit during "
                               "internal deliberation before any visible content. Skip lengthy "
                               "deliberation and reply with a concise plain-text response "
                               "stating the current result or the blocking limitation.";
                }
                continue;
            }
            result.outcome = ConversationOutcome::Failed;
            result.safe_summary =
                "output-limit continuation budget exhausted; the reply was truncated at the "
                "per-call output limit";
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
            const bool carries_thinking = std::any_of(
                outcome.response.output.begin(), outcome.response.output.end(),
                [](const auto &item) { return std::holds_alternative<ThinkingPart>(item); });
            // Dialects whose encoder rejects thinking replay (DEC-051) keep
            // their reasoning output observability-only (DEC-052): tool rounds
            // replay the canonical call/result items without it.
            bool thinking_replay = false;
            if (carries_thinking)
                if (const auto profile = gateway_.router().find(outcome.response.profile_id))
                    thinking_replay = dialect_accepts_thinking_replay(profile->dialect);
            if (thinking_replay) {
                ModelInputItem assistant;
                assistant.role = ModelRole::Assistant;
                for (const auto &item : outcome.response.output) {
                    if (const auto *thinking = std::get_if<ThinkingPart>(&item))
                        assistant.content.emplace_back(*thinking);
                    else if (const auto *message = std::get_if<MessageOutput>(&item)) {
                        for (const auto &part : message->content)
                            if (const auto *text = std::get_if<OutputTextPart>(&part))
                                assistant.content.emplace_back(TextPart{text->text});
                    } else if (const auto *tool_call = std::get_if<ToolCallOutput>(&item))
                        assistant.content.emplace_back(
                            ToolCallPart{tool_call->provider_call_id, tool_call->provider_name,
                                         tool_call->arguments, tool_call->arguments_digest});
                }
                history.push_back(std::move(assistant));
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
                result.tool_executions = tool_executions;
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
                if (!thinking_replay)
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
            // MaxOutputTokens cutoffs never reach this block: they settle by
            // bounded continuation above. What is left here is an empty or
            // otherwise unusable reply (or a ContextWindow/Other cutoff).
            // An Incomplete here lacks the MaxOutputTokens reason (provider
            // did not report one): keep truncation-aware guidance so the
            // retry does not regenerate the same long output.
            if (parse_outcome == DecisionParseOutcome::Incomplete) {
                record.note = "output cut off; recovering";
                result.turns.push_back(std::move(record));
                feedback = "Your previous reply was cut off by the output-token limit before "
                           "it completed. Reply again with a concise plain-text response "
                           "only: no tool calls and no long deliberation — state the current "
                           "result or the blocking limitation in a few sentences.";
            } else {
                record.note = "no usable answer text; recovering";
                result.turns.push_back(std::move(record));
                feedback = "The previous reply carried no answer text. Answer the request "
                           "directly with text.";
            }
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
