#include <mira/context_consolidation.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace mira {
namespace {

// ---------------------------------------------------------------------------
// Error factory and untrusted-text filters
// ---------------------------------------------------------------------------

[[nodiscard]] Error consolidation_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.consolidation";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
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
[[nodiscard]] bool carries_marker(std::string_view text, const std::vector<std::string> &markers) {
    const std::string lowered = to_lower_ascii(text);
    for (const auto &marker : markers) {
        if (!marker.empty() && lowered.find(to_lower_ascii(marker)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::int64_t wall_nanos(const Timestamp &timestamp) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.wall.time_since_epoch())
            .count());
}

[[nodiscard]] std::int64_t monotonic_nanos(const Timestamp &timestamp) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.monotonic.time_since_epoch())
            .count());
}

[[nodiscard]] Timestamp timestamp_from_nanos(std::int64_t wall, std::int64_t monotonic) {
    Timestamp timestamp;
    timestamp.wall = std::chrono::system_clock::time_point(std::chrono::nanoseconds(wall));
    timestamp.monotonic = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(monotonic));
    return timestamp;
}

// ---------------------------------------------------------------------------
// Statement JSON (schema "mira.context.checkpoint.v1")
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue statement_to_json(const ConversationStatement &statement) {
    JsonValue::Object object;
    object.emplace_back("content", statement.content);
    JsonValue::Array events;
    for (const auto &event : statement.source_events) {
        events.emplace_back(event.to_string());
    }
    object.emplace_back("source_events", JsonValue(std::move(events)));
    object.emplace_back("source_sequence", static_cast<std::int64_t>(statement.source_sequence));
    object.emplace_back("confidence", statement.confidence);
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ConversationStatement>
statement_from_json(const JsonValue &json, std::string_view section) {
    const auto *object = json.as_object();
    if (object == nullptr) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "checkpoint statement must be an object in " +
                                       std::string(section));
    }
    ConversationStatement statement;
    const auto *content = json.find("content");
    if (content == nullptr || !content->is_string()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "checkpoint statement requires string content");
    }
    statement.content = *content->as_string();
    const auto *events = json.find("source_events");
    if (events != nullptr && events->is_array()) {
        for (const auto &event : *events->as_array()) {
            const auto *text = event.as_string();
            if (text == nullptr) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint statement event must be a string");
            }
            const auto parsed = EventId::parse(*text);
            if (!parsed) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint statement event id is malformed");
            }
            statement.source_events.push_back(*parsed);
        }
    }
    if (const auto *sequence = json.find("source_sequence"); sequence != nullptr) {
        const auto value = sequence->as_integer();
        if (!value || *value < 0) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "checkpoint statement sequence must be a non-negative "
                                       "integer");
        }
        statement.source_sequence = static_cast<SessionSequence>(*value);
    }
    if (const auto *confidence = json.find("confidence"); confidence != nullptr) {
        const auto value = confidence->as_number();
        if (!value || !std::isfinite(*value)) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "checkpoint statement confidence must be a finite number");
        }
        statement.confidence = std::clamp(*value, 0.0, 1.0);
    }
    return statement;
}

[[nodiscard]] JsonValue statements_to_json(const std::vector<ConversationStatement> &statements) {
    JsonValue::Array array;
    for (const auto &statement : statements) {
        array.emplace_back(statement_to_json(statement));
    }
    return JsonValue(std::move(array));
}

} // namespace

// ---------------------------------------------------------------------------
// Options and checkpoint contracts
// ---------------------------------------------------------------------------

Result<void> ConsolidationOptions::validate() const {
    if (max_statements_per_kind == 0 || max_statements_per_kind > 1'024) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "statement bound per kind is out of range");
    }
    if (max_preferences > 1'024) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "preference bound is out of range");
    }
    if (max_summary_chars > 64 * 1024) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "summary bound is out of range");
    }
    if (max_statement_chars < 16 || max_statement_chars > 8 * 1024) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "statement byte bound is out of range");
    }
    if (max_source_events == 0 || max_source_events > 4'096) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "source event bound is out of range");
    }
    if (!(min_confidence >= 0.0 && min_confidence <= 1.0)) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "confidence floor must be within [0,1]");
    }
    if (deadline <= std::chrono::milliseconds::zero()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "consolidation deadline must be positive");
    }
    if (max_output_tokens == 0 || max_output_tokens > 1'000'000) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "output token bound is out of range");
    }
    for (const auto &marker : forbidden_markers) {
        if (marker.empty()) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "forbidden markers must not be empty");
        }
    }
    for (const auto &marker : injection_markers) {
        if (marker.empty()) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "injection markers must not be empty");
        }
    }
    return Result<void>{};
}

