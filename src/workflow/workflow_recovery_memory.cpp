#include <mira/workflow_runtime.hpp>

#include <algorithm>
#include <utility>

namespace mira {
namespace {

Error recovery_memory_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = message;
    return error;
}

bool recovery_access_allowed(const WorkflowRuntime::RecoveryLessonAuthorizer &authorize,
                             const MemoryScope &scope, const RecoveryLessonKey &key) {
    try {
        return authorize && authorize(scope, key);
    } catch (...) {
        return false;
    }
}

bool matches_lesson(const MemoryRecord &record, const MemoryScope &scope,
                    const RecoveryLessonKey &key, std::chrono::system_clock::time_point now) {
    if (record.scope != scope || record.kind != MemoryKind::RecoveryLesson ||
        record.status != MemoryStatus::Active || record.source_namespace ||
        record.provenance.empty() ||
        memory_verification_rank(record.verification) <
            memory_verification_rank(MemoryVerification::Verified) ||
        record.recorded_at > now || record.validity.valid_from > now ||
        (record.validity.valid_until && *record.validity.valid_until <= now) ||
        (record.expires_at && *record.expires_at <= now))
        return false;

    const auto lesson = recovery_lesson_from_record(record);
    const auto workflow = key.workflow.to_string();
    const auto step = key.step ? std::optional{key.step->to_string()} : std::nullopt;
    return lesson && lesson.value().workflow_id == workflow &&
           lesson.value().ir_digest == key.ir_digest.to_string() &&
           lesson.value().failure ==
               WorkflowFailureSignature{workflow, step, key.step_kind, key.reason_code};
}

} // namespace

Result<MemoryQueryResult>
WorkflowRuntime::query_verified_recovery_lessons(const IMemory &memory, const MemoryQuery &query) {
    auto recalled = memory.query(query);
    if (!recalled)
        return recalled.error();
    const auto now = std::chrono::system_clock::now();
    if (recalled.value().records.size() > query.max_results ||
        recalled.value().tokens_estimate > query.token_budget ||
        !std::ranges::all_of(recalled.value().records, [&](const MemoryRecord &record) {
            return matches_lesson(record, query.scopes.front(), *query.recovery_lesson_key, now);
        }))
        return recovery_memory_error(ErrorCode::DataLoss, "recovery recall returned invalid data");
    return std::move(recalled).value();
}

Result<MemoryQueryResult>
WorkflowRuntime::recall_recovery_lessons(const RecoveryLessonKey &key,
                                         const RecoveryLessonAuthorizer &authorize) const {
    if (auto valid = key.validate(); !valid)
        return valid.error();
    std::shared_ptr<IMemory> memory;
    MemoryScope scope;
    WorkflowLearningLimits limits;
    std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_);
        if (shut_down_)
            return recovery_memory_error(ErrorCode::Unavailable, "workflow runtime is shut down");
        if (!learning_memory_)
            return recovery_memory_error(ErrorCode::InvalidState,
                                         "recovery recall requires a learning context");
        memory = learning_memory_;
        scope = learning_scope_;
        limits = learning_limits_;
        revision = learning_revision_;
    }

    if (!recovery_access_allowed(authorize, scope, key))
        return recovery_memory_error(ErrorCode::PermissionDenied, "recovery recall access denied");
    MemoryQuery query;
    query.scopes = {scope};
    query.kinds = std::vector{MemoryKind::RecoveryLesson};
    query.recovery_lesson_key = key;
    query.max_results = limits.max_retrieval_results;
    query.token_budget = limits.retrieval_token_budget;
    query.deadline = limits.retrieval_deadline;
    auto recalled = query_verified_recovery_lessons(*memory, query);
    if (!recovery_access_allowed(authorize, scope, key))
        return recovery_memory_error(ErrorCode::PermissionDenied, "recovery recall access denied");
    {
        std::lock_guard lock(mutex_);
        if (shut_down_)
            return recovery_memory_error(ErrorCode::Unavailable, "workflow runtime is shut down");
        if (revision != learning_revision_)
            return recovery_memory_error(ErrorCode::InvalidState,
                                         "recovery learning context changed");
    }
    return recalled;
}

} // namespace mira
