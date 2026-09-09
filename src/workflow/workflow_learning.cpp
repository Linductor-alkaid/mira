#include <mira/workflow_learning.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace mira {

// ---------------------------------------------------------------------------
// Memory four-domain organization (DEC-029 §1)
// ---------------------------------------------------------------------------

std::string memory_domain_name(MemoryDomain domain) {
    switch (domain) {
    case MemoryDomain::EnvironmentModel:
        return "environment";
    case MemoryDomain::UserModel:
        return "user";
    case MemoryDomain::ProceduralMemory:
        return "procedural";
    case MemoryDomain::EpisodicMemory:
        return "episodic";
    }
    return "environment";
}

Result<MemoryDomain> parse_memory_domain(std::string_view name) {
    if (name == "environment") {
        return MemoryDomain::EnvironmentModel;
    }
    if (name == "user") {
        return MemoryDomain::UserModel;
    }
    if (name == "procedural") {
        return MemoryDomain::ProceduralMemory;
    }
    if (name == "episodic") {
        return MemoryDomain::EpisodicMemory;
    }
    return make_workflow_learning_error(WorkflowLearningError::InvalidShape,
                                        "unknown memory domain name");
}

MemoryDomain memory_domain_of(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::EnvironmentFact:
    case MemoryKind::ApplicationFact:
        return MemoryDomain::EnvironmentModel;
    case MemoryKind::Preference:
        return MemoryDomain::UserModel;
    case MemoryKind::Procedure:
    case MemoryKind::SkillHint:
    case MemoryKind::RecoveryLesson:
        return MemoryDomain::ProceduralMemory;
    case MemoryKind::Episode:
        return MemoryDomain::EpisodicMemory;
    }
    return MemoryDomain::EpisodicMemory;
}

std::vector<MemoryKind> memory_kinds_of_domain(MemoryDomain domain) {
    // Declaration order of MemoryKind, filtered by the domain mapping.
    const std::array<MemoryKind, 7> all = {
        MemoryKind::Preference,     MemoryKind::EnvironmentFact, MemoryKind::ApplicationFact,
        MemoryKind::Episode,        MemoryKind::Procedure,       MemoryKind::SkillHint,
        MemoryKind::RecoveryLesson,
    };
    std::vector<MemoryKind> kinds;
    kinds.reserve(all.size());
    for (const auto kind : all) {
        if (memory_domain_of(kind) == domain) {
            kinds.push_back(kind);
        }
    }
    return kinds;
}

// ---------------------------------------------------------------------------
// Shared decode helpers (fail-closed, same discipline as the IR decoder)
// ---------------------------------------------------------------------------

Error make_workflow_learning_error(WorkflowLearningError code, std::string detail) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = "workflow learning: " + detail;
    return error;
}

Result<void> WorkflowLearningLimits::validate() const {
    if (max_document_bytes == 0 || max_id_bytes == 0 || max_reason_code_bytes == 0 ||
        max_recovery_actions == 0) {
        return make_workflow_learning_error(WorkflowLearningError::LimitExceeded,
                                            "learning limits must be positive");
    }
    if (max_retrieval_results == 0 || max_retrieval_results > 512) {
        return make_workflow_learning_error(WorkflowLearningError::LimitExceeded,
                                            "retrieval result bound must be within (0, 512]");
    }
    if (retrieval_token_budget == 0) {
        return make_workflow_learning_error(WorkflowLearningError::LimitExceeded,
                                            "retrieval token budget must be positive");
    }
    if (retrieval_deadline.count() < 0) {
        return make_workflow_learning_error(WorkflowLearningError::LimitExceeded,
                                            "retrieval deadline must not be negative");
    }
    return Result<void>{};
}

const WorkflowLearningLimits kDefaultWorkflowLearningLimits{};

