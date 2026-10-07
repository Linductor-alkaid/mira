#pragma once

// Internal support shared by the context layer's model-backed surfaces
// (consolidator, curator) and its lexical legs (retrieval, rerank). Not
// installed and not part of the public API. Single-sources the deterministic
// text utilities, the numbered-statement wire parsing, the strict output
// schema fragments and the model-request assembly so parallel surfaces
// cannot drift.

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_schema.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira::context_support {

// ---------------------------------------------------------------------------
// Deterministic text utilities
// ---------------------------------------------------------------------------

// Deterministic ASCII lowercasing shared by the Layer 1 lexical index, the
// rerank legs and the marker filters: recall and rerank must agree on what a
// token is, and every surface must agree on what a marker matches.
[[nodiscard]] inline std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char &character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// Case-insensitive marker scan with the same semantics as the Layer 1 index
// and `MemoryConsolidator`: one shared secret-marker vocabulary across every
// surface model output could reach.
[[nodiscard]] inline bool carries_marker(std::string_view text,
                                         const std::vector<std::string> &markers) {
    const std::string lowered = to_lower_ascii(text);
    for (const auto &marker : markers) {
        if (!marker.empty() && lowered.find(to_lower_ascii(marker)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Lexical tokenizer for the in-process lexical legs: lowercase [a-z0-9_] runs.
[[nodiscard]] inline std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : to_lower_ascii(text)) {
        const bool word = (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
        if (word) {
            current += character;
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

[[nodiscard]] inline bool contains_case_insensitive(std::string_view haystack,
                                                    std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const std::string lowered = to_lower_ascii(haystack);
    const std::string lowered_needle = to_lower_ascii(needle);
    return lowered.find(lowered_needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Model statement wire parsing
// ---------------------------------------------------------------------------

// Model statement as parsed: content plus transcript-number citations. The
// provenance binding happens in the surface's bind_statement so fabricated
// citations can be dropped before any provenance exists.
struct RawStatement final {
    std::string content;
    std::vector<std::int64_t> citations;
    double confidence = 0.0;
};

[[nodiscard]] inline std::optional<RawStatement> parse_statement(const JsonValue &value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    RawStatement raw;
    const auto *content = value.find("content");
    if (content == nullptr || !content->is_string()) {
        return std::nullopt;
    }
    raw.content = *content->as_string();
    const auto *sources = value.find("sources");
    if (sources == nullptr || !sources->is_array() || sources->as_array()->empty()) {
        return std::nullopt;
    }
    for (const auto &source : *sources->as_array()) {
        const auto index = source.as_integer();
        if (!index || *index < 0) {
            return std::nullopt;
        }
        raw.citations.push_back(*index);
    }
    if (const auto *confidence = value.find("confidence"); confidence != nullptr) {
        const auto number = confidence->as_number();
        if (!number) {
            return std::nullopt;
        }
        raw.confidence = *number;
    }
    return raw;
}

// Admission checks applied before any provenance binding (RULE-09
// preconditions): content bounds, secret/injection marker filters and the
// confidence floor. Returns the clamped confidence, or nullopt when the
// statement must be dropped.
[[nodiscard]] inline std::optional<double> gate_statement(
    const RawStatement &raw, std::size_t max_chars, double min_confidence,
    const std::vector<std::string> &forbidden_markers,
    const std::vector<std::string> &injection_markers) {
    if (raw.content.empty() || raw.content.size() > max_chars) {
        return std::nullopt;
    }
    if (carries_marker(raw.content, forbidden_markers) ||
        carries_marker(raw.content, injection_markers)) {
        return std::nullopt;
    }
    if (!std::isfinite(raw.confidence)) {
        return std::nullopt;
    }
    const double confidence = std::clamp(raw.confidence, 0.0, 1.0);
    if (confidence < min_confidence) {
        return std::nullopt;
    }
    return confidence;
}

// Collects the plain text of one model response; the output must carry
// message outputs with text parts only. `make_error` adapts the failures to
// the calling surface's error domain; `label` prefixes the stable wording
// ("consolidation" / "curation").
template <typename MakeError>
[[nodiscard]] Result<std::string> message_response_text(const ModelResponse &response,
                                                        MakeError &&make_error,
                                                        std::string_view label) {
    std::string text;
    for (const auto &item : response.output) {
        const auto *message = std::get_if<MessageOutput>(&item);
        if (message == nullptr) {
            return make_error(ErrorCode::InvalidModelOutput,
                              std::string(label) + " response must carry message outputs only");
        }
        for (const auto &part : message->content) {
            if (const auto *text_part = std::get_if<OutputTextPart>(&part)) {
                text += text_part->text;
            } else {
                return make_error(ErrorCode::InvalidModelOutput,
                                  std::string(label) + " response message carries a refusal");
            }
        }
    }
    if (text.empty()) {
        return make_error(ErrorCode::InvalidModelOutput,
                          std::string(label) + " response carried no text");
    }
    return text;
}

// ---------------------------------------------------------------------------
// Strict output schema fragments
// ---------------------------------------------------------------------------

// One numbered statement in the strict output schemas both model-backed
// surfaces send: content, integer transcript citations, confidence.
[[nodiscard]] inline JsonValue statement_schema() {
    return JsonValue::Object{
        {"type", std::string("object")},
        {"additionalProperties", false},
        {"required", JsonValue::Array{std::string("content"), std::string("sources"),
                                      std::string("confidence")}},
        {"properties",
         JsonValue::Object{
             {"content", JsonValue::Object{{"type", std::string("string")}}},
             {"sources",
              JsonValue::Object{
                  {"type", std::string("array")},
                  {"items", JsonValue::Object{{"type", std::string("integer")},
                                              {"minimum", static_cast<std::int64_t>(0)}}}}},
             {"confidence", JsonValue::Object{{"type", std::string("number")}}},
         }},
    };
}

[[nodiscard]] inline JsonValue statement_array_schema() {
    return JsonValue::Object{{"type", std::string("array")}, {"items", statement_schema()}};
}

// Parses one numbered output section, binds provenance through `bind` and
// enforces the output bound (RULE-08): the first `cap` bound statements win,
// everything after is dropped. `make_error` adapts the section-shape failure
// to the calling surface's domain; `label` prefixes the stable wording.
template <typename Statement, typename MakeError, typename Bind>
[[nodiscard]] Result<std::vector<Statement>> parse_statement_section(
    const JsonValue &root, const char *key, std::string_view label, std::size_t cap,
    MakeError &&make_error, Bind &&bind) {
    std::vector<Statement> bound;
    const auto *section = root.find(key);
    if (section == nullptr || !section->is_array()) {
        return make_error(ErrorCode::InvalidModelOutput,
                          std::string(label) + " output section must be an array: " +
                              std::string(key));
    }
    for (const auto &value : *section->as_array()) {
        if (bound.size() >= cap) {
            break;
        }
        const auto raw = parse_statement(value);
        if (!raw) {
            continue;
        }
        auto statement = bind(*raw);
        if (!statement) {
            continue;
        }
        bound.push_back(std::move(*statement));
    }
    return bound;
}

// ---------------------------------------------------------------------------
// Model request assembly
// ---------------------------------------------------------------------------

// Builds the text part every context surface sends: internal sensitivity by
// contract, because the text is Mira-side instruction or transcript render.
[[nodiscard]] inline TextPart make_text_part(std::string text) {
    TextPart part;
    part.text = std::move(text);
    part.sensitivity = Sensitivity::Internal;
    return part;
}

// Attaches the strict-JSON output contract for one model-backed context
// surface and pins the prompt provenance digests. `schema_id_hex` is the
// surface's hex schema id; `system_template` names the fixed instruction
// template whose digest closes the provenance chain.
inline void attach_output_contract(ModelRequest &request, const JsonSchema &schema,
                                   std::string_view schema_id_hex,
                                   std::string_view system_template) {
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id = SchemaId::parse(schema_id_hex).value_or(SchemaId{});
    request.output_contract.schema_version = SemanticVersion{1, 0, 0};
    request.output_contract.schema = schema;
    request.output_contract.canonical_schema_digest = canonical_json_digest(schema.root);
    request.prompt_provenance.system_template_digest = digest_string(system_template);
    request.prompt_provenance.decision_schema_digest =
        request.output_contract.canonical_schema_digest;
}

// The bounded single-request generation policy shared by both surfaces: one
// request, no storage retention.
inline void apply_generation_budget(ModelRequest &request, std::uint64_t max_output_tokens) {
    request.generation.max_output_tokens = max_output_tokens;
    request.budget.max_output_tokens = max_output_tokens;
    request.budget.max_requests = 1;
    request.data_policy.store = false;
}

} // namespace mira::context_support