ConversationCheckpointId conversation_checkpoint_id_from_seed(std::string_view seed) {
    const auto asset = context_asset_id_from_seed("mira.conversation.checkpoint|" + std::string(seed));
    return ConversationCheckpointId{asset.value};
}

Result<void> ConversationCheckpoint::validate() const {
    if (const auto supported = validate_schema_version(schema_version,
                                                       conversation_checkpoint_schema_current());
        !supported) {
        return supported.error();
    }
    if (id.is_nil()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "conversation checkpoint requires an id");
    }
    if (session_id.is_nil()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "conversation checkpoint requires a session");
    }
    if (through_event_sequence == 0) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "conversation checkpoint requires a positive watermark");
    }
    if (!(confidence >= 0.0 && confidence <= 1.0)) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "conversation checkpoint confidence must be within [0,1]");
    }
    const auto validate_statements = [this](const std::vector<ConversationStatement> &statements,
                                            std::string_view section) -> Result<void> {
        if (statements.size() > 1'024) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "checkpoint section " + std::string(section) +
                                           " exceeds the absolute statement bound");
        }
        for (const auto &statement : statements) {
            if (statement.content.empty() || statement.content.size() > 8 * 1024) {
                return consolidation_error(
                    ErrorCode::InvalidArgument,
                    "checkpoint statement content is empty or beyond the absolute byte bound");
            }
            if (statement.source_events.empty()) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint statements require provenance");
            }
            if (!(statement.confidence >= 0.0 && statement.confidence <= 1.0)) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint statement confidence must be within [0,1]");
            }
        }
        return Result<void>{};
    };
    if (const auto result = validate_statements(constraints, "constraints"); !result) {
        return result;
    }
    if (const auto result = validate_statements(decisions, "decisions"); !result) {
        return result;
    }
    if (const auto result = validate_statements(unresolved_threads, "unresolved_threads"); !result) {
        return result;
    }
    if (const auto result = validate_statements(preferences, "preferences"); !result) {
        return result;
    }
    if (source_events.size() > 4'096) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "checkpoint provenance exceeds the absolute bound");
    }
    return Result<void>{};
}

Hash ConversationCheckpoint::projection_digest() const {
    JsonValue::Object object;
    object.emplace_back("schema_version",
                        JsonValue::Object{{"major", static_cast<std::int64_t>(schema_version.major)},
                                          {"minor", static_cast<std::int64_t>(schema_version.minor)}});
    object.emplace_back("session_id", session_id.to_string());
    object.emplace_back("task_id", task_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(task_epoch));
    object.emplace_back("environment_epoch", static_cast<std::int64_t>(environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(through_event_sequence));
    object.emplace_back("constraints", statements_to_json(constraints));
    object.emplace_back("decisions", statements_to_json(decisions));
    object.emplace_back("unresolved_threads", statements_to_json(unresolved_threads));
    object.emplace_back("preferences", statements_to_json(preferences));
    JsonValue::Array events;
    for (const auto &event : source_events) {
        events.emplace_back(event.to_string());
    }
    object.emplace_back("source_events", JsonValue(std::move(events)));
    object.emplace_back("generated_by", generated_by.to_string());
    return canonical_json_digest(JsonValue(std::move(object)));
}

JsonValue conversation_checkpoint_to_json(const ConversationCheckpoint &checkpoint) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(checkpoint.schema_version.major)},
                          {"minor", static_cast<std::int64_t>(checkpoint.schema_version.minor)}});
    object.emplace_back("id", checkpoint.id.to_string());
    object.emplace_back("session_id", checkpoint.session_id.to_string());
    object.emplace_back("task_id", checkpoint.task_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(checkpoint.task_epoch));
    object.emplace_back("environment_epoch",
                        static_cast<std::int64_t>(checkpoint.environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(checkpoint.through_event_sequence));
    object.emplace_back("created_at", wall_nanos(checkpoint.created_at));
    object.emplace_back("created_at_monotonic", monotonic_nanos(checkpoint.created_at));
    object.emplace_back("summary", checkpoint.summary);
    object.emplace_back("constraints", statements_to_json(checkpoint.constraints));
    object.emplace_back("decisions", statements_to_json(checkpoint.decisions));
    object.emplace_back("unresolved_threads", statements_to_json(checkpoint.unresolved_threads));
    object.emplace_back("preferences", statements_to_json(checkpoint.preferences));
    JsonValue::Array events;
    for (const auto &event : checkpoint.source_events) {
        events.emplace_back(event.to_string());
    }
    object.emplace_back("source_events", JsonValue(std::move(events)));
    object.emplace_back("generated_by", checkpoint.generated_by.to_string());
    object.emplace_back("confidence", checkpoint.confidence);
    return JsonValue(std::move(object));
}

