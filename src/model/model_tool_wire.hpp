#pragma once

// Internal to the model layer: renders the canonical tool-call/result input
// parts (DEC-047) into OpenAI wire items so the dialect mappers stay thin.
// Tool parts must ride alone in their input item; mixed shapes fail closed.

#include <mira/model_contracts.hpp>
#include <mira/model_tool.hpp>

#include <vector>

namespace mira::tool_wire {

enum class ItemShape : std::uint8_t {
    Ordinary,  // No tool parts: the mapper's default role/content path.
    CallsOnly, // Assistant call echo item.
    ResultsOnly,
    Mixed, // Tool parts combined with other content or with each other.
};

[[nodiscard]] ItemShape classify_item(const ModelInputItem &item);

// Responses dialect: one `function_call` input item per call; Chat
// Completions dialect: one assistant message carrying a `tool_calls` array.
[[nodiscard]] Result<std::vector<JsonValue>> encode_calls(const ModelInputItem &item,
                                                          ProtocolDialect dialect);

// Responses dialect: one `function_call_output` input item per result; Chat
// Completions dialect: one `role: tool` message per result. Reuses
// build_tool_result_input so the payload envelope has exactly one shape.
[[nodiscard]] Result<std::vector<JsonValue>> encode_results(const ModelInputItem &item,
                                                            ProtocolDialect dialect);

} // namespace mira::tool_wire
