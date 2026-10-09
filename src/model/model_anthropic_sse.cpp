#include <mira/model_sse.hpp>

#include <algorithm>
#include <utility>

namespace mira {
namespace {
Error invalid() {
    return make_model_error(ModelDomainCode::ProtocolViolation,
                            "invalid Messages SSE lifecycle or budget");
}
const std::string *string(const JsonValue &object, const char *key) {
    const auto *value = object.find(key);
    return value ? value->as_string() : nullptr;
}
void set(JsonValue &object, const char *key, JsonValue value) { object.set(key, std::move(value)); }
} // namespace
AnthropicMessagesSseParser::AnthropicMessagesSseParser(const ModelRequest &request,
                                                       const ModelProfile &profile,
                                                       SseStreamLimits limits)
    : request_(request), profile_(profile), limits_(limits), framer_(limits.framing) {}
Result<void> AnthropicMessagesSseParser::feed(std::string_view chunk) {
    auto events = framer_.feed(chunk);
    if (!events)
        return events.error();
    stats_.bytes = framer_.bytes_fed();
    for (const auto &event : events.value()) {
        auto status = reduce(event);
        if (!status)
            return status.error();
    }
    return {};
}
Result<void> AnthropicMessagesSseParser::reduce(const SseMessage &event) {
    ++stats_.events;
    ++stats_.stream_sequence;
    if (stats_.terminal_seen)
        return invalid();
    auto parsed = parse_json(event.data);
    if (!parsed || !parsed.value().is_object())
        return invalid();
    const auto &body = parsed.value();
    const auto *type = string(body, "type");
    if (!type || (!event.event.empty() && event.event != *type))
        return invalid();
    if (*type == "ping")
        return {};
    if (*type == "error")
        return invalid();
    if (*type == "message_start") {
        const auto *message = body.find("message");
        if (started_ || !message || !message->is_object())
            return invalid();
        const auto *content = message->find("content");
        if (!content || !content->is_array() || !content->as_array()->empty())
            return invalid();
        message_ = *message;
        started_ = true;
        return {};
    }
    if (!started_)
        return invalid();
    if (*type == "message_delta") {
        if (delta_seen_ ||
            std::any_of(blocks_.begin(), blocks_.end(), [](const auto &b) { return !b.closed; }))
            return invalid();
        const auto *delta = body.find("delta"), *usage = body.find("usage");
        if (!delta || !delta->is_object() || !string(*delta, "stop_reason") || !usage ||
            !usage->is_object())
            return invalid();
        set(message_, "stop_reason", *delta->find("stop_reason"));
        auto previous =
            message_.find("usage") ? *message_.find("usage") : JsonValue{JsonValue::Object{}};
        if (!previous.is_object())
            return invalid();
        for (const auto &[key, count] : *usage->as_object()) {
            if (key != "input_tokens" && key != "output_tokens" &&
                key != "cache_read_input_tokens" && key != "cache_creation_input_tokens")
                continue;
            if (!count.as_integer() || *count.as_integer() < 0)
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "Messages SSE invalid usage counter");
            if (const auto *old = previous.find(key);
                old && old->as_integer() && *count.as_integer() < *old->as_integer())
                return invalid();
            set(previous, key.c_str(), count);
        }
        set(message_, "usage", std::move(previous));
        delta_seen_ = true;
        return {};
    }
    if (*type == "message_stop") {
        if (!delta_seen_)
            return invalid();
        stats_.terminal_seen = true;
        return {};
    }
    if (delta_seen_)
        return invalid();
    const auto *index = body.find("index");
    const auto number = index ? index->as_integer() : std::nullopt;
    if (!number || *number < 0 ||
        static_cast<std::uint64_t>(*number) >= limits_.max_open_output_items)
        return invalid();
    const auto position = static_cast<std::size_t>(*number);
    if (*type == "content_block_start") {
        const auto *block = body.find("content_block");
        if (position != blocks_.size() || !block || !block->is_object())
            return invalid();
        const auto *kind = string(*block, "type");
        if (!kind || (*kind != "text" && *kind != "tool_use" && *kind != "thinking" &&
                      *kind != "redacted_thinking"))
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "unsupported Messages content block");
        Block entry{*block, {}, {}, false};
        if (*kind == "text") {
            const auto *initial = string(*block, "text");
            if (!initial || !initial->empty())
                return invalid();
        } else if (*kind == "thinking") {
            const auto *initial = string(*block, "thinking"),
                       *signature = string(*block, "signature");
            if (!initial || !initial->empty() || (signature && !signature->empty()))
                return invalid();
        } else if (*kind == "redacted_thinking") {
            const auto *data = string(*block, "data");
            if (!data || data->size() > limits_.max_accumulated_text_bytes - text_bytes_)
                return invalid();
            text_bytes_ += data->size();
        } else {
            const auto *input = block->find("input");
            if (!input || !input->is_object() || !input->as_object()->empty())
                return invalid();
        }
        blocks_.push_back(std::move(entry));
        return {};
    }
    // A provider may close a block it never opened when the output budget
    // ends between block scheduling and the first start event (observed on
    // MiniMax max_tokens cutoffs): synthesize closed empty text blocks for
    // the missing positions. They decode to no content. Deltas for
    // never-started blocks and duplicate stops stay invalid.
    if (*type == "content_block_stop" && position >= blocks_.size()) {
        while (blocks_.size() <= position)
            blocks_.push_back(
                Block{JsonValue::Object{{"type", "text"}, {"text", ""}}, {}, {}, true});
        return {};
    }
    if (position >= blocks_.size() || blocks_[position].closed)
        return invalid();
    auto &block = blocks_[position];
    const auto &kind = *string(block.value, "type");
    if (*type == "content_block_delta") {
        const auto *delta = body.find("delta");
        const auto *delta_type = delta ? string(*delta, "type") : nullptr;
        if (!delta_type)
            return invalid();
        if (kind == "text" && *delta_type == "text_delta") {
            const auto *part = string(*delta, "text");
            if (!part || part->size() > limits_.max_accumulated_text_bytes - text_bytes_)
                return invalid();
            block.text += *part;
            text_bytes_ += part->size();
            ++stats_.text_deltas;
            if (part->size() <= limits_.max_preview_bytes - preview_.size())
                preview_ += *part;
            else {
                ++pending_drops_;
                ++stats_.preview_drops;
            }
        } else if (kind == "thinking" &&
                   (*delta_type == "thinking_delta" || *delta_type == "signature_delta")) {
            const auto *part =
                string(*delta, *delta_type == "thinking_delta" ? "thinking" : "signature");
            if (!part || part->size() > limits_.max_accumulated_text_bytes - text_bytes_)
                return invalid();
            if (*delta_type == "thinking_delta")
                block.text += *part;
            else
                block.arguments += *part;
            text_bytes_ += part->size();
        } else if (kind == "tool_use" && *delta_type == "input_json_delta") {
            const auto *part = string(*delta, "partial_json");
            if (!part || part->size() > limits_.max_arguments_buffer_bytes - argument_bytes_)
                return invalid();
            block.arguments += *part;
            argument_bytes_ += part->size();
        } else
            return invalid();
        return {};
    }
    if (*type == "content_block_stop") {
        if (kind == "text")
            set(block.value, "text", block.text);
        else if (kind == "thinking") {
            set(block.value, "thinking", block.text);
            set(block.value, "signature", block.arguments);
        } else if (kind == "tool_use" && !block.arguments.empty()) {
            const auto arguments = parse_json(block.arguments);
            // The terminal stop reason decides whether an unparseable
            // argument stream is a tolerated output-budget truncation, and
            // that reason only arrives with message_delta — so the verdict
            // moves to finish().
            if (!arguments || !arguments.value().is_object())
                truncated_tool_arguments_ = true;
            else
                set(block.value, "input", arguments.value());
        }
        block.closed = true;
        return {};
    }
    return invalid();
}
Result<ModelResponse> AnthropicMessagesSseParser::finish() {
    auto events = framer_.finish();
    if (!events)
        return events.error();
    for (const auto &event : events.value()) {
        auto status = reduce(event);
        if (!status)
            return status.error();
    }
    if (!stats_.terminal_seen)
        return make_model_error(ModelDomainCode::AmbiguousCompletion,
                                "Messages SSE ended without message_stop");
    // An argument stream that never became JSON is only acceptable when the
    // provider reports the output budget as the terminal reason; the block
    // keeps its start-shape empty input, so the completion stays decodable
    // and the caller sees a bounded length result instead of a lifecycle
    // violation.
    if (truncated_tool_arguments_) {
        const auto *stop = string(message_, "stop_reason");
        if (!stop || *stop != "max_tokens")
            return invalid();
    }
    JsonValue::Array content;
    for (const auto &block : blocks_)
        content.push_back(block.value);
    set(message_, "content", std::move(content));
    return AnthropicMessagesV1Mapper{}.decode_response(
        request_, profile_, WireHttpResponse{200, {}, to_json_string(message_)});
}
UnvalidatedModelPreview AnthropicMessagesSseParser::take_preview() {
    const auto drops = std::exchange(pending_drops_, 0);
    return {std::exchange(preview_, {}), drops, drops != 0};
}
} // namespace mira