namespace {

[[nodiscard]] Error learning_error(WorkflowLearningError code, std::string detail) {
    return make_workflow_learning_error(code, std::move(detail));
}

[[nodiscard]] Result<void> check_keys(const JsonValue &object,
                                      std::span<const std::string_view> required,
                                      std::span<const std::string_view> optional,
                                      std::string_view where) {
    if (!object.is_object()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + " must be an object");
    }
    for (const auto &member : *object.as_object()) {
        const bool known =
            std::any_of(required.begin(), required.end(),
                        [&](std::string_view key) { return key == member.first; }) ||
            std::any_of(optional.begin(), optional.end(),
                        [&](std::string_view key) { return key == member.first; });
        if (!known) {
            return learning_error(WorkflowLearningError::UnknownField,
                                  std::string{where} + ": unknown field '" + member.first + "'");
        }
    }
    for (const auto key : required) {
        if (object.find(key) == nullptr) {
            return learning_error(WorkflowLearningError::InvalidShape,
                                  std::string{where} + ": missing field '" + std::string{key} +
                                      "'");
        }
    }
    return Result<void>{};
}

[[nodiscard]] std::optional<std::string> bounded_string(const JsonValue &value,
                                                        std::size_t max_bytes) {
    const auto *text = value.as_string();
    if (text == nullptr || text->empty() || text->size() > max_bytes) {
        return std::nullopt;
    }
    return *text;
}

// Sanitized identifier charset (DEC-029 §4): [A-Za-z0-9._:-] only. Raw error
// text, model output or user text never passes this gate.
[[nodiscard]] bool is_sanitized_identifier(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '.' || value == '_' || value == ':' ||
               value == '-';
    });
}

[[nodiscard]] Result<std::string> parse_identifier_member(const JsonValue &json, const char *key,
                                                          std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_string()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": '" + key + "' must be a string");
    }
    return member->as_string()->empty() ? learning_error(WorkflowLearningError::InvalidShape,
                                                          std::string{where} + ": '" + key +
                                                              "' must be non-empty")
                                        : Result<std::string>{*member->as_string()};
}

[[nodiscard]] Result<std::uint64_t> parse_count_member(const JsonValue &json, const char *key,
                                                       std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_integer()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": '" + key + "' must be an integer");
    }
    const auto value = member->as_integer().value();
    if (value < 0) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": '" + key + "' must be non-negative");
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] Result<std::string> parse_hex_digest_member(const JsonValue &json, const char *key,
                                                          std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_string()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": '" + key + "' must be a hex digest string");
    }
    if (!digest_from_hex(*member->as_string()).has_value()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": '" + key + "' is not a sha-256 hex digest");
    }
    return Result<std::string>{*member->as_string()};
}

[[nodiscard]] Result<SchemaVersion> parse_schema_version(const JsonValue &json,
                                                         std::string_view where) {
    static constexpr std::string_view kVersionKeys[] = {"major", "minor"};
    if (auto check = check_keys(json, kVersionKeys, {}, "schema_version"); !check.has_value()) {
        return check.error();
    }
    const auto major = parse_count_member(json, "major", where);
    if (!major.has_value()) {
        return major.error();
    }
    const auto minor = parse_count_member(json, "minor", where);
    if (!minor.has_value()) {
        return minor.error();
    }
    SchemaVersion version;
    if (major.value() > std::numeric_limits<std::uint16_t>::max() ||
        minor.value() > std::numeric_limits<std::uint16_t>::max()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              std::string{where} + ": schema version fields exceed bounds");
    }
    version.major = static_cast<std::uint16_t>(major.value());
    version.minor = static_cast<std::uint16_t>(minor.value());
    return version;
}

[[nodiscard]] JsonValue schema_version_to_json(const SchemaVersion &version) {
    JsonValue::Object object;
    object.emplace_back("major", static_cast<std::uint64_t>(version.major));
    object.emplace_back("minor", static_cast<std::uint64_t>(version.minor));
    return JsonValue{std::move(object)};
}

