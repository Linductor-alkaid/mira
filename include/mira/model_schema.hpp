#pragma once

#include <mira/model_contracts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Strict JSON Schema subset (M3-09)
// ---------------------------------------------------------------------------

// Limits for schemas accepted by the local gate. Schemas beyond the verified
// subset are rejected before the request is sent, not after the provider
// fails on it.
struct SchemaSubsetLimits final {
    std::size_t max_depth = 8;
    std::size_t max_properties = 64;
    std::size_t max_enum_values = 64;
    std::size_t max_pattern_bytes = 256;
    std::size_t max_schema_bytes = 64 * 1024;
};

extern const SchemaSubsetLimits kDefaultSchemaSubsetLimits;

// Keywords the M3 validator implements. Any other validation keyword is
// rejected by gate_schema_subset so behavior is explicit and testable.
inline constexpr std::array<std::string_view, 18> kSupportedSchemaKeywords = {
    "type",       "properties", "required",    "additionalProperties", "enum",
    "const",      "items",      "minItems",    "maxItems",             "minLength",
    "maxLength",  "minimum",    "maximum",     "pattern",              "title",
    "description", "$schema",   "default",
};

// Rejects schemas that use unsupported keywords or exceed the subset limits.
[[nodiscard]] Result<void> gate_schema_subset(const JsonSchema &schema,
                                              const SchemaSubsetLimits &limits = kDefaultSchemaSubsetLimits);

struct SchemaViolation final {
    std::string path;    // JSON path of the offending instance value.
    std::string keyword; // Schema keyword that failed.
    std::string message;
};

// Validates an instance against a schema that already passed the subset gate.
// The validator is strict: objects reject properties not present in
// "properties" unless "additionalProperties" is explicitly true.
[[nodiscard]] std::vector<SchemaViolation>
validate_instance_against_schema(const JsonValue &instance, const JsonSchema &schema);

// ---------------------------------------------------------------------------
// Decision parsing (three-layer success condition, layer 3)
// ---------------------------------------------------------------------------

// A terminal response reduces to exactly one of: a DecisionCandidate, a
// ToolProposalBatch (declared in model_tool.hpp), or a terminal non-decision
// status. Mixed or conflicting executable output is AmbiguousModelOutput.
struct DecisionCandidate final {
    SchemaId schema_id;
    SemanticVersion schema_version;
    JsonValue value;
    Hash decision_digest_field{};
};

// Where the structured payload was found in the response.
enum class DecisionSource : std::uint8_t {
    TextJson,
    StrictToolCall,
    JsonObject,
};

enum class DecisionParseOutcome : std::uint8_t {
    Decision,
    ToolProposals,
    Refused,
    Incomplete,
    ContentFiltered,
    Failed,
    NoExecutableOutput,
    Ambiguous,
    Malformed,
};

struct DecisionParseResult final {
    DecisionParseOutcome outcome = DecisionParseOutcome::NoExecutableOutput;
    std::optional<DecisionCandidate> decision;
    DecisionSource source = DecisionSource::TextJson;
    std::vector<SchemaViolation> violations;
    std::string safe_summary;
};

// Parses and locally re-validates the structured output of a terminal model
// response against the request's output contract. Provider-side validation is
// never trusted.
[[nodiscard]] DecisionParseResult parse_decision(const ModelRequest &request,
                                                 const ModelResponse &response);

// ---------------------------------------------------------------------------
// Bounded repair policy
// ---------------------------------------------------------------------------

struct RepairPolicy final {
    std::uint32_t max_attempts = 1;
    std::size_t max_error_summary_bytes = 2048;
};

struct RepairBudget final {
    std::uint32_t attempts_used = 0;
    [[nodiscard]] bool exhausted(const RepairPolicy &policy) const noexcept {
        return attempts_used >= policy.max_attempts;
    }
};

// Builds the follow-up repair request for a Completed-but-malformed response.
// The repair is a new paid operation with a new ModelRequestId that quotes a
// bounded, redacted validation summary; it never embeds raw model output.
[[nodiscard]] Result<ModelRequest>
build_schema_repair_request(const ModelRequest &original, const DecisionParseResult &failure,
                            const RepairPolicy &policy, const RepairBudget &budget);

} // namespace mira
