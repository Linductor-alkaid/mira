#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_sse.hpp>

#include <string>
#include <type_traits>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] ModelRequest base_request_alias() {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart text;
    text.text = "s";
    system_item.content.emplace_back(std::move(text));
    request.input = {std::move(system_item)};
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    return request;
}

[[nodiscard]] std::string sse_event(const std::string &name, const std::string &json) {
    return "event: " + name + "\ndata: " + json + "\n\n";
}

[[nodiscard]] std::string completed_event(int sequence, const std::string &output_text) {
    const std::string response =
        R"({"id":"resp_1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":")" +
        output_text + R"("}]}],"usage":{"input_tokens":5,"output_tokens":2}})";
    return sse_event("response.completed", R"({"type":"response.completed","sequence_number":)" +
                                               std::to_string(sequence) + R"(,"response":)" +
                                               response + "}");
}

int framing_handles_arbitrary_fragmentation() {
    SseFramingParser parser;
    const std::string stream = "event: alpha\r\ndata: line1\ndata: line2\r\n\r\n"
                               ": comment only\n\n"
                               "event: beta\ndata: {\"x\":1}\n\n";
    std::vector<SseMessage> messages;
    // Feed one byte at a time: no framing assumption may survive.
    for (const char c : stream) {
        auto produced = parser.feed(std::string_view(&c, 1));
        MIRA_CHECK(produced.has_value());
        for (auto &message : produced.value()) {
            messages.push_back(std::move(message));
        }
    }
    auto tail = parser.finish();
    MIRA_CHECK(tail.has_value());
    for (auto &message : tail.value()) {
        messages.push_back(std::move(message));
    }
    MIRA_CHECK(messages.size() == 2);
    MIRA_CHECK(messages[0].event == "alpha");
    MIRA_CHECK(messages[0].data == "line1\nline2");
    MIRA_CHECK(messages[1].event == "beta");
    MIRA_CHECK(messages[1].data == "{\"x\":1}");

    // Oversized single events fail closed.
    SseFramingLimits limits;
    limits.max_event_data_bytes = 8;
    SseFramingParser bounded(limits);
    MIRA_CHECK(!bounded.feed("data: 0123456789abcdef\n\n").has_value());
    return 0;
}

int reducer_happy_path_and_terminal_reduction() {
    const auto request = base_request_alias();
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    ResponsesSseParser parser(request, profile);

    std::string stream;
    stream +=
        sse_event("response.created",
                  R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_1"}})");
    stream += sse_event(
        "response.output_item.added",
        R"({"type":"output_item.added","sequence_number":1,"item":{"id":"msg_1","type":"message"}})");
    stream += sse_event(
        "response.content_part.added",
        R"({"type":"content_part.added","sequence_number":2,"item_id":"msg_1","output_index":0})");
    stream += sse_event(
        "response.output_text.delta",
        R"({"type":"output_text.delta","sequence_number":3,"item_id":"msg_1","delta":"{\"act"})");
    stream += sse_event(
        "response.output_text.delta",
        R"({"type":"output_text.delta","sequence_number":4,"item_id":"msg_1","delta":"ion\":\"back\"}"})");
    stream += sse_event(
        "response.output_text.done",
        R"({"type":"output_text.done","sequence_number":5,"item_id":"msg_1","text":"{\"action\":\"back\"}"})");
    stream += sse_event(
        "response.content_part.done",
        R"({"type":"content_part.done","sequence_number":6,"item_id":"msg_1","output_index":0})");
    stream += sse_event(
        "response.output_item.done",
        R"({"type":"output_item.done","sequence_number":7,"item":{"id":"msg_1","type":"message"}})");
    stream += completed_event(8, "dGFw");
    MIRA_CHECK(parser.feed(stream).has_value());
    auto preview = parser.take_preview();
    MIRA_CHECK(preview.text == "{\"action\":\"back\"}");
    MIRA_CHECK(!preview.truncated);

    auto finished = parser.finish();
    MIRA_CHECK(finished.has_value());
    MIRA_CHECK(finished.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(finished.value().provider_response_id == "resp_1");
    MIRA_CHECK(finished.value().usage.input_tokens == 5);
    MIRA_CHECK(finished.value().output.size() == 1);
    const auto *message = std::get_if<MessageOutput>(&finished.value().output[0]);
    MIRA_CHECK(message != nullptr);
    MIRA_CHECK(std::get_if<OutputTextPart>(&message->content[0]) != nullptr);
    MIRA_CHECK(parser.stats().terminal_seen);
    MIRA_CHECK(parser.stats().stream_sequence >= 8);
    return 0;
}

