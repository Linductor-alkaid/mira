#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_tool.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace mira {

struct OperationIdHash final {
    std::size_t operator()(const OperationId &id) const noexcept {
        return Id128Hash{}(id.value);
    }
};

// ---------------------------------------------------------------------------
// BuiltIn tool execution boundary (DEC-015)
// ---------------------------------------------------------------------------

// One in-process BuiltIn tool registration. The spec is the full public
// description: everything the model sees and everything the executor verifies
// against at dispatch time. Registering a tool never grants authority; the
// model only reaches a tool through ModelRequest.tools and resolve_tool_calls.
struct BuiltinToolSpec final {
    ToolId tool_id = ToolId::generate();
    SemanticVersion version{1, 0, 0};
    // Provider-visible alias; unique within one registry and never a hosted
    // provider tool name ("bash", "web_search", ...).
    std::string wire_name;
    std::string description;
    JsonSchema parameters_schema;
    bool has_side_effects = false;
};

// Executes one call inline on the caller's worker. Handlers must stay bounded,
// poll OperationContext cancellation between blocking slices and must not
// throw (the registry converts an escaping exception into a failed record).
// Tool-visible arguments were already schema-validated by the registry.
using BuiltinToolHandler =
    std::function<Result<JsonValue>(const JsonValue &arguments, const OperationContext &context)>;

// The minimal BuiltIn execution boundary ahead of the DEC-009 module system:
// a name -> handler table with fail-closed dispatch. Unknown tools, identity
// mismatches against the exposed snapshot and re-dispatched operation ids are
// rejected; at-most-once dispatch per OperationId is enforced here (W-02).
// Model-attributable failures (invalid arguments, handler error) come back as
// failed ToolExecutionRecords so the loop can feed them to the model; only
// cancellation propagates as a Result error.
class BuiltinToolRegistry final {
  public:
    BuiltinToolRegistry() = default;
    ~BuiltinToolRegistry() = default;
    BuiltinToolRegistry(const BuiltinToolRegistry &) = delete;
    BuiltinToolRegistry &operator=(const BuiltinToolRegistry &) = delete;

    // Fails closed on: empty identity/description, hosted wire names, schemas
    // outside the supported subset, and duplicate wire names or tool ids.
    [[nodiscard]] Result<void> register_tool(BuiltinToolSpec spec, BuiltinToolHandler handler);

    // Deterministic snapshot (sorted by wire_name) for ModelRequest.tools.
    [[nodiscard]] std::vector<ExposedToolSpec> exposed_tools() const;

    [[nodiscard]] std::size_t size() const;

    // Executes one resolved proposal. The proposal must reference the same
    // tool identity that was exposed to the producing request; a registry
    // mutated between exposure and dispatch fails closed.
    [[nodiscard]] Result<ToolExecutionRecord> execute(const ToolProposal &proposal,
                                                      const OperationContext &context);

  private:
    struct Entry final {
        BuiltinToolSpec spec;
        BuiltinToolHandler handler;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> tools_; // Sorted by wire_name.
    std::unordered_set<OperationId, OperationIdHash> dispatched_;
};

struct BuiltinToolRegistration final {
    BuiltinToolSpec spec;
    BuiltinToolHandler handler;
};

// Reference BuiltIn tool: waits a bounded duration (0..10000 ms) polling
// cancellation. Read-only with respect to the environment; hosts register it
// explicitly through the same boundary as their own tools.
[[nodiscard]] BuiltinToolRegistration make_wait_tool();

} // namespace mira
