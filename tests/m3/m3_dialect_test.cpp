#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>

#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] ModelRequest base_request() {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart system_text;
    system_text.text = "be brief";
    system_item.content.emplace_back(std::move(system_text));
    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    TextPart user_text;
    user_text.text = "hello";
    user_item.content.emplace_back(std::move(user_text));
    request.input = {std::move(system_item), std::move(user_item)};
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    return request;
}

[[nodiscard]] std::shared_ptr<IArtifactSource> image_source(std::size_t &fetches) {
    class Source final : public IArtifactSource {
      public:
        explicit Source(std::size_t &counter) : counter_(counter) {}
        Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
            ++counter_;
            return std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        }
        std::size_t &counter_;
    };
    return std::make_shared<Source>(fetches);
}

int responses_request_golden() {
    ResponsesV1Mapper mapper;
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    auto request = base_request();
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id = SchemaId::generate();
    request.output_contract.schema.root =
        parse_json(
            R"({"type":"object","properties":{"a":{"type":"integer"}},"required":["a"],"additionalProperties":false})")
            .value();
    request.output_contract.canonical_schema_digest =
        canonical_json_digest(request.output_contract.schema.root);
    request.generation.temperature = 0.2;
    request.generation.max_output_tokens = 128;

    NullArtifactSource artifacts;
    auto wire = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(wire.has_value());
    const auto text = to_json_string(wire.value());
    MIRA_CHECK(text.find("\"model\":\"test-model\"") != std::string::npos);
    MIRA_CHECK(text.find("\"role\":\"system\"") != std::string::npos);
    // `store` is always explicit; the provider default is never relied on.
    MIRA_CHECK(text.find("\"store\":false") != std::string::npos);
    MIRA_CHECK(text.find("\"stream\":false") != std::string::npos);
    MIRA_CHECK(text.find("json_schema") != std::string::npos);
    MIRA_CHECK(text.find("\"strict\":true") != std::string::npos);
    MIRA_CHECK(text.find("\"temperature\":0.2") != std::string::npos);

    // Golden digest: the canonical wire form is stable.
    const auto golden = canonical_json_string(wire.value());
    MIRA_CHECK(golden == canonical_json_string(parse_json(to_json_string(wire.value())).value()));

    // Tool exposure and named choice.
    request.tools.push_back(
        ExposedToolSpec{ToolId::generate(), SemanticVersion{1, 0, 0}, "lookup", "finds things",
                        JsonSchema{parse_json(R"({"type":"object"})").value()}, Hash{}, true});
    request.tools[0].spec_digest = tool_snapshot_digest(request.tools);
    request.tool_choice.mode = ToolChoiceMode::Named;
    request.tool_choice.required_tool = request.tools[0].tool_id;
    auto with_tools = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(with_tools.has_value());
    const auto tool_text = to_json_string(with_tools.value());
    MIRA_CHECK(tool_text.find("\"name\":\"lookup\"") != std::string::npos);
    MIRA_CHECK(tool_text.find("\"tool_choice\"") != std::string::npos);
    MIRA_CHECK(tool_text.find("\"name\":\"lookup\"") != std::string::npos);
    MIRA_CHECK(tool_text.find("\"type\":\"function\"") != std::string::npos);

    // Unsupported generation parameters fail closed before sending.
    request.generation.seed = 42;
    auto rejected = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::UnsupportedCapability);
    return 0;
}