Result<ConversationCheckpoint>
conversation_checkpoint_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "conversation checkpoint payload must be an object");
    }
    ConversationCheckpoint checkpoint;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto major = schema->find("major");
        const auto minor = schema->find("minor");
        if (major != nullptr && minor != nullptr) {
            const auto major_value = major->as_integer();
            const auto minor_value = minor->as_integer();
            if (major_value && minor_value) {
                checkpoint.schema_version = SchemaVersion{
                    static_cast<std::uint16_t>(*major_value),
                    static_cast<std::uint16_t>(*minor_value)};
            }
        }
    }
    if (const auto supported = validate_schema_version(checkpoint.schema_version,
                                                       conversation_checkpoint_schema_current());
        !supported) {
        return supported.error();
    }
    const auto parse_id_field = [&json](const char *key, auto &target,
                                        const char *message) -> Result<void> {
        const auto *field = json.find(key);
        if (field == nullptr || !field->is_string()) {
            return consolidation_error(ErrorCode::InvalidArgument, message);
        }
        const auto parsed = std::decay_t<decltype(target)>::parse(*field->as_string());
        if (!parsed) {
            return consolidation_error(ErrorCode::InvalidArgument, message);
        }
        target = *parsed;
        return Result<void>{};
    };
    if (const auto result =
            parse_id_field("id", checkpoint.id, "conversation checkpoint id is malformed");
        !result) {
        return result.error();
    }
    if (const auto result = parse_id_field("session_id", checkpoint.session_id,
                                           "conversation checkpoint session is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_id_field("task_id", checkpoint.task_id, "conversation checkpoint task is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_id_field("generated_by", checkpoint.generated_by,
                           "conversation checkpoint generator profile is malformed");
        !result) {
        return result.error();
    }
    const auto parse_uint_field = [&json](const char *key, std::uint64_t &target,
                                          const char *message) -> Result<void> {
        const auto *field = json.find(key);
        if (field == nullptr) {
            return Result<void>{};
        }
        const auto value = field->as_integer();
        if (!value || *value < 0) {
            return consolidation_error(ErrorCode::InvalidArgument, message);
        }
        target = static_cast<std::uint64_t>(*value);
        return Result<void>{};
    };
    if (const auto result = parse_uint_field("task_epoch", checkpoint.task_epoch,
                                             "conversation checkpoint task epoch is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_uint_field("environment_epoch", checkpoint.environment_epoch,
                             "conversation checkpoint environment epoch is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_uint_field("through_event_sequence", checkpoint.through_event_sequence,
                             "conversation checkpoint watermark is malformed");
        !result) {
        return result.error();
    }
    if (const auto *created = json.find("created_at");
        created != nullptr && created->is_number()) {
        const auto *monotonic = json.find("created_at_monotonic");
        const auto wall = created->as_integer();
        const auto monotonic_value = monotonic != nullptr ? monotonic->as_integer() : std::nullopt;
        if (wall && monotonic_value) {
            checkpoint.created_at = timestamp_from_nanos(*wall, *monotonic_value);
        }
    }
    if (const auto *summary = json.find("summary"); summary != nullptr && summary->is_string()) {
        checkpoint.summary = *summary->as_string();
    }
    const auto parse_section = [&json](const char *key,
                                       std::vector<ConversationStatement> &target,
                                       const char *message) -> Result<void> {
        const auto *section = json.find(key);
        if (section == nullptr) {
            return Result<void>{};
        }
        const auto *array = section->as_array();
        if (array == nullptr) {
            return consolidation_error(ErrorCode::InvalidArgument, message);
        }
        target.reserve(array->size());
        for (const auto &entry : *array) {
            auto statement = statement_from_json(entry, key);
            if (!statement) {
                return statement.error();
            }
            target.push_back(std::move(statement).value());
        }
        return Result<void>{};
    };
    if (const auto result =
            parse_section("constraints", checkpoint.constraints, "constraints must be an array");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_section("decisions", checkpoint.decisions, "decisions must be an array");
        !result) {
        return result.error();
    }
    if (const auto result = parse_section("unresolved_threads", checkpoint.unresolved_threads,
                                          "unresolved_threads must be an array");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_section("preferences", checkpoint.preferences, "preferences must be an array");
        !result) {
        return result.error();
    }
    if (const auto *events = json.find("source_events"); events != nullptr && events->is_array()) {
        for (const auto &event : *events->as_array()) {
            const auto *text = event.as_string();
            if (text == nullptr) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint provenance must be event id strings");
            }
            const auto parsed = EventId::parse(*text);
            if (!parsed) {
                return consolidation_error(ErrorCode::InvalidArgument,
                                           "checkpoint provenance event id is malformed");
            }
            checkpoint.source_events.push_back(*parsed);
        }
    }
    if (const auto *confidence = json.find("confidence"); confidence != nullptr) {
        const auto value = confidence->as_number();
        if (!value || !std::isfinite(*value)) {
            return consolidation_error(ErrorCode::InvalidArgument,
                                       "checkpoint confidence must be a finite number");
        }
        checkpoint.confidence = std::clamp(*value, 0.0, 1.0);
    }
    if (const auto valid = checkpoint.validate(); !valid) {
        return valid.error();
    }
    return checkpoint;
}

