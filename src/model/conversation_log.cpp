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

[[nodiscard]] Result<ConversationEntry>
user_message_entry(const EventEnvelope &envelope) {
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
    return entry;
}

[[nodiscard]] Result<ConversationEntry> loop_outcome_entry(const EventEnvelope &envelope) {
    const auto payload = parse_json(envelope.payload.data);
    const auto *detail = event_detail(payload);
    if (detail == nullptr) {
        return projection_error("LoopSettled payload is not readable");
    }
    const auto *outcome = detail->find("outcome");
    if (outcome == nullptr || !outcome->is_string()) {
        return projection_error("LoopSettled payload carries no outcome");
    }
    std::ostringstream text;
    text << "loop settled: " << *outcome->as_string();
    if (const auto *steps = detail->find("steps"); steps != nullptr && steps->is_number()) {
        text << " (steps " << static_cast<long long>(steps->as_number().value_or(0.0)) << ")";
    }
    ConversationEntry entry;
    entry.kind = ConversationEntry::Kind::LoopOutcome;
    entry.recorded_at = envelope.timestamp;
    entry.text = text.str();
    entry.origin = envelope.event_id;
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
            } else if (envelope.payload.type == "LoopSettled") {
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
