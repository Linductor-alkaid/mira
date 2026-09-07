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
// never a second store.
struct ConversationEntry final {
    enum class Kind : std::uint8_t { UserMessage, LoopOutcome };
    Kind kind = Kind::UserMessage;
    Timestamp recorded_at;
    std::string text;
    EventId origin;
};

// Rebuilds the conversation view for one session from `UserMessageInjected`
// and `LoopSettled` events in session order. Events with unparsable payloads
// fail closed instead of being silently skipped.
[[nodiscard]] Result<std::vector<ConversationEntry>>
build_conversation_view(const IEventStore &store, const SessionId &session);

} // namespace mira
