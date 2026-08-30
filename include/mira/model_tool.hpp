#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// Limits enforced while resolving provider tool calls.
struct ToolBridgeLimits final {
    std::size_t max_calls_per_response = 16;
    std::size_t max_arguments_bytes = 256 * 1024;
    std::size_t max_result_bytes = 1024 * 1024;
};

extern const ToolBridgeLimits kDefaultToolBridgeLimits;

// One resolved, locally verified tool proposal. Nothing here executes: the
// batch only becomes actions after Policy and Planner admit it.
struct ToolProposal final {
    ProviderToolCallId provider_call_id;
    ToolId tool_id;
    std::string wire_name;
    SemanticVersion tool_version;
    JsonValue arguments;
    Hash arguments_digest{};
    // Stable operation identity derived from (request, call, tool, arguments).
    OperationId operation_id;
    bool has_side_effects = false;
    // Set when an identical duplicate call was collapsed into this proposal.
    bool deduplicated = false;
};

struct ToolBridgeDiagnostic final {
    ProviderToolCallId provider_call_id;
    std::string message;
};

struct ToolProposalBatch final {
    std::vector<ToolProposal> proposals;
    std::vector<ToolBridgeDiagnostic> diagnostics;
    [[nodiscard]] bool empty() const noexcept { return proposals.empty(); }
};

// Hosted tool names that Mira must fail closed on in M3: the profile allowlist
// is empty, so any call to one of these (or any name not exposed by the
// request) is a protocol violation, never a capability.
[[nodiscard]] bool is_known_hosted_tool_name(std::string_view wire_name);

// Resolves provider tool calls against the tools exposed by the request.
// Fails closed on unknown/hosted names, unresolved IDs, duplicate call IDs
// with conflicting arguments, oversize arguments and batches over the limit.
// Duplicate calls with identical digests are collapsed and recorded.
[[nodiscard]] Result<ToolProposalBatch>
resolve_tool_calls(const ModelRequest &request, const ModelResponse &response,
                   const ToolBridgeLimits &limits = kDefaultToolBridgeLimits);

// The outcome of one executed tool, ready to be fed back to the provider.
struct ToolExecutionRecord final {
    ProviderToolCallId provider_call_id;
    ToolId tool_id;
    JsonValue result;
    // Large payloads are stored as artifacts; only the reference is returned.
    std::optional<ArtifactRef> large_payload;
    bool failed = false;
    std::string safe_error_summary;
};

// Builds the wire items that carry tool results back into the next request.
// Responses dialect: function_call_output items; Chat Completions dialect:
// tool role messages. Unrepresentable records produce an error, never a
// silent drop.
[[nodiscard]] Result<std::vector<JsonValue>>
build_tool_result_input(ProtocolDialect dialect, std::span<const ToolExecutionRecord> records);

// Derives the stable operation ID for one tool call.
[[nodiscard]] OperationId derive_tool_operation_id(const ModelRequestId &request_id,
                                                  const ProviderToolCallId &call_id,
                                                  const ToolId &tool_id, const Hash &arguments_digest);

} // namespace mira
