#include <mira/model_sse.hpp>

#include <utility>

namespace mira {
namespace {
Error invalid(std::string message) {
    return make_model_error(ModelDomainCode::ProtocolViolation, std::move(message));
}
bool append(std::string &value, const JsonValue *part, std::size_t limit) {
    if (!part || part->is_null())
        return true;
    const auto text = part->as_string();
    if (!text || value.size() > limit || text->size() > limit - value.size())
        return false;
    value += *text;
    return true;
}
} // namespace

ChatCompletionsSseParser::ChatCompletionsSseParser(const ModelRequest &request,
                                                   const ModelProfile &profile,
                                                   SseStreamLimits limits)
    : request_(request), profile_(profile), limits_(limits), framer_(limits.framing) {}

Result<void> ChatCompletionsSseParser::feed(std::string_view chunk) {
    auto messages = framer_.feed(chunk);
    if (!messages)
        return messages.error();
    stats_.bytes = framer_.bytes_fed();
    for (const auto &message : messages.value()) {
        auto reduced = reduce(message);
        if (!reduced)
            return reduced.error();
    }
    return {};
}

Result<void> ChatCompletionsSseParser::reduce(const SseMessage &message) {
    ++stats_.events;
    ++stats_.stream_sequence;
    if (stats_.terminal_seen)
        return invalid("chat SSE event after terminal");
    if (message.data == "[DONE]") {
        if (finish_reason_.empty())
            return invalid("chat SSE terminal before finish reason");
        stats_.terminal_seen = true;
        return {};
    }
    auto parsed = parse_json(message.data);
    if (!parsed || !parsed.value().is_object())
        return invalid("chat SSE chunk is not a JSON object");
    const auto &body = parsed.value();
    if (body.find("error"))
        return invalid("chat SSE provider error");
    for (const auto &[key, target] : {std::pair{"id", &id_}, std::pair{"model", &model_}}) {
        if (const auto *value = body.find(key)) {
            if (!value->is_string() || value->as_string()->empty() ||
                value->as_string()->size() > 1024)
                return invalid("chat SSE invalid response identity");
            if (!target->empty() && *target != *value->as_string())
                return invalid("chat SSE response identity changed");
            *target = *value->as_string();
        }
    }
    if (const auto *usage = body.find("usage"); usage && !usage->is_null()) {
        if (!usage->is_object())
            return invalid("chat SSE malformed usage");
        // Compatible providers may report cumulative counters on every chunk.
        // They remain provisional until the canonical terminal; never sum them.
        for (const auto key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
            if (const auto *count = usage->find(key)) {
                const auto number = count->as_integer();
                if (!number || *number < 0)
                    return invalid("chat SSE invalid usage counter");
                const auto *previous = usage_ ? usage_->find(key) : nullptr;
                if (previous && previous->as_integer() && *number < *previous->as_integer())
                    return invalid("chat SSE usage counter decreased");
            }
        }
        usage_ = *usage;
    }
    const auto *choices = body.find("choices");
    if (!choices || !choices->is_array() || choices->as_array()->size() > 1)
        return invalid("chat SSE requires one choice");
    if (choices->as_array()->empty()) {
        if (finish_reason_.empty() || !usage_)
            return invalid("chat SSE empty choices before usage terminal");
        return {};
    }
    if (!finish_reason_.empty())
        return invalid("chat SSE choice after finish reason");
    const auto &choice = choices->as_array()->front();
    const auto *index = choice.find("index");
    if (!choice.is_object() || !index || index->as_integer() != std::optional<std::int64_t>{0})
        return invalid("chat SSE choice index must be zero");
    const auto *delta = choice.find("delta");
    if (!delta || !delta->is_object())
        return invalid("chat SSE choice has no delta object");
    if (const auto *role = delta->find("role");
        role && (!role->is_string() || *role->as_string() != "assistant"))
        return invalid("chat SSE delta role must be assistant");
    const auto old_size = text_.size();
    if (!append(text_, delta->find("content"), limits_.max_accumulated_text_bytes) ||
        !append(refusal_, delta->find("refusal"), limits_.max_accumulated_text_bytes))
        return invalid("chat SSE text exceeds budget or has invalid type");
    if (text_.size() > old_size) {
        ++stats_.text_deltas;
        const auto part = text_.substr(old_size);
        if (preview_.size() + part.size() <= limits_.max_preview_bytes)
            preview_ += part;
        else {
            ++stats_.preview_drops;
            ++pending_preview_drops_;
        }
    }
    if (const auto *calls = delta->find("tool_calls"); calls && !calls->is_null()) {
        if (!calls->is_array())
            return invalid("chat SSE tool calls must be an array");
        for (const auto &call : *calls->as_array()) {
            const auto *tool_index = call.find("index");
            const auto number = tool_index ? tool_index->as_integer() : std::nullopt;
            if (!call.is_object() || !number || *number < 0 ||
                static_cast<std::uint64_t>(*number) >= limits_.max_open_output_items)
                return invalid("chat SSE tool index exceeds budget");
            auto &tool = tools_[*number];
            if (const auto *id = call.find("id"); id && !id->is_null()) {
                if (!id->is_string() || id->as_string()->empty() ||
                    id->as_string()->size() > 1024 ||
                    (!tool.id.empty() && tool.id != *id->as_string()))
                    return invalid("chat SSE tool identity changed");
                tool.id = *id->as_string();
            }
            if (const auto *type = call.find("type");
                type && !type->is_null() &&
                (!type->is_string() || *type->as_string() != "function"))
                return invalid("chat SSE unsupported tool type");
            if (const auto *function = call.find("function")) {
                if (!function->is_object() || !append(tool.name, function->find("name"), 1024))
                    return invalid("chat SSE invalid tool name");
                const auto before = tool.arguments.size();
                if (!append(tool.arguments, function->find("arguments"),
                            before + limits_.max_arguments_buffer_bytes - argument_bytes_))
                    return invalid("chat SSE tool arguments exceed budget");
                argument_bytes_ += tool.arguments.size() - before;
            }
        }
    }
    if (const auto *finish = choice.find("finish_reason"); finish && !finish->is_null()) {
        if (!finish->is_string())
            return invalid("chat SSE invalid finish reason");
        finish_reason_ = *finish->as_string();
        if (finish_reason_ != "stop" && finish_reason_ != "tool_calls" &&
            finish_reason_ != "length" && finish_reason_ != "content_filter")
            return invalid("chat SSE unknown finish reason");
    }
    return {};
}

Result<ModelResponse> ChatCompletionsSseParser::finish() {
    auto tail = framer_.finish();
    if (!tail)
        return tail.error();
    for (const auto &message : tail.value()) {
        auto reduced = reduce(message);
        if (!reduced)
            return reduced.error();
    }
    if (!stats_.terminal_seen)
        return make_model_error(ModelDomainCode::AmbiguousCompletion,
                                "chat SSE stream ended without [DONE]");
    JsonValue::Object message{{"role", "assistant"}, {"content", text_}};
    if (!refusal_.empty())
        message.emplace_back("refusal", refusal_);
    JsonValue::Array calls;
    for (const auto &[index, tool] : tools_) {
        (void)index;
        if (tool.id.empty() || tool.name.empty())
            return invalid("chat SSE incomplete tool identity");
        calls.emplace_back(JsonValue::Object{
            {"id", tool.id},
            {"type", "function"},
            {"function", JsonValue::Object{{"name", tool.name}, {"arguments", tool.arguments}}}});
    }
    if (!calls.empty())
        message.emplace_back("tool_calls", std::move(calls));
    JsonValue::Object body{
        {"id", id_},
        {"model", model_},
        {"choices",
         JsonValue::Array{JsonValue::Object{
             {"index", 0}, {"message", std::move(message)}, {"finish_reason", finish_reason_}}}}};
    if (usage_)
        body.emplace_back("usage", *usage_);
    return ChatCompletionsV1Mapper{}.decode_response(
        request_, profile_, WireHttpResponse{200, {}, to_json_string(JsonValue{std::move(body)})});
}

UnvalidatedModelPreview ChatCompletionsSseParser::take_preview() {
    const auto drops = std::exchange(pending_preview_drops_, 0);
    return {std::exchange(preview_, {}), drops, drops != 0};
}
} // namespace mira
