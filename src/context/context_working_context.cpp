#include <mira/context_working_context.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

namespace mira {
namespace {

// ---------------------------------------------------------------------------
// Error factory and serialization helpers
// ---------------------------------------------------------------------------

[[nodiscard]] Error working_context_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.working_context";
    error.safe_message = std::move(message);
    return error;
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
    // Clock durations are platform-defined (e.g. microseconds on Windows and
    // the NDK); convert through the clock's own duration explicitly.
    timestamp.wall = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(wall)));
    timestamp.monotonic = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(monotonic)));
    return timestamp;
}

[[nodiscard]] JsonValue items_to_json(const std::vector<WorkingContextItem> &items) {
    JsonValue::Array array;
    for (const auto &item : items) {
        JsonValue::Object object;
        object.emplace_back("content", item.content);
        JsonValue::Array events;
        for (const auto &event : item.source_events) {
            events.emplace_back(event.to_string());
        }
        object.emplace_back("source_events", JsonValue(std::move(events)));
        object.emplace_back("source_sequence", static_cast<std::int64_t>(item.source_sequence));
        object.emplace_back("confidence", item.confidence);
        array.emplace_back(JsonValue(std::move(object)));
    }
    return JsonValue(std::move(array));
}

[[nodiscard]] Result<WorkingContextItem> item_from_json(const JsonValue &json,
                                                        std::string_view section) {
    const auto *object = json.as_object();
    if (object == nullptr) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context item must be an object in " +
                                         std::string(section));
    }
    WorkingContextItem item;
    const auto *content = json.find("content");
    if (content == nullptr || !content->is_string()) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context item requires string content");
    }
    item.content = *content->as_string();
    const auto *events = json.find("source_events");
    if (events != nullptr && events->is_array()) {
        for (const auto &event : *events->as_array()) {
            const auto *text = event.as_string();
            if (text == nullptr) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context item event must be a string");
            }
            const auto parsed = EventId::parse(*text);
            if (!parsed) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context item event id is malformed");
            }
            item.source_events.push_back(*parsed);
        }
    }
    if (const auto *sequence = json.find("source_sequence"); sequence != nullptr) {
        const auto value = sequence->as_integer();
        if (!value || *value < 0) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context item sequence must be a non-negative "
                                         "integer");
        }
        item.source_sequence = static_cast<SessionSequence>(*value);
    }
    if (const auto *confidence = json.find("confidence"); confidence != nullptr) {
        const auto value = confidence->as_number();
        if (!value || !std::isfinite(*value)) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context item confidence must be a finite "
                                         "number");
        }
        item.confidence = std::clamp(*value, 0.0, 1.0);
    }
    return item;
}

[[nodiscard]] Result<std::vector<WorkingContextItem>> items_from_json(const JsonValue &json,
                                                                      std::string_view section) {
    std::vector<WorkingContextItem> items;
    const auto *array = json.as_array();
    if (array == nullptr) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context section must be an array: " +
                                         std::string(section));
    }
    items.reserve(array->size());
    for (const auto &entry : *array) {
        auto item = item_from_json(entry, section);
        if (!item) {
            return item.error();
        }
        items.push_back(item.value());
    }
    return items;
}

[[nodiscard]] JsonValue
snapshot_id_list_to_json(const std::vector<ConversationCheckpointId> &checkpoints) {
    JsonValue::Array array;
    for (const auto &checkpoint : checkpoints) {
        array.emplace_back(checkpoint.to_string());
    }
    return JsonValue(std::move(array));
}

} // namespace

// ---------------------------------------------------------------------------
// Contracts
// ---------------------------------------------------------------------------

WorkingContextSnapshotId working_context_snapshot_id_from_seed(std::string_view seed) {
    const ContextAssetId asset =
        context_asset_id_from_seed("mira.working_context.snapshot|" + std::string(seed));
    return WorkingContextSnapshotId{asset.value};
}

Result<void> WorkingContextMergeOptions::validate() const {
    if (max_items_per_section == 0 || max_items_per_section > 1'024) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "item bound per section is out of range");
    }
    if (max_item_chars < 16 || max_item_chars > 8 * 1024) {
        return working_context_error(ErrorCode::InvalidArgument, "item byte bound is out of range");
    }
    if (max_source_events == 0 || max_source_events > 4'096) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "source event bound is out of range");
    }
    return Result<void>{};
}

