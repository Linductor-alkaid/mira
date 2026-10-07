#include "support/m3_support.hpp"
#include "support/test.hpp"
#include <mira/adapters/net/socket_transport.hpp>
#include <mira/model_digest.hpp>
#include <mira/model_sse.hpp>
#include <mira/model_tool.hpp>

using namespace mira;
using namespace mira::testing;
namespace {
ModelRequest request() {
    ModelRequest r;
    r.request_id = ModelRequestId::generate();
    r.generation.max_output_tokens = 512;
    r.output_contract.mode = OutputMode::Text;
    r.input = {{ModelRole::System, {TextPart{"be brief"}}, {}, Sensitivity::Internal},
               {ModelRole::User, {TextPart{"hello"}}, {}, Sensitivity::Internal}};
    return r;
}
std::string event(const char *type, const std::string &data) {
    return "event: " + std::string(type) + "\ndata: " + data + "\n\n";
}
const std::string start = event(
    "message_start",
    R"({"type":"message_start","message":{"id":"msg_1","type":"message","role":"assistant","model":"m","content":[],"stop_reason":null,"usage":{"input_tokens":5,"output_tokens":1}}})");
const std::string text_stream =
    start +
    event("content_block_start",
          R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
    event(
        "content_block_delta",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"你好"}})") +
    event("content_block_stop", R"({"type":"content_block_stop","index":0})") +
    event(
        "message_delta",
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2,"input_tokens_details":{"cached_tokens":0}}})") +
    event("message_stop", R"({"type":"message_stop"})");
