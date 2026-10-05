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
