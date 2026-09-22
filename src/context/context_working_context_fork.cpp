#include <mira/context_working_context_fork.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace mira {
namespace {

// ---------------------------------------------------------------------------
// Error factory and section plumbing
// ---------------------------------------------------------------------------

[[nodiscard]] Error fork_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.working_context";
    error.safe_message = std::move(message);
    return error;
}

constexpr std::size_t kSectionCount = 8;

struct SectionRef final {
    const char *name;
    const std::vector<WorkingContextItem> *items;
};

// The frozen eight-section table in struct declaration order (M24 §4.2) —
// the baseline canonical numbering and the delta emission order both derive
// from it.
[[nodiscard]] std::array<SectionRef, kSectionCount>
sections_of(const WorkingContextSnapshot &snapshot) {
    return {{SectionRef{"constraints", &snapshot.constraints},
             SectionRef{"decisions", &snapshot.decisions},
             SectionRef{"open_issues", &snapshot.open_issues},
             SectionRef{"active_tasks", &snapshot.active_tasks},
             SectionRef{"verified_facts", &snapshot.verified_facts},
             SectionRef{"failed_attempts", &snapshot.failed_attempts},
             SectionRef{"important_refs", &snapshot.important_refs},
             SectionRef{"next_actions", &snapshot.next_actions}}};
}

[[nodiscard]] std::optional<std::size_t>
section_index_by_name(const std::array<SectionRef, kSectionCount> &sections,
                      std::string_view name) {
    for (std::size_t index = 0; index < sections.size(); ++index) {
        if (name == sections[index].name) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool shares_any_event(const std::vector<EventId> &left,
                                    const std::vector<EventId> &right) {
    return std::any_of(left.begin(), left.end(), [&right](const EventId &event) {
        return std::any_of(right.begin(), right.end(),
                           [&event](const EventId &other) { return event == other; });
    });
}

// Applies the projection/merge bounds to one projected delta item (RULE-08);
// violations reject the whole operation, never truncate silently here.
[[nodiscard]] Result<void> check_item_bounds(const WorkingContextItem &item,
                                             const WorkingContextMergeOptions &options,
                                             std::string_view section) {
    if (item.content.empty() || item.content.size() > options.max_item_chars) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context fork item content is empty or beyond the byte bound "
                          "in " +
                              std::string(section));
    }
    if (item.source_events.empty() || item.source_events.size() > options.max_source_events) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context fork item provenance is empty or beyond the event "
                          "bound in " +
                              std::string(section));
    }
    return Result<void>{};
}

// One entry of the fork baseline's canonical numbering: declaration order x
// vector order flattened to 0..N-1 (M24 §4.3).
struct CanonicalBaseEntry final {
    std::size_t section = 0; // 0..7 declaration order
    std::size_t index = 0;   // vector index inside the section
    const std::string *content = nullptr;
};

[[nodiscard]] std::vector<CanonicalBaseEntry>
canonical_base_entries(const WorkingContextSnapshot &fork_base) {
    const auto sections = sections_of(fork_base);
    std::vector<CanonicalBaseEntry> entries;
    for (std::size_t section = 0; section < sections.size(); ++section) {
        for (std::size_t index = 0; index < sections[section].items->size(); ++index) {
            entries.push_back(
                CanonicalBaseEntry{section, index, &(*sections[section].items)[index].content});
        }
    }
    return entries;
}

[[nodiscard]] WorkingContextItem delta_item(const WorkingContextDeltaEntry &entry) {
    WorkingContextItem item;
    item.content = entry.content;
    item.source_events = entry.source_events;
    item.source_sequence = entry.source_sequence;
    item.confidence = entry.confidence;
    return item;
}

} // namespace

// ---------------------------------------------------------------------------
// Delta contract (M24 §4.3)
// ---------------------------------------------------------------------------