// ---------------------------------------------------------------------------
// Model-backed reference consolidator
// ---------------------------------------------------------------------------

JsonSchema consolidation_output_schema() {
    const JsonValue statement = JsonValue::Object{
        {"type", std::string("object")},
        {"additionalProperties", false},
        {"required", JsonValue::Array{std::string("content"), std::string("sources"),
                                      std::string("confidence")}},
        {"properties",
         JsonValue::Object{
             {"content", JsonValue::Object{{"type", std::string("string")}}},
             {"sources",
              JsonValue::Object{{"type", std::string("array")},
                                {"items", JsonValue::Object{
                                              {"type", std::string("integer")},
                                              {"minimum", static_cast<std::int64_t>(0)}}}}},
             {"confidence", JsonValue::Object{{"type", std::string("number")}}},
         }},
    };
    const JsonValue statement_array = JsonValue::Object{
        {"type", std::string("array")}, {"items", statement}};
    JsonValue::Object root;
    root.emplace_back("type", std::string("object"));
    root.emplace_back("additionalProperties", false);
    root.emplace_back("required",
                      JsonValue::Array{std::string("summary"), std::string("confidence"),
                                       std::string("constraints"), std::string("decisions"),
                                       std::string("unresolved_threads"),
                                       std::string("preferences")});
    root.emplace_back(
        "properties",
        JsonValue::Object{
            {"summary", JsonValue::Object{{"type", std::string("string")}}},
            {"confidence", JsonValue::Object{{"type", std::string("number")}}},
            {"constraints", statement_array},
            {"decisions", statement_array},
            {"unresolved_threads", statement_array},
            {"preferences", statement_array},
        });
    JsonSchema schema;
    schema.root = JsonValue(std::move(root));
    return schema;
}

namespace {

// Fixed consolidation instruction (template "mira.context.consolidation.system.v1").
// The citation rule is what makes provenance binding mechanical: the model
// may only reference entries by their transcript number, and every statement
// must cite at least one.
[[nodiscard]] std::string consolidation_system_instruction() {
    return "You are Mira's conversation consolidator. Extract only what the "
           "presented conversation establishes: constraints the user imposed, "
           "decisions that were made, threads that are still unresolved, and "
           "preference candidates. Never invent content and never restate "
           "secrets (api keys, passwords, authorization headers). Cite every "
           "statement with the transcript entry numbers it came from in "
           "\"sources\". Respond with one JSON object matching the schema: "
           "\"summary\" (narrative), \"confidence\" (0..1), and the arrays "
           "\"constraints\", \"decisions\", \"unresolved_threads\", "
           "\"preferences\" where each item carries \"content\", \"sources\" "
           "and \"confidence\".";
}

// Renders the numbered transcript. Entry text already carries its role prefix
// ("user: "/"loop: "); the transcript adds stable zero-based citation numbers
// and the source sequence for traceability.
[[nodiscard]] std::string build_transcript(const ConversationSegment &segment) {
    std::ostringstream transcript;
    transcript << "Session " << segment.session.to_string() << " conversation prefix through "
               << "sequence " << segment.through_sequence << "; " << segment.entries.size()
               << " numbered entries:";
    for (std::size_t index = 0; index < segment.entries.size(); ++index) {
        transcript << "\n[" << index << "|seq=" << segment.entries[index].session_sequence
                   << "] " << segment.entries[index].text;
    }
    return transcript.str();
}

[[nodiscard]] Result<std::string> response_text(const ModelResponse &response) {
    std::string text;
    for (const auto &item : response.output) {
        const auto *message = std::get_if<MessageOutput>(&item);
        if (message == nullptr) {
            return consolidation_error(ErrorCode::InvalidModelOutput,
                                       "consolidation response must carry message outputs only");
        }
        for (const auto &part : message->content) {
            if (const auto *text_part = std::get_if<OutputTextPart>(&part)) {
                text += text_part->text;
            } else {
                return consolidation_error(ErrorCode::InvalidModelOutput,
                                           "consolidation response message carries a refusal");
            }
        }
    }
    if (text.empty()) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation response carried no text");
    }
    return text;
}

