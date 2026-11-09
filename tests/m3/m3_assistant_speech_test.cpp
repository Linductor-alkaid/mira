// AssistantSpeech event contract tests (DEC-104): the ConversationLoop
// records the assistant's own words — tool-round speech with per-call
// execution statuses and the terminal answer — as bounded, sensitivity-
// labelled state events, and never records outputs that were not spoken
// (refusals, ambiguous batches, dropped fragments). Also pins the DEC-103
// wire shape of the loop's multi-part user envelope on the Chat dialect.

#include "support/conversation_loop_support.hpp"

#include "support/test.hpp"

#include <mira/conversation_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// The detail objects of every AssistantSpeech event of one session, in
// session order.
[[nodiscard]] std::vector<JsonValue> speech_details(const MemoryEventStore &store,
                                                    const SessionId &session) {
    std::vector<JsonValue> details;
    EventQuery query;
    query.session_id = session;
    const auto page = store.read(query);
    if (!page.has_value()) {
        return details;
    }
    for (const auto &event : page.value().events) {
        if (event.payload.type != "AssistantSpeech") {
            continue;
        }
        const auto parsed = parse_json(event.payload.data);
        const auto *detail = parsed.has_value() ? parsed.value().find("detail") : nullptr;
        if (detail != nullptr && detail->is_object()) {
            details.push_back(*detail);
        }
    }
    return details;
}

[[nodiscard]] std::vector<std::pair<std::string, std::int64_t>>
speech_and_turn_record_sequence(const MemoryEventStore &store, const SessionId &session) {
    std::vector<std::pair<std::string, std::int64_t>> sequence;
    EventQuery query;
    query.session_id = session;
    const auto page = store.read(query);
    if (!page.has_value()) {
        return sequence;
    }
    for (const auto &event : page.value().events) {
        if (event.payload.type != "AssistantSpeech" &&
            event.payload.type != "ConversationTurnRecorded") {
            continue;
        }
        const auto parsed = parse_json(event.payload.data);
        const auto *detail = parsed.has_value() ? parsed.value().find("detail") : nullptr;
        const auto *turn = detail != nullptr ? detail->find("turn") : nullptr;
        sequence.emplace_back(event.payload.type,
                              turn != nullptr ? turn->as_integer().value_or(-1) : -1);
    }
    return sequence;
}

[[nodiscard]] BuiltinToolSpec tool_spec(const std::string &name) {
    BuiltinToolSpec spec;
    spec.wire_name = name;
    spec.description = "Fixture tool " + name + " for the speech event tests.";
    spec.parameters_schema = JsonSchema{parse_json(R"json({
        "type": "object",
        "properties": {"message": {"type": "string"}},
        "additionalProperties": false
    })json")
                                            .value()};
    return spec;
}

int tool_round_and_terminal_answer_each_emit_one_speech() {
    ConversationFixture fixture;
    const auto arguments = parse_json(R"({"message":"ping"})");
    MIRA_CHECK(arguments.has_value());
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), R"({"message":"ping"})"), text_response("done")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "done");

    const auto speeches = speech_details(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(speeches.size() == 2);

    // Tool round: the proposals of the turn with their observed statuses.
    const auto &tool_speech = speeches.front();
    const auto *turn = tool_speech.find("turn");
    MIRA_CHECK(turn != nullptr && turn->as_integer() == std::optional<std::int64_t>(1));
    const auto *sensitivity = tool_speech.find("sensitivity");
    MIRA_CHECK(sensitivity != nullptr && sensitivity->is_string() &&
               *sensitivity->as_string() == "Internal");
    MIRA_CHECK(tool_speech.find("text") == nullptr);
    MIRA_CHECK(tool_speech.find("text_truncated") == nullptr);
    const auto *calls = tool_speech.find("tool_calls");
    MIRA_CHECK(calls != nullptr && calls->is_array() && calls->as_array()->size() == 1);
    const auto &call = calls->as_array()->front();
    MIRA_CHECK(call.find("wire_name") != nullptr && *call.find("wire_name")->as_string() == "echo");
    MIRA_CHECK(call.find("arguments") != nullptr &&
               *call.find("arguments")->as_string() == to_json_string(arguments.value()));
    MIRA_CHECK(call.find("arguments_digest") != nullptr &&
               *call.find("arguments_digest")->as_string() ==
                   digest_string(to_json_string(arguments.value())).to_string());
    MIRA_CHECK(call.find("status") != nullptr && *call.find("status")->as_string() == "executed");

    // Terminal answer: text only, no tool calls.
    const auto &answer_speech = speeches.back();
    const auto *answer_turn = answer_speech.find("turn");
    MIRA_CHECK(answer_turn != nullptr &&
               answer_turn->as_integer() == std::optional<std::int64_t>(2));
    MIRA_CHECK(answer_speech.find("sensitivity") != nullptr &&
               *answer_speech.find("sensitivity")->as_string() == "Internal");
    MIRA_CHECK(answer_speech.find("text") != nullptr &&
               *answer_speech.find("text")->as_string() == "done");
    MIRA_CHECK(answer_speech.find("text_truncated") == nullptr);
    MIRA_CHECK(answer_speech.find("tool_calls") == nullptr);
    return 0;
}