Result<void> WorkingContextSnapshot::validate() const {
    if (const auto supported =
            validate_schema_version(schema_version, working_context_schema_current());
        !supported) {
        return supported.error();
    }
    if (id.is_nil()) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot requires an id");
    }
    if (session_id.is_nil()) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot requires a session");
    }
    if (task_id.is_nil()) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot requires a task");
    }
    if (through_event_sequence == 0) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot requires a positive watermark");
    }
    if (source_checkpoints.empty() || source_checkpoints.size() > 64) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot requires 1..64 source "
                                     "checkpoints");
    }
    for (const auto &checkpoint : source_checkpoints) {
        if (checkpoint.is_nil()) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context snapshot source checkpoint is nil");
        }
    }
    const auto validate_items = [](const std::vector<WorkingContextItem> &items,
                                   std::string_view section) -> Result<void> {
        if (items.size() > 1'024) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context section " + std::string(section) +
                                             " exceeds the absolute item bound");
        }
        for (const auto &item : items) {
            if (item.content.empty() || item.content.size() > 8 * 1024) {
                return working_context_error(
                    ErrorCode::InvalidArgument,
                    "working context item content is empty or beyond the absolute byte bound");
            }
            if (item.source_events.empty()) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context items require provenance");
            }
            if (!(item.confidence >= 0.0 && item.confidence <= 1.0)) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context item confidence must be within "
                                             "[0,1]");
            }
        }
        return Result<void>{};
    };
    if (const auto result = validate_items(constraints, "constraints"); !result) {
        return result;
    }
    if (const auto result = validate_items(decisions, "decisions"); !result) {
        return result;
    }
    if (const auto result = validate_items(open_issues, "open_issues"); !result) {
        return result;
    }
    if (const auto result = validate_items(active_tasks, "active_tasks"); !result) {
        return result;
    }
    if (const auto result = validate_items(verified_facts, "verified_facts"); !result) {
        return result;
    }
    if (const auto result = validate_items(failed_attempts, "failed_attempts"); !result) {
        return result;
    }
    if (const auto result = validate_items(important_refs, "important_refs"); !result) {
        return result;
    }
    if (const auto result = validate_items(next_actions, "next_actions"); !result) {
        return result;
    }
    return Result<void>{};
}

Hash WorkingContextSnapshot::state_digest() const {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(schema_version.major)},
                          {"minor", static_cast<std::int64_t>(schema_version.minor)}});
    object.emplace_back("session_id", session_id.to_string());
    object.emplace_back("task_id", task_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(task_epoch));
    object.emplace_back("environment_epoch", static_cast<std::int64_t>(environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(through_event_sequence));
    object.emplace_back("source_checkpoints", snapshot_id_list_to_json(source_checkpoints));
    object.emplace_back("generated_by", generated_by.to_string());
    object.emplace_back("constraints", items_to_json(constraints));
    object.emplace_back("decisions", items_to_json(decisions));
    object.emplace_back("open_issues", items_to_json(open_issues));
    object.emplace_back("active_tasks", items_to_json(active_tasks));
    object.emplace_back("verified_facts", items_to_json(verified_facts));
    object.emplace_back("failed_attempts", items_to_json(failed_attempts));
    object.emplace_back("important_refs", items_to_json(important_refs));
    object.emplace_back("next_actions", items_to_json(next_actions));
    return canonical_json_digest(JsonValue(std::move(object)));
}

