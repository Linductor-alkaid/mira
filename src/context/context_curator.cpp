#include <mira/context_curator.hpp>

#include "context_support.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace mira {
namespace {

using context_support::carries_marker;
using context_support::make_text_part;
using context_support::parse_statement;
using context_support::RawStatement;

// ---------------------------------------------------------------------------
// Error factory
// ---------------------------------------------------------------------------

[[nodiscard]] Error curator_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.curator";
    error.safe_message = std::move(message);
    return error;
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
constexpr const char *kSectionTags[8] = {"constraint",    "decision",      "issue",
                                         "active_task",   "verified_fact", "failed_attempt",
                                         "important_ref", "next_action"};

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
               << " working context refresh through sequence " << checkpoint.through_event_sequence
               << "; " << entries.size()
               << " numbered entries (previous snapshot, then new checkpoint, "
               << "then recent events):";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        transcript << "\n[" << index << entries[index].line;
    }
    return transcript.str();
}

[[nodiscard]] Result<std::string> response_text(const ModelResponse &response) {
    return context_support::message_response_text(response, curator_error, "curation");
}

// Binds one model statement to transcript provenance. Content bounds, marker
// filters and the confidence floor drop the statement; any citation outside
// the numbered inputs is fabricated provenance — dropped, never repaired
// (RULE-09). Provenance is the union of the cited entries' events (bounded,
// de-duplicated in citation order); the sequence is the smallest cited entry's.
[[nodiscard]] std::optional<WorkingContextItem>
bind_statement(const RawStatement &raw, const std::vector<TranscriptEntry> &entries,
               const ContextCurationOptions &options, bool *cites_previous) {
    const auto gated =
        context_support::gate_statement(raw, options.max_item_chars, options.min_confidence,
                                        options.forbidden_markers, options.injection_markers);
    if (!gated) {
        return std::nullopt;
    }
    WorkingContextItem bound;
    bound.content = raw.content;
    bound.confidence = *gated;
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
[[nodiscard]] Result<SectionItems> parse_section(const JsonValue &root, const char *key,
                                                 const std::vector<TranscriptEntry> &entries,
                                                 const ContextCurationOptions &options,
                                                 bool &cites_previous) {
    return context_support::parse_statement_section<WorkingContextItem>(
        root, key, "curation", options.max_items_per_section, curator_error,
        [&](const RawStatement &raw) {
            return bind_statement(raw, entries, options, &cites_previous);
        });
}

} // namespace

// ---------------------------------------------------------------------------
// Options and output schema
// ---------------------------------------------------------------------------

Result<void> ContextCurationOptions::validate() const {
    if (max_items_per_section == 0 || max_items_per_section > 1'024) {
        return curator_error(ErrorCode::InvalidArgument, "item bound per section is out of range");
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
        return curator_error(ErrorCode::InvalidArgument, "confidence floor must be within [0,1]");
    }
    if (deadline <= std::chrono::milliseconds::zero()) {
        return curator_error(ErrorCode::InvalidArgument, "curation deadline must be positive");
    }
    if (max_output_tokens == 0 || max_output_tokens > 1'000'000) {
        return curator_error(ErrorCode::InvalidArgument, "output token bound is out of range");
    }
    for (const auto &marker : forbidden_markers) {
        if (marker.empty()) {
            return curator_error(ErrorCode::InvalidArgument, "forbidden markers must not be empty");
        }
    }
    for (const auto &marker : injection_markers) {
        if (marker.empty()) {
            return curator_error(ErrorCode::InvalidArgument, "injection markers must not be empty");
        }
    }
    return Result<void>{};
}

JsonSchema working_context_curation_output_schema() {
    const JsonValue statement_array = context_support::statement_array_schema();
    JsonValue::Object root;
    root.emplace_back("type", std::string("object"));
    root.emplace_back("additionalProperties", false);
    root.emplace_back("required",
                      JsonValue::Array{std::string("confidence"), std::string("constraints"),
                                       std::string("decisions"), std::string("open_issues"),
                                       std::string("active_tasks"), std::string("verified_facts"),
                                       std::string("failed_attempts"),
                                       std::string("important_refs"), std::string("next_actions")});
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

ProviderContextCurator::ProviderContextCurator(IModelProvider &provider) : provider_(provider) {}
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
    context_support::attach_output_contract(request, schema, "6d6972612d6375726174696f6e2d7631",
                                            "mira.context.curator.system.v1");
    context_support::apply_generation_budget(request, options.max_output_tokens);

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
        return curator_error(ErrorCode::InvalidModelOutput, "curation model call did not complete");
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
        return curator_error(ErrorCode::InvalidModelOutput,
                             "curation output must be a JSON object");
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
        "constraints",    "decisions",       "open_issues",    "active_tasks",
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
    const std::string seed =
        checkpoint.session_id.to_string() + "|" + checkpoint.task_id.to_string() + "|" +
        std::to_string(checkpoint.task_epoch) + "|" + std::to_string(checkpoint.environment_epoch) +
        "|" + std::to_string(checkpoint.through_event_sequence);
    snapshot.id = working_context_snapshot_id_from_seed(seed);

    if (const auto valid = snapshot.validate(); !valid) {
        return valid.error();
    }
    return snapshot;
}

} // namespace mira
