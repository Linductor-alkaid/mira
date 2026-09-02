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
    const std::string response = R"({"id":"resp_1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":")" +
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
    stream += sse_event("response.created", R"({"type":"response.created","sequence_number":0,"response":{"id":"resp_1"}})");
    stream += sse_event("response.output_item.added",
                        R"({"type":"output_item.added","sequence_number":1,"item":{"id":"msg_1","type":"message"}})");
    stream += sse_event("response.content_part.added",
                        R"({"type":"content_part.added","sequence_number":2,"item_id":"msg_1","output_index":0})");
    stream += sse_event("response.output_text.delta",
                        R"({"type":"output_text.delta","sequence_number":3,"item_id":"msg_1","delta":"{\"act"})");
    stream += sse_event("response.output_text.delta",
                        R"({"type":"output_text.delta","sequence_number":4,"item_id":"msg_1","delta":"ion\":\"back\"}"})");
    stream += sse_event("response.output_text.done",
                        R"({"type":"output_text.done","sequence_number":5,"item_id":"msg_1","text":"{\"action\":\"back\"}"})");
    stream += sse_event("response.content_part.done",
                        R"({"type":"content_part.done","sequence_number":6,"item_id":"msg_1","output_index":0})");
    stream += sse_event("response.output_item.done",
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
        stream += sse_event("response.created", R"({"type":"response.created","sequence_number":0})");
        stream += sse_event("output_item.added",
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
            sse_event("output_item.added",
                      R"({"type":"output_item.added","sequence_number":2,"item":{"id":"b","type":"message"}})");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Unpaired done for an unknown item.
    {
        ResponsesSseParser parser(request, profile);
        MIRA_CHECK(!parser
                       .feed(sse_event("output_item.done",
                                       R"({"type":"output_item.done","sequence_number":0,"item":{"id":"ghost","type":"message"}})"))
                       .has_value());
    }
    // Terminal while an item is still open.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event("output_item.added",
                            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
        stream += completed_event(1, "eA==");
        MIRA_CHECK(!parser.feed(stream).has_value());
    }
    // Declared text must match the accumulated deltas.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event("output_item.added",
                            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
        stream += sse_event("output_text.delta",
                            R"({"type":"output_text.delta","sequence_number":1,"item_id":"a","delta":"abc"})");
        stream += sse_event("output_text.done",
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
        MIRA_CHECK(!parser
                       .feed(sse_event("error",
                                       R"({"type":"error","code":"server_error"})"))
                       .has_value());
    }
    // Function arguments must be complete before the item closes.
    {
        ResponsesSseParser parser(request, profile);
        std::string stream;
        stream += sse_event("output_item.added",
                            R"({"type":"output_item.added","sequence_number":0,"item":{"id":"f","type":"function_call"}})");
        stream += sse_event("function_call_arguments.delta",
                            R"({"type":"function_call_arguments.delta","sequence_number":1,"item_id":"f","delta":"{\"q\":"})");
        stream += sse_event("output_item.done",
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
    stream += sse_event("output_item.added",
                        R"({"type":"output_item.added","sequence_number":0,"item":{"id":"a","type":"message"}})");
    for (int index = 0; index < 8; ++index) {
        stream += sse_event("output_text.delta",
                            R"({"type":"output_text.delta","sequence_number":)" +
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

} // namespace

int main() {
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