JsonValue working_context_to_json(const WorkingContextSnapshot &snapshot) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(snapshot.schema_version.major)},
                          {"minor", static_cast<std::int64_t>(snapshot.schema_version.minor)}});
    object.emplace_back("id", snapshot.id.to_string());
    object.emplace_back("session_id", snapshot.session_id.to_string());
    object.emplace_back("task_id", snapshot.task_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(snapshot.task_epoch));
    object.emplace_back("environment_epoch", static_cast<std::int64_t>(snapshot.environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(snapshot.through_event_sequence));
    object.emplace_back("source_checkpoints",
                        snapshot_id_list_to_json(snapshot.source_checkpoints));
    object.emplace_back("created_at", wall_nanos(snapshot.created_at));
    object.emplace_back("created_at_monotonic", monotonic_nanos(snapshot.created_at));
    object.emplace_back("generated_by", snapshot.generated_by.to_string());
    object.emplace_back("constraints", items_to_json(snapshot.constraints));
    object.emplace_back("decisions", items_to_json(snapshot.decisions));
    object.emplace_back("open_issues", items_to_json(snapshot.open_issues));
    object.emplace_back("active_tasks", items_to_json(snapshot.active_tasks));
    object.emplace_back("verified_facts", items_to_json(snapshot.verified_facts));
    object.emplace_back("failed_attempts", items_to_json(snapshot.failed_attempts));
    object.emplace_back("important_refs", items_to_json(snapshot.important_refs));
    object.emplace_back("next_actions", items_to_json(snapshot.next_actions));
    return JsonValue(std::move(object));
}

