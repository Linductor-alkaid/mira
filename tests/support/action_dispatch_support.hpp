#pragma once

#include <mira/agent_loop.hpp>

namespace mira::testing {

// Process-local scripted fixture only. This is not a production admission
// policy and its MemoryEventStore cannot prove crash durability.
class ScriptedActionLease final : public IActionDispatchLease {
  public:
    Result<void> validate(const ActionIntent &, const OperationContext &) override { return {}; }
};

inline void wire_scripted_action_dispatch(AgentLoop &loop, const SessionId &session,
                                          std::shared_ptr<IEventStore> events = nullptr,
                                          RuntimeId runtime = RuntimeId::generate()) {
    if (!events) {
        events = std::make_shared<MemoryEventStore>();
    }
    loop.set_action_dispatcher(std::make_shared<JournaledActionDispatcher>(
        runtime, session, std::move(events),
        [](const ActionIntent &,
           const OperationContext &) -> Result<std::unique_ptr<IActionDispatchLease>> {
            return std::unique_ptr<IActionDispatchLease>(std::make_unique<ScriptedActionLease>());
        }));
}

} // namespace mira::testing