[[nodiscard]] std::optional<std::string> sanitized_reason(const std::string &text,
                                                          const WorkflowLearningLimits &limits) {
    if (text.size() > limits.max_reason_code_bytes || !is_sanitized_identifier(text)) {
        return std::nullopt;
    }
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// WorkflowFailureSignature (DEC-029 §4)
// ---------------------------------------------------------------------------

JsonValue failure_signature_to_json(const WorkflowFailureSignature &signature) {
    JsonValue::Object object;
    object.emplace_back("workflow_id", signature.workflow_id);
    object.emplace_back("step_id", signature.step_id.has_value()
                                       ? JsonValue{*signature.step_id}
                                       : JsonValue{});
    object.emplace_back("step_kind", signature.step_kind.has_value()
                                         ? JsonValue{*signature.step_kind}
                                         : JsonValue{});
    object.emplace_back("reason_code", signature.reason_code);
    return JsonValue{std::move(object)};
}

Result<WorkflowFailureSignature>
failure_signature_from_json(const JsonValue &json, const WorkflowLearningLimits &limits) {
    static constexpr std::string_view kRequired[] = {"workflow_id", "reason_code"};
    static constexpr std::string_view kOptional[] = {"step_id", "step_kind"};
    if (auto check = check_keys(json, kRequired, kOptional, "failure signature");
        !check.has_value()) {
        return check.error();
    }
    WorkflowFailureSignature signature;
    auto workflow_id = parse_identifier_member(json, "workflow_id", "failure signature");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    if (workflow_id.value().size() > limits.max_id_bytes) {
        return learning_error(WorkflowLearningError::LimitExceeded,
                              "failure signature workflow_id exceeds limit");
    }
    signature.workflow_id = std::move(workflow_id.value());
    for (const auto &[key, target] :
         std::array<std::pair<const char *, std::optional<std::string> *>, 2>{
             std::pair{"step_id", &signature.step_id},
             std::pair{"step_kind", &signature.step_kind}}) {
        const auto *member = json.find(key);
        if (member != nullptr && !member->is_null()) {
            auto text = bounded_string(*member, limits.max_id_bytes);
            if (!text.has_value()) {
                return learning_error(WorkflowLearningError::InvalidShape,
                                      std::string{"failure signature '"} + key +
                                          "' must be a bounded string");
            }
            *target = *text;
        }
    }
    const auto *reason = json.find("reason_code");
    if (reason == nullptr || !reason->is_string()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "failure signature: 'reason_code' must be a string");
    }
    auto sanitized = sanitized_reason(*reason->as_string(), limits);
    if (!sanitized.has_value()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "failure signature reason_code is not a sanitized identifier");
    }
    signature.reason_code = *sanitized;
    return signature;
}

// ---------------------------------------------------------------------------
// WorkflowEpisodeRecord (DEC-029 §2)
// ---------------------------------------------------------------------------

JsonValue workflow_episode_to_json(const WorkflowEpisodeRecord &episode) {
    JsonValue::Object object;
    object.emplace_back("schema_version", schema_version_to_json(episode.schema_version));
    object.emplace_back("run_id", episode.run_id);
    object.emplace_back("workflow_id", episode.workflow_id);
    object.emplace_back("ir_digest", episode.ir_digest);
    object.emplace_back("policy", episode.policy);
    object.emplace_back("outcome", episode.outcome);
    object.emplace_back("failed_step_id", episode.failed_step_id.has_value()
                                              ? JsonValue{*episode.failed_step_id}
                                              : JsonValue{});
    object.emplace_back("failure_reason_code", episode.failure_reason_code.has_value()
                                                   ? JsonValue{*episode.failure_reason_code}
                                                   : JsonValue{});
    object.emplace_back("escalations", static_cast<std::uint64_t>(episode.escalations));
    object.emplace_back("checkpoint_handoffs",
                        static_cast<std::uint64_t>(episode.checkpoint_handoffs));
    object.emplace_back("recorded_at_ms", episode.recorded_at_ms);
    return JsonValue{std::move(object)};
}