Result<void> WorkingContextDelta::validate() const {
    if (const auto supported = validate_schema_version(schema_version, SchemaVersion{1, 0});
        !supported) {
        return supported.error();
    }
    if (fork_base_snapshot_id.is_nil()) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context delta requires the fork base snapshot id");
    }
    if (child_session_id.is_nil()) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context delta requires the child session");
    }
    // The delta discipline is pinned to the existing merge-option bounds
    // (M24 §4.3, RULE-08); the options struct documents their documented
    // defaults and this validate() enforces them as absolute contract bounds.
    const WorkingContextMergeOptions bounds;
    if (entries.size() > bounds.max_items_per_section * kSectionCount) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context delta exceeds the total entry bound");
    }
    std::array<std::size_t, kSectionCount> per_section{};
    const auto section_names = sections_of(WorkingContextSnapshot{});
    for (const WorkingContextDeltaEntry &entry : entries) {
        const auto section = section_index_by_name(section_names, entry.section);
        if (!section.has_value()) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta section is outside the frozen vocabulary");
        }
        auto &count = per_section[*section];
        ++count;
        if (count > bounds.max_items_per_section) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta exceeds the per-section entry bound in " +
                                  entry.section);
        }
        if (entry.content.empty() || entry.content.size() > bounds.max_item_chars) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta entry content is empty or beyond the "
                              "byte bound");
        }
        if (entry.source_events.empty() || entry.source_events.size() > bounds.max_source_events) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta entry requires 1..N source events");
        }
        if (!(entry.confidence >= 0.0 && entry.confidence <= 1.0)) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta entry confidence must be within [0,1]");
        }
    }
    return Result<void>{};
}

JsonValue working_context_delta_to_json(const WorkingContextDelta &delta) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(delta.schema_version.major)},
                          {"minor", static_cast<std::int64_t>(delta.schema_version.minor)}});
    object.emplace_back("fork_base_snapshot_id", delta.fork_base_snapshot_id.to_string());
    object.emplace_back("child_session_id", delta.child_session_id.to_string());
    object.emplace_back("generated_by", delta.generated_by.to_string());
    object.emplace_back("inherited_skipped", static_cast<std::int64_t>(delta.inherited_skipped));
    JsonValue::Array entries;
    for (const WorkingContextDeltaEntry &entry : delta.entries) {
        JsonValue::Object encoded;
        encoded.emplace_back("kind", entry.kind == WorkingContextDeltaEntryKind::Supersede
                                         ? std::string("supersede")
                                         : std::string("addition"));
        encoded.emplace_back("superseded_base_index",
                             static_cast<std::int64_t>(entry.superseded_base_index));
        encoded.emplace_back("section", entry.section);
        encoded.emplace_back("content", entry.content);
        JsonValue::Array events;
        for (const auto &event : entry.source_events) {
            events.emplace_back(event.to_string());
        }
        encoded.emplace_back("source_events", JsonValue(std::move(events)));
        encoded.emplace_back("source_sequence", static_cast<std::int64_t>(entry.source_sequence));
        encoded.emplace_back("confidence", entry.confidence);
        entries.emplace_back(JsonValue(std::move(encoded)));
    }
    object.emplace_back("entries", JsonValue(std::move(entries)));
    return JsonValue(std::move(object));
}