// Model statement as parsed: content plus transcript-number citations. The
// EventId binding happens in bind_statement so fabricated citations can be
// dropped before any provenance exists.
struct RawStatement final {
    std::string content;
    std::vector<std::int64_t> citations;
    double confidence = 0.0;
};

[[nodiscard]] std::optional<RawStatement> parse_statement(const JsonValue &value) {
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

// Binds one model statement to segment provenance. Content bounds, marker
// filters and the confidence floor drop the statement; any citation outside
// the input segment is fabricated provenance — dropped, never repaired
// (RULE-09).
[[nodiscard]] std::optional<ConversationStatement>
bind_statement(const RawStatement &raw, const ConversationSegment &segment,
               const ConsolidationOptions &options) {
    if (raw.content.empty() || raw.content.size() > options.max_statement_chars) {
        return std::nullopt;
    }
    if (carries_marker(raw.content, options.forbidden_markers) ||
        carries_marker(raw.content, options.injection_markers)) {
        return std::nullopt;
    }
    if (!std::isfinite(raw.confidence)) {
        return std::nullopt;
    }
    const double confidence = std::clamp(raw.confidence, 0.0, 1.0);
    if (confidence < options.min_confidence) {
        return std::nullopt;
    }
    ConversationStatement bound;
    bound.content = raw.content;
    bound.confidence = confidence;
    std::set<std::size_t> seen;
    SessionSequence smallest = 0;
    bool first = true;
    for (const auto citation : raw.citations) {
        if (citation < 0 || citation >= static_cast<std::int64_t>(segment.entries.size())) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(citation);
        if (!seen.insert(index).second) {
            continue;
        }
        bound.source_events.push_back(segment.entries[index].origin);
        const auto sequence = segment.entries[index].session_sequence;
        if (first || sequence < smallest) {
            smallest = sequence;
            first = false;
        }
    }
    if (bound.source_events.empty()) {
        return std::nullopt;
    }
    bound.source_sequence = smallest;
    return bound;
}

// Parses one output section, binds provenance and enforces the output bound
// (RULE-08): the first `cap` bound statements win, everything after is dropped.
[[nodiscard]] Result<std::vector<ConversationStatement>>
parse_section(const JsonValue &root, const char *key, const ConversationSegment &segment,
              const ConsolidationOptions &options, std::size_t cap) {
    std::vector<ConversationStatement> bound;
    const auto *section = root.find(key);
    if (section == nullptr || !section->is_array()) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation output section must be an array: " +
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
        auto statement = bind_statement(*raw, segment, options);
        if (!statement) {
            continue;
        }
        bound.push_back(std::move(*statement));
    }
    return bound;
}

[[nodiscard]] TextPart make_text_part(std::string text) {
    TextPart part;
    part.text = std::move(text);
    part.sensitivity = Sensitivity::Internal;
    return part;
}

} // namespace

ProviderSemanticConsolidator::ProviderSemanticConsolidator(IModelProvider &provider)
    : provider_(provider) {}
ProviderSemanticConsolidator::~ProviderSemanticConsolidator() = default;