Result<WorkflowEpisodeRecord>
workflow_episode_from_json(const JsonValue &json, const WorkflowLearningLimits &limits) {
    static constexpr std::string_view kRequired[] = {
        "schema_version", "run_id", "workflow_id", "ir_digest", "policy",
        "outcome",        "escalations", "checkpoint_handoffs", "recorded_at_ms"};
    static constexpr std::string_view kOptional[] = {"failed_step_id", "failure_reason_code"};
    if (auto check = check_keys(json, kRequired, kOptional, "episode"); !check.has_value()) {
        return check.error();
    }
    WorkflowEpisodeRecord episode;
    const auto *version = json.find("schema_version");
    auto parsed_version = parse_schema_version(*version, "episode");
    if (!parsed_version.has_value()) {
        return parsed_version.error();
    }
    if (parsed_version.value().major != 1) {
        return learning_error(WorkflowLearningError::VersionMismatch,
                              "episode schema major is unsupported");
    }
    episode.schema_version = parsed_version.value();
    for (const auto key : {"run_id", "workflow_id"}) {
        auto text = parse_identifier_member(json, key, "episode");
        if (!text.has_value()) {
            return text.error();
        }
        if (text.value().size() > limits.max_id_bytes) {
            return learning_error(WorkflowLearningError::LimitExceeded,
                                  std::string{"episode "} + key + " exceeds limit");
        }
        if (std::string_view{key} == "run_id") {
            episode.run_id = std::move(text.value());
        } else {
            episode.workflow_id = std::move(text.value());
        }
    }
    auto digest = parse_hex_digest_member(json, "ir_digest", "episode");
    if (!digest.has_value()) {
        return digest.error();
    }
    episode.ir_digest = std::move(digest.value());
    auto policy_text = parse_identifier_member(json, "policy", "episode");
    if (!policy_text.has_value()) {
        return policy_text.error();
    }
    if (!parse_workflow_policy(policy_text.value()).has_value()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "episode policy names no known workflow policy");
    }
    episode.policy = std::move(policy_text.value());
    auto outcome_text = parse_identifier_member(json, "outcome", "episode");
    if (!outcome_text.has_value()) {
        return outcome_text.error();
    }
    const auto &outcome = outcome_text.value();
    if (outcome != "completed" && outcome != "failed" && outcome != "cancelled") {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "episode outcome is outside the closed set");
    }
    episode.outcome = std::move(outcome_text.value());
    for (const auto &[key, target] :
         std::array<std::pair<const char *, std::optional<std::string> *>, 2>{
             std::pair{"failed_step_id", &episode.failed_step_id},
             std::pair{"failure_reason_code", &episode.failure_reason_code}}) {
        const auto *member = json.find(key);
        if (member != nullptr && !member->is_null()) {
            if (!member->is_string()) {
                return learning_error(WorkflowLearningError::InvalidShape,
                                      std::string{"episode '"} + key + "' must be a string");
            }
            const auto &text = *member->as_string();
            if (std::string_view{key} == "failure_reason_code") {
                auto sanitized = sanitized_reason(text, limits);
                if (!sanitized.has_value()) {
                    return learning_error(WorkflowLearningError::InvalidShape,
                                          "episode failure_reason_code is not sanitized");
                }
            } else if (text.empty() || text.size() > limits.max_id_bytes) {
                return learning_error(WorkflowLearningError::InvalidShape,
                                      "episode failed_step_id must be bounded");
            }
            *target = text;
        }
    }
    if (episode.outcome == "failed" &&
        (!episode.failed_step_id.has_value() || !episode.failure_reason_code.has_value())) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "failed episodes require the failure signature fields");
    }
    for (const auto &[key, target] :
         std::array<std::pair<const char *, std::uint32_t *>, 2>{
             std::pair{"escalations", &episode.escalations},
             std::pair{"checkpoint_handoffs", &episode.checkpoint_handoffs}}) {
        auto count = parse_count_member(json, key, "episode");
        if (!count.has_value()) {
            return count.error();
        }
        if (count.value() > std::numeric_limits<std::uint32_t>::max()) {
            return learning_error(WorkflowLearningError::LimitExceeded,
                                  std::string{"episode "} + key + " exceeds counter bounds");
        }
        *target = static_cast<std::uint32_t>(count.value());
    }
    auto recorded = parse_count_member(json, "recorded_at_ms", "episode");
    if (!recorded.has_value()) {
        return recorded.error();
    }
    episode.recorded_at_ms = recorded.value();
    return episode;
}