Result<WorkingContextSnapshot> working_context_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "working context snapshot payload must be an object");
    }
    WorkingContextSnapshot snapshot;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto major = schema->find("major");
        const auto minor = schema->find("minor");
        if (major != nullptr && minor != nullptr) {
            const auto major_value = major->as_integer();
            const auto minor_value = minor->as_integer();
            if (major_value && minor_value) {
                snapshot.schema_version = SchemaVersion{static_cast<std::uint16_t>(*major_value),
                                                        static_cast<std::uint16_t>(*minor_value)};
            }
        }
    }
    if (const auto supported =
            validate_schema_version(snapshot.schema_version, working_context_schema_current());
        !supported) {
        return supported.error();
    }
    const auto parse_id_field = [&json](const char *key, auto &target,
                                        const char *message) -> Result<void> {
        const auto *field = json.find(key);
        if (field == nullptr || !field->is_string()) {
            return working_context_error(ErrorCode::InvalidArgument, message);
        }
        const auto parsed = std::decay_t<decltype(target)>::parse(*field->as_string());
        if (!parsed) {
            return working_context_error(ErrorCode::InvalidArgument, message);
        }
        target = *parsed;
        return Result<void>{};
    };
    if (const auto result =
            parse_id_field("id", snapshot.id, "working context snapshot id is malformed");
        !result) {
        return result.error();
    }
    if (const auto result = parse_id_field("session_id", snapshot.session_id,
                                           "working context snapshot session is malformed");
        !result) {
        return result.error();
    }
    if (const auto result = parse_id_field("task_id", snapshot.task_id,
                                           "working context snapshot task is malformed");
        !result) {
        return result.error();
    }
    // Optional since schema 1.0 payloads predate the Curator annotation: a
    // nil generated_by is valid (deterministic projection); a malformed
    // value is not.
    if (const auto *generated = json.find("generated_by"); generated != nullptr) {
        const auto *text = generated->as_string();
        if (text == nullptr) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context snapshot generator profile is malformed");
        }
        const auto parsed = ModelProfileId::parse(*text);
        if (!parsed) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context snapshot generator profile is malformed");
        }
        snapshot.generated_by = *parsed;
    }
    const auto parse_uint_field = [&json](const char *key, std::uint64_t &target,
                                          const char *message) -> Result<void> {
        const auto *field = json.find(key);
        if (field == nullptr) {
            return Result<void>{};
        }
        const auto value = field->as_integer();
        if (!value || *value < 0) {
            return working_context_error(ErrorCode::InvalidArgument, message);
        }
        target = static_cast<std::uint64_t>(*value);
        return Result<void>{};
    };
    if (const auto result = parse_uint_field("task_epoch", snapshot.task_epoch,
                                             "working context snapshot task epoch is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_uint_field("environment_epoch", snapshot.environment_epoch,
                             "working context snapshot environment epoch is malformed");
        !result) {
        return result.error();
    }
    if (const auto result =
            parse_uint_field("through_event_sequence", snapshot.through_event_sequence,
                             "working context snapshot watermark is malformed");
        !result) {
        return result.error();
    }
    if (const auto *sources = json.find("source_checkpoints"); sources != nullptr) {
        const auto *array = sources->as_array();
        if (array == nullptr) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "working context snapshot source checkpoints must be "
                                         "an array");
        }
        for (const auto &entry : *array) {
            const auto *text = entry.as_string();
            if (text == nullptr) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context snapshot source checkpoint must "
                                             "be a string");
            }
            const auto parsed = ConversationCheckpointId::parse(*text);
            if (!parsed) {
                return working_context_error(ErrorCode::InvalidArgument,
                                             "working context snapshot source checkpoint is "
                                             "malformed");
            }
            snapshot.source_checkpoints.push_back(*parsed);
        }
    }
    const auto *created = json.find("created_at");
    const auto *created_monotonic = json.find("created_at_monotonic");
    if (created != nullptr && created_monotonic != nullptr) {
        const auto wall = created->as_integer();
        const auto monotonic = created_monotonic->as_integer();
        if (wall && monotonic) {
            snapshot.created_at = timestamp_from_nanos(*wall, *monotonic);
        }
    }
    if (const auto *constraints = json.find("constraints"); constraints != nullptr) {
        auto items = items_from_json(*constraints, "constraints");
        if (!items) {
            return items.error();
        }
        snapshot.constraints = std::move(items).value();
    }
    if (const auto *decisions = json.find("decisions"); decisions != nullptr) {
        auto items = items_from_json(*decisions, "decisions");
        if (!items) {
            return items.error();
        }
        snapshot.decisions = std::move(items).value();
    }
    if (const auto *open_issues = json.find("open_issues"); open_issues != nullptr) {
        auto items = items_from_json(*open_issues, "open_issues");
        if (!items) {
            return items.error();
        }
        snapshot.open_issues = std::move(items).value();
    }
    if (const auto *active_tasks = json.find("active_tasks"); active_tasks != nullptr) {
        auto items = items_from_json(*active_tasks, "active_tasks");
        if (!items) {
            return items.error();
        }
        snapshot.active_tasks = std::move(items).value();
    }
    if (const auto *verified_facts = json.find("verified_facts"); verified_facts != nullptr) {
        auto items = items_from_json(*verified_facts, "verified_facts");
        if (!items) {
            return items.error();
        }
        snapshot.verified_facts = std::move(items).value();
    }
    if (const auto *failed_attempts = json.find("failed_attempts"); failed_attempts != nullptr) {
        auto items = items_from_json(*failed_attempts, "failed_attempts");
        if (!items) {
            return items.error();
        }
        snapshot.failed_attempts = std::move(items).value();
    }
    if (const auto *important_refs = json.find("important_refs"); important_refs != nullptr) {
        auto items = items_from_json(*important_refs, "important_refs");
        if (!items) {
            return items.error();
        }
        snapshot.important_refs = std::move(items).value();
    }
    if (const auto *next_actions = json.find("next_actions"); next_actions != nullptr) {
        auto items = items_from_json(*next_actions, "next_actions");
        if (!items) {
            return items.error();
        }
        snapshot.next_actions = std::move(items).value();
    }
    if (const auto valid = snapshot.validate(); !valid) {
        return valid.error();
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// Deterministic projection (design §6, Stage W1)
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Result<std::vector<WorkingContextItem>>
project_items(const std::vector<ConversationStatement> &statements,
              const WorkingContextMergeOptions &options, std::string_view section) {
    if (statements.size() > options.max_items_per_section) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "checkpoint section " + std::string(section) +
                                         " exceeds the projection item bound");
    }
    std::vector<WorkingContextItem> items;
    items.reserve(statements.size());
    for (const auto &statement : statements) {
        if (statement.content.size() > options.max_item_chars) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "checkpoint statement exceeds the projection byte "
                                         "bound in " +
                                             std::string(section));
        }
        if (statement.source_events.size() > options.max_source_events) {
            return working_context_error(ErrorCode::InvalidArgument,
                                         "checkpoint statement exceeds the projection "
                                         "provenance bound in " +
                                             std::string(section));
        }
        WorkingContextItem item;
        item.content = statement.content;
        item.source_events = statement.source_events;
        item.source_sequence = statement.source_sequence;
        item.confidence = statement.confidence;
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace

Result<WorkingContextSnapshot>
working_context_from_checkpoint(const ConversationCheckpoint &checkpoint,
                                const WorkingContextIdentity &identity,
                                const WorkingContextMergeOptions &options) {
    if (const auto valid = options.validate(); !valid) {
        return valid.error();
    }
    if (const auto valid = checkpoint.validate(); !valid) {
        return valid.error();
    }
    // The caller-supplied identity must match the checkpoint's own stamping:
    // a snapshot projected for a different task or epoch than its source
    // checkpoint would be incoherent at commit time anyway, and rejecting
    // here keeps the projection self-consistent (design §6).
    if (checkpoint.task_id != identity.task) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "projection identity task does not match the checkpoint");
    }
    if (checkpoint.task_epoch != identity.task_epoch) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "projection identity task epoch does not match the "
                                     "checkpoint");
    }
    if (checkpoint.environment_epoch != identity.environment_epoch) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "projection identity environment epoch does not match "
                                     "the checkpoint");
    }
    auto constraints = project_items(checkpoint.constraints, options, "constraints");
    if (!constraints) {
        return constraints.error();
    }
    auto decisions = project_items(checkpoint.decisions, options, "decisions");
    if (!decisions) {
        return decisions.error();
    }
    auto open_issues = project_items(checkpoint.unresolved_threads, options, "open_issues");
    if (!open_issues) {
        return open_issues.error();
    }
    WorkingContextSnapshot snapshot;
    snapshot.session_id = checkpoint.session_id;
    snapshot.task_id = identity.task;
    snapshot.task_epoch = identity.task_epoch;
    snapshot.environment_epoch = identity.environment_epoch;
    snapshot.through_event_sequence = checkpoint.through_event_sequence;
    snapshot.source_checkpoints.push_back(checkpoint.id);
    snapshot.constraints = std::move(constraints).value();
    snapshot.decisions = std::move(decisions).value();
    snapshot.open_issues = std::move(open_issues).value();
    const std::string seed = checkpoint.session_id.to_string() + "|" + identity.task.to_string() +
                             "|" + std::to_string(identity.task_epoch) + "|" +
                             std::to_string(identity.environment_epoch) + "|" +
                             std::to_string(checkpoint.through_event_sequence);
    snapshot.id = working_context_snapshot_id_from_seed(seed);
    snapshot.created_at = checkpoint.created_at;
    if (const auto valid = snapshot.validate(); !valid) {
        return valid.error();
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// Snapshot store
// ---------------------------------------------------------------------------

Result<void> WorkingContextStorePolicy::validate() const {
    if (max_snapshots_per_session == 0 || max_snapshots_per_session > 1'024) {
        return working_context_error(ErrorCode::InvalidArgument,
                                     "snapshot retention bound is out of range");
    }
    return Result<void>{};
}

class InMemoryWorkingContextStore::Impl final {
  public:
    explicit Impl(WorkingContextStorePolicy policy) : policy_(policy) {}

    [[nodiscard]] Result<void> put(const WorkingContextSnapshot &snapshot) {
        if (const auto valid = snapshot.validate(); !valid) {
            return valid.error();
        }
        std::lock_guard lock(mutex_);
        auto &ring = snapshots_[snapshot.session_id];
        if (!ring.empty()) {
            const auto &latest = ring.back();
            if (latest.task_id == snapshot.task_id && latest.task_epoch == snapshot.task_epoch &&
                latest.environment_epoch == snapshot.environment_epoch &&
                snapshot.through_event_sequence < latest.through_event_sequence) {
                return working_context_error(ErrorCode::InvalidState,
                                             "snapshot watermark regresses behind the stored "
                                             "one in the same identity chain");
            }
        }
        ring.push_back(snapshot);
        if (ring.size() > policy_.max_snapshots_per_session) {
            ring.erase(ring.begin());
        }
        return Result<void>{};
    }

    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>> latest(SessionId session) const {
        std::lock_guard lock(mutex_);
        const auto found = snapshots_.find(session);
        if (found == snapshots_.end() || found->second.empty()) {
            return std::optional<WorkingContextSnapshot>{};
        }
        return Result<std::optional<WorkingContextSnapshot>>(found->second.back());
    }

    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const {
        std::lock_guard lock(mutex_);
        const auto found = snapshots_.find(session);
        if (found == snapshots_.end()) {
            return std::optional<WorkingContextSnapshot>{};
        }
        const auto &ring = found->second;
        std::optional<WorkingContextSnapshot> result;
        for (const auto &snapshot : ring) {
            if (snapshot.through_event_sequence <= max_sequence) {
                result = snapshot;
            }
        }
        return Result<std::optional<WorkingContextSnapshot>>(std::move(result));
    }

    [[nodiscard]] Result<std::size_t> count(SessionId session) const {
        std::lock_guard lock(mutex_);
        const auto found = snapshots_.find(session);
        return found == snapshots_.end() ? std::size_t{0} : found->second.size();
    }

    [[nodiscard]] Result<std::size_t> erase_session(SessionId session, const std::string &reason) {
        (void)reason; // audit surface is host-side; the reason documents intent
        std::lock_guard lock(mutex_);
        const auto found = snapshots_.find(session);
        if (found == snapshots_.end()) {
            return std::size_t{0};
        }
        const std::size_t erased = found->second.size();
        snapshots_.erase(found);
        return erased;
    }

  private:
    WorkingContextStorePolicy policy_;
    mutable std::mutex mutex_;
    std::map<SessionId, std::vector<WorkingContextSnapshot>> snapshots_;
};

InMemoryWorkingContextStore::InMemoryWorkingContextStore(WorkingContextStorePolicy policy)
    : impl_(std::make_unique<Impl>(policy)) {}
InMemoryWorkingContextStore::~InMemoryWorkingContextStore() = default;

Result<void> InMemoryWorkingContextStore::put(const WorkingContextSnapshot &snapshot) {
    return impl_->put(snapshot);
}

Result<std::optional<WorkingContextSnapshot>>
InMemoryWorkingContextStore::latest(SessionId session) const {
    return impl_->latest(session);
}

Result<std::optional<WorkingContextSnapshot>>
InMemoryWorkingContextStore::latest_at_or_before(SessionId session,
                                                 std::uint64_t max_sequence) const {
    return impl_->latest_at_or_before(session, max_sequence);
}

Result<std::size_t> InMemoryWorkingContextStore::count(SessionId session) const {
    return impl_->count(session);
}

Result<std::size_t> InMemoryWorkingContextStore::erase_session(SessionId session,
                                                               std::string reason) {
    return impl_->erase_session(session, reason); // reason is audit metadata only
}

// ---------------------------------------------------------------------------
// Commit validation (design §5.2)
// ---------------------------------------------------------------------------

std::string working_context_commit_disposition_name(WorkingContextCommitDisposition disposition) {
    switch (disposition) {
    case WorkingContextCommitDisposition::Committed:
        return "Committed";
    case WorkingContextCommitDisposition::IdempotentNoOp:
        return "IdempotentNoOp";
    case WorkingContextCommitDisposition::DiscardedStale:
        return "DiscardedStale";
    case WorkingContextCommitDisposition::DiscardedTerminal:
        return "DiscardedTerminal";
    }
    return "Unknown";
}

namespace {

[[nodiscard]] WorkingContextCommitOutcome discard(WorkingContextCommitDisposition disposition,
                                                  std::string reason_code) {
    WorkingContextCommitOutcome outcome;
    outcome.disposition = disposition;
    outcome.reason_code = std::move(reason_code);
    return outcome;
}

} // namespace

WorkingContextCommitOutcome commit_working_context(IWorkingContextStore &store,
                                                   const WorkingContextSnapshot &candidate,
                                                   const WorkingContextCommitState &live) {
    if (const auto valid = candidate.validate(); !valid) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "invalid-candidate");
    }
    // Terminal idempotency first: results arriving after the session or task
    // went terminal are dropped, never committed (design §5.2).
    if (live.session_terminal) {
        return discard(WorkingContextCommitDisposition::DiscardedTerminal, "session-terminal");
    }
    if (live.task_terminal) {
        return discard(WorkingContextCommitDisposition::DiscardedTerminal, "task-terminal");
    }
    // Commit-tuple validation: any mismatch discards the candidate and keeps
    // the stored snapshot.
    if (candidate.session_id != live.session) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "session-mismatch");
    }
    if (candidate.task_id != live.task) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "task-mismatch");
    }
    if (candidate.task_epoch != live.task_epoch) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "task-epoch-mismatch");
    }
    if (candidate.environment_epoch != live.environment_epoch) {
        return discard(WorkingContextCommitDisposition::DiscardedStale,
                       "environment-epoch-mismatch");
    }
    const auto stored = store.latest(candidate.session_id);
    if (!stored) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "store-unavailable");
    }
    if (stored.value().has_value()) {
        const auto &existing = *stored.value();
        if (existing.task_id == candidate.task_id && existing.task_epoch == candidate.task_epoch &&
            existing.environment_epoch == candidate.environment_epoch) {
            if (candidate.through_event_sequence < existing.through_event_sequence) {
                return discard(WorkingContextCommitDisposition::DiscardedStale, "stale-watermark");
            }
            if (candidate.through_event_sequence == existing.through_event_sequence) {
                if (candidate.state_digest() == existing.state_digest()) {
                    return discard(WorkingContextCommitDisposition::IdempotentNoOp,
                                   "idempotent-replay");
                }
                return discard(WorkingContextCommitDisposition::DiscardedStale,
                               "conflicting-watermark");
            }
        }
    }
    const auto put = store.put(candidate);
    if (!put) {
        return discard(WorkingContextCommitDisposition::DiscardedStale, "store-rejected");
    }
    WorkingContextCommitOutcome outcome;
    outcome.disposition = WorkingContextCommitDisposition::Committed;
    outcome.reason_code = "committed";
    outcome.committed = candidate;
    return outcome;
}

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