int reducer_rejects_protocol_violations() {
    const auto request = base_request_alias();
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");

    // EOF without a terminal is an ambiguous completion.
    {
        ResponsesSseParser parser(request, profile);
        MIRA_CHECK(parser
                       .feed(sse_event("response.created",
                                       R"({"type":"response.created","sequence_number":0})"))
                       .has_value());
        auto finished = parser.finish();
        MIRA_CHECK(!finished.has_value());
        MIRA_CHECK(finished.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::AmbiguousCompletion));
    }
    // Remote sequence gaps are violations.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream +=
            sse_event("response.created", R"({"type":"response.created","sequence_number":0})");
        stream += sse_event(
            "output_item.added",
            R"({"type":"output_item.added","sequence_number":5,"item":{"id":"a","type":"message"}})");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Duplicate terminal events are rejected.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream = completed_event(1, "eA==") + completed_event(2, "eA==");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Events after the terminal are rejected.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream =
            completed_event(1, "eA==") +
            sse_event(
                "output_item.added",
                R"({"type":"output_item.added","sequence_number":2,"item":{"id":"b","type":"message"}})");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Unpaired done for an unknown item.
    {
        ResponsesSseParser parser(request, profile);
        MIRA_CHECK(
            !parser
                 .feed(sse_event(
                     "output_item.done",
                     R"({"type":"output_item.done","sequence_number":0,"item":{"id":"ghost","type":"message"}})"))
                 .has_value());
    }
    // Terminal while an item is still open.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event(
            "output_item.added",
            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
        stream += completed_event(1, "eA==");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Declared text must match the accumulated deltas.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event(
            "output_item.added",
            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
        stream += sse_event(
            "output_text.delta",
            R"({"type":"output_text.delta","sequence_number":1,"item_id":"a","delta":"abc"})");
        stream += sse_event(
            "output_text.done",
            R"({"type":"output_text.done","sequence_number":2,"item_id":"a","text":"xyz"})");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Unknown event names fail closed.
    {
        ResponsesSseParser parser(request, profile);
        MIRA_CHECK(!parser.feed(sse_event("response.teleported", R"({"x":1})")).has_value());
    }
    // Server error events surface as protocol failures.
    {
        ResponsesSseParser parser(request, profile);
        MIRA_CHECK(!parser.feed(sse_event("error", R"({"type":"error","code":"server_error"})"))
                        .has_value());
    }
    // Function arguments must be complete before the item closes.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event(
            "output_item.added",
            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"f","type":"function_call"}})");
        stream += sse_event(
            "function_call_arguments.delta",
            R"({"type":"function_call_arguments.delta","sequence_number":1,"item_id":"f","delta":"{\"q\":"})");
        stream += sse_event(
            "output_item.done",
            R"({"type":"output_item.done","sequence_number":2,"item":{"id":"f","type":"function_call"}})");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    return 0;
}