Sha256Digest workflow_episode_digest(const WorkflowEpisodeRecord &episode) {
    return canonical_json_digest(workflow_episode_to_json(episode));
}

// ---------------------------------------------------------------------------
// WorkflowRecoveryLesson (DEC-029 §3)
// ---------------------------------------------------------------------------

JsonValue recovery_lesson_to_json(const WorkflowRecoveryLesson &lesson) {
    JsonValue::Object object;
    object.emplace_back("schema_version", schema_version_to_json(lesson.schema_version));
    object.emplace_back("lesson_id", lesson.lesson_id);
    object.emplace_back("workflow_id", lesson.workflow_id);
    object.emplace_back("ir_digest", lesson.ir_digest);
    object.emplace_back("recovered_run_id", lesson.recovered_run_id);
    object.emplace_back("failure", failure_signature_to_json(lesson.failure));
    JsonValue::Array recovery;
    recovery.reserve(lesson.recovery.size());
    for (const auto &action : lesson.recovery) {
        JsonValue::Object entry;
        entry.emplace_back("patch_id", action.patch_id);
        entry.emplace_back("patch_digest", action.patch_digest);
        JsonValue::Array targets;
        targets.reserve(action.targets.size());
        for (const auto &target : action.targets) {
            targets.emplace_back(target);
        }
        entry.emplace_back("targets", JsonValue{std::move(targets)});
        recovery.emplace_back(JsonValue{std::move(entry)});
    }
    object.emplace_back("recovery", JsonValue{std::move(recovery)});
    object.emplace_back("resumed_without_patch", lesson.resumed_without_patch);
    object.emplace_back("outcome", lesson.outcome);
    object.emplace_back("recorded_at_ms", lesson.recorded_at_ms);
    return JsonValue{std::move(object)};
}

