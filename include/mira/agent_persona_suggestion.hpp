#pragma once

#include <mira/agent_persona.hpp>
#include <mira/event_store.hpp>

#include <string_view>

namespace mira {

struct AgentPersonaSuggestionSource final {
    RuntimeId runtime;
    SessionSequence sequence = 0;
    EventId event;
};

struct AgentPersonaSuggestion final {
    Id128 id;
    AgentPersonaKey key;
    std::uint64_t base_version = 0;
    Sha256Digest base_digest;
    AgentPersonaSuggestionSource source;
    SessionId session;
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::size_t quote_offset = 0;
    std::string quote;
};

struct AgentPersonaSuggestionReceipt final {
    Id128 id;
    bool idempotent_replay = false;
};

// The host stages review data with the supplied stable identity. The sink
// owns its storage, permissions and revalidation before a configuration change.
using AgentPersonaSuggestionSink = std::function<Result<AgentPersonaSuggestionReceipt>(
    const AgentPersonaSuggestion &, const OperationContext &)>;

// Called on a host-managed Executor operation. Bind the source work to
// its authorized review target. A repeated quote needs a UTF-8 byte offset.
[[nodiscard]] Result<AgentPersonaSuggestionReceipt> submit_agent_persona_suggestion(
    const IEventStore &events, const AgentPersonaSuggestionSource &source, std::string_view quote,
    std::optional<std::size_t> quote_offset, const OperationContext &operation,
    const AgentPersonaSupplier &bind, const AgentPersonaSuggestionSink &stage);

} // namespace mira