Result<WorkingContextDelta> working_context_delta_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context delta payload must be an object");
    }
    WorkingContextDelta delta;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto major = schema->find("major");
        const auto minor = schema->find("minor");
        if (major != nullptr && minor != nullptr) {
            const auto major_value = major->as_integer();
            const auto minor_value = minor->as_integer();
            if (major_value && minor_value) {
                delta.schema_version = SchemaVersion{static_cast<std::uint16_t>(*major_value),
                                                     static_cast<std::uint16_t>(*minor_value)};
            }
        }
    }
    const auto parse_id_field = [&json](const char *key, auto &target,
                                        const char *message) -> Result<void> {
        const auto *field = json.find(key);
        if (field == nullptr || !field->is_string()) {
            return fork_error(ErrorCode::InvalidArgument, message);
        }
        const auto parsed = std::decay_t<decltype(target)>::parse(*field->as_string());
        if (!parsed) {
            return fork_error(ErrorCode::InvalidArgument, message);
        }
        target = *parsed;
        return Result<void>{};
    };
    if (const auto result = parse_id_field("fork_base_snapshot_id", delta.fork_base_snapshot_id,
                                           "working context delta fork base id is malformed");
        !result) {
        return result.error();
    }
    if (const auto result = parse_id_field("child_session_id", delta.child_session_id,
                                           "working context delta child session is malformed");
        !result) {
        return result.error();
    }
    if (const auto *generated = json.find("generated_by"); generated != nullptr) {
        const auto *text = generated->as_string();
        if (text == nullptr) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta generator profile is malformed");
        }
        const auto parsed = ModelProfileId::parse(*text);
        if (!parsed) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta generator profile is malformed");
        }
        delta.generated_by = *parsed;
    }
    if (const auto *skipped = json.find("inherited_skipped"); skipped != nullptr) {
        const auto value = skipped->as_integer();
        if (!value || *value < 0) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta inherited counter is malformed");
        }
        delta.inherited_skipped = static_cast<std::size_t>(*value);
    }
    if (const auto *entries = json.find("entries"); entries != nullptr) {
        const auto *array = entries->as_array();
        if (array == nullptr) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context delta entries must be an array");
        }
        for (const auto &encoded : *array) {
            const auto *object = encoded.as_object();
            if (object == nullptr) {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta entry must be an object");
            }
            WorkingContextDeltaEntry entry;
            const auto *kind = encoded.find("kind");
            if (kind == nullptr || !kind->is_string()) {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta entry kind is malformed");
            }
            if (*kind->as_string() == "supersede") {
                entry.kind = WorkingContextDeltaEntryKind::Supersede;
            } else if (*kind->as_string() == "addition") {
                entry.kind = WorkingContextDeltaEntryKind::Addition;
            } else {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta entry kind is malformed");
            }
            if (const auto *index = encoded.find("superseded_base_index"); index != nullptr) {
                const auto value = index->as_integer();
                if (!value || *value < 0) {
                    return fork_error(ErrorCode::InvalidArgument,
                                      "working context delta base index is malformed");
                }
                entry.superseded_base_index = static_cast<std::size_t>(*value);
            }
            const auto *section = encoded.find("section");
            if (section == nullptr || !section->is_string()) {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta entry section is malformed");
            }
            entry.section = *section->as_string();
            const auto *content = encoded.find("content");
            if (content == nullptr || !content->is_string()) {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta entry content is malformed");
            }
            entry.content = *content->as_string();
            if (const auto *events = encoded.find("source_events"); events != nullptr) {
                const auto *event_array = events->as_array();
                if (event_array == nullptr) {
                    return fork_error(ErrorCode::InvalidArgument,
                                      "working context delta entry events must be an array");
                }
                for (const auto &event : *event_array) {
                    const auto *text = event.as_string();
                    if (text == nullptr) {
                        return fork_error(ErrorCode::InvalidArgument,
                                          "working context delta entry event must be a string");
                    }
                    const auto parsed = EventId::parse(*text);
                    if (!parsed) {
                        return fork_error(ErrorCode::InvalidArgument,
                                          "working context delta entry event id is malformed");
                    }
                    entry.source_events.push_back(*parsed);
                }
            }
            if (const auto *sequence = encoded.find("source_sequence"); sequence != nullptr) {
                const auto value = sequence->as_integer();
                if (!value || *value < 0) {
                    return fork_error(ErrorCode::InvalidArgument,
                                      "working context delta entry sequence is malformed");
                }
                entry.source_sequence = static_cast<SessionSequence>(*value);
            }
            if (const auto *confidence = encoded.find("confidence"); confidence != nullptr) {
                const auto value = confidence->as_number();
                if (!value || !std::isfinite(*value)) {
                    return fork_error(ErrorCode::InvalidArgument,
                                      "working context delta entry confidence must be a finite "
                                      "number");
                }
                entry.confidence = std::clamp(*value, 0.0, 1.0);
            }
            delta.entries.push_back(std::move(entry));
        }
    }
    if (const auto valid = delta.validate(); !valid) {
        return valid.error();
    }
    return delta;
}