int responses_response_golden() {
    ResponsesV1Mapper mapper;
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    const auto request = base_request();

    const std::string body = R"({
        "id": "resp_1",
        "object": "response",
        "status": "completed",
        "model": "test-model-2026",
        "output": [
            {"type": "message", "role": "assistant", "content": [
                {"type": "output_text", "text": "{\"action\":\"back\",\"reason\":\"r\"}"}
            ]},
            {"type": "reasoning", "id": "rs_1"}
        ],
        "usage": {
            "input_tokens": 21,
            "output_tokens": 6,
            "input_tokens_details": {"cached_tokens": 4},
            "output_tokens_details": {"reasoning_tokens": 2}
        }
    })";
    WireHttpResponse wire;
    wire.status = 200;
    wire.body = body;
    wire.headers = {{"x-ratelimit-remaining-requests", "99"},
                    {"x-ratelimit-remaining-tokens", "1000"}};
    auto decoded = mapper.decode_response(request, profile, wire);
    MIRA_CHECK(decoded.has_value());
    MIRA_CHECK(decoded.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(decoded.value().provider_response_id == "resp_1");
    MIRA_CHECK(decoded.value().resolved_model == "test-model-2026");
    MIRA_CHECK(decoded.value().usage.input_tokens == 21);
    MIRA_CHECK(decoded.value().usage.cached_input_tokens == 4);
    MIRA_CHECK(decoded.value().usage.reasoning_tokens == 2);
    MIRA_CHECK(decoded.value().usage.quality == UsageQuality::ProviderReported);
    MIRA_CHECK(decoded.value().rate_limit.remaining_requests == 99);
    MIRA_CHECK(decoded.value().output.size() == 2);
    MIRA_CHECK(std::get_if<MessageOutput>(&decoded.value().output[0]) != nullptr);
    // Unknown diagnostic items keep a digest summary, not the payload.
    const auto *unknown = std::get_if<UnknownOutput>(&decoded.value().output[1]);
    MIRA_CHECK(unknown != nullptr);
    MIRA_CHECK(unknown->provider_type.find("reasoning") != std::string::npos);

    // Tool call decode resolves exposed tool identities.
    auto tool_request = base_request();
    tool_request.tools.push_back(
        ExposedToolSpec{ToolId::generate(), SemanticVersion{1, 0, 0}, "lookup", "d",
                        JsonSchema{parse_json(R"({"type":"object"})").value()}, Hash{}, false});
    const std::string tool_body = R"({
        "id": "resp_2", "status": "completed", "model": "m",
        "output": [{"type": "function_call", "call_id": "call_9", "name": "lookup",
                    "arguments": "{\"q\": 1}"}],
        "usage": {"input_tokens": 5, "output_tokens": 5}
    })";
    wire.body = tool_body;
    auto tool_decoded = mapper.decode_response(tool_request, profile, wire);
    MIRA_CHECK(tool_decoded.has_value());
    MIRA_CHECK(tool_decoded.value().output.size() == 1);
    const auto *call = std::get_if<ToolCallOutput>(&tool_decoded.value().output[0]);
    MIRA_CHECK(call != nullptr);
    MIRA_CHECK(call->provider_call_id.value == "call_9");
    MIRA_CHECK(call->tool_id == tool_request.tools[0].tool_id);
    MIRA_CHECK(call->arguments.as_object() != nullptr);

    // Malformed function arguments fail closed.
    wire.body = R"({
        "id": "resp_3", "status": "completed", "model": "m",
        "output": [{"type": "function_call", "call_id": "c", "name": "lookup",
                    "arguments": "[1,2]"}],
        "usage": {}
    })";
    auto bad_args = mapper.decode_response(tool_request, profile, wire);
    MIRA_CHECK(!bad_args.has_value());

    // Incomplete with reason maps canonically.
    wire.body =
        R"({"id":"r","status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[],"usage":{}})";
    auto incomplete = mapper.decode_response(request, profile, wire);
    MIRA_CHECK(incomplete.has_value());
    MIRA_CHECK(incomplete.value().status == ModelCompletionStatus::Incomplete);
    MIRA_CHECK(incomplete.value().incomplete_reason == IncompleteReason::MaxOutputTokens);

    // Non-terminal status on a sync body is a protocol violation.
    wire.body = R"({"id":"r","status":"in_progress","output":[]})";
    MIRA_CHECK(!mapper.decode_response(request, profile, wire).has_value());

    // Unknown terminal status fails closed.
    wire.body = R"({"id":"r","status":"exploded","output":[]})";
    MIRA_CHECK(!mapper.decode_response(request, profile, wire).has_value());
    return 0;
}