std::vector<ContextItem>
context_items_from_working_context(const WorkingContextSnapshot &snapshot) {
    std::vector<ContextItem> items;
    std::size_t index = 0;
    const auto push_item = [&](const WorkingContextItem &item, ContextItemKind kind,
                               std::string_view tag) {
        ContextItem context_item;
        const std::string seed = "mira.context.item|wc|" + snapshot.id.to_string() + "|" +
                                 std::string(tag) + "|" + std::to_string(index);
        context_item.id = ContextItemId{context_asset_id_from_seed(seed).value};
        context_item.kind = kind;
        // Model-mediated derived projection: never SystemPolicy, never
        // VerifiedState (RULE-09; design §7). Layer 0 stays the admission
        // authority and its partition rules decide what enters the request.
        context_item.authority = ContextAuthority::UntrustedExternalData;
        context_item.priority = ContextPriority::Normal;
        TextPart part;
        part.text = item.content;
        part.sensitivity = Sensitivity::Internal;
        context_item.content.emplace_back(std::move(part));
        context_item.provenance = item.source_events;
        context_item.sequence = item.source_sequence;
        context_item.consumed = true;
        context_item.task_epoch = snapshot.task_epoch;
        context_item.environment_epoch = snapshot.environment_epoch;
        items.push_back(std::move(context_item));
        ++index;
    };
    for (const auto &item : snapshot.constraints) {
        // Design §7: projected constraints enter as P1-level candidates.
        push_item(item, ContextItemKind::UserConstraint, "constraint");
    }
    for (const auto &item : snapshot.decisions) {
        push_item(item, ContextItemKind::CheckpointSummary, "decision");
    }
    for (const auto &item : snapshot.open_issues) {
        push_item(item, ContextItemKind::CheckpointSummary, "issue");
    }
    for (const auto &item : snapshot.active_tasks) {
        push_item(item, ContextItemKind::CheckpointSummary, "active_task");
    }
    for (const auto &item : snapshot.verified_facts) {
        push_item(item, ContextItemKind::CheckpointSummary, "verified_fact");
    }
    for (const auto &item : snapshot.failed_attempts) {
        push_item(item, ContextItemKind::CheckpointSummary, "failed_attempt");
    }
    for (const auto &item : snapshot.important_refs) {
        push_item(item, ContextItemKind::CheckpointSummary, "important_ref");
    }
    for (const auto &item : snapshot.next_actions) {
        push_item(item, ContextItemKind::CheckpointSummary, "next_action");
    }
    return items;
}

} // namespace mira