Result<ConversationCheckpoint>
ProviderSemanticConsolidator::consolidate(const ConversationSegment &segment,
                                          const ConsolidationOptions &options) {
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    if (segment.session.is_nil()) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "consolidation requires a session");
    }
    if (segment.entries.empty() || segment.entries.size() != segment.entry_count) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "consolidation requires a non-empty segment with aligned "
                                   "entries");
    }
    if (options.cancelled()) {
        return consolidation_error(ErrorCode::Cancelled,
                                   "consolidation was cancelled before dispatch");
    }

    OperationContext context;
    context.session = segment.session;
    context.task = options.identity.task;
    context.task_epoch = options.identity.task_epoch;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + options.deadline;
    context.cancellation_requested = options.cancellation_requested;

    const auto schema = consolidation_output_schema();
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = context.operation;
    request.task_id = options.identity.task;
    request.task_epoch = options.identity.task_epoch;
    request.profile_id = provider_.profile().id;

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = "mira.context.consolidation.system.v1";
    system_item.authority = Sensitivity::Internal;
    system_item.content.emplace_back(make_text_part(consolidation_system_instruction()));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = "mira.context.consolidation.transcript.v1";
    user_item.authority = Sensitivity::Internal;
    user_item.content.emplace_back(make_text_part(build_transcript(segment)));

    request.input = {std::move(system_item), std::move(user_item)};
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id =
        SchemaId::parse("6d6972612d636f6e736f6c69642d7631").value_or(SchemaId{});
    request.output_contract.schema_version = SemanticVersion{1, 0, 0};
    request.output_contract.schema = schema;
    request.output_contract.canonical_schema_digest = canonical_json_digest(schema.root);

    request.generation.max_output_tokens = options.max_output_tokens;
    request.budget.max_output_tokens = options.max_output_tokens;
    request.budget.max_requests = 1;
    request.data_policy.store = false;
    request.prompt_provenance.system_template_digest =
        digest_string("mira.context.consolidation.system.v1");
    request.prompt_provenance.decision_schema_digest =
        request.output_contract.canonical_schema_digest;

    auto inferred = provider_.infer(request, context, ProviderInferOptions{});
    if (!inferred) {
        return inferred.error();
    }
    if (options.cancelled()) {
        return consolidation_error(ErrorCode::Cancelled,
                                   "consolidation was cancelled during the model call");
    }
    if (context.expired(Timestamp::now())) {
        return consolidation_error(ErrorCode::DeadlineExceeded,
                                   "consolidation deadline expired");
    }
    const ModelResponse &response = inferred.value();
    if (response.status != ModelCompletionStatus::Completed) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation model call did not complete");
    }

    auto text = response_text(response);
    if (!text) {
        return text.error();
    }
    auto parsed = parse_json(text.value());
    if (!parsed) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation output is not valid JSON");
    }
    const JsonValue &root = parsed.value();
    if (!root.is_object()) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation output must be a JSON object");
    }
    const auto *summary = root.find("summary");
    if (summary == nullptr || !summary->is_string()) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation output requires a string summary");
    }
    const auto *confidence = root.find("confidence");
    if (confidence == nullptr || !confidence->is_number()) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation output requires a numeric confidence");
    }
    const auto overall_raw = confidence->as_number();
    if (!overall_raw || !std::isfinite(*overall_raw)) {
        return consolidation_error(ErrorCode::InvalidModelOutput,
                                   "consolidation confidence must be a finite number");
    }

    auto constraints = parse_section(root, "constraints", segment, options,
                                     options.max_statements_per_kind);
    if (!constraints) {
        return constraints.error();
    }
    auto decisions =
        parse_section(root, "decisions", segment, options, options.max_statements_per_kind);
    if (!decisions) {
        return decisions.error();
    }
    auto threads = parse_section(root, "unresolved_threads", segment, options,
                                 options.max_statements_per_kind);
    if (!threads) {
        return threads.error();
    }
    auto preferences =
        parse_section(root, "preferences", segment, options, options.max_preferences);
    if (!preferences) {
        return preferences.error();
    }

    ConversationCheckpoint checkpoint;
    checkpoint.schema_version = conversation_checkpoint_schema_current();
    checkpoint.id = conversation_checkpoint_id_from_seed(
        segment.session.to_string() + "|" + std::to_string(segment.through_sequence));
    checkpoint.session_id = segment.session;
    checkpoint.task_id = options.identity.task;
    checkpoint.task_epoch = options.identity.task_epoch;
    checkpoint.environment_epoch = options.identity.environment_epoch;
    checkpoint.through_event_sequence = segment.through_sequence;
    checkpoint.created_at = Timestamp::now();
    checkpoint.summary = summary->as_string()->substr(
        0, options.max_summary_chars); // narrative is non-authoritative; safe to truncate
    checkpoint.constraints = std::move(constraints).value();
    checkpoint.decisions = std::move(decisions).value();
    checkpoint.unresolved_threads = std::move(threads).value();
    checkpoint.preferences = std::move(preferences).value();
    std::set<EventId> seen_events;
    const auto collect = [&seen_events, &checkpoint, &options](
                             const std::vector<ConversationStatement> &statements) {
        for (const auto &statement : statements) {
            for (const auto &event : statement.source_events) {
                if (checkpoint.source_events.size() >= options.max_source_events) {
                    return;
                }
                if (seen_events.insert(event).second) {
                    checkpoint.source_events.push_back(event);
                }
            }
        }
    };
    collect(checkpoint.constraints);
    collect(checkpoint.decisions);
    collect(checkpoint.unresolved_threads);
    collect(checkpoint.preferences);
    checkpoint.generated_by = provider_.profile().id;
    checkpoint.confidence = std::clamp(*overall_raw, 0.0, 1.0);

    if (const auto valid = checkpoint.validate(); !valid) {
        return valid.error();
    }
    return checkpoint;
}

// ---------------------------------------------------------------------------
// Checkpoint store
// ---------------------------------------------------------------------------

Result<void> ConversationCheckpointStorePolicy::validate() const {
    if (max_checkpoints_per_session == 0 || max_checkpoints_per_session > 256) {
        return consolidation_error(ErrorCode::InvalidArgument,
                                   "checkpoint retention bound is out of range");
    }
    return Result<void>{};
}

