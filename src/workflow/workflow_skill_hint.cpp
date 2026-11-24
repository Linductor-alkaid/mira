#include <mira/content_markers.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/tool_module.hpp>
#include <mira/tool_skill.hpp>
#include <mira/workflow_runtime.hpp>

#include <algorithm>
#include <utility>

namespace mira {
namespace {

constexpr std::string_view kSkillHintSchema = "mira.skill.hint.v1";
constexpr std::size_t kMaxHintBytes = 512;

Error hint_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow.skill_hint";
    error.safe_message = message;
    return error;
}

bool hint_access_allowed(const WorkflowRuntime::SkillHintAuthorizer &authorize,
                         const MemoryScope &scope, std::string_view name,
                         WorkflowRuntime::SkillHintOperation operation) {
    try {
        return authorize && authorize(scope, name, operation);
    } catch (...) {
        return false;
    }
}

std::string hint_seed(const MemoryScope &scope, const Hash &descriptor, WorkflowRunId run) {
    return std::string(kSkillHintSchema) + "|" +
           canonical_json_digest(memory_scope_to_json(scope)).to_string() + "|" +
           descriptor.to_string() + "|" + run.to_string();
}

std::string hint_label(const Hash &descriptor) {
    return digest_string("mira.skill.hint.label|" + descriptor.to_string()).to_string();
}

bool valid_hint_text(std::string_view hint) {
    return hint.size() <= kMaxHintBytes &&
           hint.find_first_not_of(" \t\r\n") != std::string_view::npos &&
           hint.find('\0') == std::string_view::npos &&
           parse_json(canonical_json_string(JsonValue{std::string(hint)}));
}

std::string hint_statement(const Hash &descriptor, WorkflowRunId run, std::string_view hint) {
    return canonical_json_string(JsonValue::Object{{"schema", std::string(kSkillHintSchema)},
                                                   {"descriptor", descriptor.to_string()},
                                                   {"run_id", run.to_string()},
                                                   {"hint", std::string(hint)}});
}

bool matches_hint(const MemoryRecord &record, const MemoryScope &scope, const Hash &descriptor,
                  std::chrono::system_clock::time_point now) {
    if (record.scope != scope || record.kind != MemoryKind::SkillHint ||
        record.status != MemoryStatus::Active ||
        record.verification != MemoryVerification::Observed || record.source_namespace ||
        record.provenance.size() != 1 || record.provenance.front().is_nil() ||
        record.recorded_at > now || record.validity.valid_from > now ||
        (record.validity.valid_until && *record.validity.valid_until <= now) ||
        (record.expires_at && *record.expires_at <= now))
        return false;
    const auto value = parse_json(record.statement);
    if (!value)
        return false;
    const auto *run_id = value.value().find("run_id");
    const auto *hint = value.value().find("hint");
    if (!run_id || !run_id->as_string() || !hint || !hint->as_string())
        return false;
    const auto run = WorkflowRunId::parse(*run_id->as_string());
    return run && !run->is_nil() && valid_hint_text(*hint->as_string()) &&
           record.id == MemoryId{learning_id_from_seed(hint_seed(scope, descriptor, *run))} &&
           record.statement == hint_statement(descriptor, *run, *hint->as_string());
}

} // namespace

Result<WorkflowRuntime::SkillHintContext> WorkflowRuntime::skill_hint_context() const {
    std::lock_guard lock(mutex_);
    if (shut_down_)
        return hint_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    if (!learning_memory_)
        return hint_error(ErrorCode::InvalidState, "skill hint requires a learning context");
    return SkillHintContext{learning_memory_, learning_scope_, learning_limits_,
                            learning_revision_};
}