int error_status_mapping_and_retry_after() {
    const auto check = [](int status, ModelDomainCode expected) {
        WireHttpResponse wire;
        wire.status = status;
        wire.body = R"({"error":{"code":"bad_thing"}})";
        const auto error = map_http_error_status(wire);
        if (error.domain != "mira.model" ||
            error.domain_code != static_cast<std::int32_t>(expected)) {
            return 1;
        }
        // Response bodies never leak into safe messages; only bounded codes.
        if (error.safe_message.find("bad_thing") != std::string::npos &&
            error.safe_message.find("provider code: bad_thing") == std::string::npos) {
            return 1;
        }
        return 0;
    };
    MIRA_CHECK(check(401, ModelDomainCode::AuthenticationFailed) == 0);
    MIRA_CHECK(check(403, ModelDomainCode::ProviderPermissionDenied) == 0);
    MIRA_CHECK(check(404, ModelDomainCode::InvalidModelRequest) == 0);
    MIRA_CHECK(check(400, ModelDomainCode::InvalidModelRequest) == 0);
    MIRA_CHECK(check(429, ModelDomainCode::RateLimited) == 0);
    MIRA_CHECK(check(500, ModelDomainCode::ProviderOverloaded) == 0);
    MIRA_CHECK(check(503, ModelDomainCode::ProviderOverloaded) == 0);
    MIRA_CHECK(check(418, ModelDomainCode::ProtocolViolation) == 0);

    std::vector<std::pair<std::string, std::string>> headers = {{"Retry-After", "2"}};
    MIRA_CHECK(parse_retry_after(headers, std::chrono::milliseconds{10'000}).has_value());
    MIRA_CHECK(parse_retry_after(headers, std::chrono::milliseconds{10'000}).value() ==
               std::chrono::milliseconds{2'000});
    headers[0].second = "not-a-number";
    MIRA_CHECK(!parse_retry_after(headers, std::chrono::milliseconds{10'000}).has_value());
    // Caps apply: a day-long Retry-After clamps instead of stalling.
    headers[0].second = "86400";
    MIRA_CHECK(parse_retry_after(headers, std::chrono::milliseconds{5'000}).value() ==
               std::chrono::milliseconds{5'000});

    const auto encoded =
        base64_encode(std::vector<std::byte>{std::byte{'M'}, std::byte{'a'}, std::byte{'n'}});
    MIRA_CHECK(encoded == "TWFu");
    MIRA_CHECK(base64_encode(std::vector<std::byte>{std::byte{'M'}}) == "TQ==");
    MIRA_CHECK(base64_encode(std::vector<std::byte>{std::byte{'M'}, std::byte{'a'}}) == "TWE=");
    return 0;
}

int chat_completions_request_and_response() {
    ChatCompletionsV1Mapper mapper;
    const auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    auto request = base_request();
    NullArtifactSource artifacts;

    auto wire = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(wire.has_value());
    const auto text = to_json_string(wire.value());
    MIRA_CHECK(text.find("\"messages\"") != std::string::npos);
    MIRA_CHECK(text.find("\"role\":\"system\"") != std::string::npos);
    MIRA_CHECK(text.find("\"content\":\"hello\"") != std::string::npos);

    // Unrepresentable canonical fields fail closed; the mapper never falls
    // back to the Responses endpoint.
    auto with_continuation = request;
    with_continuation.continuation = ProviderContinuation{};
    with_continuation.continuation->provider_state = "opaque";
    with_continuation.continuation->profile_id = request.profile_id;
    with_continuation.continuation->task_id = request.task_id;
    auto cont = mapper.encode_request(with_continuation, profile, false, artifacts);
    MIRA_CHECK(!cont.has_value());
    MIRA_CHECK(cont.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));

    auto storing = request;
    storing.data_policy.store = true;
    storing.data_policy.remote_retention = std::chrono::seconds{60};
    auto no_store = mapper.encode_request(storing, profile, false, artifacts);
    MIRA_CHECK(!no_store.has_value());

    auto with_file = request;
    FilePart file;
    file.source.id = ArtifactId::generate();
    file.media_type = "text/plain";
    with_file.input[1].content.emplace_back(std::move(file));
    auto no_file = mapper.encode_request(with_file, profile, false, artifacts);
    MIRA_CHECK(!no_file.has_value());

    // Streaming requires fixture-verified SSE capability on this dialect.
    auto no_stream_profile = profile;
    no_stream_profile.capabilities.sse = CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    auto no_stream = mapper.encode_request(request, no_stream_profile, true, artifacts);
    MIRA_CHECK(!no_stream.has_value());
    auto with_stream = mapper.encode_request(request, profile, true, artifacts);
    MIRA_CHECK(with_stream.has_value());
    MIRA_CHECK(to_json_string(with_stream.value()).find("\"stream\":true") != std::string::npos);

    // Response decode: text, tool call, finish reasons.
    WireHttpResponse wire_response;
    wire_response.status = 200;
    wire_response.body = R"({
        "id": "chat_1",
        "model": "test-model-2026",
        "choices": [{
            "message": {"role": "assistant", "content": "{\"action\":\"home\",\"reason\":\"r\"}"},
            "finish_reason": "stop"
        }],
        "usage": {"prompt_tokens": 9, "completion_tokens": 3,
                  "prompt_tokens_details": {"cached_tokens": 1},
                  "completion_tokens_details": {"reasoning_tokens": 1}}
    })";
    auto decoded = mapper.decode_response(request, profile, wire_response);
    MIRA_CHECK(decoded.has_value());
    MIRA_CHECK(decoded.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(decoded.value().usage.input_tokens == 9);
    MIRA_CHECK(decoded.value().usage.output_tokens == 3);
    MIRA_CHECK(decoded.value().usage.cached_input_tokens == 1);
    MIRA_CHECK(decoded.value().usage.reasoning_tokens == 1);

    auto tool_request = request;
    tool_request.tools.push_back(
        ExposedToolSpec{ToolId::generate(), SemanticVersion{1, 0, 0}, "lookup", "d",
                        JsonSchema{parse_json(R"({"type":"object"})").value()}, Hash{}, false});
    wire_response.body = R"({
        "id": "chat_2", "model": "m",
        "choices": [{
            "message": {"role": "assistant", "content": null, "tool_calls": [
                {"id": "call_1", "type": "function",
                 "function": {"name": "lookup", "arguments": "{\"q\": 2}"}}
            ]},
            "finish_reason": "tool_calls"
        }],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1}
    })";
    auto tools_decoded = mapper.decode_response(tool_request, profile, wire_response);
    MIRA_CHECK(tools_decoded.has_value());
    MIRA_CHECK(tools_decoded.value().output.size() == 1);
    const auto *call = std::get_if<ToolCallOutput>(&tools_decoded.value().output[0]);
    MIRA_CHECK(call != nullptr);
    MIRA_CHECK(call->tool_id == tool_request.tools[0].tool_id);
    MIRA_CHECK(call->arguments_digest == digest_string("{\"q\":2}"));

    wire_response.body = R"({
        "id": "chat_3", "model": "m",
        "choices": [{"message": {"role": "assistant", "content": "x"}, "finish_reason": "length"}],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1}
    })";
    auto length = mapper.decode_response(request, profile, wire_response);
    MIRA_CHECK(length.value().status == ModelCompletionStatus::Incomplete);
    MIRA_CHECK(length.value().incomplete_reason == IncompleteReason::MaxOutputTokens);

    wire_response.body = R"({
        "id": "chat_4", "model": "m",
        "choices": [{"message": {"role": "assistant", "content": "x",
                                 "refusal": "cannot help"},
                     "finish_reason": "stop"}],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1}
    })";
    auto refusal = mapper.decode_response(request, profile, wire_response);
    MIRA_CHECK(refusal.value().status == ModelCompletionStatus::Refused);

    // Unknown finish reason and multi-choice bodies fail closed.
    wire_response.body =
        R"({"id":"c","model":"m","choices":[{"message":{"role":"assistant","content":"x"},"finish_reason":"vibes"}],"usage":{}})";
    MIRA_CHECK(!mapper.decode_response(request, profile, wire_response).has_value());
    wire_response.body = R"({"id":"c","model":"m","choices":[],"usage":{}})";
    MIRA_CHECK(!mapper.decode_response(request, profile, wire_response).has_value());
    return 0;
}

int image_transport_uses_artifact_bytes() {
    ResponsesV1Mapper mapper;
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    auto request = base_request();
    ImagePart image;
    image.source.id = ArtifactId::generate();
    image.source.digest = digest_string("pixels");
    image.source.byte_size = 4;
    image.source.media_type = "image/png";
    image.media_type = "image/png";
    request.input[1].content.emplace_back(std::move(image));

    std::size_t fetches = 0;
    auto source = image_source(fetches);
    auto wire = mapper.encode_request(request, profile, false, *source);
    MIRA_CHECK(wire.has_value());
    MIRA_CHECK(fetches == 1);
    const auto text = to_json_string(wire.value());
    MIRA_CHECK(text.find("\"type\":\"input_image\"") != std::string::npos);
    // Payload bytes travel as inline data, base64-encoded.
    MIRA_CHECK(text.find("data:image/png;base64,AQIDBA==") != std::string::npos);

    // A profile without image input refuses before any fetch.
    auto no_image_profile = profile;
    no_image_profile.capabilities.image_input =
        CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    fetches = 0;
    auto rejected = mapper.encode_request(request, no_image_profile, false, *source);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(fetches == 0);
    return 0;
}

int reasoning_maps_to_bounded_thinking() {
    // Issue #83 / MIRA-20261008-001: Chat Completions and Responses dialects
    // map provider reasoning output to the same bounded ThinkingPart
    // semantics as the Messages dialect instead of dropping it or degrading
    // it to a digest-only UnknownOutput.
    const auto chat_profile =
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    const auto responses_profile =
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    const auto request = base_request();
    constexpr std::size_t kThinkingBudget = 4ULL * 1024ULL * 1024ULL;

    // (a) Chat non-stream: reasoning_content + content.
    {
        ChatCompletionsV1Mapper mapper;
        WireHttpResponse wire;
        wire.status = 200;
        wire.body = R"({
            "id": "chat_r1", "model": "m",
            "choices": [{"message": {"role": "assistant", "content": "the answer",
                                     "reasoning_content": "chain of thought"},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1}
        })";
        auto decoded = mapper.decode_response(request, chat_profile, wire);
        MIRA_CHECK(decoded.has_value());
        MIRA_CHECK(decoded.value().status == ModelCompletionStatus::Completed);
        MIRA_CHECK(decoded.value().output.size() == 2);
        const auto *thinking = std::get_if<ThinkingPart>(&decoded.value().output[0]);
        MIRA_CHECK(thinking != nullptr);
        MIRA_CHECK(thinking->text == "chain of thought");
        MIRA_CHECK(thinking->signature.empty());
        MIRA_CHECK(!thinking->redacted);
        MIRA_CHECK(std::get_if<MessageOutput>(&decoded.value().output[1]) != nullptr);

        // Reasoning ahead of tool calls keeps the thinking-first order.
        auto tool_request = base_request();
        tool_request.tools.push_back(
            ExposedToolSpec{ToolId::generate(), SemanticVersion{1, 0, 0}, "lookup", "d",
                            JsonSchema{parse_json(R"({"type":"object"})").value()}, Hash{}, false});
        wire.body = R"({
            "id": "chat_r2", "model": "m",
            "choices": [{"message": {"role": "assistant", "content": null,
                                     "reasoning_content": "why",
                                     "tool_calls": [{"id": "call_1", "type": "function",
                                                     "function": {"name": "lookup",
                                                                  "arguments": "{\"q\":1}"}}]},
                         "finish_reason": "tool_calls"}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1}
        })";
        auto with_calls = mapper.decode_response(tool_request, chat_profile, wire);
        MIRA_CHECK(with_calls.has_value());
        MIRA_CHECK(with_calls.value().output.size() == 2);
        MIRA_CHECK(std::get_if<ThinkingPart>(&with_calls.value().output[0]) != nullptr);
        const auto *call = std::get_if<ToolCallOutput>(&with_calls.value().output[1]);
        MIRA_CHECK(call != nullptr && call->provider_call_id.value == "call_1");

        // Only reasoning_content, no content: exactly one ThinkingPart and no
        // UnknownOutput downgrade; the completion still finishes.
        wire.body = R"({
            "id": "chat_r3", "model": "m",
            "choices": [{"message": {"role": "assistant", "content": null,
                                     "reasoning_content": "only think"},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1}
        })";
        auto reasoning_only = mapper.decode_response(request, chat_profile, wire);
        MIRA_CHECK(reasoning_only.has_value());
        MIRA_CHECK(reasoning_only.value().status == ModelCompletionStatus::Completed);
        MIRA_CHECK(reasoning_only.value().output.size() == 1);
        MIRA_CHECK(std::get_if<ThinkingPart>(&reasoning_only.value().output[0]) != nullptr);
        MIRA_CHECK(std::get_if<UnknownOutput>(&reasoning_only.value().output[0]) == nullptr);

        // Empty or missing reasoning_content stays absent; non-string values
        // are treated as absent too (pre-change behavior is preserved).
        wire.body = R"({
            "id": "chat_r4", "model": "m",
            "choices": [{"message": {"role": "assistant", "content": "x",
                                     "reasoning_content": ""},
                         "finish_reason": "stop"}],
            "usage": {}
        })";
        auto empty_reasoning = mapper.decode_response(request, chat_profile, wire);
        MIRA_CHECK(empty_reasoning.has_value());
        MIRA_CHECK(empty_reasoning.value().output.size() == 1);
        MIRA_CHECK(std::get_if<MessageOutput>(&empty_reasoning.value().output[0]) != nullptr);
        wire.body = R"({
            "id": "chat_r5", "model": "m",
            "choices": [{"message": {"role": "assistant", "content": "x",
                                     "reasoning_content": 42},
                         "finish_reason": "stop"}],
            "usage": {}
        })";
        auto non_string_reasoning = mapper.decode_response(request, chat_profile, wire);
        MIRA_CHECK(non_string_reasoning.has_value());
        MIRA_CHECK(non_string_reasoning.value().output.size() == 1);

        // Exactly-at-budget decoding cannot be observed through the wire:
        // JsonLimits::max_string_bytes (1 MiB) already rejects any reasoning
        // string above 1 MiB during body parsing, so the 4 MiB thinking
        // budget check is shadowed on every public decode path. Oversized
        // reasoning still fails closed with a ProtocolViolation either way.
        wire.body = "{\"id\":\"chat_r7\",\"model\":\"m\",\"choices\":[{\"message\":{\"role\":"
                    "\"assistant\",\"content\":\"a\",\"reasoning_content\":\"" +
                    std::string(kThinkingBudget + 1, 'x') +
                    "\"},\"finish_reason\":\"stop\"}],\"usage\":{}}";
        auto over_budget = mapper.decode_response(request, chat_profile, wire);
        MIRA_CHECK(!over_budget.has_value());
        MIRA_CHECK(over_budget.error().domain == "mira.model");
        MIRA_CHECK(over_budget.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
    }

    // (b) Responses terminal bodies: reasoning items surface as raw thinking
    // when the provider exposes reasoning_text, as redacted summary thinking
    // otherwise, and keep the digest fallback only for undisplayable items.
    {
        ResponsesV1Mapper mapper;
        WireHttpResponse wire;
        wire.status = 200;

        const auto decode_output = [&](const std::string &output_json) {
            wire.body = "{\"id\":\"resp_r\",\"status\":\"completed\",\"model\":\"m\",\"output\":" +
                        output_json + ",\"usage\":{}}";
            return mapper.decode_response(request, responses_profile, wire);
        };

        // Raw reasoning text under content[].
        auto raw = decode_output(
            R"([{"type":"reasoning","id":"rs_1","content":[{"type":"reasoning_text","text":"raw thought"}]}])");
        MIRA_CHECK(raw.has_value());
        MIRA_CHECK(raw.value().output.size() == 1);
        const auto *raw_thinking = std::get_if<ThinkingPart>(&raw.value().output[0]);
        MIRA_CHECK(raw_thinking != nullptr);
        MIRA_CHECK(raw_thinking->text == "raw thought");
        MIRA_CHECK(raw_thinking->signature.empty());
        MIRA_CHECK(!raw_thinking->redacted);

        // Summary-only items keep the redacted summary semantics.
        auto summary = decode_output(
            R"([{"type":"reasoning","id":"rs_2","summary":[{"type":"summary_text","text":"sum"}]}])");
        MIRA_CHECK(summary.has_value());
        MIRA_CHECK(summary.value().output.size() == 1);
        const auto *summary_thinking = std::get_if<ThinkingPart>(&summary.value().output[0]);
        MIRA_CHECK(summary_thinking != nullptr);
        MIRA_CHECK(summary_thinking->text == "sum");
        MIRA_CHECK(summary_thinking->redacted);

        // Multiple displayable parts each become one ThinkingPart, in order.
        auto two_parts = decode_output(
            R"([{"type":"reasoning","id":"rs_3","content":[{"type":"reasoning_text","text":"p1"},{"type":"reasoning_text","text":"p2"}]}])");
        MIRA_CHECK(two_parts.has_value());
        MIRA_CHECK(two_parts.value().output.size() == 2);
        const auto *first_part = std::get_if<ThinkingPart>(&two_parts.value().output[0]);
        const auto *second_part = std::get_if<ThinkingPart>(&two_parts.value().output[1]);
        MIRA_CHECK(first_part != nullptr && first_part->text == "p1" && !first_part->redacted);
        MIRA_CHECK(second_part != nullptr && second_part->text == "p2" && !second_part->redacted);

        // Raw content wins over the summary when both are present.
        auto both = decode_output(
            R"([{"type":"reasoning","id":"rs_4","content":[{"type":"reasoning_text","text":"raw"}],"summary":[{"type":"summary_text","text":"sum"}]}])");
        MIRA_CHECK(both.has_value());
        MIRA_CHECK(both.value().output.size() == 1);
        const auto *both_thinking = std::get_if<ThinkingPart>(&both.value().output[0]);
        MIRA_CHECK(both_thinking != nullptr && both_thinking->text == "raw" &&
                   !both_thinking->redacted);

        // Items carrying nothing displayable keep the UnknownOutput digest
        // fallback (the pre-change contract for empty reasoning items).
        auto empty_item = decode_output(R"([{"type":"reasoning","id":"rs_5"}])");
        MIRA_CHECK(empty_item.has_value());
        MIRA_CHECK(empty_item.value().output.size() == 1);
        const auto *unknown = std::get_if<UnknownOutput>(&empty_item.value().output[0]);
        MIRA_CHECK(unknown != nullptr);
        MIRA_CHECK(unknown->provider_type == "responses.item.reasoning");
        auto empty_arrays =
            decode_output(R"([{"type":"reasoning","id":"rs_6","content":[],"summary":[]}])");
        MIRA_CHECK(empty_arrays.has_value());
        MIRA_CHECK(std::get_if<UnknownOutput>(&empty_arrays.value().output[0]) != nullptr);

        // Malformed text parts fail closed.
        for (
            const auto *malformed :
            {R"([{"type":"reasoning","id":"bad_1","summary":"nope"}])",
             R"([{"type":"reasoning","id":"bad_2","content":[{"text":"no type"}]}])",
             R"([{"type":"reasoning","id":"bad_3","content":[{"type":"reasoning_text"}]}])",
             R"([{"type":"reasoning","id":"bad_4","content":[{"type":"reasoning_text","text":5}]}])",
             R"([{"type":"reasoning","id":"bad_5","content":["not an object"]}])"}) {
            auto bad = decode_output(malformed);
            MIRA_CHECK(!bad.has_value());
            MIRA_CHECK(bad.error().domain_code ==
                       static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
            MIRA_CHECK(bad.error().safe_message.find("malformed text parts") != std::string::npos);
        }

        // Over-budget reasoning text fails closed (shadowed by the JSON
        // string limit on public paths, exactly like the Chat side above).
        wire.body = "{\"id\":\"resp_big\",\"status\":\"completed\",\"model\":\"m\",\"output\":"
                    "[{\"type\":\"reasoning\",\"id\":\"rs_big\",\"content\":[{\"type\":"
                    "\"reasoning_text\",\"text\":\"" +
                    std::string(kThinkingBudget + 1, 'x') + "\"}]}],\"usage\":{}}";
        auto big = mapper.decode_response(request, responses_profile, wire);
        MIRA_CHECK(!big.has_value());
        MIRA_CHECK(big.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));

        // Reasoning items keep their relative position among other output.
        auto ordered = decode_output(
            R"([{"type":"message","role":"assistant","content":[{"type":"output_text","text":"hi"}]},{"type":"reasoning","id":"rs_7","content":[{"type":"reasoning_text","text":"think"}]},{"type":"function_call","call_id":"call_1","name":"lookup","arguments":"{\"q\":1}"}])");
        MIRA_CHECK(ordered.has_value());
        MIRA_CHECK(ordered.value().output.size() == 3);
        MIRA_CHECK(std::get_if<MessageOutput>(&ordered.value().output[0]) != nullptr);
        MIRA_CHECK(std::get_if<ThinkingPart>(&ordered.value().output[1]) != nullptr);
        MIRA_CHECK(std::get_if<ToolCallOutput>(&ordered.value().output[2]) != nullptr);
    }

    // (c) Chat/Responses encoders still reject ThinkingPart replay input;
    // only the Messages mapper accepts it (DEC-051).
    {
        auto replay = base_request();
        ModelInputItem assistant_item;
        assistant_item.role = ModelRole::Assistant;
        assistant_item.content.emplace_back(ThinkingPart{"thought", "sig", false});
        replay.input.push_back(std::move(assistant_item));
        NullArtifactSource artifacts;
        MIRA_CHECK(
            !ChatCompletionsV1Mapper{}.encode_request(replay, chat_profile, false, artifacts));
        MIRA_CHECK(
            !ResponsesV1Mapper{}.encode_request(replay, responses_profile, false, artifacts));
        MIRA_CHECK(dialect_accepts_thinking_replay(ProtocolDialect::AnthropicMessagesV1));
        MIRA_CHECK(!dialect_accepts_thinking_replay(ProtocolDialect::OpenAIChatCompletionsV1));
        MIRA_CHECK(!dialect_accepts_thinking_replay(ProtocolDialect::OpenAIResponsesV1));
    }
    return 0;
}