class InMemoryConversationCheckpointStore::Impl final {
  public:
    explicit Impl(ConversationCheckpointStorePolicy policy) : policy_(policy) {}

    [[nodiscard]] Result<void> put(const ConversationCheckpoint &checkpoint) {
        if (const auto valid = checkpoint.validate(); !valid) {
            return valid.error();
        }
        std::lock_guard lock(mutex_);
        auto &ring = checkpoints_[checkpoint.session_id];
        if (!ring.empty() &&
            checkpoint.through_event_sequence < ring.back().through_event_sequence) {
            return consolidation_error(ErrorCode::InvalidState,
                                       "checkpoint watermark regresses behind the stored one");
        }
        ring.push_back(checkpoint);
        if (ring.size() > policy_.max_checkpoints_per_session) {
            ring.erase(ring.begin());
        }
        return Result<void>{};
    }

    [[nodiscard]] Result<std::optional<ConversationCheckpoint>> latest(SessionId session) const {
        std::lock_guard lock(mutex_);
        const auto found = checkpoints_.find(session);
        if (found == checkpoints_.end() || found->second.empty()) {
            return std::optional<ConversationCheckpoint>{};
        }
        return Result<std::optional<ConversationCheckpoint>>(found->second.back());
    }

    [[nodiscard]] Result<std::optional<ConversationCheckpoint>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const {
        std::lock_guard lock(mutex_);
        const auto found = checkpoints_.find(session);
        if (found == checkpoints_.end()) {
            return std::optional<ConversationCheckpoint>{};
        }
        const auto &ring = found->second;
        std::optional<ConversationCheckpoint> result;
        for (const auto &checkpoint : ring) {
            if (checkpoint.through_event_sequence <= max_sequence) {
                result = checkpoint;
            }
        }
        return Result<std::optional<ConversationCheckpoint>>(std::move(result));
    }

    [[nodiscard]] Result<std::size_t> count(SessionId session) const {
        std::lock_guard lock(mutex_);
        const auto found = checkpoints_.find(session);
        return found == checkpoints_.end() ? std::size_t{0} : found->second.size();
    }

    [[nodiscard]] Result<std::size_t> erase_session(SessionId session, const std::string &reason) {
        (void)reason; // audit surface is host-side; the reason documents intent
        std::lock_guard lock(mutex_);
        const auto found = checkpoints_.find(session);
        if (found == checkpoints_.end()) {
            return std::size_t{0};
        }
        const std::size_t erased = found->second.size();
        checkpoints_.erase(found);
        return erased;
    }

  private:
    ConversationCheckpointStorePolicy policy_;
    mutable std::mutex mutex_;
    std::map<SessionId, std::vector<ConversationCheckpoint>> checkpoints_;
};

InMemoryConversationCheckpointStore::InMemoryConversationCheckpointStore(
    ConversationCheckpointStorePolicy policy)
    : impl_(std::make_unique<Impl>(policy)) {}
InMemoryConversationCheckpointStore::~InMemoryConversationCheckpointStore() = default;

Result<void> InMemoryConversationCheckpointStore::put(const ConversationCheckpoint &checkpoint) {
    return impl_->put(checkpoint);
}

Result<std::optional<ConversationCheckpoint>>
InMemoryConversationCheckpointStore::latest(SessionId session) const {
    return impl_->latest(session);
}

Result<std::optional<ConversationCheckpoint>>
InMemoryConversationCheckpointStore::latest_at_or_before(SessionId session,
                                                         std::uint64_t max_sequence) const {
    return impl_->latest_at_or_before(session, max_sequence);
}

Result<std::size_t> InMemoryConversationCheckpointStore::count(SessionId session) const {
    return impl_->count(session);
}

Result<std::size_t>
InMemoryConversationCheckpointStore::erase_session(SessionId session, std::string reason) {
    return impl_->erase_session(session, reason); // reason is audit metadata only
}

// ---------------------------------------------------------------------------
// Commit validation (design §6.2)
// ---------------------------------------------------------------------------

std::string conversation_commit_disposition_name(ConversationCommitDisposition disposition) {
    switch (disposition) {
    case ConversationCommitDisposition::Committed:
        return "Committed";
    case ConversationCommitDisposition::IdempotentNoOp:
        return "IdempotentNoOp";
    case ConversationCommitDisposition::DiscardedStale:
        return "DiscardedStale";
    case ConversationCommitDisposition::DiscardedTerminal:
        return "DiscardedTerminal";
    }
    return "Unknown";
}