int preview_is_bounded_and_unvalidated() {
    const auto request = base_request_alias();
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    SseStreamLimits limits;
    limits.max_preview_bytes = 16;
    ResponsesSseParser parser(request, profile, limits);

    std::string stream;
    stream += sse_event(
        "output_item.added",
        R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
    for (int index = 0; index < 8; ++index) {
        stream +=
            sse_event("output_text.delta", R"({"type":"output_text.delta","sequence_number":)" +
                                               std::to_string(index + 1) +
                                               R"(,"item_id":"a","delta":"0123456789"})");
    }
    MIRA_CHECK(parser.feed(stream).has_value());
    auto preview = parser.take_preview();
    MIRA_CHECK(preview.text.size() <= 16);
    MIRA_CHECK(preview.dropped_updates > 0);
    MIRA_CHECK(preview.truncated);
    // The preview is explicitly typed and must never be a decision source.
    static_assert(std::is_same_v<decltype(preview.text), std::string>);
    MIRA_CHECK(parser.stats().preview_drops == preview.dropped_updates);
    return 0;
}

int cancel_flag_records_late_terminal_as_diagnostic() {
    const auto request = base_request_alias();
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    ResponsesSseParser parser(request, profile);
    parser.note_cancel_requested();
    // The stream still completes canonically; admission (not the parser)
    // decides whether the completion influences the task.
    MIRA_CHECK(parser.feed(completed_event(1, "aGk=")).has_value());
    auto finished = parser.finish();
    MIRA_CHECK(finished.has_value());
    MIRA_CHECK(finished.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(parser.stats().cancel_seen);
    return 0;
}

std::string chat_chunk(const std::string &delta, const std::string &finish = "null") {
    return "data: {\"id\":\"chat_1\",\"model\":\"m\",\"choices\":[{\"index\":0,\"delta\":" + delta +
           ",\"finish_reason\":" + finish + "}]}\n\n";
}
int chat_stream_fragmentation_tools_and_limits() {
    auto request = base_request_alias();
    auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    const auto stream = chat_chunk(R"({"role":"assistant","content":"你好"})") +
                        chat_chunk(R"({"content":"世界"})") + chat_chunk("{}", "\"stop\"") +
                        "data: "
                        "{\"id\":\"chat_1\",\"model\":\"m\",\"choices\":[],\"usage\":{\"prompt_"
                        "tokens\":8,\"completion_tokens\":4,\"total_tokens\":12}}\n\n" +
                        "data: [DONE]\n\n";
    ChatCompletionsSseParser parser(request, profile);
    std::string preview;
    for (const char c : stream) {
        MIRA_CHECK(parser.feed(std::string_view(&c, 1)));
        preview += parser.take_preview().text;
    }
    MIRA_CHECK(preview == "你好世界");
    auto result = parser.finish();
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().usage.input_tokens == 8);
    MIRA_CHECK(
        std::get<OutputTextPart>(std::get<MessageOutput>(result.value().output[0]).content[0])
            .text == preview);
    MIRA_CHECK(!parser.feed("data: [DONE]\n\n"));

    const auto tool_stream =
        chat_chunk(
            R"({"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"wait","arguments":"{\"seconds\":"}}]})") +
        chat_chunk(R"({"tool_calls":[{"index":0,"type":null,"function":{"arguments":"0}"}}]})") +
        chat_chunk("{}", "\"tool_calls\"") + "data: [DONE]\n\n";
    ChatCompletionsSseParser tools(request, profile);
    MIRA_CHECK(tools.feed(tool_stream));
    auto tool_result = tools.finish();
    MIRA_CHECK(tool_result);
    auto tool =
        std::find_if(tool_result.value().output.begin(), tool_result.value().output.end(),
                     [](const auto &item) { return std::holds_alternative<ToolCallOutput>(item); });
    MIRA_CHECK(tool != tool_result.value().output.end());
    MIRA_CHECK(std::get<ToolCallOutput>(*tool).provider_name == "wait");
    // No provisional tool may leak into UI preview.
    MIRA_CHECK(tools.take_preview().text.empty());

    for (const auto &bad :
         {chat_chunk("{}") + "data: [DONE]\n\n", chat_chunk("{}", "\"bogus\"") + "data: [DONE]\n\n",
          chat_chunk(R"({"content":123})"),
          chat_chunk("{}", "\"stop\"") + chat_chunk(R"({"content":"late"})")}) {
        ChatCompletionsSseParser invalid_parser(request, profile);
        auto status = invalid_parser.feed(bad);
        MIRA_CHECK(!status || !invalid_parser.finish());
    }
    ChatCompletionsSseParser cumulative(request, profile);
    const std::string usage_first =
        "data: "
        "{\"id\":\"chat_1\",\"model\":\"m\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"a\"}"
        ",\"finish_reason\":null}],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":1}}\n\n";
    const std::string usage_last = "data: "
                                   "{\"id\":\"chat_1\",\"model\":\"m\",\"choices\":[{\"index\":0,"
                                   "\"delta\":{\"content\":\"b\"},\"finish_reason\":\"stop\"}],"
                                   "\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":2}}\n\n";
    MIRA_CHECK(cumulative.feed(usage_first + usage_last + "data: [DONE]\n\n"));
    MIRA_CHECK(cumulative.finish().value().usage.output_tokens == 2);
    ChatCompletionsSseParser decreasing(request, profile);
    MIRA_CHECK(!decreasing.feed(usage_last + usage_first));
    ChatCompletionsSseParser eof(request, profile);
    MIRA_CHECK(eof.feed(chat_chunk(R"({"content":"partial"})")));
    MIRA_CHECK(!eof.finish());
    SseStreamLimits limits;
    limits.max_preview_bytes = 3;
    ChatCompletionsSseParser bounded(request, profile, limits);
    MIRA_CHECK(bounded.feed(chat_chunk(R"({"content":"你好"})")));
    MIRA_CHECK(bounded.take_preview().truncated);
    MIRA_CHECK(bounded.take_preview().dropped_updates == 0);
    limits.max_accumulated_text_bytes = 1;
    ChatCompletionsSseParser overflow(request, profile, limits);
    MIRA_CHECK(!overflow.feed(chat_chunk(R"({"content":"你好"})")));
    limits.max_accumulated_text_bytes = 4096;
    limits.max_arguments_buffer_bytes = 1;
    ChatCompletionsSseParser tool_overflow(request, profile, limits);
    MIRA_CHECK(!tool_overflow.feed(tool_stream));
    return 0;
}
int chat_reasoning_stream_maps_to_thinking() {
    // Issue #83 / MIRA-20261008-001: streamed delta.reasoning_content
    // accumulates out-of-band and surfaces as a ThinkingPart at the terminal
    // decode, never as answer preview and never as a text delta.
    auto request = base_request_alias();
    auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    const auto stream = chat_chunk(R"({"role":"assistant","reasoning_content":"think "})") +
                        chat_chunk(R"({"reasoning_content":"more","content":"ans"})") +
                        chat_chunk(R"({"content":"wer"})") + chat_chunk("{}", "\"stop\"") +
                        "data: [DONE]\n\n";

    // Every split point: accumulation and terminal decode must not depend on
    // chunk boundaries (mirrors the Messages dialect thinking split loop).
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        ChatCompletionsSseParser parser(request, profile);
        MIRA_CHECK(parser.feed(std::string_view(stream).substr(0, split)));
        MIRA_CHECK(parser.feed(std::string_view(stream).substr(split)));
        auto result = parser.finish();
        MIRA_CHECK(result);
        MIRA_CHECK(result.value().status == ModelCompletionStatus::Completed);
        MIRA_CHECK(result.value().output.size() == 2);
        const auto *thinking = std::get_if<ThinkingPart>(&result.value().output[0]);
        MIRA_CHECK(thinking != nullptr);
        MIRA_CHECK(thinking->text == "think more");
        MIRA_CHECK(thinking->signature.empty());
        MIRA_CHECK(!thinking->redacted);
        const auto *message = std::get_if<MessageOutput>(&result.value().output[1]);
        MIRA_CHECK(message != nullptr);
        MIRA_CHECK(std::get<OutputTextPart>(message->content[0]).text == "answer");
        MIRA_CHECK(parser.take_preview().text == "answer");
    }

    // Pure reasoning increments are invisible to the live preview and the
    // text-delta counter, yet still produce the terminal ThinkingPart.
    {
        const auto reasoning_only = chat_chunk(R"({"role":"assistant","reasoning_content":"a"})") +
                                    chat_chunk(R"({"reasoning_content":"b"})") +
                                    chat_chunk("{}", "\"stop\"") + "data: [DONE]\n\n";
        ChatCompletionsSseParser parser(request, profile);
        MIRA_CHECK(parser.feed(reasoning_only));
        MIRA_CHECK(parser.stats().text_deltas == 0);
        MIRA_CHECK(parser.take_preview().text.empty());
        auto result = parser.finish();
        MIRA_CHECK(result);
        // finish() always serializes content:"" and the shared decode maps an
        // empty-string content to an empty OutputTextPart, so a pure-reasoning
        // stream yields the ThinkingPart plus an empty-text MessageOutput —
        // the same shape a non-stream body with "content":"" produces.
        MIRA_CHECK(result.value().output.size() == 2);
        const auto *thinking = std::get_if<ThinkingPart>(&result.value().output[0]);
        MIRA_CHECK(thinking != nullptr && thinking->text == "ab");
        const auto *empty_message = std::get_if<MessageOutput>(&result.value().output[1]);
        MIRA_CHECK(empty_message != nullptr && empty_message->content.size() == 1);
        MIRA_CHECK(std::get<OutputTextPart>(empty_message->content[0]).text.empty());
        MIRA_CHECK(std::get_if<UnknownOutput>(&result.value().output[0]) == nullptr);
        MIRA_CHECK(std::get_if<UnknownOutput>(&result.value().output[1]) == nullptr);
    }

    // Reasoning bytes share the accumulated-text budget and fail closed when
    // exceeded; non-string reasoning deltas are protocol violations.
    {
        SseStreamLimits limits;
        limits.max_accumulated_text_bytes = 3;
        ChatCompletionsSseParser reasoning_over(request, profile, limits);
        MIRA_CHECK(!reasoning_over.feed(chat_chunk(R"({"reasoning_content":"abcd"})")));
        // Answer text within the same small budget still streams.
        ChatCompletionsSseParser content_within(request, profile, limits);
        MIRA_CHECK(content_within.feed(chat_chunk(R"({"content":"ab"})")));
        ChatCompletionsSseParser bad_type(request, profile);
        MIRA_CHECK(!bad_type.feed(chat_chunk(R"({"reasoning_content":42})")));
    }

    // Reasoning + answer + tool calls in one stream: the terminal output is
    // the full ordered set (thinking, message, tool call).
    {
        const auto mixed =
            chat_chunk(R"({"role":"assistant","reasoning_content":"why"})") +
            chat_chunk(R"({"content":"calling "})") +
            chat_chunk(
                R"({"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"wait","arguments":"{\"seconds\":"}}]})") +
            chat_chunk(
                R"({"tool_calls":[{"index":0,"type":null,"function":{"arguments":"1}"}}]})") +
            chat_chunk("{}", "\"tool_calls\"") + "data: [DONE]\n\n";
        ChatCompletionsSseParser parser(request, profile);
        MIRA_CHECK(parser.feed(mixed));
        MIRA_CHECK(parser.take_preview().text == "calling ");
        auto result = parser.finish();
        MIRA_CHECK(result);
        MIRA_CHECK(result.value().status == ModelCompletionStatus::Completed);
        MIRA_CHECK(result.value().output.size() == 3);
        const auto *thinking = std::get_if<ThinkingPart>(&result.value().output[0]);
        MIRA_CHECK(thinking != nullptr && thinking->text == "why" && !thinking->redacted);
        const auto *message = std::get_if<MessageOutput>(&result.value().output[1]);
        MIRA_CHECK(message != nullptr);
        MIRA_CHECK(std::get<OutputTextPart>(message->content[0]).text == "calling ");
        const auto *call = std::get_if<ToolCallOutput>(&result.value().output[2]);
        MIRA_CHECK(call != nullptr && call->provider_name == "wait");
    }
    return 0;
}