int non_image_media_type_fails_closed() {
    // Raw unencoded payloads (e.g. application/octet-stream frames) must
    // fail with a diagnosable CapabilityMismatch before the wire instead of
    // an opaque endpoint rejection (DEC-013).
    ImagePart image;
    image.source.id = ArtifactId::generate();
    image.source.digest = digest_string("pixels");
    image.source.byte_size = 4;
    image.source.media_type = "application/octet-stream";
    image.media_type = image.source.media_type;

    {
        ResponsesV1Mapper mapper;
        const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
        auto request = base_request();
        request.input[1].content.emplace_back(image);
        std::size_t fetches = 0;
        auto source = image_source(fetches);
        const auto wire = mapper.encode_request(request, profile, false, *source);
        MIRA_CHECK(!wire.has_value());
        MIRA_CHECK(wire.error().code == ErrorCode::UnsupportedCapability);
        // Refused before any payload fetch.
        MIRA_CHECK(fetches == 0);
    }
    {
        ChatCompletionsV1Mapper mapper;
        const auto profile =
            make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
        auto request = base_request();
        request.input[1].content.emplace_back(image);
        std::size_t fetches = 0;
        auto source = image_source(fetches);
        const auto wire = mapper.encode_request(request, profile, false, *source);
        MIRA_CHECK(!wire.has_value());
        MIRA_CHECK(wire.error().code == ErrorCode::UnsupportedCapability);
        MIRA_CHECK(fetches == 0);
    }
    return 0;
}

} // namespace

int main() {
    if (const int status = responses_request_golden(); status != 0) {
        return status;
    }
    if (const int status = responses_response_golden(); status != 0) {
        return status;
    }
    if (const int status = error_status_mapping_and_retry_after(); status != 0) {
        return status;
    }
    if (const int status = chat_completions_request_and_response(); status != 0) {
        return status;
    }
    if (const int status = reasoning_maps_to_bounded_thinking(); status != 0) {
        return status;
    }
    if (const int status = image_transport_uses_artifact_bytes(); status != 0) {
        return status;
    }
    if (const int status = non_image_media_type_fails_closed(); status != 0) {
        return status;
    }
    return 0;
}