Result<WorkflowRecoveryLesson>
recovery_lesson_from_json(const JsonValue &json, const WorkflowLearningLimits &limits) {
    static constexpr std::string_view kRequired[] = {"schema_version", "lesson_id",    "workflow_id",
                                                     "ir_digest",      "recovered_run_id",
                                                     "failure",        "recovery",
                                                     "resumed_without_patch", "outcome",
                                                     "recorded_at_ms"};
    if (auto check = check_keys(json, kRequired, {}, "lesson"); !check.has_value()) {
        return check.error();
    }
    WorkflowRecoveryLesson lesson;
    const auto *version = json.find("schema_version");
    auto parsed_version = parse_schema_version(*version, "lesson");
    if (!parsed_version.has_value()) {
        return parsed_version.error();
    }
    if (parsed_version.value().major != 1) {
        return learning_error(WorkflowLearningError::VersionMismatch,
                              "lesson schema major is unsupported");
    }
    lesson.schema_version = parsed_version.value();
    for (const auto key : {"lesson_id", "workflow_id", "recovered_run_id"}) {
        auto text = parse_identifier_member(json, key, "lesson");
        if (!text.has_value()) {
            return text.error();
        }
        if (text.value().size() > limits.max_id_bytes) {
            return learning_error(WorkflowLearningError::LimitExceeded,
                                  std::string{"lesson "} + key + " exceeds limit");
        }
        if (std::string_view{key} == "lesson_id") {
            lesson.lesson_id = std::move(text.value());
        } else if (std::string_view{key} == "workflow_id") {
            lesson.workflow_id = std::move(text.value());
        } else {
            lesson.recovered_run_id = std::move(text.value());
        }
    }
    auto digest = parse_hex_digest_member(json, "ir_digest", "lesson");
    if (!digest.has_value()) {
        return digest.error();
    }
    lesson.ir_digest = std::move(digest.value());
    auto failure = failure_signature_from_json(*json.find("failure"), limits);
    if (!failure.has_value()) {
        return failure.error();
    }
    lesson.failure = failure.value();
    const auto *recovery = json.find("recovery");
    if (recovery == nullptr || !recovery->is_array()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "lesson recovery must be an array");
    }
    if (recovery->as_array()->size() > limits.max_recovery_actions) {
        return learning_error(WorkflowLearningError::LimitExceeded,
                              "lesson recovery actions exceed limit");
    }
    for (const auto &entry : *recovery->as_array()) {
        static constexpr std::string_view kEntryKeys[] = {"patch_id", "patch_digest", "targets"};
        if (auto check = check_keys(entry, kEntryKeys, {}, "lesson recovery entry");
            !check.has_value()) {
            return check.error();
        }
        WorkflowRecoveryAction action;
        auto patch_id = parse_identifier_member(entry, "patch_id", "lesson recovery entry");
        if (!patch_id.has_value()) {
            return patch_id.error();
        }
        if (patch_id.value().size() > limits.max_id_bytes) {
            return learning_error(WorkflowLearningError::LimitExceeded,
                                  "lesson recovery patch_id exceeds limit");
        }
        action.patch_id = std::move(patch_id.value());
        auto patch_digest = parse_hex_digest_member(entry, "patch_digest", "lesson recovery entry");
        if (!patch_digest.has_value()) {
            return patch_digest.error();
        }
        action.patch_digest = std::move(patch_digest.value());
        const auto *targets = entry.find("targets");
        if (targets == nullptr || !targets->is_array()) {
            return learning_error(WorkflowLearningError::InvalidShape,
                                  "lesson recovery targets must be an array");
        }
        for (const auto &target : *targets->as_array()) {
            auto text = bounded_string(target, limits.max_id_bytes);
            if (!text.has_value()) {
                return learning_error(WorkflowLearningError::InvalidShape,
                                      "lesson recovery target must be a bounded string");
            }
            if (!parse_workflow_patch_target(*text).has_value() && *text != "skip") {
                return learning_error(WorkflowLearningError::InvalidShape,
                                      "lesson recovery target is outside the closed set");
            }
            action.targets.push_back(*text);
        }
        lesson.recovery.push_back(std::move(action));
    }
    const auto *resumed = json.find("resumed_without_patch");
    if (resumed == nullptr || !resumed->is_boolean()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "lesson resumed_without_patch must be a boolean");
    }
    lesson.resumed_without_patch = *resumed->as_boolean();
    auto outcome_text = parse_identifier_member(json, "outcome", "lesson");
    if (!outcome_text.has_value()) {
        return outcome_text.error();
    }
    if (outcome_text.value() != "recovered") {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "lesson outcome must be 'recovered'");
    }
    lesson.outcome = std::move(outcome_text.value());
    auto recorded = parse_count_member(json, "recorded_at_ms", "lesson");
    if (!recorded.has_value()) {
        return recorded.error();
    }
    lesson.recorded_at_ms = recorded.value();
    if (!lesson.resumed_without_patch && lesson.recovery.empty()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "lesson requires recovery actions or the resumed_without_patch mark");
    }
    return lesson;
}

Sha256Digest recovery_lesson_digest(const WorkflowRecoveryLesson &lesson) {
    return canonical_json_digest(recovery_lesson_to_json(lesson));
}

// ---------------------------------------------------------------------------
// Deterministic ids and pure conversions (DEC-029 §5)
// ---------------------------------------------------------------------------

Id128 learning_id_from_seed(std::string_view seed) {
    const auto digest = digest_string(seed);
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id128{bytes};
}

MemoryId workflow_episode_memory_id(const std::string &run_id) {
    return MemoryId{learning_id_from_seed("mira.workflow.episode|" + run_id)};
}

MutationId workflow_episode_mutation_id(const std::string &run_id) {
    return MutationId{learning_id_from_seed("mira.workflow.episode.mutation|" + run_id)};
}

MemoryId recovery_lesson_memory_id(const std::string &run_id) {
    return MemoryId{learning_id_from_seed("mira.workflow.lesson|" + run_id)};
}

MutationId recovery_lesson_mutation_id(const std::string &run_id) {
    return MutationId{learning_id_from_seed("mira.workflow.lesson.mutation|" + run_id)};
}