int responses_reasoning_stream_events_and_terminal_reduction() {
    // Reasoning stream events are identity/budget checks only; the canonical
    // thinking output comes from the terminal body decode.
    const auto request = base_request_alias();
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");

    const auto reasoning_item_added = [](int sequence, const char *id) {
        return sse_event("response.output_item.added",
                         std::string(R"({"type":"output_item.added","sequence_number":)") +
                             std::to_string(sequence) + R"(,"item":{"id":")" + id +
                             R"(","type":"reasoning"}})");
    };
    const auto reasoning_item_done = [](int sequence, const char *id) {
        return sse_event("response.output_item.done",
                         std::string(R"({"type":"output_item.done","sequence_number":)") +
                             std::to_string(sequence) + R"(,"item":{"id":")" + id +
                             R"(","type":"reasoning"}})");
    };

    // Summary stream: reasoning summary events around an open reasoning item,
    // then a plain message item, then a terminal body carrying both items.
    {
        std::string stream;
        stream += sse_event(
            "response.created",
            R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_rs"}})");
        stream += reasoning_item_added(1, "rs_1");
        stream += sse_event(
            "response.reasoning_summary_part.added",
            R"({"type":"reasoning_summary_part.added","sequence_number":2,"item_id":"rs_1","summary_index":0})");
        stream += sse_event(
            "response.reasoning_summary_text.delta",
            R"({"type":"reasoning_summary_text.delta","sequence_number":3,"item_id":"rs_1","delta":"sum"})");
        stream += sse_event(
            "response.reasoning_summary_text.delta",
            R"({"type":"reasoning_summary_text.delta","sequence_number":4,"item_id":"rs_1","delta":"mary"})");
        stream += sse_event(
            "response.reasoning_summary_text.done",
            R"({"type":"reasoning_summary_text.done","sequence_number":5,"item_id":"rs_1","text":"summary"})");
        stream += sse_event(
            "response.reasoning_summary_part.done",
            R"({"type":"reasoning_summary_part.done","sequence_number":6,"item_id":"rs_1","summary_index":0})");
        stream += reasoning_item_done(7, "rs_1");
        stream += sse_event(
            "response.output_item.added",
            R"({"type":"output_item.added","sequence_number":8,"item":{"id":"msg_1","type":"message"}})");
        stream += sse_event(
            "response.output_text.delta",
            R"({"type":"output_text.delta","sequence_number":9,"item_id":"msg_1","delta":"final"})");
        stream += sse_event(
            "response.output_text.done",
            R"({"type":"output_text.done","sequence_number":10,"item_id":"msg_1","text":"final"})");
        stream += sse_event(
            "response.output_item.done",
            R"({"type":"output_item.done","sequence_number":11,"item":{"id":"msg_1","type":"message"}})");
        stream += sse_event(
            "response.completed",
            R"({"type":"response.completed","sequence_number":12,"response":{"id":"resp_rs","status":"completed","model":"m","output":[{"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"summary"}]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"final"}]}],"usage":{"input_tokens":3,"output_tokens":2}}})");

        // Per-byte split points across the whole reasoning stream.
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            ResponsesSseParser parser(request, profile);
            MIRA_CHECK(parser.feed(std::string_view(stream).substr(0, split)));
            MIRA_CHECK(parser.feed(std::string_view(stream).substr(split)));
            auto finished = parser.finish();
            MIRA_CHECK(finished.has_value());
            MIRA_CHECK(finished.value().status == ModelCompletionStatus::Completed);
            MIRA_CHECK(finished.value().output.size() == 2);
            const auto *thinking = std::get_if<ThinkingPart>(&finished.value().output[0]);
            MIRA_CHECK(thinking != nullptr);
            MIRA_CHECK(thinking->text == "summary");
            MIRA_CHECK(thinking->redacted); // Summary text keeps redacted semantics.
            MIRA_CHECK(thinking->signature.empty());
            const auto *message = std::get_if<MessageOutput>(&finished.value().output[1]);
            MIRA_CHECK(message != nullptr);
            MIRA_CHECK(std::get<OutputTextPart>(message->content[0]).text == "final");
        }

        // Reasoning text never enters the answer preview.
        ResponsesSseParser preview_parser(request, profile);
        MIRA_CHECK(preview_parser.feed(stream));
        MIRA_CHECK(preview_parser.take_preview().text == "final");
    }

    // Raw stream: reasoning_text deltas plus summary events; the terminal
    // body carries both, and raw reasoning wins with redacted=false.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event(
            "response.created",
            R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_raw"}})");
        stream += reasoning_item_added(1, "rs_2");
        stream += sse_event(
            "response.reasoning_text.delta",
            R"({"type":"reasoning_text.delta","sequence_number":2,"item_id":"rs_2","delta":"raw "})");
        stream += sse_event(
            "response.reasoning_text.delta",
            R"({"type":"reasoning_text.delta","sequence_number":3,"item_id":"rs_2","delta":"thought"})");
        stream += sse_event(
            "response.reasoning_text.done",
            R"({"type":"reasoning_text.done","sequence_number":4,"item_id":"rs_2","text":"raw thought"})");
        stream += reasoning_item_done(5, "rs_2");
        stream += sse_event(
            "response.completed",
            R"({"type":"response.completed","sequence_number":6,"response":{"id":"resp_raw","status":"completed","model":"m","output":[{"type":"reasoning","id":"rs_2","content":[{"type":"reasoning_text","text":"raw thought"}],"summary":[{"type":"summary_text","text":"sum"}]}],"usage":{}}})");
        MIRA_CHECK(parser.feed(stream));
        auto finished = parser.finish();
        MIRA_CHECK(finished.has_value());
        MIRA_CHECK(finished.value().output.size() == 1);
        const auto *thinking = std::get_if<ThinkingPart>(&finished.value().output[0]);
        MIRA_CHECK(thinking != nullptr);
        MIRA_CHECK(thinking->text == "raw thought");
        MIRA_CHECK(!thinking->redacted);
        MIRA_CHECK(parser.take_preview().text.empty());
    }

    // Reasoning delta budget: the streamed reasoning byte counter is shared
    // across summary and raw deltas and enforces the accumulated limit.
    {
        SseStreamLimits limits;
        limits.max_accumulated_text_bytes = 3;
        ResponsesSseParser over(request, profile, limits);
        std::string stream;
        stream += sse_event(
            "response.created",
            R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_b"}})");
        stream += reasoning_item_added(1, "rs_b");
        stream += sse_event(
            "response.reasoning_summary_text.delta",
            R"({"type":"reasoning_summary_text.delta","sequence_number":2,"item_id":"rs_b","delta":"ab"})");
        MIRA_CHECK(over.feed(stream));
        MIRA_CHECK(
            !over
                 .feed(sse_event(
                     "response.reasoning_summary_text.delta",
                     R"({"type":"reasoning_summary_text.delta","sequence_number":3,"item_id":"rs_b","delta":"cd"})"))
                 .has_value());
        MIRA_CHECK(
            over
                .feed(sse_event(
                    "response.reasoning_summary_text.delta",
                    R"({"type":"reasoning_summary_text.delta","sequence_number":3,"item_id":"rs_b","delta":"cd"})"))
                .error()
                .domain_code == static_cast<std::int32_t>(ModelDomainCode::ResponseTooLarge));

        // A fresh parser: raw deltas draw from the same reasoning budget.
        ResponsesSseParser raw_over(request, profile, limits);
        std::string raw_stream;
        raw_stream += sse_event(
            "response.created",
            R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_c"}})");
        raw_stream += reasoning_item_added(1, "rs_c");
        raw_stream += sse_event(
            "response.reasoning_text.delta",
            R"({"type":"reasoning_text.delta","sequence_number":2,"item_id":"rs_c","delta":"abcd"})");
        MIRA_CHECK(!raw_over.feed(raw_stream).has_value());
    }

    // Identity enforcement: reasoning events need a string item_id bound to
    // an open item; closed or unknown items are protocol violations.
    {
        const auto probe = [&](const std::string &event, const std::string &data,
                               bool open_item_first) {
            ResponsesSseParser parser(request, profile);
            std::string stream;
            stream += sse_event(
                "response.created",
                R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_x"}})");
            if (open_item_first) {
                stream += reasoning_item_added(1, "rs");
            }
            stream += sse_event(event, data);
            auto status = parser.feed(stream);
            if (status) {
                auto finished = parser.finish();
                return finished.has_value() ? std::string{} : finished.error().safe_message;
            }
            return status.error().safe_message;
        };
        // Unknown item.
        MIRA_CHECK(
            probe(
                "response.reasoning_summary_text.delta",
                R"({"type":"reasoning_summary_text.delta","sequence_number":1,"item_id":"ghost","delta":"x"})",
                false) == "reasoning event references a closed item");
        // Missing item id.
        MIRA_CHECK(probe("response.reasoning_text.delta",
                         R"({"type":"reasoning_text.delta","sequence_number":1,"delta":"x"})",
                         false) == "reasoning event carries no item id");
        // Delta without string payload.
        MIRA_CHECK(
            probe("response.reasoning_text.delta",
                  R"({"type":"reasoning_text.delta","sequence_number":2,"item_id":"rs","delta":5})",
                  true) == "reasoning delta carries no delta text");
        // Part events also require the string item id.
        MIRA_CHECK(probe("response.reasoning_summary_part.added",
                         R"({"type":"reasoning_summary_part.added","sequence_number":1})",
                         false) == "reasoning event carries no item id");

        // An item that closed before the reasoning event is rejected too.
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event(
            "response.created",
            R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_y"}})");
        stream += reasoning_item_added(1, "rs_y");
        stream += reasoning_item_done(2, "rs_y");
        stream += sse_event(
            "response.reasoning_text.delta",
            R"({"type":"reasoning_text.delta","sequence_number":3,"item_id":"rs_y","delta":"late"})");
        auto status = parser.feed(stream);
        MIRA_CHECK(!status.has_value());
        MIRA_CHECK(status.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
        MIRA_CHECK(status.error().safe_message == "reasoning event references a closed item");
    }
    return 0;
}