Result<MemoryMutationResult>
WorkflowRuntime::record_skill_hint(const SkillPublicationRegistry &skills, std::string_view name,
                                   const WorkflowRunId &source_run, std::string_view hint,
                                   const SkillHintAuthorizer &authorize) {
    if (!is_valid_vocabulary_id(name) || source_run.is_nil() || !valid_hint_text(hint))
        return hint_error(ErrorCode::InvalidArgument, "invalid skill hint");
    const auto context = skill_hint_context();
    if (!context)
        return context.error();
    const auto &binding = context.value();
    if (!hint_access_allowed(authorize, binding.scope, name, SkillHintOperation::Record))
        return hint_error(ErrorCode::PermissionDenied, "skill hint access denied");
    const auto published = skills.find(name);
    if (!published || published->status != SkillPublicationStatus::Published)
        return hint_error(ErrorCode::NotFound, "skill publication is unavailable");
    const auto &descriptor = published->descriptor.digest;
    const auto evidence = skill_hint_evidence(source_run, descriptor, binding.revision);
    if (!evidence)
        return evidence.error();

    const auto seed = hint_seed(binding.scope, descriptor, source_run);
    const auto id = MemoryId{learning_id_from_seed(seed)};
    MemoryCandidate candidate;
    candidate.proposed.id = id;
    candidate.proposed.scope = binding.scope;
    candidate.proposed.kind = MemoryKind::SkillHint;
    candidate.proposed.statement = hint_statement(descriptor, source_run, hint);
    candidate.proposed.provenance = {evidence.value()};
    candidate.proposed.verification = MemoryVerification::Observed;
    candidate.proposed.confidence = 0.3F;
    candidate.proposed.labels = {hint_label(descriptor)};
    candidate.evidence = candidate.proposed.provenance;
    candidate.immutable_add_id = MutationId{learning_id_from_seed(seed + "|mutation")};
    const auto result = MemoryConsolidator{}.consolidate_candidates(
        *binding.memory, {std::move(candidate)}, binding.scope, Timestamp::now());
    if (!result)
        return result.error();
    const auto &entry = result.value().entries.front();
    switch (entry.disposition) {
    case CandidateDisposition::Applied:
        return MemoryMutationResult{entry.idempotent_replay ? MemoryMutationType::Noop
                                                            : MemoryMutationType::Add,
                                    id, entry.resulting_version, entry.idempotent_replay};
    case CandidateDisposition::RejectedForbidden:
    case CandidateDisposition::RejectedInjection:
        return hint_error(ErrorCode::SafetyRejected, "skill hint content was rejected");
    case CandidateDisposition::RejectedInvalid:
        return hint_error(ErrorCode::InvalidArgument,
                          "skill hint is invalid in this learning scope");
    case CandidateDisposition::PendingApproval:
    case CandidateDisposition::RejectedConflict:
        return hint_error(ErrorCode::InvalidState, "skill hint could not be stored");
    }
    return hint_error(ErrorCode::Internal, "unknown skill hint result");
}

Result<MemoryQueryResult>
WorkflowRuntime::recall_skill_hints(const SkillPublicationRegistry &skills, std::string_view name,
                                    const SkillHintAuthorizer &authorize,
                                    std::optional<Hash> expected_descriptor) const {
    if (!is_valid_vocabulary_id(name))
        return hint_error(ErrorCode::InvalidArgument, "invalid skill name");
    const auto context = skill_hint_context();
    if (!context)
        return context.error();
    const auto &binding = context.value();
    if (!hint_access_allowed(authorize, binding.scope, name, SkillHintOperation::Read))
        return hint_error(ErrorCode::PermissionDenied, "skill hint access denied");
    const auto published = skills.find(name);
    if (!published || published->status != SkillPublicationStatus::Published ||
        (expected_descriptor && published->descriptor.digest != *expected_descriptor))
        return MemoryQueryResult{};

    const auto &descriptor = published->descriptor.digest;
    MemoryQuery query;
    query.scopes = {binding.scope};
    query.kinds = std::vector{MemoryKind::SkillHint};
    query.labels = std::vector{hint_label(descriptor)};
    query.max_results = binding.limits.max_retrieval_results;
    query.token_budget = binding.limits.retrieval_token_budget;
    query.deadline = binding.limits.retrieval_deadline;
    auto recalled = binding.memory->query(query);
    if (!hint_access_allowed(authorize, binding.scope, name, SkillHintOperation::Read))
        return hint_error(ErrorCode::PermissionDenied, "skill hint access denied");
    {
        std::lock_guard lock(mutex_);
        if (shut_down_ || binding.revision != learning_revision_)
            return hint_error(ErrorCode::InvalidState, "skill hint learning context changed");
    }
    const auto current = skills.find(name);
    if (!current || current->status != SkillPublicationStatus::Published ||
        current->descriptor.digest != descriptor)
        return MemoryQueryResult{};
    if (!recalled)
        return recalled.error();
    const auto now = std::chrono::system_clock::now();
    if (recalled.value().records.size() > query.max_results ||
        recalled.value().tokens_estimate > query.token_budget ||
        !std::ranges::all_of(recalled.value().records, [&](const MemoryRecord &record) {
            return matches_hint(record, binding.scope, descriptor, now);
        }))
        return hint_error(ErrorCode::DataLoss, "skill hint query returned invalid data");
    return std::move(recalled).value();
}

} // namespace mira