class ImageSource final : public IArtifactSource {
  public:
    Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
        return std::vector<std::byte>{std::byte{1}, std::byte{2}};
    }
};
int mappings() {
    auto r = request();
    auto p = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://api.anthropic.com");
    NullArtifactSource none;
    AnthropicMessagesV1Mapper mapper;
    MIRA_CHECK(protocol_dialect_from("anthropic.messages.v1") == p.dialect);
    MIRA_CHECK(p.endpoint_url() == "https://api.anthropic.com/v1/messages");
    auto encoded = mapper.encode_request(r, p, true, none);
    MIRA_CHECK(encoded);
    MIRA_CHECK(encoded.value().find("max_tokens")->as_integer() == 512);
    MIRA_CHECK(!encoded.value().find("stream_options"));
    MIRA_CHECK(encoded.value().find("system")->as_array()->size() == 1);
    MIRA_CHECK(encoded.value().find("messages")->as_array()->size() == 1);
    r.generation.seed = 1;
    MIRA_CHECK(!mapper.encode_request(r, p, false, none));
    r.generation.seed.reset();
    r.generation.reasoning_effort = ReasoningEffort::High;
    MIRA_CHECK(!mapper.encode_request(r, p, false, none));
    r.generation.reasoning_effort.reset();
    r.output_contract.mode = OutputMode::StrictJsonSchema;
    MIRA_CHECK(!mapper.encode_request(r, p, false, none));
    r.output_contract.mode = OutputMode::Text;
    ImagePart image;
    image.source.media_type = "image/png";
    image.source.byte_size = 2;
    r.input.back().content.emplace_back(image);
    ImageSource images;
    auto image_request = mapper.encode_request(r, p, false, images);
    MIRA_CHECK(image_request);
    const auto &part = image_request.value()
                           .find("messages")
                           ->as_array()
                           ->front()
                           .find("content")
                           ->as_array()
                           ->back();
    MIRA_CHECK(*part.find("type")->as_string() == "image");
    MIRA_CHECK(*part.find("source")->find("data")->as_string() == "AQI=");
    MIRA_CHECK(!mapper.encode_request(r, p, false, none));
    p.capabilities.image_input.supported = false;
    MIRA_CHECK(!mapper.encode_request(r, p, false, images));
    r = request();
    ExposedToolSpec tool;
    tool.tool_id = ToolId::generate();
    tool.wire_name = "wait";
    tool.parameters_schema.root = JsonValue::Object{{"type", "object"}};
    r.tools = {tool};
    ToolProposal proposal;
    proposal.tool_id = tool.tool_id;
    proposal.wire_name = "wait";
    proposal.provider_call_id.value = "call1";
    proposal.arguments = JsonValue::Object{{"ms", 1}};
    proposal.arguments_digest = digest_string(to_json_string(proposal.arguments));
    r.input.push_back(make_tool_call_item(proposal));
    ToolExecutionRecord record;
    record.provider_call_id = proposal.provider_call_id;
    record.tool_id = tool.tool_id;
    record.result = JsonValue::Object{{"ok", true}};
    r.input.push_back(make_tool_result_item(record));
    r.tool_choice.mode = ToolChoiceMode::Required;
    auto tool_request = mapper.encode_request(r, p, false, none);
    MIRA_CHECK(tool_request);
    MIRA_CHECK(*tool_request.value().find("tool_choice")->find("type")->as_string() == "any");
    MIRA_CHECK(tool_request.value().find("tools")->as_array()->front().find("input_schema"));
    const auto serialized = to_json_string(tool_request.value());
    MIRA_CHECK(serialized.find("tool_use_id") != std::string::npos);
    auto native_results =
        build_tool_result_input(p.dialect, std::span<const ToolExecutionRecord>{&record, 1});
    MIRA_CHECK(native_results &&
               to_json_string(native_results.value().front()).find("tool_use_id") !=
                   std::string::npos);
    const auto sync = mapper.decode_response(
        r, p,
        {200,
         {},
         R"({"id":"msg_1","type":"message","role":"assistant","model":"m","content":[{"type":"tool_use","id":"call1","name":"wait","input":{"ms":1}}],"stop_reason":"tool_use","usage":{"input_tokens":5,"output_tokens":2,"cache_read_input_tokens":3,"cache_creation_input_tokens":4}})"});
    MIRA_CHECK(sync && std::any_of(sync.value().output.begin(), sync.value().output.end(),
                                   [](const auto &item) {
                                       return std::holds_alternative<ToolCallOutput>(item);
                                   }));
    MIRA_CHECK(sync.value().usage.input_tokens == 12);
    MIRA_CHECK(sync.value().usage.cached_input_tokens == 3);
    MIRA_CHECK(!mapper.decode_response(
        r, p,
        {200,
         {},
         R"({"id":"x","type":"message","role":"assistant","model":"m","content":[{"type":"thinking","thinking":"secret","signature":"x"}],"stop_reason":"end_turn"})"}));
    MIRA_CHECK(!mapper.decode_response(r, p, {401, {}, "{}"}));
    MIRA_CHECK(!mapper.decode_response(
        r, p,
        {200,
         {},
         R"({"id":"x","type":"message","role":"assistant","model":"m","content":[],"stop_reason":"pause_turn"})"}));
    return 0;
}
int streams() {
    auto r = request();
    auto p = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://example.com");
    for (std::size_t split = 0; split <= text_stream.size(); ++split) {
        AnthropicMessagesSseParser parser(r, p);
        MIRA_CHECK(parser.feed(std::string_view(text_stream).substr(0, split)));
        MIRA_CHECK(parser.feed(std::string_view(text_stream).substr(split)));
        auto response = parser.finish();
        MIRA_CHECK(response && response.value().status == ModelCompletionStatus::Completed);
        MIRA_CHECK(response.value().usage.input_tokens == 5);
        MIRA_CHECK(response.value().usage.output_tokens == 2);
        MIRA_CHECK(parser.take_preview().text == "你好");
        MIRA_CHECK(!parser.feed(event("message_stop", R"({"type":"message_stop"})")));
    }
    AnthropicMessagesSseParser eof(r, p);
    MIRA_CHECK(eof.feed(start));
    MIRA_CHECK(!eof.finish());
    AnthropicMessagesSseParser ordering(r, p);
    MIRA_CHECK(!ordering.feed(event("message_stop", R"({"type":"message_stop"})")));
    auto limits = SseStreamLimits{};
    limits.max_accumulated_text_bytes = 1;
    AnthropicMessagesSseParser budget(r, p, limits);
    MIRA_CHECK(!budget.feed(text_stream));
    limits.max_accumulated_text_bytes = 100;
    limits.max_preview_bytes = 1;
    AnthropicMessagesSseParser preview(r, p, limits);
    MIRA_CHECK(preview.feed(text_stream));
    MIRA_CHECK(preview.take_preview().truncated);
    MIRA_CHECK(preview.finish()); // Preview drop never corrupts authoritative text.
    AnthropicMessagesSseParser error(r, p);
    MIRA_CHECK(
        !error.feed(event("error", R"({"type":"error","error":{"type":"overloaded_error"}})")));
    const auto tools =
        start +
        event(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"c","name":"wait","input":{}}})") +
        event(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"ms\":"}})") +
        event(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"1}"}})") +
        event("content_block_stop", R"({"type":"content_block_stop","index":0})") +
        event(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":4}})") +
        event("message_stop", R"({"type":"message_stop"})");
    AnthropicMessagesSseParser tool(r, p);
    for (const char byte : tools)
        MIRA_CHECK(tool.feed(std::string_view{&byte, 1}));
    auto response = tool.finish();
    MIRA_CHECK(response && std::any_of(response.value().output.begin(),
                                       response.value().output.end(), [](const auto &item) {
                                           return std::holds_alternative<ToolCallOutput>(item);
                                       }));
    MIRA_CHECK(std::get<ToolCallOutput>(
                   *std::find_if(response.value().output.begin(), response.value().output.end(),
                                 [](const auto &item) {
                                     return std::holds_alternative<ToolCallOutput>(item);
                                 }))
                   .arguments.find("ms")
                   ->as_integer() == 1);
    return 0;
}
// A deterministic mutation corpus exercises the new parser without external
// services. Seeds stay in source; mutated artifacts are never checked in.
int mutation_corpus() {
    auto r = request();
    auto p = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://api.example.test");
    for (std::size_t at = 0; at < text_stream.size(); ++at) {
        for (const char replacement : {char{0}, char{'{'}, char{'\n'}, char{0x7f}}) {
            auto mutated = text_stream;
            mutated[at] = replacement;
            AnthropicMessagesSseParser parser(r, p);
            // Malformed seeds may fail admission or final validation; neither
            // path may throw, exceed a budget, or yield a tool call.
            if (parser.feed(mutated)) {
                auto result = parser.finish();
                if (result)
                    MIRA_CHECK(std::none_of(result.value().output.begin(),
                                            result.value().output.end(), [](const auto &item) {
                                                return std::holds_alternative<ToolCallOutput>(item);
                                            }));
            }
        }
    }
    return 0;
}
int authentication() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    executor.initialize(config);
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("key", "fixture-key");
    adapters::net::SocketHttpTransport transport(executor, secrets);
    MIRA_CHECK(transport.start());
    ScriptedHttpServer server;
    MIRA_CHECK(server.valid());
    auto task = executor.submit_auto([&server] {
        if (!server.accept_client(std::chrono::seconds{5}))
            return std::string{};
        auto received = server.read_request(std::chrono::seconds{2});
        server.write_all("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
        server.close_client();
        return received;
    });
    HttpRequest http;
    http.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/messages";
    http.authorization = SecretRef{"key"};
    http.credential_scheme = HttpCredentialScheme::ApiKey;
    http.headers.emplace_back("anthropic-version", "2023-06-01");
    TransportLimits limits;
    limits.allow_private_endpoints = true;
    limits.deadlines.total = std::chrono::seconds{5};
    OperationContext context;
    TransportTrace trace;
    auto result = transport.execute(http, limits, context, [](std::string_view) {}, trace);
    const auto received = task.get();
    MIRA_CHECK(result);
    MIRA_CHECK(received.find("x-api-key: fixture-key\r\n") != std::string::npos);
    MIRA_CHECK(received.find("Authorization:") == std::string::npos);
    http.headers.emplace_back("x-api-key", "inline-secret-forbidden");
    MIRA_CHECK(!transport.execute(http, limits, context, [](std::string_view) {}, trace));
    http.headers.pop_back();
    secrets->set("key", "bad\r\nInjected: value");
    MIRA_CHECK(!transport.execute(http, limits, context, [](std::string_view) {}, trace));
    transport.shutdown();
    (void)executor.shutdown(true);
    return 0;
}
} // namespace
int main() {
    if (mappings() || streams() || mutation_corpus() || authentication())
        return 1;
    return 0;
}
