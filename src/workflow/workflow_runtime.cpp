#include <mira/workflow_runtime.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
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
    }
    return error;
}

[[nodiscard]] std::string bounded_summary(std::string summary) {
    if (summary.size() > kWorkflowEventMaxSummaryBytes) {
        summary.resize(kWorkflowEventMaxSummaryBytes);
    }
    return summary;
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
    const auto digest = workflow_definition_digest(definition);
    std::lock_guard lock(mutex_);
    const bool fresh_history = library_.find(definition.workflow_id) == library_.end();
    if (fresh_history) {
        WorkflowVersionHistory history;
        history.workflow_id = definition.workflow_id;
        library_.emplace(definition.workflow_id, std::move(history));
    }
    auto &history = library_[definition.workflow_id];
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
    return digest;
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
        if (step.kind == WorkflowStepKind::Navigate && dispatches) {
            return make_runtime_error(
                WorkflowRuntimeError::NavigateUnresolvable,
                "navigate steps cannot be resolved before phase E; dispatching policies "
                "reject them at admission");
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

void WorkflowRuntime::emit_run_settled(const RunRecord &run, const std::string &safe_summary) {
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
    if (!events_->append(append).has_value()) {
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
                evaluate_workflow_predicate(*step.precondition, predicate_context_copy(run));
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
            // Only reachable under DryRun: Strict rejects navigate steps at
            // admission (navigate resolver arrives with phase E).
            record.safe_summary = "planned navigation";
            settle_step(run, step, record);
            ++run.cursor;
            continue;
        }

        if (!dispatches) {
            if (step.verification.has_value()) {
                const auto verdict = evaluate_workflow_predicate(*step.verification,
                                                                 predicate_context_copy(run));
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
                evaluate_workflow_predicate(*step.verification, predicate_context_copy(run));
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
    }
    return enter_waiting_agent(run, reason);
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

    ToolProposal proposal;
    proposal.provider_call_id = ProviderToolCallId{
        "workflow:" + run.view.run_id.to_string() + ":" + step.id.to_string() + ":" +
        std::to_string(run.attempts[index])};
    proposal.tool_id = tool->tool_id;
    proposal.wire_name = wire_name;
    proposal.tool_version = tool->version;
    proposal.arguments = input;
    proposal.arguments_digest = canonical_json_digest(input);
    proposal.operation_id = OperationId::generate();
    proposal.has_side_effects = tool->has_side_effects;

    OperationContext context = parent;
    context.step = step.id;
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
            return Result<void>{};
        }
        return outcome.error();
    }
    const ToolExecutionRecord &execution = outcome.value();
    if (execution.failed) {
        return make_runtime_error(WorkflowRuntimeError::ToolDispatchFailed,
                                  execution.safe_error_summary.empty()
                                      ? "tool execution failed"
                                      : execution.safe_error_summary);
    }
    {
        std::lock_guard lock(mutex_);
        run.predicate_context.set("step_result:" + step.id.to_string(), execution.result);
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
        evaluate_workflow_predicate(*step.verification, predicate_context_copy(run));
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
        evaluate_workflow_predicate(*step.verification, predicate_context_copy(run));
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
                if (navigate != run.definition.steps.end()) {
                    return reject("navigate-unresolvable",
                                  make_runtime_error(WorkflowRuntimeError::PolicySwitchRejected,
                                                     "navigate steps cannot be resolved before "
                                                     "phase E; dispatching policies are "
                                                     "rejected"));
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
            if (step.kind != WorkflowStepKind::ToolCall ||
                new_overrides.count(step.id.to_string()) != 0) {
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
            if (step->kind == WorkflowStepKind::ToolCall) {
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
    if (run.cursor < run.definition.steps.size()) {
        const auto &step = run.definition.steps[run.cursor];
        continuation.current_step = step.id;
        continuation.current_step_kind = workflow_step_kind_name(step.kind);
        continuation.current_step_attempts = run.attempts[run.cursor];
    }
    return continuation;
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