// ---------------------------------------------------------------------------
// Snapshot fork (M24 §4.2)
// ---------------------------------------------------------------------------

Result<WorkingContextSnapshot> fork_working_context(const WorkingContextSnapshot &parent_base,
                                                    const WorkingContextForkSeed &seed,
                                                    const WorkingContextMergeOptions &options) {
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    // All-or-nothing: an invalid or out-of-bounds parent never yields a
    // partial baseline (M24 §4.2).
    if (const auto valid = parent_base.validate(); !valid) {
        return valid.error();
    }
    if (seed.child_session.is_nil() || seed.child_identity.task.is_nil()) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context fork seed requires the child session and task");
    }
    if (seed.child_watermark == 0) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context fork seed requires a positive child watermark");
    }
    const auto sections = sections_of(parent_base);
    for (const auto &[name, items] : sections) {
        if (items->size() > options.max_items_per_section) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context fork baseline section exceeds the item bound in " +
                                  std::string(name));
        }
        for (const auto &item : *items) {
            if (const auto result = check_item_bounds(item, options, name); !result) {
                return result.error();
            }
        }
    }

    WorkingContextSnapshot child;
    child.schema_version = working_context_schema_current();
    // Verbatim eight-section copy plus the inherited checkpoint chain
    // (cross-session EventId/checkpoint references stay legal — global
    // 128-bit identifiers).
    child.constraints = parent_base.constraints;
    child.decisions = parent_base.decisions;
    child.open_issues = parent_base.open_issues;
    child.active_tasks = parent_base.active_tasks;
    child.verified_facts = parent_base.verified_facts;
    child.failed_attempts = parent_base.failed_attempts;
    child.important_refs = parent_base.important_refs;
    child.next_actions = parent_base.next_actions;
    child.source_checkpoints = parent_base.source_checkpoints;
    // Identity rebase: the child five-tuple; the parent watermark is never
    // advanced and the parent store is never touched.
    child.session_id = seed.child_session;
    child.task_id = seed.child_identity.task;
    child.task_epoch = seed.child_identity.task_epoch;
    child.environment_epoch = seed.child_identity.environment_epoch;
    child.through_event_sequence = seed.child_watermark;
    // Fork provenance (schema 1.2): the read-only lineage back to the fork
    // source. created_at inherits the fork point so the pure function stays
    // byte-deterministic across invocations (it never enters the digest).
    child.fork = WorkingContextForkProvenance{parent_base.id, parent_base.session_id,
                                              parent_base.through_event_sequence};
    child.created_at = parent_base.created_at;
    const std::string fork_seed = "mira.working_context.fork|" + parent_base.id.to_string() + "|" +
                                  seed.child_session.to_string() + "|" +
                                  seed.child_identity.task.to_string() + "|" +
                                  std::to_string(seed.child_identity.task_epoch) + "|" +
                                  std::to_string(seed.child_identity.environment_epoch) + "|" +
                                  std::to_string(seed.child_watermark);
    child.id = working_context_snapshot_id_from_seed(fork_seed);
    if (const auto valid = child.validate(); !valid) {
        return valid.error();
    }
    return child;
}

// ---------------------------------------------------------------------------
// Mechanical delta projection (M24 §4.3)
// ---------------------------------------------------------------------------

