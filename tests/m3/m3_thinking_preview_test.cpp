#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <kairo/executor.hpp>
#include <mira/model_sse.hpp>
#include <stdexcept>
#include <string>

namespace {
using namespace mira;
using namespace mira::testing;
ModelRequest request_for(const ModelProfile &profile) {
    ModelRequest request;
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = profile.id;
    ModelInputItem input;
    input.role = ModelRole::User;
    input.content.emplace_back(TextPart{"hello"});
    request.input.push_back(std::move(input));
    request.output_contract.mode = OutputMode::Text;
    return request;
}
std::string chat(const std::string &thinking, const std::string &text = "",
                 const char *finish = "null") {
    JsonValue::Object delta;
    if (!thinking.empty())
        delta.emplace_back("reasoning_content", thinking);
    if (!text.empty())
        delta.emplace_back("content", text);
    return "data: " +
           to_json_string(JsonValue{JsonValue::Object{
               {"id", "chat-thinking"},
               {"model", "m"},
               {"choices",
                JsonValue::Array{JsonValue::Object{
                    {"index", 0},
                    {"delta", std::move(delta)},
                    {"finish_reason",
                     std::string(finish) == "null" ? JsonValue{} : JsonValue{finish}}}}}}}) +
           "\n\n";
}
std::string event(const char *type, const char *data) {
    return std::string("event: ") + type + "\ndata: " + data + "\n\n";
}
int bounded_pending_fragments() {
    const auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://test");
    const auto request = request_for(profile);
    for (const auto limit : {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{7}}) {
        SseStreamLimits limits;
        limits.max_preview_bytes = limit;
        ChatCompletionsSseParser parser(request, profile, limits);
        MIRA_CHECK(parser.feed(chat("前文")));
        MIRA_CHECK(parser.feed(chat("新🙂")));
        const auto preview = parser.take_thinking_preview();
        MIRA_CHECK(preview.text.size() <= limit);
        MIRA_CHECK(preview.truncated && preview.dropped_updates > 0);
        MIRA_CHECK(limit != 4 || preview.text == "🙂");
        MIRA_CHECK(limit != 7 || preview.text == "新🙂");
        MIRA_CHECK(limit > 1 || preview.text.empty());
        MIRA_CHECK(parser.take_thinking_preview().text.empty());
        MIRA_CHECK(parser.take_preview().text.empty());
        MIRA_CHECK(parser.feed(chat("", "", "stop") + "data: [DONE]\n\n"));
        auto result = parser.finish();
        MIRA_CHECK(result);
        MIRA_CHECK(std::get<ThinkingPart>(result.value().output.front()).text == "前文新🙂");
    }
    return 0;
}
int provider_delivery_is_separate_and_optional() {
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://test"));
    const auto request = request_for(*profile);
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    MockStep step;
    step.chunk_pieces = {chat("正在"), chat("检查", "answer"),
                         chat("", "", "stop") + "data: [DONE]\n\n"};
    OpenAiCompatibleProvider provider(profile, transport, nullptr);
    ProviderInferOptions options;
    options.stream = true;
    std::vector<std::string> body, thinking;
    bool finished = false;
    options.preview_sink = [&](const auto &, const auto &value) { body.push_back(value.text); };
    options.thinking_preview_sink = [&](const auto &id, const auto &value) {
        if (id != request.request_id || finished)
            throw std::runtime_error("preview identity/timing mismatch");
        thinking.push_back(value.text);
    };
    transport->enqueue(step);
    auto response = provider.infer(request, OperationContext{}, options);
    finished = true;
    MIRA_CHECK(response);
    MIRA_CHECK((body == std::vector<std::string>{"", "answer"}));
    MIRA_CHECK((thinking == std::vector<std::string>{"", "正在", "正在检查"}));
    MIRA_CHECK(provider.last_sse_stats().text_deltas == 1);
    // A second attempt starts with an empty thinking snapshot, even with the
    // same request id. No stale text survives retry/reuse.
    finished = false;
    thinking.clear();
    transport->enqueue(step);
    MIRA_CHECK(provider.infer(request, OperationContext{}, options));
    MIRA_CHECK(thinking.front().empty() && thinking.back() == "正在检查");
    // Old callers receive exactly their original answer-only callbacks.
    options.thinking_preview_sink = {};
    body.clear();
    transport->enqueue(step);
    MIRA_CHECK(provider.infer(request, OperationContext{}, options));
    MIRA_CHECK((body == std::vector<std::string>{"", "answer"}));
    return 0;
}
int cancellation_failure_and_snapshot_budget() {
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://test"));
    const auto request = request_for(*profile);
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    OpenAiCompatibleProvider provider(profile, transport, nullptr);
    ProviderInferOptions options;
    options.stream = true;
    MockStep step;
    step.chunk_pieces = {chat(std::string(9000, 'a')), chat(std::string(9000, 'b')), chat("最新🙂"),
                         chat("", "ok", "stop") + "data: [DONE]\n\n"};
    UnvalidatedModelThinkingPreview last;
    options.thinking_preview_sink = [&](const auto &, const auto &value) { last = value; };
    transport->enqueue(step);
    MIRA_CHECK(provider.infer(request, OperationContext{}, options));
    MIRA_CHECK(last.text.size() <= 16384 && last.text.ends_with("最新🙂"));
    MIRA_CHECK(last.truncated && last.dropped_updates > 0);
    MIRA_CHECK(provider.last_sse_stats().thinking_preview_drops == last.dropped_updates);
    options.thinking_preview_sink = [](const auto &, const auto &) {
        throw std::runtime_error("consumer refused");
    };
    transport->enqueue(step);
    MIRA_CHECK(provider.infer(request, OperationContext{}, options));
    MIRA_CHECK(provider.last_sse_stats().thinking_preview_drops >= 3);
    bool cancelled = false;
    std::size_t callbacks = 0;
    OperationContext context;
    context.cancellation_requested = [&] { return cancelled; };
    options.thinking_preview_sink = [&](const auto &, const auto &value) {
        ++callbacks;
        if (!value.text.empty())
            cancelled = true;
    };
    transport->enqueue(step);
    (void)provider.infer(request, context, options);
    MIRA_CHECK(callbacks == 2); // initial clear + first thought; nothing after cancel
    callbacks = 0;
    transport->enqueue(step);
    (void)provider.infer(request, context, options);
    MIRA_CHECK(callbacks == 0); // already cancelled: even clear is suppressed
    // A broken stream stays a failure despite receiving a provisional thought.
    options.thinking_preview_sink = [&](const auto &, const auto &) { ++callbacks; };
    MockStep broken;
    broken.chunk_pieces = {chat("still provisional")};
    transport->enqueue(broken);
    MIRA_CHECK(!provider.infer(request, OperationContext{}, options));
    MIRA_CHECK(callbacks > 0);
    return 0;
}
int gateway_forwards_the_optional_sink() {
    struct Owner {
        kairo::Executor executor;
        ~Owner() { (void)executor.shutdown(true); }
    } owner;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    config.queue_capacity = 8;
    MIRA_CHECK(owner.executor.initialize(config));
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(owner.executor, router, nullptr, PriceTable{});
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    gateway.register_provider(
        std::make_shared<OpenAiCompatibleProvider>(profile, transport, nullptr));
    auto request = request_for(*profile);
    request.input.front().role = ModelRole::System;
    request.budget.max_input_tokens = 100000;
    request.budget.max_output_tokens = 2000;
    request.budget.max_requests = 2;
    MockStep step;
    step.chunk_pieces = {chat("live gateway thought"),
                         chat("", "answer", "stop") + "data: [DONE]\n\n"};
    transport->enqueue(step);
    InferOptions options;
    options.stream = true;
    std::string observed;
    options.thinking_preview_sink = [&](const auto &, const auto &value) { observed = value.text; };
    const auto result = gateway.infer(request, OperationContext{}, options);
    if (!result)
        std::cerr << result.error().safe_message << "\n";
    MIRA_CHECK(result);
    MIRA_CHECK(observed == "live gateway thought");
    return 0;
}
int redacted_and_signatures_never_enter_the_channel() {
    const auto profile = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://test");
    const auto request = request_for(profile);
    const auto stream =
        event(
            "message_start",
            R"({"type":"message_start","message":{"id":"m","type":"message","role":"assistant","model":"m","content":[],"usage":{"input_tokens":1,"output_tokens":0}}})") +
        event(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"redacted_thinking","data":"never-display"}})") +
        event("content_block_stop", R"({"type":"content_block_stop","index":0})") +
        event(
            "content_block_start",
            R"({"type":"content_block_start","index":1,"content_block":{"type":"thinking","thinking":""}})") +
        event(
            "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"thinking_delta","thinking":"公开🙂"}})") +
        event(
            "content_block_delta",
            R"({"type":"content_block_delta","index":1,"delta":{"type":"signature_delta","signature":"never-signature"}})") +
        event("content_block_stop", R"({"type":"content_block_stop","index":1})") +
        event(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}})") +
        event("message_stop", R"({"type":"message_stop"})");
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        AnthropicMessagesSseParser parser(request, profile);
        MIRA_CHECK(parser.feed(std::string_view(stream).substr(0, split)));
        const auto first = parser.take_thinking_preview();
        MIRA_CHECK(parser.feed(std::string_view(stream).substr(split)));
        const auto second = parser.take_thinking_preview();
        MIRA_CHECK(first.text + second.text == "公开🙂");
        MIRA_CHECK(parser.take_preview().text.empty());
        MIRA_CHECK(parser.finish());
    }
    return 0;
}
} // namespace
int main() {
    if (const auto result = bounded_pending_fragments())
        return result;
    if (const auto result = provider_delivery_is_separate_and_optional())
        return result;
    if (const auto result = cancellation_failure_and_snapshot_budget())
        return result;
    if (const auto result = gateway_forwards_the_optional_sink())
        return result;
    return redacted_and_signatures_never_enter_the_channel();
}
