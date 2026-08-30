#include <mira/model_sse.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error sse_error(ModelDomainCode code, std::string message) {
    return make_model_error(code, std::move(message), false, std::nullopt);
}

} // namespace

// ---------------------------------------------------------------------------
// SseFramingParser
// ---------------------------------------------------------------------------

SseFramingParser::SseFramingParser(SseFramingLimits limits) : limits_(limits) {}

Result<std::vector<SseMessage>> SseFramingParser::feed(std::string_view chunk) {
    if (bytes_fed_ + chunk.size() > limits_.max_total_bytes) {
        return sse_error(ModelDomainCode::ResponseTooLarge, "sse stream exceeded the byte limit");
    }
    bytes_fed_ += chunk.size();
    buffer_.append(chunk.data(), chunk.size());
    return process_line_buffer();
}

Result<std::vector<SseMessage>> SseFramingParser::finish() {
    // EOF terminates a final unterminated line; dispatch whatever fields it
    // carried, exactly as a trailing newline would have.
    if (!buffer_.empty()) {
        std::string last_line = std::exchange(buffer_, {});
        auto messages = process_line(last_line);
        if (!messages) {
            return messages;
        }
        auto remaining = dispatch();
        if (!remaining) {
            return remaining;
        }
        std::vector<SseMessage> all = messages.value();
        for (auto &message : remaining.value()) {
            all.push_back(std::move(message));
        }
        return all;
    }
    return dispatch();
}

Result<std::vector<SseMessage>> SseFramingParser::process_line_buffer() {
    std::vector<SseMessage> messages;
    while (!buffer_.empty()) {
        const auto newline = buffer_.find('\n');
        const auto carriage = buffer_.find('\r');
        if (newline == std::string::npos && carriage == std::string::npos) {
            break; // Incomplete line; wait for more bytes.
        }
        // A trailing CR could be the first half of a CRLF pair; wait for the
        // next byte instead of emitting a spurious blank line.
        if (carriage != std::string::npos &&
            (newline == std::string::npos || carriage < newline) &&
            carriage + 1 == buffer_.size()) {
            break;
        }
        std::size_t line_length = 0;
        if (carriage != std::string::npos &&
            (newline == std::string::npos || carriage < newline)) {
            line_length = (newline == carriage + 1) ? newline + 1 : carriage + 1;
        } else {
            line_length = newline + 1;
        }
        const std::string line = buffer_.substr(0, line_length);
        buffer_.erase(0, line_length);
        auto produced = process_line(line);
        if (!produced) {
            return produced;
        }
        for (auto &message : produced.value()) {
            messages.push_back(std::move(message));
        }
        if (events_dispatched_ > limits_.max_events) {
            return sse_error(ModelDomainCode::ResponseTooLarge,
                             "sse stream exceeded the event limit");
        }
    }
    return messages;
}

Result<std::vector<SseMessage>> SseFramingParser::process_line(const std::string &line) {
    // Strip the line terminator itself.
    std::string_view content(line);
    if (!content.empty() && content.back() == '\n') {
        content.remove_suffix(1);
    }
    if (!content.empty() && content.back() == '\r') {
        content.remove_suffix(1);
    }
    if (content.size() > limits_.max_line_bytes) {
        return sse_error(ModelDomainCode::ResponseTooLarge, "sse line exceeded the byte limit");
    }
    if (content.empty()) {
        return dispatch();
    }
    if (content.front() == ':') {
        return std::vector<SseMessage>{};
    }
    const auto colon = content.find(':');
    std::string_view field = content;
    std::string_view value;
    if (colon != std::string_view::npos) {
        field = content.substr(0, colon);
        value = content.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }
    if (field == "event") {
        pending_event_ = std::string(value);
    } else if (field == "data") {
        if (!pending_data_.empty()) {
            pending_data_.push_back('\n');
        }
        if (pending_data_.size() + value.size() > limits_.max_event_data_bytes) {
            return sse_error(ModelDomainCode::ResponseTooLarge,
                             "sse event data exceeded the byte limit");
        }
        pending_data_.append(value);
    } else if (field == "id") {
        // IDs containing NUL are ignored per the spec.
        if (value.find('\0') == std::string_view::npos) {
            pending_id_ = std::string(value);
        }
    } else if (field == "retry") {
        try {
            pending_retry_ms_ = std::stoll(std::string(value));
        } catch (const std::exception &) {
            // Malformed retry fields are ignored; they are advisory only.
        }
    }
    // Unknown fields are ignored per the spec.
    return std::vector<SseMessage>{};
}

Result<std::vector<SseMessage>> SseFramingParser::dispatch() {
    if (pending_event_.empty() && pending_data_.empty()) {
        return std::vector<SseMessage>{};
    }
    SseMessage message;
    message.event = pending_event_;
    message.data = std::exchange(pending_data_, {});
    message.id = std::move(pending_id_);
    message.retry_ms = pending_retry_ms_;
    pending_event_.clear();
    pending_retry_ms_.reset();
    ++events_dispatched_;
    std::vector<SseMessage> messages;
    messages.push_back(std::move(message));
    return messages;
}

