#pragma once

#include <mira/event_store.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Conversation view projection (DEC-016)
// ---------------------------------------------------------------------------

// One user-visible line of the session conversation. The event store remains
// the only source of truth (RULE-07); this view is a rebuildable projection,
// never a second store. `session_sequence` echoes the source envelope's
// sequence so downstream projections (e.g. M17 conversation segmentation) can
// stamp rebuild watermarks without re-reading the store.
struct ConversationEntry final {
    enum class Kind : std::uint8_t { UserMessage, AssistantSpeech, LoopOutcome };
    Kind kind = Kind::UserMessage;
    Timestamp recorded_at;
    std::string text;
    EventId origin;
    SessionSequence session_sequence = 0;
};

// Rebuilds the conversation view for one session from `UserMessageInjected`,
// `AssistantSpeech` (both ConversationLoop, DEC-104) and both `LoopSettled`
// (AgentLoop) and `ConversationSettled` (ConversationLoop) events in session
// order. Recognized events with unreadable required payloads fail closed;
// unrelated event types are skipped without parsing their payloads.
[[nodiscard]] Result<std::vector<ConversationEntry>>
build_conversation_view(const IEventStore &store, const SessionId &session);

} // namespace mira