int speech_payload_is_bounded_and_flags_truncation() {
    ConversationFixture fixture;
    const auto long_arguments = R"({"message":")" + std::string(2000, 'a') + R"("})";
    const std::string long_text(5000, 'b');
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), long_arguments), text_response(long_text)});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == long_text);

    const auto speeches = speech_details(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(speeches.size() == 2);
    const auto *calls = speeches.front().find("tool_calls");
    MIRA_CHECK(calls != nullptr && calls->as_array()->size() == 1);
    const auto &call = calls->as_array()->front();
    const auto *arguments = call.find("arguments");
    MIRA_CHECK(arguments != nullptr && arguments->is_string());
    // Serialized arguments are bounded at 1024 characters plus the "..." marker.
    MIRA_CHECK(arguments->as_string()->size() == 1024 + 3);
    MIRA_CHECK(arguments->as_string()->ends_with("..."));
    // The digest always covers the full arguments, never the bounded rendering.
    MIRA_CHECK(*call.find("arguments_digest")->as_string() ==
               digest_string(long_arguments).to_string());
    MIRA_CHECK(*call.find("status")->as_string() == "executed");
    MIRA_CHECK(speeches.front().find("text_truncated") == nullptr);

    const auto *text = speeches.back().find("text");
    MIRA_CHECK(text != nullptr && text->is_string());
    MIRA_CHECK(text->as_string()->size() == 4096 + 3);
    MIRA_CHECK(text->as_string()->ends_with("..."));
    const auto *truncated = speeches.back().find("text_truncated");
    MIRA_CHECK(truncated != nullptr && truncated->as_boolean() == std::optional<bool>(true));
    return 0;
}

int speech_statuses_cover_failed() {
    // failed: the registry turns a rejected handler outcome into a failed
    // record, which the loop feeds back and reports as a failed speech call.
    ConversationFixture fixture;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    auto boom = tool_spec("boom");
    MIRA_CHECK(fixture.registry_->register_tool(
        boom, [](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "mira.test";
            error.safe_message = "boom failed on purpose";
            return Result<JsonValue>{std::move(error)};
        }));
    fixture.use_provider(
        {tool_call_response(boom, R"({"message":"x"})"), text_response("after failure")});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().tool_executions == 1);
    const auto speeches = speech_details(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(speeches.size() == 2);
    const auto *calls = speeches.front().find("tool_calls");
    MIRA_CHECK(calls != nullptr && calls->as_array()->size() == 1);
    MIRA_CHECK(*calls->as_array()->front().find("status")->as_string() == "failed");
    // The observed failure is also on the execution event.
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolExecuted") != types.end());
    return 0;
}

int speech_records_not_executed_when_execution_aborts() {
    // A tool execution that errors (here: a Cancelled outcome) aborts the
    // run, and the half-processed round still records what the assistant
    // uttered, with the unexecuted calls marked not_executed (DEC-104).
    ConversationFixture fixture;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    auto abort = tool_spec("abort");
    MIRA_CHECK(fixture.registry_->register_tool(
        abort, [](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            Error error;
            error.code = ErrorCode::Cancelled;
            error.domain = "mira.test";
            error.safe_message = "aborted on purpose";
            return Result<JsonValue>{std::move(error)};
        }));
    fixture.use_provider({tool_call_response(abort, R"({"message":"x"})")});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Cancelled);
    const auto speeches = speech_details(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(speeches.size() == 1);
    MIRA_CHECK(speeches.front().find("turn") != nullptr &&
               speeches.front().find("turn")->as_integer() == std::optional<std::int64_t>(1));
    MIRA_CHECK(speeches.front().find("text") == nullptr);
    const auto *calls = speeches.front().find("tool_calls");
    MIRA_CHECK(calls != nullptr && calls->as_array()->size() == 1);
    const auto &call = calls->as_array()->front();
    MIRA_CHECK(*call.find("wire_name")->as_string() == "abort");
    MIRA_CHECK(*call.find("status")->as_string() == "not_executed");
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolExecuted") == types.end());
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolSkipped") == types.end());
    return 0;
}