// ---------------------------------------------------------------------------
// ResponsesSseParser
// ---------------------------------------------------------------------------

ResponsesSseParser::ResponsesSseParser(const ModelRequest &request, const ModelProfile &profile,
                                       SseStreamLimits limits)
    : request_(request), profile_(profile), limits_(limits), framer_(limits.framing) {}

Result<void> ResponsesSseParser::feed(std::string_view chunk) {
    auto messages = framer_.feed(chunk);
    if (!messages) {
        return messages.error();
    }
    for (const auto &message : messages.value()) {
        auto status = reduce(message);
        if (!status) {
            return status;
        }
    }
    stats_.bytes = framer_.bytes_fed();
    return Result<void>{};
}

ResponsesSseParser::OpenItem *ResponsesSseParser::find_open_item(const std::string &item_id) {
    const auto found = std::find_if(items_.begin(), items_.end(),
                                    [&](const OpenItem &item) { return item.item_id == item_id; });
    return found == items_.end() ? nullptr : &*found;
}

Result<void> ResponsesSseParser::check_remote_sequence(const JsonValue &data) {
    const auto *sequence = data.find("sequence_number");
    if (sequence == nullptr) {
        return Result<void>{}; // Remote sequence is optional in the dialect.
    }
    if (!sequence->is_integer()) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "remote sequence number is not an integer");
    }
    const auto value = sequence->as_integer().value();
    if (!stats_.last_remote_sequence.has_value()) {
        stats_.last_remote_sequence = value;
        return Result<void>{};
    }
    const auto last = *stats_.last_remote_sequence;
    if (value == last) {
        // Same sequence with identical payload collapses; a different payload
        // at the same position is a conflict.
        return Result<void>{}; // Payload equality is enforced by event pairing.
    }
    if (value != last + 1) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "remote sequence gap detected in sse stream");
    }
    stats_.last_remote_sequence = value;
    return Result<void>{};
}

Result<void> ResponsesSseParser::note_terminal(const char *kind) {
    if (terminal_seen_) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         std::string("duplicate terminal event after ") + kind);
    }
    terminal_seen_ = true;
    stats_.terminal_seen = true;
    return Result<void>{};
}

Result<void> ResponsesSseParser::append_preview(std::string_view delta) {
    if (preview_text_.size() + delta.size() > limits_.max_preview_bytes) {
        ++preview_drops_;
        return Result<void>{}; // Preview loss is reported, never fatal.
    }
    preview_text_.append(delta.data(), delta.size());
    return Result<void>{};
}

