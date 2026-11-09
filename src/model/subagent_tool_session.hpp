#pragma once

#include <mira/subagent.hpp>

namespace mira::subagent_support {
// Child-worker confined, including synchronous directory and execution handlers.
// Registry handlers borrow this session; its owner outlives the loop/registry.
class ToolSession final {
  public:
    std::shared_ptr<BuiltinToolRegistry> registry;
    std::vector<ToolId> selected;
    std::optional<Error> fatal;
    std::size_t verified_effects = 0;
    bool effects_uncertain = false;

    [[nodiscard]] Result<void> initialize(const SubagentProfile &profile,
                                          SubagentResources &resources, const AgentLoopSpec &child,
                                          std::shared_ptr<IEventStore> events, RuntimeId runtime);
    [[nodiscard]] Result<std::vector<ToolId>> select(const ModelRequest &request,
                                                     std::uint64_t max_input) const;

  private:
    std::vector<SubagentTool> catalog_;
    std::vector<ToolId> loaded_;
    SubagentToolAuthorizer authorize_;
    SubagentToolSearch search_;
    AgentLoopSpec child_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    std::size_t max_active_ = 16;
    std::vector<ToolId> controls_;
    [[nodiscard]] Result<JsonValue> discover(const JsonValue &, const OperationContext &);
    [[nodiscard]] Result<JsonValue> load(const JsonValue &, const OperationContext &);
    [[nodiscard]] Result<JsonValue> execute(std::size_t index, const JsonValue &,
                                            const OperationContext &);
    [[nodiscard]] Result<void> emit(const char *type, const BuiltinToolSpec &, const JsonValue &,
                                    const OperationContext &);
};
} // namespace mira::subagent_support
