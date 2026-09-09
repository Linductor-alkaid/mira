#include <mira/workflow_runtime.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <numeric>
#include <thread>
#include <utility>

namespace mira {
namespace {

Error workflow_error(ErrorCode code, const std::string &message, bool retryable = false) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = message;
    error.retryable = retryable;
    return error;
}

// Stable runtime domain codes (workflow_runtime_design §6; append-only).
enum class WorkflowRuntimeError : std::int32_t {
    LoopBudgetExceeded = 1,
    StepBudgetExceeded = 2,
    StepAttemptsExceeded = 3,
    VerifyFailed = 4,
    VerifyNotEvaluable = 5,
    ToolDispatchFailed = 6,
    NavigateUnresolvable = 7,
    ResumeUncertainSideEffect = 8,
    PolicyNotSupported = 9,
    ToolBindingInvalid = 10,
    VerificationRequired = 11,
    PolicyNotInteractive = 12,
    PatchIdConflict = 13,
    PatchRejected = 14,
    PolicySwitchRejected = 15,
    PatchCapacityExceeded = 16,
    DecisionNotPending = 17,
    DecisionMismatch = 18,
    UnknownPatchTarget = 19,
    EscalationBudgetExceeded = 20,
    NavigateNoScreenState = 21,
    NavigateTargetUnknown = 22,
    NavigatePlanFailed = 23,
    NavigateArrivalUnverified = 24,
    NavigateTargetInvalid = 25,
};

Error make_runtime_error(WorkflowRuntimeError code, const std::string &detail) {
    Error error;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = detail;
    switch (code) {
    case WorkflowRuntimeError::LoopBudgetExceeded:
    case WorkflowRuntimeError::StepBudgetExceeded:
    case WorkflowRuntimeError::StepAttemptsExceeded:
    case WorkflowRuntimeError::EscalationBudgetExceeded:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case WorkflowRuntimeError::VerifyFailed:
    case WorkflowRuntimeError::VerifyNotEvaluable:
        error.code = ErrorCode::InvalidObservation;
        break;
    case WorkflowRuntimeError::ToolDispatchFailed:
        error.code = ErrorCode::PlatformError;
        break;
    case WorkflowRuntimeError::NavigateUnresolvable:
    case WorkflowRuntimeError::PolicyNotSupported:
    case WorkflowRuntimeError::PolicyNotInteractive:
        error.code = ErrorCode::UnsupportedCapability;
        break;
    case WorkflowRuntimeError::ResumeUncertainSideEffect:
        error.code = ErrorCode::ExecutionUncertain;
        break;
    case WorkflowRuntimeError::ToolBindingInvalid:
    case WorkflowRuntimeError::VerificationRequired:
        error.code = ErrorCode::InvalidArgument;
        break;
    case WorkflowRuntimeError::PatchIdConflict:
        error.code = ErrorCode::AlreadyExists;
        break;
    case WorkflowRuntimeError::PatchRejected:
    case WorkflowRuntimeError::PolicySwitchRejected:
    case WorkflowRuntimeError::UnknownPatchTarget:
        error.code = ErrorCode::InvalidArgument;
        break;
    case WorkflowRuntimeError::PatchCapacityExceeded:
        error.code = ErrorCode::ResourceExhausted;
        error.retryable = true;
        break;
    case WorkflowRuntimeError::DecisionNotPending:
    case WorkflowRuntimeError::DecisionMismatch:
        error.code = ErrorCode::InvalidState;
        break;
    case WorkflowRuntimeError::NavigateNoScreenState:
    case WorkflowRuntimeError::NavigatePlanFailed:
    case WorkflowRuntimeError::NavigateArrivalUnverified:
        error.code = ErrorCode::InvalidObservation;
        break;
    case WorkflowRuntimeError::NavigateTargetUnknown:
    case WorkflowRuntimeError::NavigateTargetInvalid:
        error.code = ErrorCode::InvalidArgument;
        break;
    }
    return error;
}

[[nodiscard]] std::string bounded_summary(std::string summary) {
    if (summary.size() > kWorkflowEventMaxSummaryBytes) {
        summary.resize(kWorkflowEventMaxSummaryBytes);
    }
    return summary;
}

// Wall-clock epoch milliseconds for confidence bookkeeping (DEC-027 §2);
// system_clock periods differ across platforms, so cast explicitly.
[[nodiscard]] std::uint64_t now_millis() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] bool task_withdrawn(TaskState state) noexcept {
    switch (state) {
    case TaskState::Pausing:
    case TaskState::Paused:
    case TaskState::Cancelling:
    case TaskState::Cancelled:
    case TaskState::TakeoverSettling:
    case TaskState::SuspendedForTakeover:
    case TaskState::Completed:
    case TaskState::Failed:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool task_pause_family(TaskState state) noexcept {
    switch (state) {
    case TaskState::Pausing:
    case TaskState::Paused:
    case TaskState::TakeoverSettling:
    case TaskState::SuspendedForTakeover:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::string verification_name(WorkflowPredicateResult result) {
    switch (result) {
    case WorkflowPredicateResult::Satisfied:
        return "satisfied";
    case WorkflowPredicateResult::NotSatisfied:
        return "not_satisfied";
    case WorkflowPredicateResult::NotEvaluable:
        return "not_evaluable";
    }
    return "none";
}

// Decision payload digest (DEC-024 §4): canonical digest over the prompt and
// the serialized proposal; the pair is the decision's identity for matching.
[[nodiscard]] Sha256Digest
decision_payload_digest(const std::string &prompt,
                        const std::vector<WorkflowPatchEntry> &proposal) {
    JsonValue::Array entries;
    for (const auto &entry : proposal) {
        JsonValue::Object object;
        object.emplace_back("target", workflow_patch_target_name(entry.target));
        object.emplace_back("op", workflow_patch_op_name(entry.op));
        object.emplace_back("path", entry.path);
        if (!entry.value.is_null()) {
            object.emplace_back("value", entry.value);
        }
        entries.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("prompt", prompt);
    root.emplace_back("proposal", JsonValue{std::move(entries)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

[[nodiscard]] bool contains_string(const std::vector<std::string> &values,
                                   const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

// Shared withdrawal flag for one drive. The monitor task publishes the
// carrier task's withdrawal; the drive and its step contexts only read the
// atomic, so probing stays safe while environment locks are held (the monitor
// polls on its own worker and never calls back from inside such regions).
struct WorkflowRuntime::DriveFlags final {
    std::atomic<bool> withdrawn{false};
    std::atomic<bool> done{false};
};

// One patch accepted but not yet applied; a running run drains its queue at
// each step boundary (DEC-024 §3).
struct WorkflowRuntime::PendingPatch final {
    WorkflowPatchId patch_id;
    Sha256Digest digest{};
    std::vector<WorkflowPatchEntry> entries;
};

// One applied patch with the effective state that preceded it: the rollback
// boundary (DEC-024 §2). Records are retained for idempotency matching.
struct WorkflowRuntime::AppliedPatch final {
    WorkflowPatchId patch_id;
    Sha256Digest digest{};
    std::vector<WorkflowPatchEntry> entries;
    JsonValue parameters;
    WorkflowPolicy policy = WorkflowPolicy::Strict;
    std::vector<std::string> skipped;
    std::map<std::string, JsonValue> overrides;
};

// One run: the pinned definition, bindings, per-step execution state and the
// carrier task identity. Settled history and counters feed WorkflowRunResult.
struct WorkflowRuntime::RunRecord final {
    WorkflowRunView view;
    WorkflowDefinition definition;
    WorkflowParameterBindings bindings;
    // Resolved ToolCall arguments per step index (binding applied at
    // admission); other kinds keep the raw arguments.
    std::vector<JsonValue> resolved_arguments;
    // Effective state maintained by the patch plane (DEC-024 §3).
    JsonValue effective_parameters;
    std::vector<std::string> skipped_steps;
    std::map<std::string, JsonValue> argument_overrides;
    std::vector<AppliedPatch> applied_patches;
    std::vector<PendingPatch> pending_patches;
    TaskId task;
    std::uint64_t task_epoch = 0;
    std::size_t cursor = 0;
    std::uint32_t executions = 0;
    std::vector<std::uint32_t> attempts;
    std::vector<std::uint32_t> retries;
    std::vector<std::uint32_t> jumps;
    // Checkpoint bookkeeping (DEC-023 §2): arrivals count how often the drive
    // reached a checkpoint step; handoffs how often it was surrendered. Each
    // arrival hands off at most once; a resume consumes the obligation.
    std::vector<std::uint32_t> checkpoint_arrivals;
    std::vector<std::uint32_t> checkpoint_handoffs;
    // Failure escalations consumed against max_escalations_per_run (DEC-023
    // §1); checkpoint handoffs do not count.
    std::uint32_t escalations = 0;
    bool interrupted = false;
    JsonValue predicate_context;
    std::vector<WorkflowStepRecord> settled;
    std::uint32_t unevaluable = 0;
    std::string safe_summary;
    std::string failure_reason;
    std::optional<WorkflowDecisionRequest> decision;
    bool drive_active = false;
    std::shared_future<Result<WorkflowRunResult>> drive_future;
    std::optional<WorkflowRunResult> result;
    // Stage F learning state (DEC-030): the last failure-driven escalation's
    // signature, the bounded retrieval results attached to continuations,
    // the settlement event (memory-write provenance), the applied-patch
    // watermark at the last escalation (lesson derivation) and the episode
    // write marker (idempotency beyond the store's mutation id).
    std::optional<WorkflowFailureSignature> last_failure_signature;
    std::vector<WorkflowRetrievedLesson> retrieved_lessons;
    std::optional<EventId> settled_event;
    std::size_t patches_at_last_escalation = 0;
    bool episode_recorded = false;
};

WorkflowRuntime::WorkflowRuntime(executor::Executor &executor, MiraRuntime &runtime,
                                 SessionId session, std::shared_ptr<IEnvironment> environment,
                                 WorkflowRuntimeConfig config)
    : executor_(executor), runtime_(runtime), session_(session),
      environment_(std::move(environment)), config_(config), runtime_id_(RuntimeId::generate()) {}

WorkflowRuntime::~WorkflowRuntime() {
    if (!shut_down_) {
        static_cast<void>(shutdown());
    }
}

void WorkflowRuntime::set_event_store(std::shared_ptr<IEventStore> events) {
    std::lock_guard lock(mutex_);
    events_ = std::move(events);
}

void WorkflowRuntime::set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools) {
    std::lock_guard lock(mutex_);
    tools_ = std::move(tools);
}

Result<void> WorkflowRuntime::set_navigation_context(AppModel model, ScreenStateProvider provider) {
    if (auto valid = validate_app_model(model); !valid.has_value()) {
        return valid.error();
    }
    std::lock_guard lock(mutex_);
    app_model_ = std::move(model);
    screen_state_provider_ = std::move(provider);
    return Result<void>{};
}

Result<void> WorkflowRuntime::set_app_model(AppModel model) {
    if (auto valid = validate_app_model(model); !valid.has_value()) {
        return valid.error();
    }
    std::lock_guard lock(mutex_);
    app_model_ = std::move(model);
    return Result<void>{};
}

std::optional<AppModel> WorkflowRuntime::app_model_snapshot() const {
    std::lock_guard lock(mutex_);
    return app_model_;
}

bool WorkflowRuntime::navigation_context_installed() const {
    std::lock_guard lock(mutex_);
    return app_model_.has_value() && screen_state_provider_ != nullptr;
}

// --- Stage F: learning loop (DEC-030) ----------------------------------------

Result<void> WorkflowRuntime::set_learning_context(std::shared_ptr<IMemory> memory,
                                                    MemoryScope scope,
                                                    WorkflowLearningLimits limits) {
    if (memory == nullptr) {
        return workflow_error(ErrorCode::InvalidArgument,
                              "learning context requires a memory backend");
    }
    if (scope.kind == MemoryScopeKind::User) {
        // DEC-030 §1: episodes and lessons are automatic operational
        // experience; the User domain stays the human-approval channel.
        return workflow_error(ErrorCode::InvalidArgument,
                              "learning scope must not be the user memory domain");
    }
    if (scope.subject_id.empty() && scope.kind != MemoryScopeKind::Environment &&
        scope.kind != MemoryScopeKind::Agent) {
        return workflow_error(ErrorCode::InvalidArgument,
                              "learning scope requires a subject id");
    }
    if (auto valid = limits.validate(); !valid.has_value()) {
        return valid.error();
    }
    std::lock_guard lock(mutex_);
    learning_memory_ = std::move(memory);
    learning_scope_ = std::move(scope);
    learning_limits_ = limits;
    return Result<void>{};
}

void WorkflowRuntime::record_episode(RunRecord &run, WorkflowRunState terminal_state) {
    std::shared_ptr<IMemory> memory;
    MemoryScope scope;
    {
        std::lock_guard lock(mutex_);
        if (!learning_memory_ || run.episode_recorded) {
            return;
        }
        memory = learning_memory_;
        scope = learning_scope_;
    }
    // DryRun settlements are planning rehearsals, not environment experience;
    // skipping them is a design behavior, not a failure (DEC-030 §2).
    if (run.view.policy == WorkflowPolicy::DryRun) {
        return;
    }
    // Without the settled event there is no traceable provenance, and M4
    // memory mutations require event evidence: nothing to anchor a record on.
    if (!run.settled_event.has_value()) {
        return;
    }
    WorkflowEpisodeRecord episode;
    episode.run_id = run.view.run_id.to_string();
    episode.workflow_id = run.view.workflow_id.to_string();
    episode.ir_digest = run.view.ir_digest.to_string();
    episode.policy = workflow_policy_name(run.view.policy);
    episode.escalations = run.escalations;
    episode.checkpoint_handoffs =
        std::accumulate(run.checkpoint_handoffs.begin(), run.checkpoint_handoffs.end(), 0U);
    episode.recorded_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (terminal_state == WorkflowRunState::Completed) {
        episode.outcome = "completed";
    } else if (terminal_state == WorkflowRunState::Failed) {
        episode.outcome = "failed";
        if (run.last_failure_signature.has_value()) {
            episode.failed_step_id = run.last_failure_signature->step_id;
            episode.failure_reason_code = run.last_failure_signature->reason_code;
        } else {
            episode.failure_reason_code = "unsettled-failure-signature";
        }
    } else {
        episode.outcome = "cancelled";
    }
    MemoryMutation mutation;
    mutation.id = workflow_episode_mutation_id(episode.run_id);
    mutation.type = MemoryMutationType::Add;
    mutation.scope = scope;
    mutation.proposed = episode_to_memory_record(episode, scope, {*run.settled_event},
                                                 std::chrono::system_clock::now());
    mutation.evidence = {*run.settled_event};
    mutation.reason = MutationReasonCode::VerifiedEvent;
    const auto applied = memory->apply(mutation);
    if (!applied.has_value()) {
        ++episode_record_failures_;
        emit_episode_recorded(run, Sha256Digest{},
                              "failed",
                              bounded_summary(applied.error().domain + ":" +
                                              std::to_string(applied.error().domain_code)));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        run.episode_recorded = true;
    }
    emit_episode_recorded(run, workflow_episode_digest(episode), "recorded", std::string{});
}

void WorkflowRuntime::retrieve_lessons(RunRecord &run) {
    std::shared_ptr<IMemory> memory;
    MemoryScope scope;
    WorkflowLearningLimits limits;
    std::optional<WorkflowFailureSignature> signature;
    {
        std::lock_guard lock(mutex_);
        if (!learning_memory_) {
            return;
        }
        memory = learning_memory_;
        scope = learning_scope_;
        limits = learning_limits_;
        signature = run.last_failure_signature;
        // Cover, not accumulate: only the latest escalation's retrieval is
        // attached (DEC-030 §3).
        run.retrieved_lessons.clear();
    }
    if (!signature.has_value()) {
        return;
    }
    auto query = failure_retrieval_query(*signature, scope, limits);
    if (!query.has_value()) {
        ++retrieval_failures_;
        return;
    }
    const auto found = memory->query(query.value());
    if (!found.has_value()) {
        // Degraded retrieval keeps the escalation path intact (DEC-030 §3);
        // the diagnostic counter discloses the miss.
        ++retrieval_failures_;
        return;
    }
    std::vector<WorkflowRetrievedLesson> lessons;
    lessons.reserve(found.value().records.size());
    for (const auto &record : found.value().records) {
        if (record.kind != MemoryKind::Episode && record.kind != MemoryKind::RecoveryLesson) {
            continue;
        }
        WorkflowRetrievedLesson lesson;
        lesson.kind = record.kind;
        lesson.statement = record.statement.substr(0, limits.max_document_bytes);
        lesson.confidence = record.confidence;
        lessons.push_back(std::move(lesson));
        if (lessons.size() >= limits.max_retrieval_results) {
            break;
        }
    }
    std::lock_guard lock(mutex_);
    run.retrieved_lessons = std::move(lessons);
}

void WorkflowRuntime::emit_episode_recorded(const RunRecord &run, const Sha256Digest &digest,
                                            const std::string &outcome,
                                            const std::string &reason_code) {
    if (!events_) {
        return;
    }
    WorkflowEpisodeRecordedEvent event;
    event.run_id = run.view.run_id;
    event.workflow_id = run.view.workflow_id;
    event.episode_digest = digest;
    event.outcome = outcome;
    event.reason_code = bounded_summary(reason_code);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_lesson_recorded(const RunRecord &run, const Sha256Digest &digest,
                                           const std::string &outcome,
                                           const std::string &reason_code) {
    if (!events_) {
        return;
    }
    WorkflowLessonRecordedEvent event;
    event.run_id = run.view.run_id;
    event.workflow_id = run.view.workflow_id;
    event.lesson_digest = digest;
    event.outcome = outcome;
    event.reason_code = bounded_summary(reason_code);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

Result<WorkflowRecoveryLesson> WorkflowRuntime::record_recovery_lesson(
    const WorkflowRunId &run_id) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    std::shared_ptr<IMemory> memory;
    MemoryScope scope;
    WorkflowLearningLimits limits;
    WorkflowFailureSignature signature;
    WorkflowRunView view;
    EventId settled_event;
    std::vector<AppliedPatch> recovery_patches;
    {
        std::lock_guard lock(mutex_);
        if (shut_down_) {
            return workflow_error(ErrorCode::InvalidState,
                                  "workflow runtime is shutting down");
        }
        const auto replayed = recorded_lessons_.find(run_id);
        if (replayed != recorded_lessons_.end()) {
            return replayed->second;
        }
        if (run.view.state != WorkflowRunState::Completed) {
            return workflow_error(ErrorCode::InvalidState,
                                  "recovery lessons require a completed run");
        }
        if (run.view.policy == WorkflowPolicy::DryRun) {
            return workflow_error(ErrorCode::InvalidState,
                                  "dry-run completions carry no recovery experience");
        }
        if (run.escalations == 0) {
            return workflow_error(ErrorCode::InvalidState,
                                  "run completed without a failure-driven escalation");
        }
        if (!learning_memory_) {
            return workflow_error(ErrorCode::InvalidState,
                                  "no learning context is installed");
        }
        if (!run.last_failure_signature.has_value()) {
            return workflow_error(ErrorCode::Internal,
                                  "escalation bookkeeping lost the failure signature");
        }
        if (!run.settled_event.has_value()) {
            return workflow_error(ErrorCode::InvalidState,
                                  "run settlement event is missing as provenance");
        }
        memory = learning_memory_;
        scope = learning_scope_;
        limits = learning_limits_;
        signature = *run.last_failure_signature;
        view = run.view;
        settled_event = *run.settled_event;
        recovery_patches.assign(run.applied_patches.begin() +
                                    static_cast<std::ptrdiff_t>(run.patches_at_last_escalation),
                                run.applied_patches.end());
    }
    WorkflowRecoveryLesson lesson;
    lesson.lesson_id = view.run_id.to_string();
    lesson.workflow_id = view.workflow_id.to_string();
    lesson.ir_digest = view.ir_digest.to_string();
    lesson.recovered_run_id = view.run_id.to_string();
    lesson.failure = signature;
    lesson.resumed_without_patch = recovery_patches.empty();
    lesson.outcome = "recovered";
    lesson.recorded_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    for (const auto &patch : recovery_patches) {
        WorkflowRecoveryAction action;
        action.patch_id = patch.patch_id.to_string();
        action.patch_digest = patch.digest.to_string();
        for (const auto &entry : patch.entries) {
            if (entry.op == WorkflowPatchOp::Skip) {
                action.targets.push_back("skip");
            } else {
                action.targets.push_back(workflow_patch_target_name(entry.target));
            }
        }
        lesson.recovery.push_back(std::move(action));
        if (lesson.recovery.size() >= limits.max_recovery_actions) {
            break;
        }
    }
    MemoryMutation mutation;
    mutation.id = recovery_lesson_mutation_id(lesson.recovered_run_id);
    mutation.type = MemoryMutationType::Add;
    mutation.scope = scope;
    mutation.proposed = recovery_lesson_to_memory_record(lesson, scope, {settled_event},
                                                         std::chrono::system_clock::now());
    mutation.evidence = {settled_event};
    mutation.reason = MutationReasonCode::VerifiedEvent;
    const auto applied = memory->apply(mutation);
    if (!applied.has_value()) {
        ++lesson_record_failures_;
        emit_lesson_recorded(run, Sha256Digest{}, "failed",
                             bounded_summary(applied.error().domain + ":" +
                                             std::to_string(applied.error().domain_code)));
        return applied.error();
    }
    {
        std::lock_guard lock(mutex_);
        recorded_lessons_.emplace(run_id, lesson);
    }
    emit_lesson_recorded(run, recovery_lesson_digest(lesson), "recorded", std::string{});
    return lesson;
}

Result<WorkflowRuntime::RunRecord *> WorkflowRuntime::find_run(const WorkflowRunId &run_id,
                                                               Error &error) {
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(run_id);
    if (found == runs_.end()) {
        error = workflow_error(ErrorCode::NotFound, "workflow run was not found");
        return error;
    }
    return found->second.get();
}

WorkflowRunState WorkflowRuntime::run_state(const RunRecord &run) const {
    std::lock_guard lock(mutex_);
    return run.view.state;
}

Result<Sha256Digest> WorkflowRuntime::publish_workflow(const WorkflowDefinition &definition,
                                                       const std::string &actor,
                                                       const std::string &reason,
                                                       WorkflowValidationResult validation,
                                                       std::optional<Sha256Digest> evidence) {
    if (auto valid = validate_workflow_definition(definition); !valid.has_value()) {
        return valid.error();
    }
    auto appended = append_version_record(definition, workflow_definition_digest(definition),
                                          actor, reason, validation, evidence, false);
    if (!appended.has_value()) {
        return appended.error();
    }
    return appended.value().digest;
}

Result<WorkflowRuntime::VersionAppendResult>
WorkflowRuntime::append_version_record(const WorkflowDefinition &definition, const Sha256Digest &digest,
                                       const std::string &actor, const std::string &reason,
                                       WorkflowValidationResult validation,
                                       std::optional<Sha256Digest> evidence, bool dedupe) {
    std::lock_guard lock(mutex_);
    const bool fresh_history = library_.find(definition.workflow_id) == library_.end();
    if (fresh_history) {
        WorkflowVersionHistory history;
        history.workflow_id = definition.workflow_id;
        library_.emplace(definition.workflow_id, std::move(history));
    }
    auto &history = library_[definition.workflow_id];
    if (dedupe && !history.records.empty()) {
        const WorkflowVersionRecord &head = history.records.back();
        if (head.content_digest == digest && head.validation == validation &&
            head.validation_evidence == evidence) {
            return VersionAppendResult{digest, true};
        }
    }
    WorkflowVersionRecord record;
    record.version =
        SemanticVersion{1, 0, static_cast<std::uint16_t>(history.records.size() + 1)};
    record.actor = actor;
    record.reason = reason;
    record.content_digest = digest;
    record.parent_digest =
        history.records.empty() ? Sha256Digest{} : history.records.back().content_digest;
    record.validation = validation;
    record.validation_evidence = evidence;
    record.created_at = Timestamp::now();
    if (auto appended = append_workflow_version(history, record); !appended.has_value()) {
        if (fresh_history) {
            library_.erase(definition.workflow_id);
        }
        return appended.error();
    }
    definitions_[digest] = definition;
    return VersionAppendResult{digest, false};
}

Result<WorkflowRunView> WorkflowRuntime::create_run(const WorkflowDefinition &definition,
                                                    JsonValue parameters,
                                                    std::optional<WorkflowPolicy> policy) {
    if (auto valid = validate_workflow_definition(definition); !valid.has_value()) {
        return valid.error();
    }
    return create_run_locked(definition, std::move(parameters), policy);
}

Result<WorkflowRunView> WorkflowRuntime::create_run(const WorkflowId &workflow_id,
                                                    const Sha256Digest &ir_digest,
                                                    JsonValue parameters,
                                                    std::optional<WorkflowPolicy> policy) {
    WorkflowDefinition definition;
    {
        std::lock_guard lock(mutex_);
        const auto library = library_.find(workflow_id);
        if (library == library_.end()) {
            return workflow_error(ErrorCode::NotFound, "workflow is not published");
        }
        auto record = resolve_workflow_version(library->second, ir_digest);
        if (!record.has_value()) {
            return record.error();
        }
        if (!workflow_version_is_runnable(record.value())) {
            return workflow_error(ErrorCode::InvalidArgument,
                                  "workflow version is not validated; only DryRunPassed or "
                                  "Validated versions may run");
        }
        const auto content = definitions_.find(ir_digest);
        if (content == definitions_.end()) {
            return workflow_error(ErrorCode::NotFound,
                                  "workflow definition content is unavailable");
        }
        definition = content->second;
    }
    return create_run_locked(definition, std::move(parameters), policy);
}

Result<WorkflowRunView> WorkflowRuntime::create_run_locked(const WorkflowDefinition &definition,
                                                            JsonValue parameters,
                                                            std::optional<WorkflowPolicy> policy) {
    const auto effective = policy.value_or(definition.default_policy);
    const bool dispatches = workflow_policy_dispatches_side_effects(effective);
    if (auto compatible = validate_workflow_policy_compatibility(definition, effective);
        !compatible.has_value()) {
        return compatible.error();
    }
    auto bindings = bind_workflow_parameters(definition, parameters);
    if (!bindings.has_value()) {
        return bindings.error();
    }

    // Admission-time checks that need the registry run against the current
    // exposure; dispatch re-resolves the tool identity (DEC-015 snapshot
    // rules) and fails closed if the registry changed in between.
    std::shared_ptr<BuiltinToolRegistry> tools;
    {
        std::lock_guard lock(mutex_);
        tools = tools_;
    }
    std::vector<JsonValue> resolved_arguments(definition.steps.size());
    JsonValue context(JsonValue::Object{});
    if (const auto *values = bindings.value().values.as_object()) {
        for (const auto &member : *values) {
            context.set("run_parameter:" + member.first, member.second);
        }
    }
    for (std::size_t index = 0; index < definition.steps.size(); ++index) {
        const auto &step = definition.steps[index];
        if (step.kind == WorkflowStepKind::Navigate) {
            // Stage E (DEC-028 §3): navigate arguments must resolve to an
            // object naming a string target (the same shape the patch plane
            // enforces); dispatching policies additionally require an
            // installed navigation context.
            auto resolved = resolve_step_arguments(bindings.value(), step.arguments);
            if (!resolved.has_value()) {
                return resolved.error();
            }
            const auto *target = resolved.value().find("target");
            if (!resolved.value().is_object() || target == nullptr || !target->is_string() ||
                target->as_string()->empty()) {
                return make_runtime_error(
                    WorkflowRuntimeError::NavigateTargetInvalid,
                    "navigate arguments must be an object naming a string \"target\" member");
            }
            if (dispatches && !navigation_context_installed()) {
                return make_runtime_error(
                    WorkflowRuntimeError::NavigateUnresolvable,
                    "navigate steps require an installed navigation context under dispatching "
                    "policies");
            }
            resolved_arguments[index] = std::move(resolved.value());
            continue;
        }
        if (step.kind != WorkflowStepKind::ToolCall) {
            continue;
        }
        auto resolved = resolve_step_arguments(bindings.value(), step.arguments);
        if (!resolved.has_value()) {
            return resolved.error();
        }
        const auto *tool_member = resolved.value().find("tool");
        if (!resolved.value().is_object() || tool_member == nullptr ||
            !tool_member->is_string() || tool_member->as_string()->empty()) {
            return make_runtime_error(WorkflowRuntimeError::ToolBindingInvalid,
                                      "tool_call arguments must be an object naming the target "
                                      "tool in the reserved \"tool\" member");
        }
        if (!dispatches) {
            resolved_arguments[index] = std::move(resolved.value());
            continue;
        }
        if (!tools) {
            return workflow_error(ErrorCode::InvalidState,
                                  "a tool registry is required to run tool_call steps");
        }
        const auto exposed = tools->exposed_tools();
        const auto tool = std::find_if(
            exposed.begin(), exposed.end(),
            [&](const ExposedToolSpec &entry) { return entry.wire_name == *tool_member->as_string(); });
        if (tool == exposed.end()) {
            return workflow_error(ErrorCode::NotFound, "step tool is not registered");
        }
        if (tool->has_side_effects && !step.verification.has_value()) {
            return make_runtime_error(WorkflowRuntimeError::VerificationRequired,
                                      "side-effecting tool_call steps must declare a "
                                      "verification predicate (W-02)");
        }
        resolved_arguments[index] = std::move(resolved.value());
    }

    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (active_runs_ >= config_.max_active_runs) {
            return workflow_error(ErrorCode::ResourceExhausted, "run table is at capacity", true);
        }
    }

    const std::string goal = "workflow:" + definition.name;
    auto submission = runtime_.submit_task(session_, TaskSpec{goal});
    if (!submission.has_value()) {
        return submission.error();
    }
    const auto outcome = submission.value().command.outcome(config_.command_timeout);
    if (!outcome.has_value() || !outcome.value().task.has_value()) {
        return outcome.has_value()
                   ? outcome.value().error.value_or(
                         workflow_error(ErrorCode::Internal, "task submission failed"))
                   : workflow_error(ErrorCode::DeadlineExceeded, "task submission timed out",
                                    true);
    }

    auto record = std::make_unique<RunRecord>();
    record->definition = definition;
    record->bindings = bindings.value();
    record->resolved_arguments = std::move(resolved_arguments);
    record->predicate_context = std::move(context);
    record->effective_parameters = record->bindings.values;
    record->task = submission.value().id;
    record->task_epoch = outcome.value().task->epoch;
    record->attempts.assign(definition.steps.size(), 0);
    record->retries.assign(definition.steps.size(), 0);
    record->jumps.assign(definition.steps.size(), 0);
    record->checkpoint_arrivals.assign(definition.steps.size(), 0);
    record->checkpoint_handoffs.assign(definition.steps.size(), 0);
    record->view.run_id = WorkflowRunId::generate();
    record->view.workflow_id = definition.workflow_id;
    record->view.ir_digest = workflow_definition_digest(definition);
    record->view.policy = effective;
    record->view.created_at = Timestamp::now();

    std::lock_guard lock(mutex_);
    if (!accepting_ || active_runs_ >= config_.max_active_runs) {
        const auto rollback = runtime_.cancel_task(record->task);
        if (rollback.has_value()) {
            static_cast<void>(rollback.value().outcome(config_.command_timeout));
        }
        return workflow_error(
            accepting_ ? ErrorCode::ResourceExhausted : ErrorCode::Unavailable,
            accepting_ ? "run table is at capacity" : "workflow runtime is shutting down",
            accepting_);
    }
    ++active_runs_;
    const auto view = record->view;
    runs_.emplace(view.run_id, std::move(record));
    return view;
}

Result<void> WorkflowRuntime::start_run(const WorkflowRunId &run_id) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (run.drive_active) {
            return workflow_error(ErrorCode::InvalidState, "run already has an active drive");
        }
        if (is_terminal(run.view.state)) {
            return workflow_error(ErrorCode::InvalidState, "run is terminal");
        }
        if (async_drives_ >= config_.max_concurrent_async_drives) {
            return workflow_error(ErrorCode::ResourceExhausted,
                                  "asynchronous drive capacity is exhausted", true);
        }
        ++async_drives_;
        run.drive_active = true;
    }
    try {
        auto future = executor_.submit_auto([this, run_id] {
            Error find_error;
            auto inner = find_run(run_id, find_error);
            if (!inner.has_value()) {
                return Result<WorkflowRunResult>{find_error};
            }
            RunRecord &record = *inner.value();
            OperationContext context;
            context.session = session_;
            context.task = record.task;
            context.started_at = Timestamp::now();
            auto result = drive(record, context);
            std::lock_guard lock(mutex_);
            record.result = result;
            record.drive_active = false;
            --async_drives_;
            return Result<WorkflowRunResult>{result};
        });
        std::lock_guard lock(mutex_);
        run.drive_future = future.share();
        return Result<void>{};
    } catch (const std::exception &exception) {
        std::lock_guard lock(mutex_);
        run.drive_active = false;
        --async_drives_;
        return workflow_error(ErrorCode::ResourceExhausted, exception.what(), true);
    }
}

Result<WorkflowRunResult> WorkflowRuntime::execute_run(const WorkflowRunId &run_id,
                                                        const OperationContext &context) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (run.drive_active) {
            return workflow_error(ErrorCode::InvalidState, "run already has an active drive");
        }
        if (is_terminal(run.view.state)) {
            return workflow_error(ErrorCode::InvalidState, "run is terminal");
        }
        run.drive_active = true;
    }
    auto result = drive(run, context);
    {
        std::lock_guard lock(mutex_);
        run.result = result;
        run.drive_active = false;
    }
    return result;
}

Result<WorkflowRunResult> WorkflowRuntime::wait_run(const WorkflowRunId &run_id,
                                                    std::chrono::milliseconds timeout) {
    std::shared_future<Result<WorkflowRunResult>> future;
    {
        std::lock_guard lock(mutex_);
        const auto found = runs_.find(run_id);
        if (found == runs_.end()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        // An active (or just finished) drive decides the answer; a stored
        // result is only authoritative once no drive can still overwrite it.
        if (found->second->drive_future.valid()) {
            future = found->second->drive_future;
        } else if (found->second->result.has_value()) {
            return found->second->result.value();
        } else {
            return workflow_error(ErrorCode::InvalidState, "run has no drive to wait for");
        }
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        return workflow_error(ErrorCode::DeadlineExceeded, "run drive wait timed out", true);
    }
    try {
        return future.get();
    } catch (const std::exception &exception) {
        return workflow_error(ErrorCode::Internal, exception.what());
    }
}

std::optional<Error> WorkflowRuntime::commit_transition(RunRecord &run,
                                                        WorkflowRunState target) {
    std::lock_guard lock(mutex_);
    const auto previous = run.view.state;
    auto applied = apply_workflow_run_transition(run.view, target);
    switch (applied.status) {
    case WorkflowRunTransitionStatus::Applied:
        run.view = applied.view;
        if (previous == WorkflowRunState::WaitingUser) {
            // Leaving WaitingUser settles the decision point; the pure table
            // already cleared the view field, the record follows (DEC-022 §3).
            run.decision.reset();
        }
        return std::nullopt;
    case WorkflowRunTransitionStatus::NoOpTerminal:
        return workflow_error(ErrorCode::InvalidState,
                              "workflow run already settled in the requested terminal state");
    case WorkflowRunTransitionStatus::Rejected:
    default:
        return applied.error;
    }
}

std::optional<Error> WorkflowRuntime::settle_terminal(RunRecord &run,
                                                      WorkflowRunState terminal_state,
                                                      const std::string &safe_summary) {
    auto failure = commit_transition(run, terminal_state);
    if (failure.has_value()) {
        return failure;
    }
    {
        std::lock_guard lock(mutex_);
        run.safe_summary = safe_summary;
        --active_runs_;
    }
    emit_run_settled(run, safe_summary);
    // Task-side settlement through the control plane. Idempotent outcomes and
    // already-terminal tasks are tolerated: the invariant to keep is that a
    // terminal run never has an active task.
    if (terminal_state == WorkflowRunState::Cancelled) {
        const auto command = runtime_.cancel_task(run.task);
        if (command.has_value()) {
            static_cast<void>(command.value().outcome(config_.command_timeout));
        }
    } else {
        TaskOutcome outcome;
        outcome.terminal_state = task_state_for_run_state(terminal_state);
        outcome.error = Error{};
        outcome.error->safe_message = safe_summary;
        const auto command = runtime_.complete_task(run.task, outcome);
        if (command.has_value()) {
            const auto settled = command.value().outcome(config_.command_timeout);
            if (settled.has_value() && settled.value().status == SettlementStatus::Failed) {
                const auto snapshot = runtime_.task_snapshot(run.task);
                if (!snapshot.has_value() || !is_terminal(snapshot.value().state)) {
                    ++task_settlement_failures_;
                }
            }
        }
    }
    // Stage F (DEC-030 §2): the episodic memory write trails the committed
    // facts — the terminal state, the settled event and the carrier task are
    // already durable, so a memory failure can never un-settle the run.
    record_episode(run, terminal_state);
    return std::nullopt;
}

void WorkflowRuntime::emit_run_started(const RunRecord &run) {
    if (!events_) {
        return;
    }
    WorkflowRunStartedEvent event;
    event.run_id = run.view.run_id;
    event.workflow_id = run.view.workflow_id;
    event.ir_digest = run.view.ir_digest;
    event.parameters_digest = run.bindings.digest;
    event.policy = run.view.policy;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_step_started(const RunRecord &run, const WorkflowStep &step,
                                        std::uint32_t attempt) {
    if (!events_) {
        return;
    }
    WorkflowStepStartedEvent event;
    event.run_id = run.view.run_id;
    event.step_id = step.id;
    event.kind = step.kind;
    event.attempt = attempt;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_step_settled(const RunRecord &run, const WorkflowStep &step,
                                        const WorkflowStepRecord &record) {
    if (!events_) {
        return;
    }
    WorkflowStepSettledEvent event;
    event.run_id = run.view.run_id;
    event.step_id = step.id;
    event.disposition = record.disposition;
    event.verification = record.verification;
    event.safe_summary = bounded_summary(record.safe_summary);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_run_settled(RunRecord &run, const std::string &safe_summary) {
    if (!events_) {
        return;
    }
    WorkflowRunSettledEvent event;
    event.run_id = run.view.run_id;
    event.terminal_state = run_state(run);
    event.run_epoch = run.view.run_epoch;
    event.safe_summary = bounded_summary(safe_summary);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (events_->append(append).has_value()) {
        // Provenance anchor for stage-F memory writes (DEC-030 §2): the
        // episode derives from this committed fact.
        run.settled_event = append.event_id;
    } else {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_patch_proposed(const RunRecord &run, const WorkflowPatchId &patch_id,
                                          const Sha256Digest &digest, WorkflowPatchTarget target,
                                          const std::string &reason_code) {
    if (!events_) {
        return;
    }
    WorkflowPatchProposedEvent event;
    event.patch_id = patch_id;
    event.run_id = run.view.run_id;
    event.patch_digest = digest;
    event.target = target;
    event.reason_code = bounded_summary(reason_code);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_patch_applied(const RunRecord &run, const WorkflowPatchId &patch_id) {
    if (!events_) {
        return;
    }
    WorkflowPatchAppliedEvent event;
    event.patch_id = patch_id;
    event.run_id = run.view.run_id;
    {
        std::lock_guard lock(mutex_);
        event.run_patch_epoch = run.view.run_patch_epoch;
    }
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_patch_rejected(const RunRecord &run, const WorkflowPatchId &patch_id,
                                          const std::string &reason_code) {
    if (!events_) {
        return;
    }
    WorkflowPatchRejectedEvent event;
    event.patch_id = patch_id;
    event.run_id = run.view.run_id;
    event.reason_code = bounded_summary(reason_code);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_policy_switched(const RunRecord &run, WorkflowPolicy from,
                                           WorkflowPolicy to) {
    if (!events_) {
        return;
    }
    WorkflowPolicySwitchedEvent event;
    event.run_id = run.view.run_id;
    event.from = from;
    event.to = to;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_decision_raised(const RunRecord &run,
                                           const WorkflowDecisionRequest &decision) {
    if (!events_) {
        return;
    }
    WorkflowDecisionRaisedEvent event;
    event.run_id = run.view.run_id;
    event.decision_id = decision.decision_id;
    event.payload_digest = decision.payload_digest;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_decision_resolved(const RunRecord &run,
                                             const WorkflowDecisionId &decision_id,
                                             WorkflowDecisionResolution resolution) {
    if (!events_) {
        return;
    }
    WorkflowDecisionResolvedEvent event;
    event.run_id = run.view.run_id;
    event.decision_id = decision_id;
    event.resolution = resolution;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_publish_proposed(const WorkflowId &workflow_id,
                                            const Sha256Digest &ir_digest,
                                            const std::optional<WorkflowRunId> &source_run_id) {
    if (!events_) {
        return;
    }
    WorkflowPublishProposedEvent event;
    event.workflow_id = workflow_id;
    event.ir_digest = ir_digest;
    event.source_run_id = source_run_id;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_publish_applied(const WorkflowId &workflow_id, const Sha256Digest &ir_digest,
                                           const Sha256Digest &evidence,
                                           const WorkflowRunId &dry_run_id) {
    if (!events_) {
        return;
    }
    WorkflowPublishAppliedEvent event;
    event.workflow_id = workflow_id;
    event.ir_digest = ir_digest;
    event.evidence = evidence;
    event.dry_run_id = dry_run_id;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_publish_rejected(const WorkflowId &workflow_id,
                                            const Sha256Digest &ir_digest,
                                            const std::string &reason_code) {
    if (!events_) {
        return;
    }
    WorkflowPublishRejectedEvent event;
    event.workflow_id = workflow_id;
    event.ir_digest = ir_digest;
    event.reason_code = reason_code;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

JsonValue WorkflowRuntime::predicate_context_copy(const RunRecord &run) const {
    std::lock_guard lock(mutex_);
    return run.predicate_context;
}

WorkflowRunResult WorkflowRuntime::drive(RunRecord &run, const OperationContext &context) {
    WorkflowRunResult result;
    result.run_id = run.view.run_id;

    const bool started_fresh = run_state(run) == WorkflowRunState::Created;
    if (started_fresh) {
        if (auto failure = commit_transition(run, WorkflowRunState::Running);
            failure.has_value()) {
            static_cast<void>(
                settle_terminal(run, WorkflowRunState::Failed, "run start transition was rejected"));
            assemble_result(run, result);
            return result;
        }
        emit_run_started(run);
        if (const auto snapshot = runtime_.task_snapshot(run.task); snapshot.has_value()) {
            run.task_epoch = snapshot.value().epoch;
            if (snapshot.value().state == TaskState::Idle) {
                // The carrier enters the active phase named by the frozen
                // mapping (DEC-020 §2: Running maps onto the active task
                // states); Idle -> Observing is the table's only entry edge.
                const auto command = runtime_.resume_task(run.task);
                if (command.has_value()) {
                    static_cast<void>(command.value().outcome(config_.command_timeout));
                }
            }
        }
        if (const auto snapshot = runtime_.task_snapshot(run.task); snapshot.has_value()) {
            run.task_epoch = snapshot.value().epoch;
        }
    }

    DriveFlags flags;
    std::future<void> monitor;
    try {
        monitor = executor_.submit_auto([this, &run, &flags] {
            while (!flags.done.load(std::memory_order_relaxed)) {
                const auto snapshot = runtime_.task_snapshot(run.task);
                if (!snapshot.has_value() || task_withdrawn(snapshot.value().state)) {
                    flags.withdrawn.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    } catch (const std::exception &) {
        // Without a monitor the drive still checks the control state between
        // steps; only intra-step cancellation responsiveness is lost.
        ++monitor_failures_;
    }

    // Checkpoint arrival bookkeeping (DEC-023 §2): an arrival is a cursor
    // entry onto the step - the fresh start of the run or a movement within
    // this drive. A wait-resume consumes the outstanding arrival instead of
    // counting a new one, so each arrival hands off at most once.
    std::size_t previous_index = std::numeric_limits<std::size_t>::max();
    bool first_iteration = true;
    while (true) {
        // A control-plane submission that moved the view off Running (a
        // raised decision point, an escalation) ends this drive; the frozen
        // table keeps any concurrent late settlement safe.
        if (run_state(run) != WorkflowRunState::Running) {
            break;
        }
        // Step-boundary patch application (DEC-024 §3): queued patches land
        // here, between settled steps; the in-flight step is unaffected.
        drain_pending_patches(run);
        // Control-state convergence: withdrawal commits the run view at the
        // step boundary (DEC-020 pause semantics; a bounded discrete step's
        // completion is the safe point in stage B).
        if (flags.withdrawn.load(std::memory_order_relaxed) || context.cancelled()) {
            const auto snapshot = runtime_.task_snapshot(run.task);
            const auto state =
                snapshot.has_value() ? snapshot.value().state : TaskState::Cancelled;
            if (!context.cancelled() && task_pause_family(state)) {
                static_cast<void>(commit_transition(run, WorkflowRunState::Paused));
            } else {
                static_cast<void>(settle_terminal(run, WorkflowRunState::Cancelled,
                                                  "run withdrawn by control request"));
            }
            break;
        }
        if (run.executions >= config_.max_step_executions_per_run) {
            static_cast<void>(settle_terminal(
                run, WorkflowRunState::Failed,
                make_runtime_error(WorkflowRuntimeError::StepBudgetExceeded,
                                   "run step budget exhausted")
                    .safe_message));
            break;
        }
        if (run.cursor >= run.definition.steps.size()) {
            static_cast<void>(settle_terminal(
                run, WorkflowRunState::Completed,
                "workflow completed: " + std::to_string(run.settled.size()) + " steps settled"));
            break;
        }

        const std::size_t index = run.cursor;
        const WorkflowStep &step = run.definition.steps[index];

        // Patch-plane skip (DEC-024 §3): skipped steps settle without
        // dispatch and the cursor advances.
        if (contains_string(run.skipped_steps, step.id.to_string())) {
            WorkflowStepRecord record;
            record.step_id = step.id;
            record.attempt = run.attempts[index];
            record.disposition = WorkflowStepDisposition::Skipped;
            record.verification = "none";
            record.safe_summary = "skipped by run patch";
            settle_step(run, step, record);
            ++run.cursor;
            continue;
        }

        if (step.precondition.has_value()) {
            const auto verdict =
                evaluate_workflow_predicate(*step.precondition, evaluation_context(run));
            if (verdict == WorkflowPredicateResult::NotEvaluable) {
                if (!handle_step_failure(
                        run, step, index,
                        make_runtime_error(WorkflowRuntimeError::VerifyNotEvaluable,
                                           "precondition is not evaluable"))) {
                    break;
                }
                continue;
            }
            if (verdict == WorkflowPredicateResult::NotSatisfied) {
                WorkflowStepRecord record;
                record.step_id = step.id;
                record.attempt = run.attempts[index];
                record.disposition = WorkflowStepDisposition::Skipped;
                record.verification = "none";
                record.safe_summary = "precondition not satisfied";
                settle_step(run, step, record);
                ++run.cursor;
                continue;
            }
        }

        // Checkpoint handoff (DEC-023 §2): under AgentAssisted a step
        // declaring an AgentEscalation hook is a checkpoint; each arrival
        // surrenders to the agent at most once before executing.
        const bool entered = (first_iteration && started_fresh) ||
                             (!first_iteration && index != previous_index);
        if (entered) {
            ++run.checkpoint_arrivals[index];
        }
        previous_index = index;
        first_iteration = false;
        if (entered && run.view.policy == WorkflowPolicy::AgentAssisted &&
            step.recovery.has_value() &&
            step.recovery->mode == WorkflowRecoveryHook::Mode::AgentEscalation &&
            run.checkpoint_arrivals[index] > run.checkpoint_handoffs[index]) {
            ++run.checkpoint_handoffs[index];
            if (auto failure = enter_waiting_agent(
                    run, "checkpoint reached; agent participation requested");
                failure.has_value()) {
                static_cast<void>(
                    settle_terminal(run, WorkflowRunState::Failed, failure.value().safe_message));
            }
            break;
        }

        // Bounded retries across wait cycles (DEC-023 §1): counters never
        // reset; the wait-resume cycle itself is bounded by the escalation
        // budget checked in escalate_waiting_*.

        const bool dispatches = workflow_policy_dispatches_side_effects(run.view.policy);
        const std::uint32_t attempt = run.attempts[index] + 1;
        run.attempts[index] = attempt;
        ++run.executions;
        emit_step_started(run, step, attempt);

        if (step.kind == WorkflowStepKind::Control) {
            if (run.jumps[index] + 1 > step.max_iterations) {
                if (!handle_step_failure(
                        run, step, index,
                        make_runtime_error(WorkflowRuntimeError::LoopBudgetExceeded,
                                           "loop iteration budget exceeded"))) {
                    break;
                }
                continue;
            }
            ++run.jumps[index];
            WorkflowStepRecord record;
            record.step_id = step.id;
            record.attempt = attempt;
            record.disposition = WorkflowStepDisposition::Completed;
            record.verification = "none";
            record.safe_summary = "loop jump taken";
            settle_step(run, step, record);
            run.cursor = step_index(run, *step.jump_to);
            continue;
        }

        WorkflowStepRecord record;
        record.step_id = step.id;
        record.attempt = attempt;
        record.disposition = WorkflowStepDisposition::Completed;
        record.verification = "none";

        if (step.kind == WorkflowStepKind::Navigate) {
            // Stage E (DEC-028 §3): resolve through the navigation context.
            // DryRun plans for real when a context exists and keeps the M9
            // shape-planning settlement otherwise.
            bool nav_cancelled = false;
            auto navigated = execute_navigate_step(run, step, index, context, flags, record,
                                                   nav_cancelled);
            if (nav_cancelled) {
                record.disposition = WorkflowStepDisposition::Stale;
                record.safe_summary = "step cut short at a control boundary";
                settle_step(run, step, record);
                run.interrupted = true;
                continue;
            }
            if (navigated.has_value()) {
                if (!handle_step_failure(run, step, index, navigated.value())) {
                    break;
                }
                continue;
            }
            settle_step(run, step, record);
            ++run.cursor;
            continue;
        }

        if (!dispatches) {
            if (step.verification.has_value()) {
                const auto verdict = evaluate_workflow_predicate(*step.verification,
                                                                 evaluation_context(run));
                record.verification = verification_name(verdict);
                if (verdict == WorkflowPredicateResult::NotSatisfied) {
                    if (!handle_step_failure(
                                run, step, index,
                                make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                                   "dry-run verification failed"))) {
                        break;
                    }
                    continue;
                }
                if (verdict == WorkflowPredicateResult::NotEvaluable) {
                    ++run.unevaluable;
                    record.safe_summary = "planned; verification not evaluable";
                }
            }
            if (step.kind == WorkflowStepKind::ToolCall) {
                record.safe_summary = "planned tool call";
            }
            settle_step(run, step, record);
            ++run.cursor;
            continue;
        }

        if (step.kind == WorkflowStepKind::Verify) {
            const auto verdict =
                evaluate_workflow_predicate(*step.verification, evaluation_context(run));
            record.verification = verification_name(verdict);
            if (verdict == WorkflowPredicateResult::Satisfied) {
                settle_step(run, step, record);
                ++run.cursor;
                continue;
            }
            const auto failure =
                verdict == WorkflowPredicateResult::NotSatisfied
                    ? make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                         "verification predicate failed")
                    : make_runtime_error(WorkflowRuntimeError::VerifyNotEvaluable,
                                         "verification predicate is not evaluable");
            if (!handle_step_failure(run, step, index, failure)) {
                break;
            }
            continue;
        }

        // Strict ToolCall: dispatch through the BuiltIn boundary.
        bool cancelled = false;
        auto dispatch = dispatch_step(run, step, index, context, flags, record, cancelled);
        if (cancelled) {
            record.disposition = WorkflowStepDisposition::Stale;
            record.safe_summary = "step cut short at a control boundary";
            settle_step(run, step, record);
            run.interrupted = true;
            continue;
        }
        if (!dispatch.has_value()) {
            if (!handle_step_failure(run, step, index, dispatch.error())) {
                break;
            }
            continue;
        }
        settle_step(run, step, record);
        ++run.cursor;
    }

    flags.done.store(true, std::memory_order_relaxed);
    flags.withdrawn.store(true, std::memory_order_relaxed);
    if (monitor.valid()) {
        try {
            static_cast<void>(monitor.get());
        } catch (...) {
        }
    }
    assemble_result(run, result);
    return result;
}

void WorkflowRuntime::assemble_result(const RunRecord &run, WorkflowRunResult &result) const {
    std::lock_guard lock(mutex_);
    result.state = run.view.state;
    result.run_epoch = run.view.run_epoch;
    result.steps = run.settled;
    result.unevaluable_verifications = run.unevaluable;
    result.safe_summary = run.safe_summary;
}

void WorkflowRuntime::settle_step(RunRecord &run, const WorkflowStep &step,
                                  const WorkflowStepRecord &record) {
    {
        std::lock_guard lock(mutex_);
        run.settled.push_back(record);
    }
    emit_step_settled(run, step, record);
}

std::size_t WorkflowRuntime::step_index(const RunRecord &run, const StepId &step_id) const {
    const auto found =
        std::find_if(run.definition.steps.begin(), run.definition.steps.end(),
                     [&](const WorkflowStep &candidate) { return candidate.id == step_id; });
    return static_cast<std::size_t>(std::distance(run.definition.steps.begin(), found));
}

bool WorkflowRuntime::handle_step_failure(RunRecord &run, const WorkflowStep &step,
                                          std::size_t index, const Error &failure) {
    WorkflowStepRecord record;
    record.step_id = step.id;
    record.attempt = run.attempts[index];
    record.disposition = WorkflowStepDisposition::Failed;
    record.verification = "none";
    record.safe_summary = failure.safe_message;
    settle_step(run, step, record);
    {
        std::lock_guard lock(mutex_);
        run.failure_reason = bounded_summary(failure.safe_message);
        // Stage F (DEC-029 §4): the sanitized failure signature feeding
        // retrieval queries and episode/lesson records. domain + domain_code
        // is a stable identifier; free text never reaches the memory or
        // retrieval surface.
        WorkflowFailureSignature signature;
        signature.workflow_id = run.view.workflow_id.to_string();
        signature.step_id = step.id.to_string();
        signature.step_kind = workflow_step_kind_name(step.kind);
        signature.reason_code =
            failure.domain + ":" + std::to_string(failure.domain_code);
        run.last_failure_signature = std::move(signature);
    }

    const auto hook = step.recovery;
    if (hook.has_value() && hook->mode == WorkflowRecoveryHook::Mode::Retry) {
        const bool attempts_left = run.attempts[index] < step.max_attempts;
        const bool retries_left = run.retries[index] < hook->max_retries;
        if (attempts_left && retries_left) {
            ++run.retries[index];
            // A side effect may have happened before the failure: re-observe
            // before any redispatch (RULE-05; blind retries forbidden).
            if (step.kind == WorkflowStepKind::ToolCall &&
                step_tool_has_side_effects(run, index)) {
                OperationContext context;
                context.session = session_;
                context.task = run.task;
                context.started_at = Timestamp::now();
                if (!observe_for_verification(run, step, context).has_value()) {
                    static_cast<void>(settle_terminal(run, WorkflowRunState::Failed,
                                                      "recovery observation failed"));
                    return false;
                }
            }
            return true; // retry the same step; the cursor stays put
        }
    }
    if (hook.has_value() && hook->mode == WorkflowRecoveryHook::Mode::FallbackStep) {
        run.cursor = step_index(run, *hook->fallback_step);
        return true;
    }
    if (hook.has_value() && hook->mode == WorkflowRecoveryHook::Mode::AgentEscalation &&
        workflow_policy_allows_agent(run.view.policy)) {
        // Declared escalation point: hand the repair to the agent directly
        // (DEC-023 §1; Strict/DryRun runs never reach here).
        const auto escalated = escalate_waiting_agent(run, failure.safe_message);
        if (!escalated.has_value()) {
            return false;
        }
        static_cast<void>(
            settle_terminal(run, WorkflowRunState::Failed, escalated.value().safe_message));
        return false;
    }
    // Local recovery exhausted: policy decides the outcome (DEC-023 §1).
    if (run.view.policy == WorkflowPolicy::Interactive) {
        WorkflowDecisionRequest decision;
        decision.decision_id = WorkflowDecisionId::generate();
        decision.kind = WorkflowDecisionKind::StepFailure;
        decision.step_id = step.id;
        decision.safe_summary = bounded_summary(failure.safe_message);
        WorkflowPatchEntry skip;
        skip.target = WorkflowPatchTarget::StepArguments;
        skip.op = WorkflowPatchOp::Skip;
        skip.path = step.id.to_string();
        decision.proposal = {std::move(skip)};
        decision.payload_digest =
            decision_payload_digest(decision.safe_summary, decision.proposal);
        const auto escalated = escalate_waiting_user(run, std::move(decision));
        if (!escalated.has_value()) {
            return false;
        }
        static_cast<void>(
            settle_terminal(run, WorkflowRunState::Failed, escalated.value().safe_message));
        return false;
    }
    if (workflow_policy_allows_agent(run.view.policy)) {
        const auto escalated = escalate_waiting_agent(run, failure.safe_message);
        if (!escalated.has_value()) {
            return false;
        }
        static_cast<void>(
            settle_terminal(run, WorkflowRunState::Failed, escalated.value().safe_message));
        return false;
    }
    static_cast<void>(settle_terminal(run, WorkflowRunState::Failed, failure.safe_message));
    return false;
}

std::optional<Error> WorkflowRuntime::enter_waiting_agent(RunRecord &run,
                                                           const std::string &reason) {
    if (auto failure = commit_transition(run, WorkflowRunState::WaitingAgent);
        failure.has_value()) {
        return failure;
    }
    const auto command = runtime_.begin_task_recovery(run.task);
    if (command.has_value()) {
        static_cast<void>(command.value().outcome(config_.command_timeout));
    }
    {
        std::lock_guard lock(mutex_);
        run.safe_summary = bounded_summary("waiting for agent: " + reason);
    }
    return std::nullopt;
}

std::optional<Error> WorkflowRuntime::escalate_waiting_agent(RunRecord &run,
                                                             const std::string &reason) {
    {
        std::lock_guard lock(mutex_);
        if (run.escalations >= config_.max_escalations_per_run) {
            return make_runtime_error(WorkflowRuntimeError::EscalationBudgetExceeded,
                                      "run escalation budget exhausted");
        }
        ++run.escalations;
        // Lesson derivation anchor (DEC-030 §4): recovery actions are the
        // patches applied after this escalation.
        run.patches_at_last_escalation = run.applied_patches.size();
    }
    auto failure = enter_waiting_agent(run, reason);
    if (failure.has_value()) {
        return failure;
    }
    // Failure-driven escalation only (DEC-030 §3): attach the retrieved
    // episodic material before the agent asks for its continuation.
    retrieve_lessons(run);
    return std::nullopt;
}

std::optional<Error> WorkflowRuntime::escalate_waiting_user(RunRecord &run,
                                                             WorkflowDecisionRequest decision) {
    {
        std::lock_guard lock(mutex_);
        if (run.escalations >= config_.max_escalations_per_run) {
            return make_runtime_error(WorkflowRuntimeError::EscalationBudgetExceeded,
                                      "run escalation budget exhausted");
        }
        ++run.escalations;
        // Stage before the transition: entering WaitingUser keeps the
        // decision the caller staged (DEC-022 §3, apply_workflow_run_transition).
        run.view.pending_decision =
            WorkflowPendingDecision{decision.decision_id, decision.payload_digest};
        run.decision = decision;
    }
    if (auto failure = commit_transition(run, WorkflowRunState::WaitingUser);
        failure.has_value()) {
        std::lock_guard lock(mutex_);
        run.view.pending_decision.reset();
        run.decision.reset();
        return failure;
    }
    emit_decision_raised(run, decision);
    const auto command = runtime_.pause_task(run.task);
    if (command.has_value()) {
        static_cast<void>(command.value().outcome(config_.command_timeout));
    }
    {
        std::lock_guard lock(mutex_);
        run.safe_summary = bounded_summary("waiting for user decision: " +
                                           decision.safe_summary);
    }
    return std::nullopt;
}

bool WorkflowRuntime::step_tool_has_side_effects(const RunRecord &run,
                                                 std::size_t index) const {
    if (!tools_) {
        return false;
    }
    const auto &arguments = run.resolved_arguments[index];
    const auto *tool_member = arguments.find("tool");
    if (tool_member == nullptr || !tool_member->is_string()) {
        return false;
    }
    const auto exposed = tools_->exposed_tools();
    const auto tool = std::find_if(
        exposed.begin(), exposed.end(),
        [&](const ExposedToolSpec &entry) { return entry.wire_name == *tool_member->as_string(); });
    return tool != exposed.end() && tool->has_side_effects;
}

Result<ToolExecutionRecord>
WorkflowRuntime::dispatch_tool_invocation(RunRecord &run, const ExposedToolSpec &tool,
                                          const JsonValue &input, const std::string &call_id,
                                          const OperationContext &parent, const DriveFlags &flags,
                                          bool &cancelled) {
    ToolProposal proposal;
    proposal.provider_call_id = ProviderToolCallId{call_id};
    proposal.tool_id = tool.tool_id;
    proposal.wire_name = tool.wire_name;
    proposal.tool_version = tool.version;
    proposal.arguments = input;
    proposal.arguments_digest = canonical_json_digest(input);
    proposal.operation_id = OperationId::generate();
    proposal.has_side_effects = tool.has_side_effects;

    OperationContext context = parent;
    context.task_epoch = run.task_epoch;
    context.started_at = Timestamp::now();
    context.deadline = std::chrono::steady_clock::now() + config_.step_deadline;
    context.cancellation_requested = [&flags] {
        return flags.withdrawn.load(std::memory_order_relaxed);
    };

    auto tools = tools_;
    std::future<Result<ToolExecutionRecord>> future;
    try {
        future = executor_.submit_auto(
            [tools, proposal, context] { return tools->execute(proposal, context); });
    } catch (const std::exception &exception) {
        return workflow_error(ErrorCode::ResourceExhausted, exception.what(), true);
    }
    Result<ToolExecutionRecord> outcome{Error{}};
    try {
        outcome = future.get();
    } catch (const std::exception &exception) {
        return workflow_error(ErrorCode::Internal, exception.what());
    }
    if (!outcome.has_value()) {
        if (outcome.error().code == ErrorCode::Cancelled) {
            cancelled = true;
            return Result<ToolExecutionRecord>{Error{}};
        }
        return outcome.error();
    }
    if (outcome.value().failed) {
        return make_runtime_error(WorkflowRuntimeError::ToolDispatchFailed,
                                  outcome.value().safe_error_summary.empty()
                                      ? "tool execution failed"
                                      : outcome.value().safe_error_summary);
    }
    return outcome;
}

Result<void> WorkflowRuntime::dispatch_step(RunRecord &run, const WorkflowStep &step,
                                            std::size_t index, const OperationContext &parent,
                                            const DriveFlags &flags, WorkflowStepRecord &record,
                                            bool &cancelled) {
    const JsonValue &resolved = run.resolved_arguments[index];
    JsonValue input(JsonValue::Object{});
    if (const auto *object = resolved.as_object()) {
        for (const auto &member : *object) {
            if (member.first != "tool") {
                input.set(member.first, member.second);
            }
        }
    }
    const std::string wire_name = *resolved.find("tool")->as_string();

    const auto exposed = tools_->exposed_tools();
    const auto tool = std::find_if(
        exposed.begin(), exposed.end(),
        [&](const ExposedToolSpec &entry) { return entry.wire_name == wire_name; });
    if (tool == exposed.end()) {
        return workflow_error(ErrorCode::NotFound, "step tool is not registered");
    }

    const std::string call_id = "workflow:" + run.view.run_id.to_string() + ":" +
                                step.id.to_string() + ":" +
                                std::to_string(run.attempts[index]);
    OperationContext context = parent;
    context.step = step.id;
    auto execution =
        dispatch_tool_invocation(run, *tool, input, call_id, context, flags, cancelled);
    if (cancelled) {
        return Result<void>{};
    }
    if (!execution.has_value()) {
        return execution.error();
    }
    {
        std::lock_guard lock(mutex_);
        run.predicate_context.set("step_result:" + step.id.to_string(),
                                  execution.value().result);
    }

    record.safe_summary = "tool: " + wire_name;
    if (!step.verification.has_value()) {
        record.verification = "none";
        return Result<void>{};
    }
    if (tool->has_side_effects) {
        // W-02: verify against a fresh observation after every side effect.
        if (!observe_for_verification(run, step, context).has_value()) {
            return make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                      "verification observation failed");
        }
    }
    const auto verdict =
        evaluate_workflow_predicate(*step.verification, evaluation_context(run));
    record.verification = verification_name(verdict);
    switch (verdict) {
    case WorkflowPredicateResult::Satisfied:
        return Result<void>{};
    case WorkflowPredicateResult::NotSatisfied:
        return make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                  "verification predicate failed");
    case WorkflowPredicateResult::NotEvaluable:
    default:
        return make_runtime_error(WorkflowRuntimeError::VerifyNotEvaluable,
                                  "verification predicate is not evaluable");
    }
}

JsonValue WorkflowRuntime::evaluation_context(const RunRecord &run) const {
    JsonValue context;
    ScreenStateProvider provider;
    {
        std::lock_guard lock(mutex_);
        context = run.predicate_context;
        provider = screen_state_provider_;
    }
    // The provider is a host callback: never invoked under the runtime mutex
    // (DEC-027 §3); a missing or empty reading injects nothing, so every
    // screen_state predicate stays NotEvaluable (fail closed).
    if (provider) {
        const auto snapshot = provider();
        if (snapshot.has_value() && !snapshot.value().state_id.empty()) {
            context.set("screen_state:" + snapshot.value().state_id, true);
            context.set("screen_state:current", snapshot.value().state_id);
        }
    }
    return context;
}

void WorkflowRuntime::emit_navigation_planned(const RunRecord &run, const WorkflowStep &step,
                                              const NavigationPlan &plan) {
    if (!events_) {
        return;
    }
    WorkflowNavigationPlannedEvent event;
    event.run_id = run.view.run_id;
    event.step_id = step.id;
    event.from_state = plan.from_state;
    event.to_state = plan.to_state;
    event.edge_count = plan.transition_ids.size();
    event.plan_digest = plan.plan_digest;
    event.total_cost = plan.total_cost;
    event.guards_blocked = plan.guards_blocked;
    event.guards_unevaluable = plan.guards_unevaluable;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::emit_navigation_observed(const RunRecord &run, const WorkflowStep &step,
                                               const AppModelTransition &transition,
                                               bool success) {
    if (!events_) {
        return;
    }
    WorkflowNavigationObservedEvent event;
    event.run_id = run.view.run_id;
    event.step_id = step.id;
    event.transition_id = transition.id;
    event.from_state = transition.from_state;
    event.to_state = transition.to_state;
    event.success = success;
    event.confidence = transition.confidence.confidence;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = run.task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRuntime::note_navigation_outcome(RunRecord &run, const WorkflowStep &step,
                                              const AppModelTransition &transition,
                                              bool success, std::uint64_t now_ms) {
    std::optional<AppModelTransition> updated;
    {
        std::lock_guard lock(mutex_);
        if (!app_model_.has_value()) {
            return;
        }
        const auto found = std::find_if(
            app_model_->transitions.begin(), app_model_->transitions.end(),
            [&](const AppModelTransition &candidate) { return candidate.id == transition.id; });
        if (found == app_model_->transitions.end()) {
            return;
        }
        found->confidence = note_transition_outcome(found->confidence, success, now_ms);
        updated = *found;
    }
    // Audit outside the runtime mutex (the event store is host-supplied).
    emit_navigation_observed(run, step, *updated, success);
}

std::optional<Error> WorkflowRuntime::execute_navigate_step(RunRecord &run,
                                                            const WorkflowStep &step,
                                                            std::size_t index,
                                                            const OperationContext &parent,
                                                            const DriveFlags &flags,
                                                            WorkflowStepRecord &record,
                                                            bool &cancelled) {
    JsonValue arguments;
    ScreenStateProvider provider;
    std::optional<AppModel> model;
    {
        std::lock_guard lock(mutex_);
        arguments = run.resolved_arguments[index];
        provider = screen_state_provider_;
        model = app_model_;
    }
    const auto *target_member =
        arguments.is_object() ? arguments.find("target") : nullptr;
    if (target_member == nullptr || !target_member->is_string() ||
        target_member->as_string()->empty()) {
        return make_runtime_error(WorkflowRuntimeError::NavigateTargetInvalid,
                                  "navigate arguments must name a string \"target\" member");
    }
    const std::string target = *target_member->as_string();

    const bool dispatches = workflow_policy_dispatches_side_effects(run.view.policy);
    const bool context_ready = model.has_value() && provider != nullptr;
    if (!context_ready) {
        if (dispatches) {
            // The context was required at admission; losing it since (host
            // reinstallation raced) still fails closed.
            return make_runtime_error(WorkflowRuntimeError::NavigateUnresolvable,
                                      "navigation context is no longer installed");
        }
        // M9 shape-planning semantics for context-less DryRun runs.
        record.safe_summary = "planned navigation";
        return std::nullopt;
    }

    // 1. Screen read (DEC-028 §3): no current reading -> fail closed.
    const auto snapshot = provider();
    if (!snapshot.has_value() || snapshot.value().state_id.empty()) {
        return make_runtime_error(WorkflowRuntimeError::NavigateNoScreenState,
                                  "no current screen state reading");
    }
    // 2. The target must be a declared state of the installed model.
    const bool declared = std::any_of(
        model->states.begin(), model->states.end(),
        [&](const AppModelState &state) { return state.id == target; });
    if (!declared) {
        return make_runtime_error(WorkflowRuntimeError::NavigateTargetUnknown,
                                  "navigate target '" + target +
                                      "' is not declared in the app model");
    }
    // 3. Plan with the configured weights and budgets; agent edges stay
    //    unusable for workflow navigation (DEC-028 §1).
    NavigationPlanOptions options;
    options.allow_agent_edges = false;
    options.max_edge_evaluations = config_.nav_max_edge_evaluations;
    options.max_path_edges = config_.nav_max_path_edges;
    auto plan = plan_navigation(model.value(), snapshot.value().state_id, target,
                                config_.nav_cost_profile, evaluation_context(run), options);
    if (!plan.has_value()) {
        return make_runtime_error(WorkflowRuntimeError::NavigatePlanFailed,
                                  plan.error().safe_message);
    }
    emit_navigation_planned(run, step, plan.value());
    record.safe_summary = "navigation planned: " +
                          std::to_string(plan.value().transition_ids.size()) +
                          " edge(s) to '" + target + "'";

    if (!dispatches) {
        // DryRun: planning only. Verification predicates evaluate against the
        // merged context; unevaluable ones are disclosed, never claimed
        // (RULE-10). No dispatch, no write-back, no Observed events.
        if (step.verification.has_value()) {
            const auto verdict =
                evaluate_workflow_predicate(*step.verification, evaluation_context(run));
            record.verification = verification_name(verdict);
            if (verdict == WorkflowPredicateResult::NotSatisfied) {
                return make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                          "dry-run verification failed");
            }
            if (verdict == WorkflowPredicateResult::NotEvaluable) {
                ++run.unevaluable;
                record.safe_summary = "planned; verification not evaluable";
            }
        }
        return std::nullopt;
    }

    // 4. Per-edge execution: dispatch the action through the tool registry
    //    channel, then one arrival read (W-02). No blind redispatch
    //    (RULE-05); recovery hooks apply at the step level.
    if (!tools_) {
        return workflow_error(ErrorCode::InvalidState,
                              "a tool registry is required to dispatch navigation edges");
    }
    const auto exposed = tools_->exposed_tools();
    const std::string attempt_tag = std::to_string(run.attempts[index]);
    JsonValue last_edge_result;
    for (const auto &edge_id : plan.value().transition_ids) {
        if (flags.withdrawn.load(std::memory_order_relaxed) || parent.cancelled()) {
            cancelled = true;
            return std::nullopt;
        }
        if (run.executions >= config_.max_step_executions_per_run) {
            return make_runtime_error(WorkflowRuntimeError::StepBudgetExceeded,
                                      "run step budget exhausted");
        }
        // Resolve the edge against the live projection: a reinstall between
        // planning and execution must not dispatch a stale action.
        std::optional<AppModelTransition> edge;
        {
            std::lock_guard lock(mutex_);
            if (app_model_.has_value()) {
                const auto found = std::find_if(
                    app_model_->transitions.begin(), app_model_->transitions.end(),
                    [&](const AppModelTransition &candidate) { return candidate.id == edge_id; });
                if (found != app_model_->transitions.end()) {
                    edge = *found;
                }
            }
        }
        if (!edge.has_value()) {
            std::string detail = "planned edge '";
            detail += edge_id;
            detail += "' is no longer present in the app model";
            return make_runtime_error(WorkflowRuntimeError::NavigatePlanFailed, detail);
        }
        const auto *tool_member = edge->action.find("tool");
        if (tool_member == nullptr || !tool_member->is_string()) {
            std::string detail = "edge '";
            detail += edge_id;
            detail += "' action misses the \"tool\" member";
            return make_runtime_error(WorkflowRuntimeError::NavigateTargetInvalid, detail);
        }
        const std::string wire_name = *tool_member->as_string();
        const auto tool = std::find_if(
            exposed.begin(), exposed.end(),
            [&](const ExposedToolSpec &entry) { return entry.wire_name == wire_name; });
        if (tool == exposed.end()) {
            std::string detail = "navigation edge tool '";
            detail += wire_name;
            detail += "' is not registered";
            return workflow_error(ErrorCode::NotFound, detail);
        }
        JsonValue input(JsonValue::Object{});
        if (const auto *object = edge->action.as_object()) {
            for (const auto &member : *object) {
                if (member.first != "tool") {
                    input.set(member.first, member.second);
                }
            }
        }
        ++run.executions;
        // += chaining: this runs once per planned edge inside a loop, where a
        // + chain would allocate one temporary per operand.
        std::string call_id = "workflow-nav:";
        call_id += run.view.run_id.to_string();
        call_id += ':';
        call_id += step.id.to_string();
        call_id += ':';
        call_id += attempt_tag;
        call_id += ':';
        call_id += edge_id;
        OperationContext context = parent;
        context.step = step.id;
        auto execution =
            dispatch_tool_invocation(run, *tool, input, call_id, context, flags, cancelled);
        if (cancelled) {
            return std::nullopt;
        }
        if (!execution.has_value()) {
            note_navigation_outcome(run, step, *edge, false, now_millis());
            return execution.error();
        }
        last_edge_result = execution.value().result;
        // Arrival verification: one fresh read; the observed state must be
        // the edge target (W-02; navigate-arrival-unverified otherwise).
        const auto arrival = provider();
        const bool arrived = arrival.has_value() &&
                             arrival.value().state_id == edge->to_state;
        note_navigation_outcome(run, step, *edge, arrived, now_millis());
        if (!arrived) {
            std::string detail = "edge '";
            detail += edge_id;
            detail += "' did not arrive at '";
            detail += edge->to_state;
            detail += "'";
            return make_runtime_error(WorkflowRuntimeError::NavigateArrivalUnverified, detail);
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (!last_edge_result.is_null()) {
            run.predicate_context.set("step_result:" + step.id.to_string(), last_edge_result);
        }
    }

    record.safe_summary = "navigated to '" + target + "' (" +
                          std::to_string(plan.value().transition_ids.size()) + " edges)";
    if (!step.verification.has_value()) {
        record.verification = "none";
        return std::nullopt;
    }
    const auto verdict =
        evaluate_workflow_predicate(*step.verification, evaluation_context(run));
    record.verification = verification_name(verdict);
    switch (verdict) {
    case WorkflowPredicateResult::Satisfied:
        return std::nullopt;
    case WorkflowPredicateResult::NotSatisfied:
        return make_runtime_error(WorkflowRuntimeError::VerifyFailed,
                                  "verification predicate failed");
    case WorkflowPredicateResult::NotEvaluable:
    default:
        return make_runtime_error(WorkflowRuntimeError::VerifyNotEvaluable,
                                  "verification predicate is not evaluable");
    }
}

Result<Observation> WorkflowRuntime::observe_for_verification(const RunRecord & /*run*/,
                                                               const WorkflowStep &step,
                                                               const OperationContext &parent) {
    OperationContext context = parent;
    context.step = step.id;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = std::chrono::steady_clock::now() + config_.step_deadline;
    context.cancellation_requested = nullptr;
    auto environment = environment_;
    auto future = executor_.submit_auto([environment, context] {
        ObservationRequest request;
        request.max_age = std::chrono::milliseconds(0);
        return environment->observe(request, context);
    });
    try {
        return future.get();
    } catch (const std::exception &exception) {
        return workflow_error(ErrorCode::Internal, exception.what());
    }
}

Result<WorkflowRunView> WorkflowRuntime::pause_run(const WorkflowRunId &run_id) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (run.view.state == WorkflowRunState::Paused) {
            return run.view; // Idempotent pause.
        }
        if (run.view.state != WorkflowRunState::Running) {
            return workflow_error(ErrorCode::InvalidState,
                                  "only a running workflow run can be paused");
        }
    }
    const auto command = runtime_.pause_task(run.task);
    if (!command.has_value()) {
        return command.error();
    }
    const auto outcome = command.value().outcome(config_.command_timeout);
    if (!outcome.has_value()) {
        return outcome.error();
    }
    if (outcome.value().status == SettlementStatus::Failed) {
        return outcome.value().error.value_or(
            workflow_error(ErrorCode::InvalidState, "task pause was rejected"));
    }
    bool driven = true;
    const auto deadline = std::chrono::steady_clock::now() + config_.command_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool settled = false;
        {
            std::lock_guard lock(mutex_);
            settled = run.view.state == WorkflowRunState::Paused || is_terminal(run.view.state);
            driven = run.drive_active;
        }
        if (settled) {
            std::lock_guard lock(mutex_);
            return run.view;
        }
        if (!driven) {
            // No drive can commit the pause itself; the mutex is a leaf, so
            // the settlement runs outside the lock scope.
            static_cast<void>(commit_transition(run, WorkflowRunState::Paused));
            std::lock_guard lock(mutex_);
            return run.view;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return workflow_error(ErrorCode::DeadlineExceeded, "run pause did not converge in time", true);
}

Result<WorkflowRunView> WorkflowRuntime::resume_run(const WorkflowRunId &run_id) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (run.view.state == WorkflowRunState::WaitingUser) {
            // The decision point is the only exit from WaitingUser
            // (DEC-024 §5); resuming past an unanswered question is fail
            // closed.
            return workflow_error(ErrorCode::InvalidState,
                                  "a waiting-user run is resolved through resolve_decision, "
                                  "not resume");
        }
        if (run.view.state != WorkflowRunState::Paused &&
            run.view.state != WorkflowRunState::WaitingAgent) {
            return workflow_error(ErrorCode::InvalidState,
                                  "only a paused or waiting-agent workflow run can be resumed");
        }
    }
    return continue_run(run);
}

// Shared continuation behind resume_run and resolve_decision: resumes the
// carrier task, re-observes (DEC-020 resume contract), commits Running and
// launches the asynchronous drive.
Result<WorkflowRunView> WorkflowRuntime::continue_run(RunRecord &run) {
    // A decision point or escalation parks the view while the prior drive
    // converges (its in-flight step settles through the withdrawal path);
    // the continuation waits bounded for that convergence before launching.
    bool converged = false;
    const auto deadline = std::chrono::steady_clock::now() + config_.command_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mutex_);
            converged = !run.drive_active;
        }
        if (converged) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!converged) {
        return workflow_error(ErrorCode::DeadlineExceeded,
                              "prior drive did not converge before continuation", true);
    }
    {
        std::lock_guard lock(mutex_);
        if (async_drives_ >= config_.max_concurrent_async_drives) {
            return workflow_error(ErrorCode::ResourceExhausted,
                                  "asynchronous drive capacity is exhausted", true);
        }
    }

    const auto command = runtime_.resume_task(run.task);
    if (command.has_value()) {
        const auto outcome = command.value().outcome(config_.command_timeout);
        if (!outcome.has_value() || outcome.value().status == SettlementStatus::Failed) {
            // A carrier already back in an active state (the task left
            // SuspendedForTakeover when a takeover was released before this
            // resume) needs no task transition; only withdrawn tasks reject.
            const auto snapshot = runtime_.task_snapshot(run.task);
            if (!snapshot.has_value() || task_withdrawn(snapshot.value().state)) {
                return outcome.has_value()
                           ? outcome.value().error.value_or(workflow_error(
                                 ErrorCode::InvalidState, "task resume was rejected"))
                           : outcome.error();
            }
        }
    } else {
        return command.error();
    }
    if (const auto snapshot = runtime_.task_snapshot(run.task); snapshot.has_value()) {
        run.task_epoch = snapshot.value().epoch;
    }

    // Resume contract (DEC-020): re-observe before any step continues.
    OperationContext context;
    context.session = session_;
    context.task = run.task;
    context.started_at = Timestamp::now();
    WorkflowStep anchor;
    if (!run.definition.steps.empty()) {
        anchor = run.definition.steps[std::min(run.cursor, run.definition.steps.size() - 1)];
    }
    if (!observe_for_verification(run, anchor, context).has_value()) {
        static_cast<void>(settle_terminal(run, WorkflowRunState::Failed,
                                          "resume re-observation failed"));
        std::lock_guard lock(mutex_);
        return run.view;
    }

    // Running comes first: the frozen table has no Paused -> Failed edge, so
    // a resume that cannot recover its interrupted step settles Failed from
    // Running (the re-observation above already ran under its safety rules).
    if (auto failure = commit_transition(run, WorkflowRunState::Running);
        failure.has_value()) {
        return failure.value();
    }
    if (run.interrupted) {
        if (auto resolution = resolve_interrupted_step(run); !resolution.has_value()) {
            return resolution.error();
        }
    }
    {
        std::lock_guard lock(mutex_);
        ++async_drives_;
        run.drive_active = true;
    }
    const WorkflowRunId run_id = run.view.run_id;
    try {
        auto future = executor_.submit_auto([this, run_id] {
            Error find_error;
            auto inner = find_run(run_id, find_error);
            if (!inner.has_value()) {
                return Result<WorkflowRunResult>{find_error};
            }
            RunRecord &record = *inner.value();
            OperationContext drive_context;
            drive_context.session = session_;
            drive_context.task = record.task;
            drive_context.started_at = Timestamp::now();
            auto result = drive(record, drive_context);
            std::lock_guard lock(mutex_);
            record.result = result;
            record.drive_active = false;
            --async_drives_;
            return Result<WorkflowRunResult>{result};
        });
        std::lock_guard lock(mutex_);
        run.drive_future = future.share();
        return run.view;
    } catch (const std::exception &exception) {
        // The resume itself failed before any step executed; the pause-family
        // safety semantics are restored by rolling the view back.
        static_cast<void>(commit_transition(run, WorkflowRunState::Paused));
        std::lock_guard lock(mutex_);
        run.drive_active = false;
        --async_drives_;
        return workflow_error(ErrorCode::ResourceExhausted, exception.what(), true);
    }
}

Result<void> WorkflowRuntime::resolve_interrupted_step(RunRecord &run) {
    const std::size_t index = run.cursor;
    if (index >= run.definition.steps.size()) {
        run.interrupted = false;
        return Result<void>{};
    }
    const WorkflowStep &step = run.definition.steps[index];
    // A patch that skipped the interrupted step wins over re-execution
    // (DEC-024 §3): settle Skipped and advance past it. The lock is a leaf,
    // so the settlement runs outside its scope.
    bool patch_skipped = false;
    {
        std::lock_guard lock(mutex_);
        patch_skipped = contains_string(run.skipped_steps, step.id.to_string());
    }
    if (patch_skipped) {
        WorkflowStepRecord record;
        record.step_id = step.id;
        record.attempt = run.attempts[index];
        record.disposition = WorkflowStepDisposition::Skipped;
        record.verification = "none";
        record.safe_summary = "skipped by run patch";
        settle_step(run, step, record);
        ++run.cursor;
        run.interrupted = false;
        return Result<void>{};
    }
    const bool dispatches = workflow_policy_dispatches_side_effects(run.view.policy);
    if (!dispatches || step.kind != WorkflowStepKind::ToolCall ||
        !step_tool_has_side_effects(run, index)) {
        // Nothing was dispatched (DryRun) or the tool is side-effect free:
        // re-executing the step is safe.
        run.interrupted = false;
        return Result<void>{};
    }
    // Uncertain side effect: RULE-05 forbids blind redispatch. W-02 guarantees
    // side-effecting steps carry a verification predicate; re-evaluate it.
    const auto verdict =
        evaluate_workflow_predicate(*step.verification, evaluation_context(run));
    if (verdict == WorkflowPredicateResult::Satisfied) {
        WorkflowStepRecord record;
        record.step_id = step.id;
        record.attempt = run.attempts[index];
        record.disposition = WorkflowStepDisposition::Completed;
        record.verification = "satisfied";
        record.safe_summary = "recovered after pause: verification satisfied";
        settle_step(run, step, record);
        ++run.cursor;
        run.interrupted = false;
        return Result<void>{};
    }
    if (step.recovery.has_value() &&
        step.recovery->mode == WorkflowRecoveryHook::Mode::FallbackStep) {
        WorkflowStepRecord record;
        record.step_id = step.id;
        record.attempt = run.attempts[index];
        record.disposition = WorkflowStepDisposition::Failed;
        record.verification = verification_name(verdict);
        record.safe_summary = "interrupted side effect not verifiable; fallback";
        settle_step(run, step, record);
        run.cursor = step_index(run, *step.recovery->fallback_step);
        run.interrupted = false;
        return Result<void>{};
    }
    run.interrupted = false;
    auto failure =
        make_runtime_error(WorkflowRuntimeError::ResumeUncertainSideEffect,
                           "interrupted side-effecting step cannot be verified or recovered");
    static_cast<void>(settle_terminal(run, WorkflowRunState::Failed, failure.safe_message));
    return failure;
}

Result<WorkflowRunView> WorkflowRuntime::cancel_run(const WorkflowRunId &run_id) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (is_terminal(run.view.state)) {
            return run.view; // Idempotent cancel on terminal runs.
        }
    }
    const auto command = runtime_.cancel_task(run.task);
    if (command.has_value()) {
        static_cast<void>(command.value().outcome(config_.command_timeout));
    }
    bool driven = true;
    const auto deadline = std::chrono::steady_clock::now() + config_.command_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool settled = false;
        {
            std::lock_guard lock(mutex_);
            settled = is_terminal(run.view.state);
            driven = run.drive_active;
        }
        if (settled) {
            std::lock_guard lock(mutex_);
            return run.view;
        }
        if (!driven) {
            static_cast<void>(
                settle_terminal(run, WorkflowRunState::Cancelled, "run cancelled by request"));
            std::lock_guard lock(mutex_);
            return run.view;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Force the view: the transition table keeps this safe against racing
    // drives, and a late drive settlement collapses to a terminal NoOp.
    static_cast<void>(settle_terminal(run, WorkflowRunState::Cancelled, "run cancelled by request"));
    {
        std::lock_guard lock(mutex_);
        return run.view;
    }
}

Result<WorkflowRunView> WorkflowRuntime::run_snapshot(const WorkflowRunId &run_id) const {
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(run_id);
    if (found == runs_.end()) {
        return workflow_error(ErrorCode::NotFound, "workflow run was not found");
    }
    return found->second->view;
}

Result<WorkflowPatchOutcome> WorkflowRuntime::patch_run(const WorkflowRunId &run_id,
                                                        const WorkflowPatchId &patch_id,
                                                        const std::vector<WorkflowPatchEntry> &entries) {
    if (shut_down_) {
        return workflow_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    }
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    return submit_patch(*found.value(), patch_id, entries, false);
}

Result<WorkflowPatchOutcome> WorkflowRuntime::rollback_run_patch(const WorkflowRunId &run_id,
                                                                 const WorkflowPatchId &patch_id) {
    if (shut_down_) {
        return workflow_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    }
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    std::vector<WorkflowPatchEntry> entries;
    {
        std::lock_guard lock(mutex_);
        const auto target = std::find_if(run.applied_patches.begin(), run.applied_patches.end(),
                                         [&](const AppliedPatch &candidate) {
                                             return candidate.patch_id == patch_id;
                                         });
        if (target == run.applied_patches.end()) {
            return workflow_error(ErrorCode::NotFound, "applied patch was not found");
        }
        entries = rollback_entries(run, *target);
    }
    if (entries.empty()) {
        return workflow_error(ErrorCode::InvalidArgument,
                              "rollback produced no entries; state already matches the boundary");
    }
    return submit_patch(run, WorkflowPatchId::generate(), entries, false);
}

Result<WorkflowPatchOutcome>
WorkflowRuntime::submit_patch(RunRecord &run, const WorkflowPatchId &patch_id,
                              const std::vector<WorkflowPatchEntry> &entries,
                              bool decision_confirmed) {
    if (entries.empty()) {
        return workflow_error(ErrorCode::InvalidArgument, "patch requires at least one entry");
    }
    for (const auto &entry : entries) {
        if (auto check = validate_workflow_patch_entry(entry); !check.has_value()) {
            return check.error();
        }
    }
    const auto digest = workflow_patch_digest(patch_id, entries);
    bool queue = false;
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        // Write-before idempotency (DEC-024 §2): replaying an accepted patch
        // is the NoOp; reusing its id for different content is the conflict.
        const auto replay = [&](const Sha256Digest &entry_digest) {
            if (entry_digest == digest) {
                WorkflowPatchOutcome outcome;
                outcome.view = run.view;
                outcome.applied = false;
                return Result<WorkflowPatchOutcome>{outcome};
            }
            return Result<WorkflowPatchOutcome>{make_runtime_error(
                WorkflowRuntimeError::PatchIdConflict,
                "patch id is already used by different content")};
        };
        for (const auto &applied : run.applied_patches) {
            if (applied.patch_id == patch_id) {
                return replay(applied.digest);
            }
        }
        for (const auto &pending : run.pending_patches) {
            if (pending.patch_id == patch_id) {
                return replay(pending.digest);
            }
        }
        if (is_terminal(run.view.state)) {
            return workflow_error(ErrorCode::InvalidState, "workflow run is terminal");
        }
        const bool boundary_state = run.view.state == WorkflowRunState::Running ||
                                    run.view.state == WorkflowRunState::Paused ||
                                    run.view.state == WorkflowRunState::WaitingUser ||
                                    run.view.state == WorkflowRunState::WaitingAgent;
        if (!boundary_state) {
            return workflow_error(ErrorCode::InvalidState,
                                  "patches apply to runs at a step boundary");
        }
        // Admission matrix (DEC-024 §1): Interactive runs accept patches in
        // every boundary state; agent-capable runs accept them while waiting
        // for the repairing agent; an accepted decision proposal is its own
        // confirmation channel.
        const bool interactive = run.view.policy == WorkflowPolicy::Interactive;
        const bool agent_repair = run.view.state == WorkflowRunState::WaitingAgent &&
                                  workflow_policy_allows_agent(run.view.policy);
        if (!decision_confirmed && !interactive && !agent_repair) {
            return make_runtime_error(
                WorkflowRuntimeError::PolicyNotInteractive,
                "patches require an interactive policy or an agent-repair wait state");
        }
        if (run.view.state == WorkflowRunState::Running) {
            if (run.pending_patches.size() >= config_.max_pending_patches_per_run) {
                return make_runtime_error(WorkflowRuntimeError::PatchCapacityExceeded,
                                          "pending patch queue is full");
            }
            queue = true;
        }
    }
    if (queue) {
        PendingPatch pending;
        pending.patch_id = patch_id;
        pending.digest = digest;
        pending.entries = entries;
        WorkflowPatchOutcome outcome;
        {
            std::lock_guard lock(mutex_);
            run.pending_patches.push_back(std::move(pending));
            outcome.view = run.view;
        }
        outcome.applied = true;
        outcome.queued = true;
        return outcome;
    }
    const std::string reason = decision_confirmed ? "decision-accept" : "direct";
    if (auto failure = apply_patch(run, patch_id, digest, entries, decision_confirmed, reason);
        failure.has_value()) {
        return failure.value();
    }
    WorkflowPatchOutcome outcome;
    {
        std::lock_guard lock(mutex_);
        outcome.view = run.view;
    }
    outcome.applied = true;
    return outcome;
}

std::optional<Error> WorkflowRuntime::apply_patch(RunRecord &run, const WorkflowPatchId &patch_id,
                                                  const Sha256Digest &digest,
                                                  const std::vector<WorkflowPatchEntry> &entries,
                                                  bool /*decision_confirmed*/,
                                                  const std::string &reason_code) {
    // Audit first (DEC-024 §2): one Proposed event per entry, then either
    // Applied or Rejected for the whole patch.
    for (const auto &entry : entries) {
        emit_patch_proposed(run, patch_id, digest, entry.target, reason_code);
    }
    const auto reject = [&](const std::string &code, Error error) -> std::optional<Error> {
        emit_patch_rejected(run, patch_id, code);
        return error;
    };

    std::shared_ptr<BuiltinToolRegistry> tools;
    JsonValue new_parameters;
    WorkflowPolicy new_policy = WorkflowPolicy::Strict;
    WorkflowPolicy previous_policy = WorkflowPolicy::Strict;
    std::vector<std::string> new_skipped;
    std::map<std::string, JsonValue> new_overrides;
    WorkflowParameterBindings new_bindings;
    std::vector<JsonValue> new_resolved;
    {
        std::lock_guard lock(mutex_);
        if (is_terminal(run.view.state)) {
            return reject("run-terminal",
                          workflow_error(ErrorCode::InvalidState, "workflow run is terminal"));
        }
        tools = tools_;
        new_parameters = run.effective_parameters;
        previous_policy = run.view.policy;
        new_policy = run.view.policy;
        new_skipped = run.skipped_steps;
        new_overrides = run.argument_overrides;
        new_bindings = run.bindings;
        new_resolved = run.resolved_arguments;
    }

    const auto step_of = [&](const std::string &path) -> const WorkflowStep * {
        for (const auto &step : run.definition.steps) {
            if (step.id.to_string() == path) {
                return &step;
            }
        }
        return nullptr;
    };
    // Shape and registry validation shared by override sets and re-resolution.
    const auto check_tool_arguments = [&](const JsonValue &arguments) -> std::optional<Error> {
        const auto *tool_member = arguments.find("tool");
        if (!arguments.is_object() || tool_member == nullptr || !tool_member->is_string() ||
            tool_member->as_string()->empty()) {
            return make_runtime_error(WorkflowRuntimeError::ToolBindingInvalid,
                                      "patched tool_call arguments must be an object naming the "
                                      "target tool in the reserved \"tool\" member");
        }
        if (workflow_policy_dispatches_side_effects(new_policy)) {
            if (!tools) {
                return workflow_error(ErrorCode::InvalidState,
                                      "a tool registry is required to run tool_call steps");
            }
            const auto exposed = tools->exposed_tools();
            const auto tool = std::find_if(exposed.begin(), exposed.end(),
                                           [&](const ExposedToolSpec &entry) {
                                               return entry.wire_name == *tool_member->as_string();
                                           });
            if (tool == exposed.end()) {
                return workflow_error(ErrorCode::NotFound, "step tool is not registered");
            }
        }
        return std::nullopt;
    };

    // Target validation against the pinned definition (DEC-024 §3).
    for (const auto &entry : entries) {
        switch (entry.target) {
        case WorkflowPatchTarget::RunParameters: {
            const bool declared = std::any_of(
                run.definition.parameters.begin(), run.definition.parameters.end(),
                [&](const WorkflowParameterSpec &spec) { return spec.name == entry.path; });
            if (!declared) {
                return reject("unknown-target",
                              make_runtime_error(WorkflowRuntimeError::UnknownPatchTarget,
                                                 "patch path does not name a declared parameter"));
            }
            break;
        }
        case WorkflowPatchTarget::StepArguments: {
            if (step_of(entry.path) == nullptr) {
                return reject("unknown-target",
                              make_runtime_error(WorkflowRuntimeError::UnknownPatchTarget,
                                                 "patch path does not name a declared step"));
            }
            break;
        }
        case WorkflowPatchTarget::ExecutionPolicy: {
            const auto parsed = parse_workflow_policy(*entry.value.as_string());
            if (!parsed.has_value()) {
                return reject("policy-unknown", parsed.error());
            }
            const bool allowed = std::any_of(run.definition.allowed_policies.begin(),
                                             run.definition.allowed_policies.end(),
                                             [&](WorkflowPolicy candidate) {
                                                 return candidate == parsed.value();
                                             });
            if (!allowed) {
                return reject("policy-not-allowed",
                              make_runtime_error(WorkflowRuntimeError::PolicySwitchRejected,
                                                 "policy is not declared in the workflow allowed "
                                                 "set"));
            }
            if (workflow_policy_dispatches_side_effects(parsed.value())) {
                const auto navigate = std::find_if(
                    run.definition.steps.begin(), run.definition.steps.end(),
                    [](const WorkflowStep &step) {
                        return step.kind == WorkflowStepKind::Navigate;
                    });
                // Stage E (DEC-028 §3): with an installed navigation context
                // the switch is admissible; without one the M9 gate stays.
                if (navigate != run.definition.steps.end() &&
                    !navigation_context_installed()) {
                    return reject("navigate-unresolvable",
                                  make_runtime_error(WorkflowRuntimeError::PolicySwitchRejected,
                                                     "navigate steps require an installed "
                                                     "navigation context under dispatching "
                                                     "policies"));
                }
            }
            break;
        }
        }
    }

    // Parameter mutations first, then one rebinding pass (DEC-024 §3).
    bool parameters_changed = false;
    for (const auto &entry : entries) {
        if (entry.target != WorkflowPatchTarget::RunParameters) {
            continue;
        }
        if (entry.op == WorkflowPatchOp::Set) {
            new_parameters.set(entry.path, entry.value);
            parameters_changed = true;
        } else if (entry.op == WorkflowPatchOp::Unset) {
            JsonValue rebuilt(JsonValue::Object{});
            if (const auto *object = new_parameters.as_object()) {
                for (const auto &member : *object) {
                    if (member.first != entry.path) {
                        rebuilt.set(member.first, member.second);
                    }
                }
            }
            new_parameters = std::move(rebuilt);
            parameters_changed = true;
        }
    }
    if (parameters_changed) {
        // Unset falls back to declared defaults; a missing required
        // parameter fails the whole patch (no partial application).
        auto rebound = bind_workflow_parameters(run.definition, new_parameters);
        if (!rebound.has_value()) {
            return reject("bind-failed", rebound.error());
        }
        new_bindings = rebound.value();
        new_parameters = new_bindings.values;
        for (std::size_t index = 0; index < run.definition.steps.size(); ++index) {
            const auto &step = run.definition.steps[index];
            if (new_overrides.count(step.id.to_string()) != 0) {
                continue;
            }
            if (step.kind == WorkflowStepKind::Navigate) {
                // Stage E: a parameterized navigate target re-resolves with
                // the new bindings; the effective shape stays checked.
                auto resolved = resolve_step_arguments(new_bindings, step.arguments);
                if (!resolved.has_value()) {
                    return reject("resolve-failed", resolved.error());
                }
                const auto *target = resolved.value().find("target");
                if (!resolved.value().is_object() || target == nullptr ||
                    !target->is_string() || target->as_string()->empty()) {
                    return reject(
                        "navigate-target-invalid",
                        make_runtime_error(WorkflowRuntimeError::PatchRejected,
                                           "navigate arguments must carry a string \"target\" "
                                           "member"));
                }
                new_resolved[index] = std::move(resolved.value());
                continue;
            }
            if (step.kind != WorkflowStepKind::ToolCall) {
                continue;
            }
            auto resolved = resolve_step_arguments(new_bindings, step.arguments);
            if (!resolved.has_value()) {
                return reject("resolve-failed", resolved.error());
            }
            if (auto shape = check_tool_arguments(resolved.value()); shape.has_value()) {
                return reject("tool-binding-invalid", shape.value());
            }
            new_resolved[index] = std::move(resolved.value());
        }
    }

    // Step-argument entries, in order (a later entry may overwrite an earlier
    // one; the digest covers the exact combination).
    for (const auto &entry : entries) {
        if (entry.target != WorkflowPatchTarget::StepArguments) {
            continue;
        }
        const WorkflowStep *step = step_of(entry.path);
        const std::size_t index = step_index(run, step->id);
        switch (entry.op) {
        case WorkflowPatchOp::Skip: {
            if (!contains_string(new_skipped, entry.path)) {
                new_skipped.push_back(entry.path);
            }
            break;
        }
        case WorkflowPatchOp::Unset: {
            new_overrides.erase(entry.path);
            new_skipped.erase(std::remove(new_skipped.begin(), new_skipped.end(), entry.path),
                              new_skipped.end());
            if (step->kind == WorkflowStepKind::ToolCall) {
                auto resolved = resolve_step_arguments(new_bindings, step->arguments);
                if (!resolved.has_value()) {
                    return reject("resolve-failed", resolved.error());
                }
                if (auto shape = check_tool_arguments(resolved.value()); shape.has_value()) {
                    return reject("tool-binding-invalid", shape.value());
                }
                new_resolved[index] = std::move(resolved.value());
            } else if (step->kind == WorkflowStepKind::Navigate) {
                // Stage E: fall back to the creation-resolved arguments.
                auto resolved = resolve_step_arguments(new_bindings, step->arguments);
                if (!resolved.has_value()) {
                    return reject("resolve-failed", resolved.error());
                }
                const auto *target = resolved.value().find("target");
                if (!resolved.value().is_object() || target == nullptr ||
                    !target->is_string() || target->as_string()->empty()) {
                    return reject(
                        "navigate-target-invalid",
                        make_runtime_error(WorkflowRuntimeError::PatchRejected,
                                           "navigate arguments must carry a string \"target\" "
                                           "member"));
                }
                new_resolved[index] = std::move(resolved.value());
            }
            break;
        }
        case WorkflowPatchOp::Set: {
            if (step->kind == WorkflowStepKind::Navigate) {
                const auto *target = entry.value.find("target");
                if (!entry.value.is_object() || target == nullptr || !target->is_string() ||
                    target->as_string()->empty()) {
                    return reject(
                        "navigate-target-invalid",
                        make_runtime_error(WorkflowRuntimeError::PatchRejected,
                                           "patched navigate arguments must carry a string "
                                           "\"target\" member"));
                }
            }
            if (step->kind == WorkflowStepKind::ToolCall) {
                if (auto shape = check_tool_arguments(entry.value); shape.has_value()) {
                    return reject("tool-binding-invalid", shape.value());
                }
            }
            auto resolved = resolve_step_arguments(new_bindings, entry.value);
            if (!resolved.has_value()) {
                return reject("resolve-failed", resolved.error());
            }
            new_overrides[entry.path] = entry.value;
            if (step->kind == WorkflowStepKind::ToolCall ||
                step->kind == WorkflowStepKind::Navigate) {
                new_resolved[index] = std::move(resolved.value());
            }
            break;
        }
        }
    }

    // Policy switch lands last so later validation saw the previous policy.
    for (const auto &entry : entries) {
        if (entry.target == WorkflowPatchTarget::ExecutionPolicy) {
            new_policy = parse_workflow_policy(*entry.value.as_string()).value();
        }
    }

    {
        std::lock_guard lock(mutex_);
        if (run.applied_patches.size() >= config_.max_applied_patches_per_run) {
            return make_runtime_error(WorkflowRuntimeError::PatchCapacityExceeded,
                                      "applied patch record capacity is exhausted");
        }
        AppliedPatch boundary;
        boundary.patch_id = patch_id;
        boundary.digest = digest;
        boundary.entries = entries;
        boundary.parameters = run.effective_parameters;
        boundary.policy = run.view.policy;
        boundary.skipped = run.skipped_steps;
        boundary.overrides = run.argument_overrides;
        run.applied_patches.push_back(std::move(boundary));
        run.effective_parameters = std::move(new_parameters);
        run.skipped_steps = std::move(new_skipped);
        run.argument_overrides = std::move(new_overrides);
        run.bindings = std::move(new_bindings);
        run.resolved_arguments = std::move(new_resolved);
        run.view.policy = new_policy;
        if (parameters_changed) {
            JsonValue refreshed(JsonValue::Object{});
            if (const auto *object = run.predicate_context.as_object()) {
                for (const auto &member : *object) {
                    if (!member.first.starts_with("run_parameter:")) {
                        refreshed.set(member.first, member.second);
                    }
                }
            }
            if (const auto *values = run.effective_parameters.as_object()) {
                for (const auto &member : *values) {
                    refreshed.set("run_parameter:" + member.first, member.second);
                }
            }
            run.predicate_context = std::move(refreshed);
        }
        ++run.view.run_patch_epoch;
    }
    emit_patch_applied(run, patch_id);
    if (new_policy != previous_policy) {
        emit_policy_switched(run, previous_policy, new_policy);
    }
    return std::nullopt;
}

void WorkflowRuntime::drain_pending_patches(RunRecord &run) {
    std::vector<PendingPatch> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(run.pending_patches);
    }
    for (auto &patch : pending) {
        // Application re-runs the full validation: the effective state may
        // have advanced since submission, and a patch that no longer applies
        // settles as Rejected without touching the epoch.
        static_cast<void>(apply_patch(run, patch.patch_id, patch.digest, patch.entries, false,
                                      "boundary"));
    }
}

std::vector<WorkflowPatchEntry>
WorkflowRuntime::rollback_entries(const RunRecord &run, const AppliedPatch &target) const {
    std::vector<WorkflowPatchEntry> entries;
    const auto append = [&entries](WorkflowPatchTarget patch_target, WorkflowPatchOp op,
                                   const std::string &path, JsonValue value = JsonValue{}) {
        WorkflowPatchEntry entry;
        entry.target = patch_target;
        entry.op = op;
        entry.path = path;
        entry.value = std::move(value);
        entries.push_back(std::move(entry));
    };
    const auto *current_parameters = run.effective_parameters.as_object();
    const auto *boundary_parameters = target.parameters.as_object();
    const auto find_member = [](const JsonValue::Object *object, const std::string &key) {
        if (object == nullptr) {
            return static_cast<const JsonValue *>(nullptr);
        }
        for (const auto &member : *object) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return static_cast<const JsonValue *>(nullptr);
    };
    if (current_parameters != nullptr) {
        for (const auto &member : *current_parameters) {
            const auto *boundary_value = find_member(boundary_parameters, member.first);
            if (boundary_value == nullptr) {
                append(WorkflowPatchTarget::RunParameters, WorkflowPatchOp::Unset, member.first);
            } else if (!(*boundary_value == member.second)) {
                append(WorkflowPatchTarget::RunParameters, WorkflowPatchOp::Set, member.first,
                       *boundary_value);
            }
        }
    }
    if (boundary_parameters != nullptr) {
        for (const auto &member : *boundary_parameters) {
            if (find_member(current_parameters, member.first) == nullptr) {
                append(WorkflowPatchTarget::RunParameters, WorkflowPatchOp::Set, member.first,
                       member.second);
            }
        }
    }
    if (run.view.policy != target.policy) {
        append(WorkflowPatchTarget::ExecutionPolicy, WorkflowPatchOp::Set, "policy",
               JsonValue{workflow_policy_name(target.policy)});
    }
    for (const auto &[step, value] : run.argument_overrides) {
        const auto boundary_value = target.overrides.find(step);
        if (boundary_value == target.overrides.end()) {
            append(WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Unset, step);
        } else if (!(boundary_value->second == value)) {
            append(WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Set, step,
                   boundary_value->second);
        }
    }
    for (const auto &[step, value] : target.overrides) {
        if (run.argument_overrides.count(step) == 0) {
            append(WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Set, step, value);
        }
    }
    for (const auto &step : run.skipped_steps) {
        if (!contains_string(target.skipped, step)) {
            append(WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Unset, step);
        }
    }
    for (const auto &step : target.skipped) {
        if (!contains_string(run.skipped_steps, step)) {
            append(WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Skip, step);
        }
    }
    return entries;
}

Result<WorkflowDecisionRequest>
WorkflowRuntime::pending_decision_request(const WorkflowRunId &run_id) const {
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(run_id);
    if (found == runs_.end()) {
        return workflow_error(ErrorCode::NotFound, "workflow run was not found");
    }
    const RunRecord &run = *found->second;
    if (run.view.state != WorkflowRunState::WaitingUser || !run.decision.has_value()) {
        return make_runtime_error(WorkflowRuntimeError::DecisionNotPending,
                                  "run has no pending decision");
    }
    return *run.decision;
}

Result<WorkflowRunView> WorkflowRuntime::resolve_decision(
    const WorkflowRunId &run_id, const WorkflowDecisionId &decision_id,
    const Sha256Digest &payload_digest, WorkflowDecisionResolution resolution) {
    Error error;
    auto found = find_run(run_id, error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
        if (run.view.state != WorkflowRunState::WaitingUser ||
            !run.view.pending_decision.has_value()) {
            return make_runtime_error(WorkflowRuntimeError::DecisionNotPending,
                                      "run has no pending decision");
        }
        const auto &pending = *run.view.pending_decision;
        if (!(pending.decision_id == decision_id) || !(pending.payload_digest == payload_digest)) {
            return make_runtime_error(WorkflowRuntimeError::DecisionMismatch,
                                      "decision identity does not match the pending decision");
        }
    }
    switch (resolution) {
    case WorkflowDecisionResolution::CancelRun: {
        emit_decision_resolved(run, decision_id, resolution);
        return cancel_run(run_id);
    }
    case WorkflowDecisionResolution::Reject: {
        WorkflowDecisionKind kind = WorkflowDecisionKind::StepFailure;
        {
            std::lock_guard lock(mutex_);
            kind = run.decision.has_value() ? run.decision->kind
                                            : WorkflowDecisionKind::StepFailure;
        }
        emit_decision_resolved(run, decision_id, resolution);
        if (kind == WorkflowDecisionKind::StepFailure) {
            // The frozen table has no WaitingUser -> Failed edge: the
            // rejection returns the run to activity first, then settles the
            // failure from Running (DEC-020 §1 edges only).
            if (auto activated = commit_transition(run, WorkflowRunState::Running);
                activated.has_value()) {
                return activated.value();
            }
            if (auto failure = settle_terminal(run, WorkflowRunState::Failed,
                                               "user rejected the failure decision");
                failure.has_value()) {
                return failure.value();
            }
            std::lock_guard lock(mutex_);
            return run.view;
        }
        // AgentPrompt reject: continue without applying any patch.
        return continue_run(run);
    }
    case WorkflowDecisionResolution::Accept: {
        std::vector<WorkflowPatchEntry> proposal;
        {
            std::lock_guard lock(mutex_);
            proposal = run.decision.has_value() ? run.decision->proposal
                                                : std::vector<WorkflowPatchEntry>{};
        }
        if (!proposal.empty()) {
            // The accepted proposal is confirmed by the decision itself but
            // still traverses the full validation pipeline (W-04).
            auto applied = submit_patch(run, WorkflowPatchId::generate(), proposal, true);
            if (!applied.has_value()) {
                return applied.error();
            }
        }
        emit_decision_resolved(run, decision_id, resolution);
        return continue_run(run);
    }
    }
    return workflow_error(ErrorCode::InvalidArgument, "unknown decision resolution");
}

Result<WorkflowAgentContinuation> WorkflowRuntime::agent_continuation(
    const WorkflowRunId &run_id) const {
    std::lock_guard lock(mutex_);
    const auto found = runs_.find(run_id);
    if (found == runs_.end()) {
        return workflow_error(ErrorCode::NotFound, "workflow run was not found");
    }
    const RunRecord &run = *found->second;
    if (run.view.state != WorkflowRunState::WaitingAgent) {
        return workflow_error(ErrorCode::InvalidState,
                              "agent continuation is available for waiting-agent runs only");
    }
    WorkflowAgentContinuation continuation;
    continuation.run_id = run.view.run_id;
    continuation.workflow_id = run.view.workflow_id;
    continuation.ir_digest = run.view.ir_digest;
    continuation.policy = run.view.policy;
    continuation.effective_parameters = run.effective_parameters;
    continuation.step_history = run.settled;
    continuation.failure_reason = run.failure_reason;
    continuation.pending_decision = run.view.pending_decision;
    continuation.relevant_lessons = run.retrieved_lessons;
    if (run.cursor < run.definition.steps.size()) {
        const auto &step = run.definition.steps[run.cursor];
        continuation.current_step = step.id;
        continuation.current_step_kind = workflow_step_kind_name(step.kind);
        continuation.current_step_attempts = run.attempts[run.cursor];
    }
    return continuation;
}

Result<WorkflowTrajectory> WorkflowRuntime::capture_trajectory(const WorkflowRunId &run_id) const {
    std::lock_guard lock(mutex_);
    if (!accepting_ || shut_down_) {
        return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
    }
    const auto found = runs_.find(run_id);
    if (found == runs_.end()) {
        return workflow_error(ErrorCode::NotFound, "workflow run was not found");
    }
    const RunRecord &run = *found->second;
    if (run.view.state != WorkflowRunState::Completed) {
        return make_workflow_compile_error(WorkflowCompileError::CaptureNotCompleted,
                                           "trajectory capture requires a completed run");
    }
    if (!workflow_policy_dispatches_side_effects(run.view.policy)) {
        return make_workflow_compile_error(
            WorkflowCompileError::CaptureNotExecuted,
            "trajectory capture requires a run that dispatched side effects");
    }

    WorkflowTrajectory trajectory;
    trajectory.workflow_id = run.view.workflow_id;
    trajectory.source_digest = run.view.ir_digest;
    trajectory.source_run_id = run.view.run_id;
    trajectory.effective_parameters = run.effective_parameters;
    trajectory.effective_policy = run.view.policy;
    trajectory.source_parameters = run.definition.parameters;
    trajectory.allowed_policies = run.definition.allowed_policies;
    trajectory.source_default_policy = run.definition.default_policy;
    trajectory.steps.reserve(run.definition.steps.size());
    for (std::size_t index = 0; index < run.definition.steps.size(); ++index) {
        const WorkflowStep &step = run.definition.steps[index];
        if (contains_string(run.skipped_steps, step.id.to_string())) {
            continue;
        }
        WorkflowTrajectoryStep captured;
        captured.source_step_id = step.id;
        captured.name = step.name;
        captured.kind = step.kind;
        captured.loop_head = step.loop_head;
        captured.arguments = run.resolved_arguments[index];
        captured.raw_arguments = step.arguments;
        captured.precondition = step.precondition;
        captured.verification = step.verification;
        captured.recovery = step.recovery;
        captured.max_attempts = step.max_attempts;
        captured.jump_to = step.jump_to;
        captured.max_iterations = step.max_iterations;
        trajectory.steps.push_back(std::move(captured));
    }
    return trajectory;
}

Result<WorkflowPublishOutcome>
WorkflowRuntime::publish_validated(const WorkflowDefinition &definition, const std::string &actor,
                                   const std::string &reason,
                                   std::optional<WorkflowRunId> source_run_id) {
    {
        std::lock_guard lock(mutex_);
        if (!accepting_ || shut_down_) {
            return workflow_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
        }
    }
    const auto digest = workflow_definition_digest(definition);
    if (auto valid = validate_workflow_definition(definition); !valid.has_value()) {
        emit_publish_rejected(definition.workflow_id, digest, "validation-failed");
        return valid.error();
    }
    emit_publish_proposed(definition.workflow_id, digest, source_run_id);

    // Gate drive (DEC-025 §3): a DryRun run over the draft with empty
    // parameters. Drafts entering the gate must therefore be default-complete;
    // binding failures reject the publish without touching the library.
    auto gate_run = create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    if (!gate_run.has_value()) {
        emit_publish_rejected(definition.workflow_id, digest, "gate-run-failed");
        return gate_run.error();
    }
    OperationContext context;
    context.started_at = Timestamp::now();
    auto driven = execute_run(gate_run.value().run_id, context);
    if (!driven.has_value()) {
        emit_publish_rejected(definition.workflow_id, digest, "publish-dryrun-failed");
        return driven.error();
    }
    if (driven.value().state != WorkflowRunState::Completed) {
        emit_publish_rejected(definition.workflow_id, digest, "publish-dryrun-failed");
        return make_workflow_compile_error(
            WorkflowCompileError::PublishDryRunFailed,
            "gate drive settled as " + workflow_run_state_name(driven.value().state));
    }

    // Content-derived evidence (DEC-025 §3): same definition plus same DryRun
    // settlement sequence yields the same digest, which the idempotent NoOp
    // below relies on. The source and gate runs are bound through the audit
    // events instead.
    JsonValue::Array step_items;
    step_items.reserve(driven.value().steps.size());
    for (const auto &record : driven.value().steps) {
        JsonValue::Object item;
        item.emplace_back("step_id", record.step_id.to_string());
        item.emplace_back("disposition", workflow_step_disposition_name(record.disposition));
        step_items.push_back(JsonValue{std::move(item)});
    }
    JsonValue::Object evidence_object;
    evidence_object.emplace_back("workflow_id", definition.workflow_id.to_string());
    evidence_object.emplace_back("ir_digest", digest.to_string());
    evidence_object.emplace_back("steps", JsonValue{std::move(step_items)});
    evidence_object.emplace_back("unevaluable_verifications",
                                 static_cast<std::int64_t>(driven.value().unevaluable_verifications));
    const Sha256Digest evidence = canonical_json_digest(JsonValue{std::move(evidence_object)});

    auto appended = append_version_record(definition, digest, actor, reason,
                                          WorkflowValidationResult::DryRunPassed, evidence, true);
    if (!appended.has_value()) {
        emit_publish_rejected(definition.workflow_id, digest, "append-failed");
        return appended.error();
    }
    if (!appended.value().idempotent) {
        emit_publish_applied(definition.workflow_id, digest, evidence, gate_run.value().run_id);
    }
    WorkflowPublishOutcome outcome;
    outcome.ir_digest = digest;
    outcome.idempotent = appended.value().idempotent;
    outcome.dry_run_id = gate_run.value().run_id;
    outcome.evidence = evidence;
    return outcome;
}

std::vector<BuiltinToolRegistration>
WorkflowRuntime::operation_tool_registrations() {
    std::vector<BuiltinToolRegistration> registrations;
    for (const auto operation :
         {WorkflowOperation::RunWorkflow, WorkflowOperation::PatchWorkflow,
          WorkflowOperation::PauseWorkflow, WorkflowOperation::ResumeWorkflow,
          WorkflowOperation::CancelWorkflow}) {
        const auto &spec = workflow_operation_spec(operation);
        BuiltinToolRegistration registration;
        registration.spec.tool_id = ToolId::generate();
        registration.spec.wire_name = std::string(workflow_operation_wire_name(operation));
        registration.spec.description = spec.description;
        registration.spec.parameters_schema = spec.parameters_schema;
        registration.spec.has_side_effects = spec.has_side_effects;
        registration.handler = [this, operation](const JsonValue &arguments,
                                                 const OperationContext &) {
            return execute_operation(operation, arguments);
        };
        registrations.push_back(std::move(registration));
    }
    return registrations;
}

std::vector<BuiltinToolRegistration> WorkflowRuntime::decision_tool_registrations() {
    const WorkflowUserInputToolSpec &spec = workflow_request_user_input_spec();
    BuiltinToolRegistration registration;
    registration.spec.tool_id = ToolId::generate();
    registration.spec.wire_name = "request_user_input";
    registration.spec.description = spec.description;
    registration.spec.parameters_schema = spec.parameters_schema;
    registration.spec.has_side_effects = false;
    registration.handler = [this](const JsonValue &arguments, const OperationContext &) {
        return execute_user_input(arguments);
    };
    return {std::move(registration)};
}

Result<JsonValue> WorkflowRuntime::execute_user_input(const JsonValue &arguments) {
    if (shut_down_) {
        return workflow_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    }
    if (auto valid = validate_request_user_input_arguments(arguments); !valid.has_value()) {
        return valid.error();
    }
    const auto workflow_id = WorkflowId::parse(*arguments.find("workflow_id")->as_string());
    if (!workflow_id.has_value()) {
        return workflow_error(ErrorCode::InvalidArgument, "workflow_id is malformed");
    }
    const auto run_id = WorkflowRunId::parse(*arguments.find("run_id")->as_string());
    if (!run_id.has_value()) {
        return workflow_error(ErrorCode::InvalidArgument, "run_id is malformed");
    }
    Error error;
    auto found = find_run(run_id.value(), error);
    if (!found.has_value()) {
        return error;
    }
    RunRecord &run = *found.value();
    auto proposal = request_user_input_proposal(arguments);
    if (!proposal.has_value()) {
        return proposal.error();
    }
    const std::string prompt = *arguments.find("prompt")->as_string();
    {
        std::lock_guard lock(mutex_);
        if (run.view.workflow_id != workflow_id.value()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        if (run.view.state != WorkflowRunState::Running) {
            return workflow_error(ErrorCode::InvalidState,
                                  "decision points are raised on running runs");
        }
        if (run.view.policy != WorkflowPolicy::Interactive) {
            return make_runtime_error(WorkflowRuntimeError::PolicyNotInteractive,
                                      "decision points require an interactive policy");
        }
        if (run.view.pending_decision.has_value()) {
            return workflow_error(ErrorCode::InvalidState,
                                  "run already has a pending decision point");
        }
    }
    WorkflowDecisionRequest decision;
    decision.decision_id = WorkflowDecisionId::generate();
    decision.kind = WorkflowDecisionKind::AgentPrompt;
    decision.safe_summary = bounded_summary(prompt);
    decision.proposal = std::move(proposal.value());
    decision.payload_digest = decision_payload_digest(prompt, decision.proposal);
    if (auto failure = escalate_waiting_user(run, std::move(decision)); failure.has_value()) {
        return failure.value();
    }
    WorkflowDecisionRequest stored;
    {
        std::lock_guard lock(mutex_);
        if (run.decision.has_value()) {
            stored = *run.decision;
        }
    }
    JsonValue::Object root;
    root.emplace_back("decision_id", stored.decision_id.to_string());
    root.emplace_back("state", "waiting_user");
    return JsonValue{std::move(root)};
}

Result<JsonValue> WorkflowRuntime::execute_operation(WorkflowOperation operation,
                                                     const JsonValue &arguments) {
    if (shut_down_) {
        return workflow_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    }
    if (auto valid = validate_workflow_operation(operation, arguments); !valid.has_value()) {
        return valid.error();
    }
    const auto run_id_text = arguments.find("run_id");
    std::optional<WorkflowRunId> run_id;
    if (run_id_text != nullptr && run_id_text->is_string()) {
        run_id = WorkflowRunId::parse(*run_id_text->as_string());
        if (!run_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "run_id is malformed");
        }
    }
    const auto workflow_text = arguments.find("workflow_id");
    auto workflow_id = WorkflowId::parse(*workflow_text->as_string());
    if (!workflow_id.has_value()) {
        return workflow_error(ErrorCode::InvalidArgument, "workflow_id is malformed");
    }

    auto view_of = [this](const WorkflowRunId &id) -> Result<WorkflowRunView> {
        std::lock_guard lock(mutex_);
        const auto found = runs_.find(id);
        if (found == runs_.end()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        return found->second->view;
    };
    auto output = [](const WorkflowRunView &view) {
        JsonValue::Object root;
        root.emplace_back("run_id", view.run_id.to_string());
        root.emplace_back("state", workflow_run_state_name(view.state));
        return JsonValue{std::move(root)};
    };

    switch (operation) {
    case WorkflowOperation::RunWorkflow: {
        const auto digest = digest_from_hex(*arguments.find("ir_digest")->as_string());
        if (!digest.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "ir_digest is malformed");
        }
        JsonValue parameters(JsonValue::Object{});
        if (const auto *member = arguments.find("parameters"); member != nullptr) {
            parameters = *member;
        }
        std::optional<WorkflowPolicy> policy;
        if (const auto *member = arguments.find("policy");
            member != nullptr && member->is_string()) {
            auto parsed = parse_workflow_policy(*member->as_string());
            if (!parsed.has_value()) {
                return parsed.error();
            }
            policy = parsed.value();
        }
        auto created =
            create_run(workflow_id.value(), digest.value(), std::move(parameters), policy);
        if (!created.has_value()) {
            return created.error();
        }
        if (auto started = start_run(created.value().run_id); !started.has_value()) {
            return started.error();
        }
        const auto snapshot = run_snapshot(created.value().run_id);
        return output(snapshot.has_value() ? snapshot.value() : created.value());
    }
    case WorkflowOperation::PauseWorkflow: {
        if (!run_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "run_id is required");
        }
        auto view = view_of(run_id.value());
        if (!view.has_value() || view.value().workflow_id != workflow_id.value()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        auto paused = pause_run(run_id.value());
        if (!paused.has_value()) {
            return paused.error();
        }
        return output(paused.value());
    }
    case WorkflowOperation::ResumeWorkflow: {
        if (!run_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "run_id is required");
        }
        auto view = view_of(run_id.value());
        if (!view.has_value() || view.value().workflow_id != workflow_id.value()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        auto resumed = resume_run(run_id.value());
        if (!resumed.has_value()) {
            return resumed.error();
        }
        return output(resumed.value());
    }
    case WorkflowOperation::PatchWorkflow: {
        if (!run_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "run_id is required");
        }
        const auto patch_id = WorkflowPatchId::parse(*arguments.find("patch_id")->as_string());
        if (!patch_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "patch_id is malformed");
        }
        auto entries = parse_workflow_patch_entries(*arguments.find("patch_entries"));
        if (!entries.has_value()) {
            return entries.error();
        }
        auto outcome = patch_run(run_id.value(), patch_id.value(), entries.value());
        if (!outcome.has_value()) {
            return outcome.error();
        }
        JsonValue::Object root;
        root.emplace_back("run_id", outcome.value().view.run_id.to_string());
        root.emplace_back("applied", outcome.value().applied);
        root.emplace_back("run_patch_epoch",
                          static_cast<std::int64_t>(outcome.value().view.run_patch_epoch));
        root.emplace_back("safe_summary", outcome.value().queued
                                              ? std::string("patch queued for the next step "
                                                            "boundary")
                                              : std::string("patch applied"));
        return JsonValue{std::move(root)};
    }
    case WorkflowOperation::CancelWorkflow: {
        if (!run_id.has_value()) {
            return workflow_error(ErrorCode::InvalidArgument, "run_id is required");
        }
        auto view = view_of(run_id.value());
        if (!view.has_value() || view.value().workflow_id != workflow_id.value()) {
            return workflow_error(ErrorCode::NotFound, "workflow run was not found");
        }
        const bool already_terminal = is_terminal(view.value().state);
        auto cancelled = cancel_run(run_id.value());
        if (!cancelled.has_value()) {
            return cancelled.error();
        }
        JsonValue::Object root;
        root.emplace_back("run_id", cancelled.value().run_id.to_string());
        root.emplace_back("state", workflow_run_state_name(cancelled.value().state));
        root.emplace_back("already_terminal", already_terminal);
        return JsonValue{std::move(root)};
    }
    }
    return workflow_error(ErrorCode::UnsupportedCapability,
                          "workflow operation is not implemented");
}

WorkflowShutdownReport WorkflowRuntime::shutdown() {
    WorkflowShutdownReport report;
    std::vector<TaskId> tasks;
    std::vector<std::shared_future<Result<WorkflowRunResult>>> futures;
    std::vector<RunRecord *> records;
    {
        std::lock_guard lock(mutex_);
        if (shut_down_) {
            report.clean = true;
            return report;
        }
        accepting_ = false;
        shut_down_ = true;
        for (auto &[id, run] : runs_) {
            records.push_back(run.get());
            if (!is_terminal(run->view.state)) {
                tasks.push_back(run->task);
            }
            if (run->drive_future.valid()) {
                futures.push_back(run->drive_future);
            }
        }
    }
    report.cancelled_runs = tasks.size();
    for (const auto &task : tasks) {
        const auto command = runtime_.cancel_task(task);
        if (command.has_value()) {
            static_cast<void>(command.value().outcome(config_.command_timeout));
        }
    }
    // Settle runs without an active drive directly through the frozen table;
    // driven runs converge on their own from the cancelled carrier task.
    for (auto *run : records) {
        bool active = false;
        {
            std::lock_guard lock(mutex_);
            active = run->drive_active;
        }
        if (!active && !is_terminal(run_state(*run))) {
            static_cast<void>(settle_terminal(*run, WorkflowRunState::Cancelled,
                                              "run cancelled by runtime shutdown"));
        }
    }
    report.clean = true;
    const auto budget = config_.command_timeout + config_.step_deadline;
    for (auto &future : futures) {
        if (future.wait_for(budget) == std::future_status::ready) {
            ++report.drained_drives;
            try {
                static_cast<void>(future.get());
            } catch (const std::exception &) {
                report.clean = false;
            }
        } else {
            report.clean = false;
        }
    }
    if (event_emit_failures_.load() != 0 || task_settlement_failures_.load() != 0 ||
        monitor_failures_.load() != 0) {
        report.clean = false;
        report.diagnostic = "event emit failures: " + std::to_string(event_emit_failures_.load()) +
                            "; task settlement failures: " +
                            std::to_string(task_settlement_failures_.load()) +
                            "; monitor failures: " + std::to_string(monitor_failures_.load());
    }
    if (report.clean && report.diagnostic.empty()) {
        report.diagnostic = "workflow runtime stopped";
    }
    return report;
}

bool WorkflowRuntime::shut_down() const noexcept {
    return shut_down_;
}

} // namespace mira
