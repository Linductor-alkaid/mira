#include <mira/conversation_log.hpp>

#include <mira/json.hpp>

#include <sstream>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error projection_error(std::string message) {
    Error error;
    error.code = ErrorCode::DataLoss;
    error.domain = "mira.conversation";
    error.safe_message = std::move(message);
    return error;
}

// The loop writes {"task_id", ..., "detail": {...}} envelopes; the projection
// only needs the detail object. The parsed payload must outlive the returned
// pointer, so callers keep the Result alive around their field access.
[[nodiscard]] const JsonValue *event_detail(const Result<JsonValue> &payload) {
    if (!payload) {
        return nullptr;
    }
    const auto *detail = payload.value().find("detail");
    if (detail == nullptr || !detail->is_object()) {
        return nullptr;
    }
    return detail;
}

[[nodiscard]] Result<ConversationEntry> user_message_entry(const EventEnvelope &envelope) {
    const auto payload = parse_json(envelope.payload.data);
    const auto *detail = event_detail(payload);
    if (detail == nullptr) {
        return projection_error("UserMessageInjected payload is not readable");
    }
    const auto *text = detail->find("text");
    if (text == nullptr || !text->is_string()) {
        return projection_error("UserMessageInjected payload carries no text");
    }
    ConversationEntry entry;
    entry.kind = ConversationEntry::Kind::UserMessage;
    entry.recorded_at = envelope.timestamp;
    entry.text = *text->as_string();
    entry.origin = envelope.event_id;
    entry.session_sequence = envelope.session_sequence;
    return entry;
}

[[nodiscard]] Result<ConversationEntry> assistant_speech_entry(const EventEnvelope &envelope) {
    const auto payload = parse_json(envelope.payload.data);
    const auto *detail = event_detail(payload);
    if (detail == nullptr) {
        return projection_error("AssistantSpeech payload is not readable");
    }
    std::ostringstream text;
    bool has_content = false;
    // The speaker is carried structurally by `entry.kind`; prefixing the text
    // here would double up once `entry_line` (the single prefix renderer)
    // adds "assistant: " for text consumers, and replay conversion emits
    // native roles without any prefix (DEC-1xx).
    if (const auto *speech = detail->find("text"); speech != nullptr) {
        if (!speech->is_string()) {
            return projection_error("AssistantSpeech payload text is not a string");
        }
        const auto &spoken = *speech->as_string();
        if (!spoken.empty()) {
            text << spoken;
            has_content = true;
        }
    }
    if (const auto *calls = detail->find("tool_calls"); calls != nullptr) {
        if (!calls->is_array()) {
            return projection_error("AssistantSpeech payload tool_calls is not an array");
        }
        for (const auto &call : *calls->as_array()) {
            if (!call.is_object()) {
                return projection_error("AssistantSpeech tool call is not an object");
            }
            const auto *name = call.find("wire_name");
            const auto *status = call.find("status");
            if (name == nullptr || !name->is_string() || status == nullptr ||
                !status->is_string()) {
                return projection_error("AssistantSpeech tool call is not readable");
            }
            if (has_content) {
                text << '\n';
            }
            text << "tool call " << *name->as_string();
            if (const auto *arguments = call.find("arguments"); arguments != nullptr &&
                                                                arguments->is_string() &&
                                                                !arguments->as_string()->empty()) {
                text << ' ' << *arguments->as_string();
            }
            text << " (" << *status->as_string() << ")";
            has_content = true;
        }
    }
    if (!has_content) {
        return projection_error("AssistantSpeech payload carries no speech");
    }
    ConversationEntry entry;
    entry.kind = ConversationEntry::Kind::AssistantSpeech;
    entry.recorded_at = envelope.timestamp;
    entry.text = text.str();
    entry.origin = envelope.event_id;
    entry.session_sequence = envelope.session_sequence;
    return entry;
}

[[nodiscard]] Result<ConversationEntry> loop_outcome_entry(const EventEnvelope &envelope) {
    const auto payload = parse_json(envelope.payload.data);
    const auto *detail = event_detail(payload);
    if (detail == nullptr) {
        return projection_error(envelope.payload.type + " payload is not readable");
    }
    const auto *outcome = detail->find("outcome");
    if (outcome == nullptr || !outcome->is_string()) {
        return projection_error(envelope.payload.type + " payload carries no outcome");
    }
    std::ostringstream text;
    text << "loop settled: " << *outcome->as_string();
    if (envelope.payload.type == "LoopSettled") {
        if (const auto *steps = detail->find("steps"); steps != nullptr && steps->is_number()) {
            text << " (steps " << static_cast<long long>(steps->as_number().value_or(0.0)) << ")";
        }
    } else {
        // DEC-104: an Answered through executed tool calls reads differently
        // from an Answered with nothing but the terminal text - in a host
        // whose speech channel is a tool, the latter means the speech channel
        // never carried anything. The loop cannot know which tools speak, so
        // the wording stays factual over the recorded counter.
        if (*outcome->as_string() == "Answered") {
            std::uint64_t executed_calls = 0;
            bool counted = false;
            if (const auto *counter = detail->find("tool_executions");
                counter != nullptr && counter->is_number()) {
                const auto value = counter->as_number().value_or(0.0);
                if (value >= 0.0 && value <= 18446744073709551615.0) {
                    executed_calls = static_cast<std::uint64_t>(value);
                    counted = true;
                }
            }
            if (counted && executed_calls > 0) {
                text << " (terminal text + " << executed_calls
                     << (executed_calls == 1 ? " tool call" : " tool calls") << ")";
            } else {
                text << " (terminal text only)";
            }
        }
        bool has_counter = false;
        for (const auto *name : {"turns", "total_turns", "recoveries", "tool_executions"}) {
            if (const auto *counter = detail->find(name);
                counter != nullptr && counter->is_number()) {
                text << (has_counter ? ", " : " (") << name << " " << to_json_string(*counter);
                has_counter = true;
            }
        }
        if (has_counter) {
            text << ")";
        }
    }
    ConversationEntry entry;
    entry.kind = ConversationEntry::Kind::LoopOutcome;
    entry.recorded_at = envelope.timestamp;
    entry.text = text.str();
    entry.origin = envelope.event_id;
    entry.session_sequence = envelope.session_sequence;
    return entry;
}

} // namespace

Result<std::vector<ConversationEntry>> build_conversation_view(const IEventStore &store,
                                                               const SessionId &session) {
    std::vector<ConversationEntry> entries;
    std::optional<SessionSequence> after;
    while (true) {
        EventQuery query;
        query.session_id = session;
        query.after_sequence = after;
        const auto page = store.read(query);
        if (!page) {
            return page.error();
        }
        for (const auto &envelope : page.value().events) {
            if (envelope.payload.type == "UserMessageInjected") {
                auto entry = user_message_entry(envelope);
                if (!entry) {
                    return entry.error();
                }
                entries.push_back(std::move(entry).value());
            } else if (envelope.payload.type == "AssistantSpeech") {
                auto entry = assistant_speech_entry(envelope);
                if (!entry) {
                    return entry.error();
                }
                entries.push_back(std::move(entry).value());
            } else if (envelope.payload.type == "LoopSettled" ||
                       envelope.payload.type == "ConversationSettled") {
                auto entry = loop_outcome_entry(envelope);
                if (!entry) {
                    return entry.error();
                }
                entries.push_back(std::move(entry).value());
            }
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        after = page.value().events.back().session_sequence;
    }
    return entries;
}

} // namespace mira