Result<WorkingContextDelta>
working_context_delta_from_fork(const WorkingContextSnapshot &fork_base,
                                const WorkingContextSnapshot &child_snapshot,
                                const WorkingContextMergeOptions &options) {
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = fork_base.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = child_snapshot.validate(); !valid) {
        return valid.error();
    }
    const auto base_sections = sections_of(fork_base);
    const auto child_sections = sections_of(child_snapshot);

    // Canonical baseline offsets: declaration order x vector order flattened
    // to 0..N-1; a section's first baseline entry sits at the running sum of
    // the previous section sizes.
    std::array<std::size_t, kSectionCount> base_offset{};
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        base_offset[section] =
            section == 0 ? 0 : base_offset[section - 1] + base_sections[section - 1].items->size();
    }

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = fork_base.id;
    delta.child_session_id = child_snapshot.session_id;
    delta.generated_by = child_snapshot.generated_by;

    for (std::size_t section = 0; section < kSectionCount; ++section) {
        const auto &base_items = *base_sections[section].items;
        const auto &child_items = *child_sections[section].items;
        std::size_t emitted = 0;
        for (const auto &item : child_items) {
            if (const auto result = check_item_bounds(item, options, base_sections[section].name);
                !result) {
                return result.error();
            }
            // (1) Verbatim baseline copy: skipped and counted, never
            // re-transcribed.
            const bool inherited = std::any_of(base_items.begin(), base_items.end(),
                                               [&item](const WorkingContextItem &candidate) {
                                                   return candidate.content == item.content;
                                               });
            if (inherited) {
                ++delta.inherited_skipped;
                continue;
            }
            // (2) Same-section lineage intersection -> Supersede of the
            // minimal matching baseline index (W2 numbered-transcript
            // provenance binding; parent/child event spaces are disjoint).
            std::optional<std::size_t> superseded;
            for (std::size_t index = 0; index < base_items.size(); ++index) {
                if (shares_any_event(item.source_events, base_items[index].source_events)) {
                    superseded = index;
                    break;
                }
            }
            WorkingContextDeltaEntry entry;
            entry.section = base_sections[section].name;
            entry.content = item.content;
            entry.source_events = item.source_events;
            entry.source_sequence = item.source_sequence;
            entry.confidence = item.confidence;
            if (superseded.has_value()) {
                entry.kind = WorkingContextDeltaEntryKind::Supersede;
                entry.superseded_base_index = base_offset[section] + *superseded;
            } else {
                // (3) Everything else is a child-fresh addition.
                entry.kind = WorkingContextDeltaEntryKind::Addition;
            }
            ++emitted;
            if (emitted > options.max_items_per_section) {
                return fork_error(ErrorCode::InvalidArgument,
                                  "working context delta projection exceeds the per-section "
                                  "entry bound in " +
                                      std::string(base_sections[section].name));
            }
            delta.entries.push_back(std::move(entry));
        }
    }
    if (delta.entries.size() > options.max_items_per_section * kSectionCount) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context delta projection exceeds the total entry bound");
    }
    if (const auto valid = delta.validate(); !valid) {
        return valid.error();
    }
    return delta;
}

// ---------------------------------------------------------------------------
// Parent merge policy (M24 §4.4)
// ---------------------------------------------------------------------------