MemoryRecord episode_to_memory_record(const WorkflowEpisodeRecord &episode,
                                      const MemoryScope &scope,
                                      const std::vector<EventId> &evidence,
                                      std::chrono::system_clock::time_point now) {
    MemoryRecord record;
    record.id = workflow_episode_memory_id(episode.run_id);
    record.scope = scope;
    record.kind = MemoryKind::Episode;
    record.statement = canonical_json_string(workflow_episode_to_json(episode));
    record.validity.valid_from = now;
    record.recorded_at = now;
    record.provenance = evidence;
    record.verification = MemoryVerification::Verified;
    record.confidence = 1.0F;
    record.sensitivity = Sensitivity::Internal;
    record.status = MemoryStatus::Active;
    record.schema_version = memory_schema_current();
    record.version = 1;
    return record;
}

MemoryRecord recovery_lesson_to_memory_record(const WorkflowRecoveryLesson &lesson,
                                              const MemoryScope &scope,
                                              const std::vector<EventId> &evidence,
                                              std::chrono::system_clock::time_point now) {
    MemoryRecord record;
    record.id = recovery_lesson_memory_id(lesson.recovered_run_id);
    record.scope = scope;
    record.kind = MemoryKind::RecoveryLesson;
    record.statement = canonical_json_string(recovery_lesson_to_json(lesson));
    record.validity.valid_from = now;
    record.recorded_at = now;
    record.provenance = evidence;
    record.verification = MemoryVerification::Verified;
    record.confidence = 1.0F;
    record.sensitivity = Sensitivity::Internal;
    record.status = MemoryStatus::Active;
    record.schema_version = memory_schema_current();
    record.version = 1;
    return record;
}

Result<WorkflowRecoveryLesson> recovery_lesson_from_record(const MemoryRecord &record) {
    JsonValue json;
    if (const auto parsed = parse_json(record.statement); parsed.has_value()) {
        json = parsed.value();
    } else {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "memory record statement is not canonical lesson JSON");
    }
    auto lesson = recovery_lesson_from_json(json);
    if (!lesson.has_value()) {
        return lesson.error();
    }
    // Canonical form check: the statement must round-trip losslessly, so the
    // reuse surface only ever consumes this contract's serialization.
    if (canonical_json_string(recovery_lesson_to_json(lesson.value())) != record.statement) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "memory record statement is not canonical lesson JSON");
    }
    return lesson;
}

// ---------------------------------------------------------------------------
// Failure-retrieval query construction (DEC-029 §4)
// ---------------------------------------------------------------------------

Result<MemoryQuery> failure_retrieval_query(const WorkflowFailureSignature &signature,
                                            const MemoryScope &scope,
                                            const WorkflowLearningLimits &limits) {
    if (auto valid = limits.validate(); !valid.has_value()) {
        return valid.error();
    }
    if (signature.workflow_id.empty()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "failure signature requires a workflow id");
    }
    if (!sanitized_reason(signature.reason_code, limits).has_value()) {
        return learning_error(WorkflowLearningError::InvalidShape,
                              "failure signature reason_code is not a sanitized identifier");
    }
    MemoryQuery query;
    query.scopes = {scope};
    query.kinds = std::vector<MemoryKind>{MemoryKind::Episode, MemoryKind::RecoveryLesson};
    query.exact_terms = {signature.workflow_id};
    if (signature.step_id.has_value() && !signature.step_id->empty()) {
        query.exact_terms.push_back(*signature.step_id);
    }
    std::string text = signature.workflow_id;
    if (signature.step_id.has_value() && !signature.step_id->empty()) {
        text += " " + *signature.step_id;
    }
    text += " " + signature.reason_code;
    query.text = std::move(text);
    query.max_results = limits.max_retrieval_results;
    query.token_budget = limits.retrieval_token_budget;
    query.deadline = limits.retrieval_deadline;
    if (auto valid = query.validate(); !valid.has_value()) {
        return valid.error();
    }
    return query;
}

} // namespace mira