int live_preview_delivery_and_failure_isolation() {
    auto request = base_request_alias();
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test"));
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    MockStep step;
    step.status = 200;
    step.chunk_pieces = {chat_chunk(R"({"content":"first"})"),
                         chat_chunk(R"({"content":" second"})"),
                         chat_chunk("{}", "\"stop\"") + "data: [DONE]\n\n"};
    transport->enqueue(step);
    OpenAiCompatibleProvider provider(profile, transport, nullptr);
    ProviderInferOptions options;
    options.stream = true;
    std::vector<std::string> previews;
    options.preview_sink = [&](const auto &id, const auto &preview) {
        (void)id;
        previews.push_back(preview.text);
    };
    auto response = provider.infer(request, OperationContext{}, options);
    MIRA_CHECK(response);
    MIRA_CHECK(previews.size() == 3 && previews[0].empty() && previews[1] == "first" &&
               previews[2] == "first second");
    MIRA_CHECK(provider.take_last_preview()->text == "first second");
    transport->enqueue(step);
    options.preview_sink = [](const auto &, const auto &) {
        throw std::runtime_error("preview refused");
    };
    MIRA_CHECK(provider.infer(request, OperationContext{}, options));
    MIRA_CHECK(provider.last_sse_stats().preview_drops == 3);
    transport->enqueue(step);
    OperationContext cancelled;
    cancelled.cancellation_requested = [] { return true; };
    std::size_t callbacks = 0;
    options.preview_sink = [&](const auto &, const auto &) { ++callbacks; };
    (void)provider.infer(request, cancelled, options);
    MIRA_CHECK(callbacks == 0);
    return 0;
}

} // namespace

int main() {
    if (auto status = chat_stream_fragmentation_tools_and_limits())
        return status;
    if (auto status = chat_reasoning_stream_maps_to_thinking())
        return status;
    if (auto status = responses_reasoning_stream_events_and_terminal_reduction())
        return status;
    if (auto status = live_preview_delivery_and_failure_isolation())
        return status;
    if (const int status = framing_handles_arbitrary_fragmentation(); status != 0) {
        return status;
    }
    if (const int status = reducer_happy_path_and_terminal_reduction(); status != 0) {
        return status;
    }
    if (const int status = reducer_rejects_protocol_violations(); status != 0) {
        return status;
    }
    if (const int status = preview_is_bounded_and_unvalidated(); status != 0) {
        return status;
    }
    if (const int status = cancel_flag_records_late_terminal_as_diagnostic(); status != 0) {
        return status;
    }
    return 0;
}