Result<WorkingContextMergeReport> merge_working_context_delta(
    const WorkingContextSnapshot &fork_base, const WorkingContextSnapshot &parent,
    const WorkingContextDelta &delta, const WorkingContextIdentity &parent_identity,
    std::uint64_t parent_watermark, const WorkingContextMergeOptions &options) {
    // 1. Preconditions — every rejection is whole and carries its frozen
    // token where the milestone freezes one.
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = fork_base.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = parent.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = delta.validate(); !valid) {
        return valid.error();
    }
    if (fork_base.id != delta.fork_base_snapshot_id) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context merge rejected: fork-base-mismatch");
    }
    if (fork_base.session_id == parent.session_id) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context merge rejected: same-session-fork");
    }
    if (parent_identity.task != parent.task_id || parent_identity.task_epoch != parent.task_epoch ||
        parent_identity.environment_epoch != parent.environment_epoch) {
        return fork_error(ErrorCode::InvalidArgument,
                          "working context merge identity does not match the parent snapshot");
    }

    // 2. Supersede scan in delta order, resolved through the fork-base
    // reference: the baseline entry at the canonical index names the content
    // to look for; the first content-equal parent item is replaced in place.
    const auto sections = sections_of(parent);
    const auto canonical = canonical_base_entries(fork_base);
    std::array<std::vector<WorkingContextItem>, kSectionCount> working;
    std::array<std::size_t, kSectionCount> appended_count{};
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        working[section] = *sections[section].items;
    }
    WorkingContextMergeReport report;
    std::vector<std::pair<std::size_t, WorkingContextItem>> additions; // (section, item)
    for (const WorkingContextDeltaEntry &entry : delta.entries) {
        if (entry.kind != WorkingContextDeltaEntryKind::Supersede) {
            additions.emplace_back(section_index_by_name(sections, entry.section).value(),
                                   delta_item(entry));
            continue;
        }
        if (entry.superseded_base_index >= canonical.size()) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context merge supersede base index is out of range");
        }
        // Resolve through the fork-base reference: the baseline entry at the
        // canonical index names the content; the scan runs in the entry's own
        // section (the projection guarantees index/section agreement; a
        // hand-built delta that diverges simply fails to hit and stales).
        const auto section = section_index_by_name(sections, entry.section);
        if (!section.has_value()) {
            return fork_error(ErrorCode::InvalidArgument,
                              "working context merge supersede section is outside the frozen "
                              "vocabulary");
        }
        const auto &target = canonical[entry.superseded_base_index];
        bool resolved = false;
        for (auto &item : working[*section]) {
            if (item.content == *target.content) {
                item = delta_item(entry);
                resolved = true;
                break;
            }
        }
        if (resolved) {
            ++report.supersedes_resolved;
        } else {
            // The parent advanced past or removed the baseline content: the
            // supersede drops and reclassifies as a counted addition.
            ++report.supersedes_dropped_stale;
            additions.emplace_back(*section, delta_item(entry));
        }
    }
    for (const auto &[section, item] : additions) {
        working[section].push_back(item);
        ++appended_count[section];
    }

    // 3./4. Fixed-order truncation: the parent entries and the in-place
    // supersedes keep their slots; appended additions fill the remainder in
    // delta order and the overflow is counted (M23 precedent, RULE-08).
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        const std::size_t total = working[section].size();
        if (total <= options.max_items_per_section) {
            report.additions_appended += appended_count[section];
            continue;
        }
        working[section].resize(options.max_items_per_section);
        report.truncated_entries += total - options.max_items_per_section;
        const std::size_t parent_size = sections[section].items->size();
        report.additions_appended += options.max_items_per_section > parent_size
                                         ? options.max_items_per_section - parent_size
                                         : 0;
    }

    // 5. Candidate fields: the parent identity at the supplied watermark,
    // the inherited checkpoint chain and `generated_by` (a zero-effect merge
    // stays digest-identical to its parent), fork provenance nil — an
    // ordinary snapshot candidate for the existing §5.2 pipeline.
    WorkingContextSnapshot &candidate = report.merged;
    candidate.schema_version = working_context_schema_current();
    candidate.session_id = parent.session_id;
    candidate.task_id = parent_identity.task;
    candidate.task_epoch = parent_identity.task_epoch;
    candidate.environment_epoch = parent_identity.environment_epoch;
    candidate.through_event_sequence = parent_watermark;
    candidate.source_checkpoints = parent.source_checkpoints;
    candidate.created_at = parent.created_at;
    candidate.generated_by = parent.generated_by;
    candidate.fork.reset();
    candidate.constraints = std::move(working[0]);
    candidate.decisions = std::move(working[1]);
    candidate.open_issues = std::move(working[2]);
    candidate.active_tasks = std::move(working[3]);
    candidate.verified_facts = std::move(working[4]);
    candidate.failed_attempts = std::move(working[5]);
    candidate.important_refs = std::move(working[6]);
    candidate.next_actions = std::move(working[7]);
    // Deterministic id over the merged state (digest covers everything but
    // id/created_at), so the same input group derives the same candidate and
    // distinct merge results never share an identity.
    const std::string merge_seed =
        "mira.working_context.merge|" + parent.id.to_string() + "|" + fork_base.id.to_string() +
        "|" + std::to_string(parent_watermark) + "|" + candidate.state_digest().to_string();
    candidate.id = working_context_snapshot_id_from_seed(merge_seed);
    if (const auto valid = candidate.validate(); !valid) {
        return valid.error();
    }
    return report;
}

} // namespace mira