namespace {

[[nodiscard]] ConversationCommitOutcome discard(ConversationCommitDisposition disposition,
                                                std::string reason_code) {
    ConversationCommitOutcome outcome;
    outcome.disposition = disposition;
    outcome.reason_code = std::move(reason_code);
    return outcome;
}

} // namespace

ConversationCommitOutcome
commit_conversation_checkpoint(IConversationCheckpointStore &store,
                               const ConversationCheckpoint &candidate,
                               const ConversationCommitState &live) {
    if (const auto valid = candidate.validate(); !valid) {
        return discard(ConversationCommitDisposition::DiscardedStale, "invalid-candidate");
    }
    // Terminal idempotency first: results arriving after the session or task
    // went terminal are dropped, never committed (design §6.2).
    if (live.session_terminal) {
        return discard(ConversationCommitDisposition::DiscardedTerminal, "session-terminal");
    }
    if (live.task_terminal) {
        return discard(ConversationCommitDisposition::DiscardedTerminal, "task-terminal");
    }
    // Five-tuple validation: any mismatch discards the candidate and keeps
    // the stored checkpoint.
    if (candidate.session_id != live.session) {
        return discard(ConversationCommitDisposition::DiscardedStale, "session-mismatch");
    }
    if (candidate.task_id != live.task) {
        return discard(ConversationCommitDisposition::DiscardedStale, "task-mismatch");
    }
    if (candidate.task_epoch != live.task_epoch) {
        return discard(ConversationCommitDisposition::DiscardedStale, "task-epoch-mismatch");
    }
    if (candidate.environment_epoch != live.environment_epoch) {
        return discard(ConversationCommitDisposition::DiscardedStale,
                       "environment-epoch-mismatch");
    }
    const auto stored = store.latest(candidate.session_id);
    if (!stored) {
        return discard(ConversationCommitDisposition::DiscardedStale, "store-unavailable");
    }
    if (stored.value().has_value()) {
        const auto &existing = *stored.value();
        if (candidate.through_event_sequence < existing.through_event_sequence) {
            return discard(ConversationCommitDisposition::DiscardedStale, "stale-watermark");
        }
        if (candidate.through_event_sequence == existing.through_event_sequence) {
            if (candidate.projection_digest() == existing.projection_digest()) {
                return discard(ConversationCommitDisposition::IdempotentNoOp,
                               "idempotent-replay");
            }
            return discard(ConversationCommitDisposition::DiscardedStale,
                           "conflicting-watermark");
        }
    }
    const auto put = store.put(candidate);
    if (!put) {
        return discard(ConversationCommitDisposition::DiscardedStale, "store-rejected");
    }
    ConversationCommitOutcome outcome;
    outcome.disposition = ConversationCommitDisposition::Committed;
    outcome.reason_code = "committed";
    outcome.committed = candidate;
    return outcome;
}

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

std::vector<ContextItem> context_items_from_checkpoint(const ConversationCheckpoint &checkpoint) {
    std::vector<ContextItem> items;
    std::size_t index = 0;
    const auto push_statement = [&](const ConversationStatement &statement,
                                    ContextItemKind kind, std::string_view tag) {
        ContextItem item;
        const std::string seed = "mira.context.item|" + checkpoint.id.to_string() + "|" +
                                 std::string(tag) + "|" + std::to_string(index);
        item.id = ContextItemId{context_asset_id_from_seed(seed).value};
        item.kind = kind;
        // Model-mediated derived projection: never SystemPolicy, never
        // VerifiedState (RULE-09; DEC-032 §2/§3). Layer 0 stays the admission
        // authority and its partition rules decide what enters the request.
        item.authority = ContextAuthority::UntrustedExternalData;
        item.priority = ContextPriority::Normal;
        item.content.emplace_back(make_text_part(statement.content));
        item.provenance = statement.source_events;
        item.sequence = statement.source_sequence;
        item.consumed = true;
        item.task_epoch = checkpoint.task_epoch;
        item.environment_epoch = checkpoint.environment_epoch;
        items.push_back(std::move(item));
        ++index;
    };
    for (const auto &statement : checkpoint.constraints) {
        // Design §5.4: consolidated constraints enter as P1-level candidates.
        push_statement(statement, ContextItemKind::UserConstraint, "constraint");
    }
    for (const auto &statement : checkpoint.decisions) {
        push_statement(statement, ContextItemKind::CheckpointSummary, "decision");
    }
    for (const auto &statement : checkpoint.unresolved_threads) {
        push_statement(statement, ContextItemKind::CheckpointSummary, "thread");
    }
    // The narrative summary is deliberately not converted: it is
    // non-authoritative and would only spend Layer 0 budget.
    // Preference candidates are not converted: promotion to Memory runs
    // through the existing human-approval pipeline.
    return items;
}

} // namespace mira