int unspoken_outputs_emit_nothing() {
    // Refusals, content filters, ambiguous tool batches and dropped empty
    // fragments are never the assistant's speech: no AssistantSpeech event.
    const auto run_without_speech = [&](std::vector<ModelResponse> script,
                                        ConversationLoopConfig config) -> int {
        ConversationFixture fixture;
        fixture.use_provider(std::move(script));
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
        const auto speeches = speech_details(*fixture.events_, fixture.spec_.session_id);
        MIRA_CHECK(speeches.empty());
        return 0;
    };
    MIRA_CHECK(run_without_speech({refused_response()}, ConversationLoopConfig{}) == 0);
    MIRA_CHECK(run_without_speech({content_filtered_response()}, ConversationLoopConfig{}) == 0);
    ConversationLoopConfig one_recovery;
    one_recovery.max_recoveries = 1;
    MIRA_CHECK(run_without_speech({text_response(""), text_response("")}, one_recovery) == 0);
    // Tool calls against tools the request never exposed downgrade to the
    // Ambiguous terminal; the batch is dropped without a speech event.
    BuiltinToolSpec ghost;
    ghost.wire_name = "ghost";
    ghost.tool_id = ToolId::generate();
    MIRA_CHECK(run_without_speech({tool_call_response(ghost, R"({"message":"x"})")},
                                  ConversationLoopConfig{}) == 0);
    return 0;
}

int continuous_mode_emits_speech_before_turn_record() {
    ConversationFixture fixture;
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), R"({"message":"ping"})"), text_response("done")});
    ConversationLoopConfig config;
    config.limit_mode = ConversationLimitMode::Continuous;
    config.max_turns = 0;
    config.max_tool_executions = 0;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);

    // Per turn, the speech event precedes the turn record of the same turn.
    const auto sequence =
        speech_and_turn_record_sequence(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(sequence.size() == 4);
    MIRA_CHECK(sequence[0].first == "AssistantSpeech" && sequence[0].second == 1);
    MIRA_CHECK(sequence[1].first == "ConversationTurnRecorded" && sequence[1].second == 1);
    MIRA_CHECK(sequence[2].first == "AssistantSpeech" && sequence[2].second == 2);
    MIRA_CHECK(sequence[3].first == "ConversationTurnRecorded" && sequence[3].second == 2);
    return 0;
}

int chat_wire_keeps_goal_and_instruction_lines() {
    // DEC-103 through the real loop: the goal part and the injected
    // instruction part render as independent lines of the Chat wire content.
    ConversationFixture fixture(ProtocolDialect::OpenAIChatCompletionsV1);
    fixture.use_provider({text_response("done")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    MIRA_CHECK(loop.enqueue_user_message("remember this follow-up"));
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(fixture.provider_->requests().size() == 1);
    NullArtifactSource artifacts;
    auto wire = ChatCompletionsV1Mapper{}.encode_request(fixture.provider_->requests().front(),
                                                         *fixture.profile_, false, artifacts);
    MIRA_CHECK(wire);
    const auto *messages = wire.value().find("messages");
    MIRA_CHECK(messages != nullptr && messages->is_array());
    const auto &user = messages->as_array()->back();
    MIRA_CHECK(*user.find("role")->as_string() == "user");
    const auto *content = user.find("content");
    MIRA_CHECK(content != nullptr && content->is_string());
    MIRA_CHECK(*content->as_string() == "Request: what is the answer\nremember this follow-up");
    return 0;
}

} // namespace

int main() {
    if (const int status = tool_round_and_terminal_answer_each_emit_one_speech(); status != 0) {
        return status;
    }
    if (const int status = speech_payload_is_bounded_and_flags_truncation(); status != 0) {
        return status;
    }
    if (const int status = speech_statuses_cover_failed(); status != 0) {
        return status;
    }
    if (const int status = speech_records_not_executed_when_execution_aborts(); status != 0) {
        return status;
    }
    if (const int status = unspoken_outputs_emit_nothing(); status != 0) {
        return status;
    }
    if (const int status = continuous_mode_emits_speech_before_turn_record(); status != 0) {
        return status;
    }
    return chat_wire_keeps_goal_and_instruction_lines();
}
