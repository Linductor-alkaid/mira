#include <mira/context_curator.hpp>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace mira {
namespace {

// ---------------------------------------------------------------------------
// Error factory and untrusted-text filters (M19 consolidation semantics)
// ---------------------------------------------------------------------------

[[nodiscard]] Error curator_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.curator";
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

// ---------------------------------------------------------------------------
// Numbered transcript inputs
// ---------------------------------------------------------------------------

// One numbered transcript entry. `block` drives the degenerate-merge guard:
// with a non-empty previous snapshot, a candidate whose bound items cite no
// previous-block entry is rejected outright (M21 plan §4.1).
enum class TranscriptBlock : std::uint8_t { Previous, Checkpoint, Event };

struct TranscriptEntry final {
    std::string line; // rendered "[i|<block>:<tag>|seq=...] <text>" minus the index
    std::vector<EventId> events;
    SessionSequence sequence = 0;
    TranscriptBlock block = TranscriptBlock::Event;
};

using SectionItems = std::vector<WorkingContextItem>;

// Fixed section order shared by the transcript rendering, the model output
// keys and the candidate assembly.
constexpr const char *kSectionTags[8] = {"constraint", "decision", "issue", "active_task",
                                         "verified_fact", "failed_attempt", "important_ref",
                                         "next_action"};

struct PreviousSections final {
    const SectionItems *sections[8];
};
[[nodiscard]] PreviousSections previous_sections(const WorkingContextSnapshot &snapshot) {
    return PreviousSections{{&snapshot.constraints, &snapshot.decisions, &snapshot.open_issues,
                             &snapshot.active_tasks, &snapshot.verified_facts,
                             &snapshot.failed_attempts, &snapshot.important_refs,
                             &snapshot.next_actions}};
}

// `WorkingContextItem` and `ConversationStatement` share the statement shape
// but are distinct types; the template covers the previous-snapshot block and
// the checkpoint block alike.
template <typename Item>
void append_section_entries(std::vector<TranscriptEntry> &entries, const std::vector<Item> &items,
                            TranscriptBlock block, std::string_view tag) {
    for (const auto &item : items) {
        TranscriptEntry entry;
        entry.line = std::string("|") + std::string(tag) +
                     "|seq=" + std::to_string(item.source_sequence) + "] " + item.content;
        entry.events = item.source_events;
        entry.sequence = item.source_sequence;
        entry.block = block;
        entries.push_back(std::move(entry));
    }
}

[[nodiscard]] std::vector<TranscriptEntry>
build_transcript_entries(const WorkingContextSnapshot *previous,
                         const ConversationCheckpoint &checkpoint,
                         std::span<const ConversationSegmentEntry> recent_events) {
    std::vector<TranscriptEntry> entries;
    if (previous != nullptr) {
        const auto sections = previous_sections(*previous);
        for (std::size_t section = 0; section < 8; ++section) {
            append_section_entries(entries, *sections.sections[section], TranscriptBlock::Previous,
                                   std::string("prev:") + kSectionTags[section]);
        }
    }
    append_section_entries(entries, checkpoint.constraints, TranscriptBlock::Checkpoint,
                           "ckpt:constraint");
    append_section_entries(entries, checkpoint.decisions, TranscriptBlock::Checkpoint,
                           "ckpt:decision");
    append_section_entries(entries, checkpoint.unresolved_threads, TranscriptBlock::Checkpoint,
                           "ckpt:thread");
    // Preferences are deliberately not presented: they never enter the
    // snapshot, they wait for the MemoryConsolidator human-approval pipeline
    // (design §4.2).
    for (const auto &event : recent_events) {
        TranscriptEntry entry;
        entry.line = "|event|seq=" + std::to_string(event.session_sequence) + "] " + event.text;
        entry.events.push_back(event.origin);
        entry.sequence = event.session_sequence;
        entry.block = TranscriptBlock::Event;
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Fixed curator instruction (template "mira.context.curator.system.v1"). The
// citation rule is what makes provenance binding mechanical; the merge
// vocabulary (retain / supersede / conflict-retain) is the frozen instruction
// contract whose mechanical shadow is the previous-citation guard.
[[nodiscard]] std::string curator_system_instruction() {
    return "You are Mira's working context curator. You maintain the standing "
           "state view of one task: carry forward what still holds from the "
           "previous snapshot by citing its entry, supersede a stale entry by "
           "citing both the entry it replaces and the new evidence, and keep "
           "conflicting variants side by side when resolution is not yours to "
           "make. Fill the eight sections from what the presented inputs "
           "establish; never invent content and never restate secrets (api "
           "keys, passwords, authorization headers). Cite every statement "
           "with the transcript entry numbers it came from in \"sources\". "
           "Respond with one JSON object matching the schema: \"confidence\" "
           "(0..1, your overall confidence in this revision) and the arrays "
           "\"constraints\", \"decisions\", \"open_issues\", \"active_tasks\", "
           "\"verified_facts\", \"failed_attempts\", \"important_refs\", "
           "\"next_actions\" where each item carries \"content\", \"sources\" "
           "and \"confidence\".";
}

[[nodiscard]] std::string build_transcript(const ConversationCheckpoint &checkpoint,
                                           const std::vector<TranscriptEntry> &entries) {
    std::ostringstream transcript;
    transcript << "Session " << checkpoint.session_id.to_string()
               << " working context refresh through sequence "
               << checkpoint.through_event_sequence << "; " << entries.size()
               << " numbered entries (previous snapshot, then new checkpoint, "
               << "then recent events):";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        transcript << "\n[" << index << entries[index].line;
    }
    return transcript.str();
}

[[nodiscard]] Result<std::string> response_text(const ModelResponse &response) {
    std::string text;
    for (const auto &item : response.output) {
        const auto *message = std::get_if<MessageOutput>(&item);
        if (message == nullptr) {
            return curator_error(ErrorCode::InvalidModelOutput,
                                 "curation response must carry message outputs only");
        }
        for (const auto &part : message->content) {
            if (const auto *text_part = std::get_if<OutputTextPart>(&part)) {
                text += text_part->text;
            } else {
                return curator_error(ErrorCode::InvalidModelOutput,
                                     "curation response message carries a refusal");
            }
        }
    }
    if (text.empty()) {
        return curator_error(ErrorCode::InvalidModelOutput, "curation response carried no text");
    }
    return text;
}

// Model statement as parsed: content plus transcript-number citations. The
// provenance binding happens in bind_statement so fabricated citations can be
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

// Binds one model statement to transcript provenance. Content bounds, marker
// filters and the confidence floor drop the statement; any citation outside
// the numbered inputs is fabricated provenance — dropped, never repaired
// (RULE-09). Provenance is the union of the cited entries' events (bounded,
// de-duplicated in citation order); the sequence is the smallest cited entry's.
[[nodiscard]] std::optional<WorkingContextItem>
bind_statement(const RawStatement &raw, const std::vector<TranscriptEntry> &entries,
               const ContextCurationOptions &options, bool *cites_previous) {
    if (raw.content.empty() || raw.content.size() > options.max_item_chars) {
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
    WorkingContextItem bound;
    bound.content = raw.content;
    bound.confidence = confidence;
    std::set<std::size_t> seen;
    SessionSequence smallest = 0;
    bool first = true;
    for (const auto citation : raw.citations) {
        if (citation < 0 || citation >= static_cast<std::int64_t>(entries.size())) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(citation);
        if (!seen.insert(index).second) {
            continue;
        }
        const auto &entry = entries[index];
        if (entry.block == TranscriptBlock::Previous) {
            *cites_previous = true;
        }
        for (const auto &event : entry.events) {
            if (bound.source_events.size() >= options.max_source_events) {
                return std::nullopt;
            }
            bound.source_events.push_back(event);
        }
        if (first || entry.sequence < smallest) {
            smallest = entry.sequence;
            first = false;
        }
    }
    if (bound.source_events.empty()) {
        return std::nullopt;
    }
    // Provenance is a set: de-duplicate while preserving citation order.
    std::set<EventId> unique_events;
    std::vector<EventId> deduped;
    for (const auto &event : bound.source_events) {
        if (unique_events.insert(event).second) {
            deduped.push_back(event);
        }
    }
    bound.source_events = std::move(deduped);
    bound.source_sequence = smallest;
    return bound;
}

// Parses one output section, binds provenance and enforces the output bound
// (RULE-08): the first `cap` bound statements win, everything after is dropped.
[[nodiscard]] Result<SectionItems>
parse_section(const JsonValue &root, const char *key, const std::vector<TranscriptEntry> &entries,
              const ContextCurationOptions &options, bool &cites_previous) {
    SectionItems bound;
    const auto *section = root.find(key);
    if (section == nullptr || !section->is_array()) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation output section must be an array: " + std::string(key));
    }
    for (const auto &value : *section->as_array()) {
        if (bound.size() >= options.max_items_per_section) {
            break;
        }
        const auto raw = parse_statement(value);
        if (!raw) {
            continue;
        }
        auto statement = bind_statement(*raw, entries, options, &cites_previous);
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

// ---------------------------------------------------------------------------
// Options and output schema
// ---------------------------------------------------------------------------

Result<void> ContextCurationOptions::validate() const {
    if (max_items_per_section == 0 || max_items_per_section > 1'024) {
        return curator_error(ErrorCode::InvalidArgument,
                             "item bound per section is out of range");
    }
    if (max_item_chars < 16 || max_item_chars > 8 * 1024) {
        return curator_error(ErrorCode::InvalidArgument, "item byte bound is out of range");
    }
    if (max_source_events == 0 || max_source_events > 4'096) {
        return curator_error(ErrorCode::InvalidArgument, "source event bound is out of range");
    }
    if (max_recent_events == 0 || max_recent_events > 4'096) {
        return curator_error(ErrorCode::InvalidArgument, "recent event bound is out of range");
    }
    if (!(min_confidence >= 0.0 && min_confidence <= 1.0)) {
        return curator_error(ErrorCode::InvalidArgument,
                             "confidence floor must be within [0,1]");
    }
    if (deadline <= std::chrono::milliseconds::zero()) {
        return curator_error(ErrorCode::InvalidArgument, "curation deadline must be positive");
    }
    if (max_output_tokens == 0 || max_output_tokens > 1'000'000) {
        return curator_error(ErrorCode::InvalidArgument, "output token bound is out of range");
    }
    for (const auto &marker : forbidden_markers) {
        if (marker.empty()) {
            return curator_error(ErrorCode::InvalidArgument,
                                 "forbidden markers must not be empty");
        }
    }
    for (const auto &marker : injection_markers) {
        if (marker.empty()) {
            return curator_error(ErrorCode::InvalidArgument,
                                 "injection markers must not be empty");
        }
    }
    return Result<void>{};
}

JsonSchema working_context_curation_output_schema() {
    const JsonValue statement = JsonValue::Object{
        {"type", std::string("object")},
        {"additionalProperties", false},
        {"required",
         JsonValue::Array{std::string("content"), std::string("sources"),
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
                      JsonValue::Array{std::string("confidence"), std::string("constraints"),
                                       std::string("decisions"), std::string("open_issues"),
                                       std::string("active_tasks"), std::string("verified_facts"),
                                       std::string("failed_attempts"), std::string("important_refs"),
                                       std::string("next_actions")});
    JsonValue::Object properties;
    properties.emplace_back("confidence", JsonValue::Object{{"type", std::string("number")}});
    for (const char *key :
         {"constraints", "decisions", "open_issues", "active_tasks", "verified_facts",
          "failed_attempts", "important_refs", "next_actions"}) {
        properties.emplace_back(key, statement_array);
    }
    root.emplace_back("properties", JsonValue(std::move(properties)));
    JsonSchema schema;
    schema.root = JsonValue(std::move(root));
    return schema;
}

// ---------------------------------------------------------------------------
// Model-backed reference curator
// ---------------------------------------------------------------------------

ProviderContextCurator::ProviderContextCurator(IModelProvider &provider)
    : provider_(provider) {}
ProviderContextCurator::~ProviderContextCurator() = default;

Result<WorkingContextSnapshot>
ProviderContextCurator::curate(const WorkingContextSnapshot *previous,
                               const ConversationCheckpoint &checkpoint,
                               std::span<const ConversationSegmentEntry> recent_events,
                               const ContextCurationOptions &options) {
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = checkpoint.validate(); !valid) {
        return valid.error();
    }
    if (recent_events.size() > options.max_recent_events) {
        return curator_error(ErrorCode::InvalidArgument,
                             "curation recent events exceed the input bound");
    }
    for (const auto &event : recent_events) {
        // The candidate watermark equals the checkpoint's: inputs reaching
        // past it would make the same identity describe different content,
        // which is a same-watermark conflict by construction (design §5.2).
        if (event.session_sequence > checkpoint.through_event_sequence) {
            return curator_error(ErrorCode::InvalidArgument,
                                 "curation recent event runs past the checkpoint watermark");
        }
    }
    if (previous != nullptr) {
        if (const auto valid = previous->validate(); !valid) {
            return valid.error();
        }
        // The previous snapshot must belong to the same identity chain: an
        // epoch bump starts a fresh chain with previous = null (M21 §4.1).
        if (previous->session_id != checkpoint.session_id ||
            previous->task_id != checkpoint.task_id ||
            previous->task_epoch != checkpoint.task_epoch ||
            previous->environment_epoch != checkpoint.environment_epoch) {
            return curator_error(ErrorCode::InvalidArgument,
                                 "curation previous snapshot identity does not match the "
                                 "checkpoint");
        }
        if (previous->through_event_sequence > checkpoint.through_event_sequence) {
            return curator_error(ErrorCode::InvalidArgument,
                                 "curation previous snapshot is ahead of the checkpoint");
        }
    }
    if (options.cancelled()) {
        return curator_error(ErrorCode::Cancelled, "curation was cancelled before dispatch");
    }

    const auto entries = build_transcript_entries(previous, checkpoint, recent_events);

    OperationContext context;
    context.session = checkpoint.session_id;
    context.task = checkpoint.task_id;
    context.task_epoch = checkpoint.task_epoch;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + options.deadline;
    context.cancellation_requested = options.cancellation_requested;

    const auto schema = working_context_curation_output_schema();
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = context.operation;
    request.task_id = checkpoint.task_id;
    request.task_epoch = checkpoint.task_epoch;
    request.profile_id = provider_.profile().id;

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = "mira.context.curator.system.v1";
    system_item.authority = Sensitivity::Internal;
    system_item.content.emplace_back(make_text_part(curator_system_instruction()));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = "mira.context.curator.transcript.v1";
    user_item.authority = Sensitivity::Internal;
    user_item.content.emplace_back(make_text_part(build_transcript(checkpoint, entries)));

    request.input = {std::move(system_item), std::move(user_item)};
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id =
        SchemaId::parse("6d6972612d6375726174696f6e2d7631").value_or(SchemaId{});
    request.output_contract.schema_version = SemanticVersion{1, 0, 0};
    request.output_contract.schema = schema;
    request.output_contract.canonical_schema_digest = canonical_json_digest(schema.root);

    request.generation.max_output_tokens = options.max_output_tokens;
    request.budget.max_output_tokens = options.max_output_tokens;
    request.budget.max_requests = 1;
    request.data_policy.store = false;
    request.prompt_provenance.system_template_digest =
        digest_string("mira.context.curator.system.v1");
    request.prompt_provenance.decision_schema_digest =
        request.output_contract.canonical_schema_digest;

    auto inferred = provider_.infer(request, context, ProviderInferOptions{});
    if (!inferred) {
        return inferred.error();
    }
    if (options.cancelled()) {
        return curator_error(ErrorCode::Cancelled, "curation was cancelled during the model call");
    }
    if (context.expired(Timestamp::now())) {
        return curator_error(ErrorCode::DeadlineExceeded, "curation deadline expired");
    }
    const ModelResponse &response = inferred.value();
    if (response.status != ModelCompletionStatus::Completed) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation model call did not complete");
    }

    auto text = response_text(response);
    if (!text) {
        return text.error();
    }
    auto parsed = parse_json(text.value());
    if (!parsed) {
        return curator_error(ErrorCode::InvalidModelOutput, "curation output is not valid JSON");
    }
    const JsonValue &root = parsed.value();
    if (!root.is_object()) {
        return curator_error(ErrorCode::InvalidModelOutput, "curation output must be a JSON object");
    }
    const auto *confidence = root.find("confidence");
    if (confidence == nullptr || !confidence->is_number()) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation output requires a numeric confidence");
    }
    const auto overall_raw = confidence->as_number();
    if (!overall_raw || !std::isfinite(*overall_raw)) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation confidence must be a finite number");
    }
    const double overall = std::clamp(*overall_raw, 0.0, 1.0);
    if (overall < options.min_confidence) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation confidence is below the configured floor");
    }

    bool cites_previous = false;
    std::vector<SectionItems> sections(8);
    static constexpr const char *kOutputKeys[8] = {
        "constraints", "decisions", "open_issues", "active_tasks",
        "verified_facts", "failed_attempts", "important_refs", "next_actions"};
    for (std::size_t section = 0; section < 8; ++section) {
        auto bound = parse_section(root, kOutputKeys[section], entries, options, cites_previous);
        if (!bound) {
            return bound.error();
        }
        sections[section] = std::move(bound).value();
    }

    // Degenerate-merge guard (M21 §4.1): with a non-empty previous snapshot,
    // a candidate that cites no previous entry would overwrite the rich
    // snapshot with fresh content and no supersede discipline — reject the
    // whole candidate; the caller keeps the previous snapshot.
    bool previous_has_items = false;
    if (previous != nullptr) {
        const auto previous_all = previous_sections(*previous);
        for (std::size_t section = 0; section < 8; ++section) {
            if (!previous_all.sections[section]->empty()) {
                previous_has_items = true;
                break;
            }
        }
    }
    if (previous_has_items && !cites_previous) {
        return curator_error(ErrorCode::InvalidModelOutput,
                             "degenerate-merge: curation output cites no previous snapshot "
                             "entry while one was supplied");
    }

    WorkingContextSnapshot snapshot;
    snapshot.schema_version = working_context_schema_current();
    snapshot.session_id = checkpoint.session_id;
    snapshot.task_id = checkpoint.task_id;
    snapshot.task_epoch = checkpoint.task_epoch;
    snapshot.environment_epoch = checkpoint.environment_epoch;
    snapshot.through_event_sequence = checkpoint.through_event_sequence;
    if (previous != nullptr) {
        snapshot.source_checkpoints = previous->source_checkpoints;
    }
    snapshot.source_checkpoints.push_back(checkpoint.id);
    // De-duplicate (defensive; ids derive from distinct watermarks) and keep
    // the most recent 64 — the absolute validate() bound.
    {
        std::set<ConversationCheckpointId> seen;
        std::vector<ConversationCheckpointId> deduped;
        for (auto it = snapshot.source_checkpoints.rbegin();
             it != snapshot.source_checkpoints.rend(); ++it) {
            if (seen.insert(*it).second) {
                deduped.push_back(*it);
            }
        }
        std::reverse(deduped.begin(), deduped.end());
        if (deduped.size() > 64) {
            deduped.erase(deduped.begin(), deduped.end() - 64);
        }
        snapshot.source_checkpoints = std::move(deduped);
    }
    snapshot.created_at = Timestamp::now();
    snapshot.generated_by = provider_.profile().id;
    snapshot.constraints = std::move(sections[0]);
    snapshot.decisions = std::move(sections[1]);
    snapshot.open_issues = std::move(sections[2]);
    snapshot.active_tasks = std::move(sections[3]);
    snapshot.verified_facts = std::move(sections[4]);
    snapshot.failed_attempts = std::move(sections[5]);
    snapshot.important_refs = std::move(sections[6]);
    snapshot.next_actions = std::move(sections[7]);
    const std::string seed = checkpoint.session_id.to_string() + "|" +
                             checkpoint.task_id.to_string() + "|" +
                             std::to_string(checkpoint.task_epoch) + "|" +
                             std::to_string(checkpoint.environment_epoch) + "|" +
                             std::to_string(checkpoint.through_event_sequence);
    snapshot.id = working_context_snapshot_id_from_seed(seed);

    if (const auto valid = snapshot.validate(); !valid) {
        return valid.error();
    }
    return snapshot;
}

} // namespace mira