Result<void> ResponsesSseParser::reduce(const SseMessage &message) {
    ++stats_.events;
    ++stats_.stream_sequence;
    if (message.event.empty()) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "unnamed sse event in a responses stream");
    }
    if (terminal_seen_) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "business event arrived after the terminal event");
    }
    auto parsed = parse_json(message.data);
    if (!parsed || !parsed.value().is_object()) {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "sse event data is not a json object");
    }
    const auto &data = parsed.value();
    if (auto sequence = check_remote_sequence(data); !sequence) {
        return sequence;
    }

    const auto &event = message.event;
    if (event == "response.created" || event == "response.in_progress") {
        return Result<void>{};
    }
    if (event == "output_item.added") {
        const auto *item = data.find("item");
        if (item == nullptr || !item->is_object()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output_item.added carries no item object");
        }
        const auto *item_id = item->find("id");
        const auto *item_type = item->find("type");
        if (item_id == nullptr || !item_id->is_string() || item_type == nullptr ||
            !item_type->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output_item.added carries no id or type");
        }
        if (find_open_item(*item_id->as_string()) != nullptr) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output item id was reused before completion");
        }
        if (items_.size() >= limits_.max_open_output_items) {
            return sse_error(ModelDomainCode::ModelResourceExhausted,
                             "too many concurrent output items");
        }
        OpenItem open;
        open.item_id = *item_id->as_string();
        open.type = *item_type->as_string();
        items_.push_back(std::move(open));
        return Result<void>{};
    }
    if (event == "output_item.done") {
        const auto *item = data.find("item");
        if (item == nullptr || !item->is_object()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output_item.done carries no item object");
        }
        const auto *item_id = item->find("id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output_item.done carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output_item.done has no matching added event");
        }
        if (!open->open_parts.empty()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "output item completed with open content parts");
        }
        if (open->type == "function_call" && !open->args_done) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "function call completed without arguments done");
        }
        open->closed = true;
        return Result<void>{};
    }
    if (event == "content_part.added" || event == "content_part.done") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "content part event carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "content part event references a closed item");
        }
        std::string part_key;
        if (const auto *index = data.find("output_index"); index != nullptr &&
                                                           index->is_integer()) {
            part_key = std::to_string(index->as_integer().value());
        } else {
            part_key = "default";
        }
        if (event == "content_part.added") {
            if (std::find(open->open_parts.begin(), open->open_parts.end(), part_key) !=
                open->open_parts.end()) {
                return sse_error(ModelDomainCode::ProtocolViolation,
                                 "content part added twice without completion");
            }
            open->open_parts.push_back(std::move(part_key));
            return Result<void>{};
        }
        const auto found = std::find(open->open_parts.begin(), open->open_parts.end(), part_key);
        if (found == open->open_parts.end()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "content_part.done has no matching added event");
        }
        open->open_parts.erase(found);
        return Result<void>{};
    }
    if (event == "output_text.delta") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation, "text delta carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "text delta references a closed item");
        }
        const auto *delta = data.find("delta");
        if (delta == nullptr || !delta->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation, "text delta carries no delta");
        }
        if (accumulated_text_.size() + open->text.size() + delta->as_string()->size() >
            limits_.max_accumulated_text_bytes) {
            return sse_error(ModelDomainCode::ResponseTooLarge,
                             "accumulated text exceeded the limit");
        }
        open->text += *delta->as_string();
        accumulated_text_ += *delta->as_string();
        ++stats_.text_deltas;
        return append_preview(*delta->as_string());
    }
    if (event == "output_text.done") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation, "text done carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "text done references a closed item");
        }
        const auto *text = data.find("text");
        if (text != nullptr && text->is_string() && open->text != *text->as_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "declared text does not match the accumulated deltas");
        }
        open->text_done = true;
        return Result<void>{};
    }
    if (event == "refusal.delta" || event == "refusal.done") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation, "refusal event carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "refusal event references a closed item");
        }
        if (event == "refusal.delta") {
            const auto *delta = data.find("delta");
            if (delta != nullptr && delta->is_string()) {
                open->refusal += *delta->as_string();
                if (open->refusal.size() > limits_.max_accumulated_text_bytes) {
                    return sse_error(ModelDomainCode::ResponseTooLarge,
                                     "accumulated refusal text exceeded the limit");
                }
            }
            return Result<void>{};
        }
        open->refusal_done = true;
        return Result<void>{};
    }
    if (event == "function_call_arguments.delta") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "arguments delta carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "arguments delta references a closed item");
        }
        const auto *delta = data.find("delta");
        if (delta == nullptr || !delta->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "arguments delta carries no delta");
        }
        // Arguments are buffered only; no JSON parsing and no tool dispatch
        // may happen before the done event.
        if (open->args.size() + delta->as_string()->size() >
            limits_.max_arguments_buffer_bytes) {
            return sse_error(ModelDomainCode::ResponseTooLarge,
                             "function arguments buffer exceeded the limit");
        }
        open->args += *delta->as_string();
        return Result<void>{};
    }
    if (event == "function_call_arguments.done") {
        const auto *item_id = data.find("item_id");
        if (item_id == nullptr || !item_id->is_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "arguments done carries no item id");
        }
        auto *open = find_open_item(*item_id->as_string());
        if (open == nullptr || open->closed) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "arguments done references a closed item");
        }
        const auto *arguments = data.find("arguments");
        if (arguments != nullptr && arguments->is_string() &&
            open->args != *arguments->as_string()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "declared arguments do not match the accumulated deltas");
        }
        open->args_done = true;
        return Result<void>{};
    }
    if (event == "response.completed" || event == "response.failed" ||
        event == "response.incomplete") {
        auto status = note_terminal(event.c_str());
        if (!status) {
            return status;
        }
        const auto *response = data.find("response");
        if (response == nullptr || !response->is_object()) {
            return sse_error(ModelDomainCode::ProtocolViolation,
                             "terminal event carries no response object");
        }
        for (const auto &item : items_) {
            if (!item.closed) {
                return sse_error(ModelDomainCode::ProtocolViolation,
                                 "terminal arrived while output items were still open");
            }
        }
        auto decoded = decode_responses_terminal_body(request_, profile_, *response);
        if (!decoded) {
            return decoded.error();
        }
        terminal_response_ = std::move(decoded).value();
        return Result<void>{};
    }
    if (event == "error") {
        return sse_error(ModelDomainCode::ProtocolViolation,
                         "provider emitted a stream error event");
    }
    // Unknown event names fail closed; they are never silently skipped.
    return sse_error(ModelDomainCode::ProtocolViolation,
                     "unknown sse event type: " + message.event);
}

Result<ModelResponse> ResponsesSseParser::finish() {
    auto tail = framer_.finish();
    if (!tail) {
        return tail.error();
    }
    for (const auto &message : tail.value()) {
        auto status = reduce(message);
        if (!status) {
            return status.error();
        }
    }
    stats_.bytes = framer_.bytes_fed();
    stats_.cancel_seen = cancel_seen_;
    if (!terminal_seen_) {
        // EOF without a terminal: the remote may have billed the request, so
        // the completion is ambiguous rather than failed.
        return sse_error(ModelDomainCode::AmbiguousCompletion,
                         "sse stream ended without a terminal event");
    }
    return terminal_response_.value_or(ModelResponse{});
}

UnvalidatedModelPreview ResponsesSseParser::take_preview() {
    UnvalidatedModelPreview preview;
    preview.text = std::move(preview_text_);
    preview.dropped_updates = preview_drops_;
    preview.truncated = preview_drops_ > 0;
    preview_text_.clear();
    preview_drops_ = 0;
    stats_.preview_drops += preview.dropped_updates;
    return preview;
}

} // namespace mira
