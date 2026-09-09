#include <mira/workflow_recovery.hpp>

#include <mira/model_schema.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace mira {
namespace {

// Summary bound shared with the workflow event surface; keeps every string
// the orchestrator forwards bounded (RULE-08).
constexpr std::size_t kMaxReasonBytes = 128;
constexpr std::size_t kMaxBoundedLessonIdBytes = 256;
constexpr std::size_t kMaxUsedLessons = 64;
constexpr std::size_t kMaxPatchEntries = 64;
constexpr std::size_t kMaxFailureReasonBytes = 2048;
// Parameter digests expose the first 16 hex chars of the canonical-content
// SHA-256 — enough to correlate, not enough to reconstruct values
// (DEC-031 §4.3 sanitization default).
constexpr std::size_t kParameterDigestHexBytes = 16;

[[nodiscard]] Error recovery_error(ErrorCode code, const std::string &message,
                                   bool retryable = false) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow.recovery";
    error.safe_message = message;
    error.retryable = retryable;
    return error;
}

[[nodiscard]] std::string bounded_reason(std::string reason) {
    if (reason.size() > kMaxReasonBytes) {
        reason.resize(kMaxReasonBytes);
    }
    return reason;
}

[[nodiscard]] std::string json_kind_name(const JsonValue &value) {
    switch (value.kind()) {
    case JsonValue::Kind::Null:
        return "null";
    case JsonValue::Kind::Boolean:
        return "boolean";
    case JsonValue::Kind::Integer:
        return "integer";
    case JsonValue::Kind::Number:
        return "number";
    case JsonValue::Kind::String:
        return "string";
    case JsonValue::Kind::Array:
        return "array";
    case JsonValue::Kind::Object:
        return "object";
    }
    return "null";
}

// Default parameter projection (DEC-031 §4.3): names, value types and a
// short content digest — never values.
[[nodiscard]] JsonValue default_project_parameters(const JsonValue &parameters) {
    JsonValue::Object projection;
    if (parameters.is_object()) {
        for (const auto &[key, value] : *parameters.as_object()) {
            JsonValue::Object entry;
            entry.emplace_back("type", json_kind_name(value));
            entry.emplace_back(
                "digest", digest_string(canonical_json_string(value)).to_string().substr(
                              0, kParameterDigestHexBytes));
            projection.emplace_back(key, JsonValue{std::move(entry)});
        }
    } else if (!parameters.is_null()) {
        JsonValue::Object entry;
        entry.emplace_back("type", json_kind_name(parameters));
        entry.emplace_back(
            "digest", digest_string(canonical_json_string(parameters)).to_string().substr(
                          0, kParameterDigestHexBytes));
        projection.emplace_back("parameters", JsonValue{std::move(entry)});
    }
    return JsonValue{std::move(projection)};
}

// One bounded violation summary carried into a repair round: stable
// machine-facing text only, raw model output never echoes back
// (build_schema_repair_request discipline).
[[nodiscard]] std::string bounded_violation(std::string violation) {
    if (violation.size() > kMaxReasonBytes) {
        violation.resize(kMaxReasonBytes);
    }
    return violation;
}

[[nodiscard]] std::string parse_violation(const DecisionParseResult &parse) {
    if (parse.outcome == DecisionParseOutcome::ToolProposals) {
        return "unexpected tool proposal; recovery decisions are structured output";
    }
    if (parse.outcome == DecisionParseOutcome::Incomplete) {
        return "model output incomplete";
    }
    if (parse.outcome == DecisionParseOutcome::ContentFiltered) {
        return "model output content filtered";
    }
    if (parse.outcome == DecisionParseOutcome::Failed) {
        return "model output failed";
    }
    if (!parse.violations.empty()) {
        const auto &first = parse.violations.front();
        return bounded_violation("schema violation at " + first.path + " (" + first.keyword +
                                 ": " + first.message + ")");
    }
    return "decision output malformed";
}

// The cooperative-abort discriminator: shutdown outranks an explicit cancel
// so the audit trail can tell them apart (design §8 matrix).
[[nodiscard]] std::optional<std::string> abort_reason(bool shutting_down,
                                                      const std::atomic_bool &cancel) {
    if (shutting_down) {
        return std::string{"shutdown"};
    }
    if (cancel.load()) {
        return std::string{"cancelled"};
    }
    return std::nullopt;
}

// Pre-submit recheck (design §4.6): a decision never lands on a run that
// moved on. Returns nullopt when the run is still WaitingAgent at the
// expected epoch; the caller updates the expected epoch from its own
// successful submissions (patch application advances it legitimately).
[[nodiscard]] std::optional<std::string> recheck_run(WorkflowRuntime &runtime,
                                                     const WorkflowRunId &run_id,
                                                     std::uint64_t expected_epoch) {
    const auto snapshot = runtime.run_snapshot(run_id);
    if (!snapshot.has_value()) {
        return std::string{"run-state-changed"};
    }
    if (snapshot.value().state != WorkflowRunState::WaitingAgent) {
        return std::string{"run-state-changed"};
    }
    if (snapshot.value().run_epoch != expected_epoch) {
        return std::string{"epoch-advanced"};
    }
    return std::nullopt;
}

// Semantic layer over a schema-validated decision candidate (design §4.5):
// the four-action closed set plus the per-action shape rules the JSON Schema
// subset cannot express (non-empty entries for patch_and_resume, none for
// the other actions, bounded used_lessons and rationale).
[[nodiscard]] Result<WorkflowRecoveryDecision> decode_decision(const JsonValue &value,
                                                               std::size_t max_rationale_bytes) {
    const auto *action = value.find("action");
    if (action == nullptr || !action->is_string()) {
        return recovery_error(ErrorCode::InvalidModelOutput, "decision carries no action");
    }
    auto parsed_action = parse_workflow_recovery_action(*action->as_string());
    if (!parsed_action.has_value()) {
        return parsed_action.error();
    }
    WorkflowRecoveryDecision decision;
    decision.action = parsed_action.value();
    const auto *entries = value.find("patch_entries");
    const bool has_entries = entries != nullptr && entries->is_array() &&
                             !entries->as_array()->empty();
    if (decision.action == WorkflowRecoveryDecisionAction::PatchAndResume) {
        if (!has_entries) {
            return recovery_error(ErrorCode::InvalidModelOutput,
                                  "patch_and_resume requires non-empty patch_entries");
        }
        if (entries->as_array()->size() > kMaxPatchEntries) {
            return recovery_error(ErrorCode::InvalidModelOutput,
                                  "patch_entries exceed the recovery bound");
        }
        auto parsed_entries = parse_workflow_patch_entries(*entries);
        if (!parsed_entries.has_value()) {
            return parsed_entries.error();
        }
        for (const auto &entry : parsed_entries.value()) {
            if (auto check = validate_workflow_patch_entry(entry); !check.has_value()) {
                return check.error();
            }
        }
        decision.patch_entries = std::move(parsed_entries.value());
    } else if (has_entries) {
        return recovery_error(ErrorCode::InvalidModelOutput,
                              "patch_entries are only valid for patch_and_resume");
    }
    if (const auto *used = value.find("used_lessons"); used != nullptr && used->is_array()) {
        if (used->as_array()->size() > kMaxUsedLessons) {
            return recovery_error(ErrorCode::InvalidModelOutput,
                                  "used_lessons exceed the recovery bound");
        }
        for (const auto &item : *used->as_array()) {
            if (!item.is_string() || item.as_string()->empty() ||
                item.as_string()->size() > kMaxBoundedLessonIdBytes) {
                return recovery_error(ErrorCode::InvalidModelOutput,
                                      "used_lessons entries must be bounded strings");
            }
            decision.used_lessons.push_back(*item.as_string());
        }
    }
    if (const auto *rationale = value.find("rationale");
        rationale != nullptr && rationale->is_string() &&
        rationale->as_string()->size() > max_rationale_bytes) {
        return recovery_error(ErrorCode::InvalidModelOutput, "rationale exceeds the bound");
    } else if (rationale != nullptr && rationale->is_string()) {
        decision.rationale = *rationale->as_string();
    }
    decision.decision_digest = canonical_json_digest(value);
    return decision;
}

// The repair round request: a fresh paid request quoting the bounded
// violation, mirroring build_schema_repair_request's discipline.
[[nodiscard]] ModelRequest make_repair_request(ModelRequest previous,
                                               const std::string &violation) {
    previous.request_id = ModelRequestId::generate();
    previous.operation_id = OperationId::generate();
    ModelInputItem item;
    item.role = ModelRole::User;
    item.provenance.source = "mira.workflow.recovery.repair.v1";
    item.authority = Sensitivity::Internal;
    TextPart text;
    text.text = "Your previous recovery decision was rejected: " + violation +
                ". Respond again with a single JSON decision matching the schema. Do not call "
                "tools.";
    text.sensitivity = Sensitivity::Internal;
    item.content.emplace_back(std::move(text));
    previous.input.emplace_back(std::move(item));
    return previous;
}

} // namespace

// ---------------------------------------------------------------------------
// Config, actions, decision schema
// ---------------------------------------------------------------------------

Result<void> WorkflowRecoveryConfig::validate() const {
    if (max_attempts_per_run == 0) {
        return recovery_error(ErrorCode::InvalidArgument, "max_attempts_per_run must be positive");
    }
    if (model_call_deadline <= std::chrono::milliseconds::zero()) {
        return recovery_error(ErrorCode::InvalidArgument, "model_call_deadline must be positive");
    }
    if (max_concurrent_attempts == 0) {
        return recovery_error(ErrorCode::InvalidArgument,
                              "max_concurrent_attempts must be positive");
    }
    if (max_tracked_runs == 0) {
        return recovery_error(ErrorCode::InvalidArgument, "max_tracked_runs must be positive");
    }
    if (max_lessons_in_context == 0) {
        return recovery_error(ErrorCode::InvalidArgument,
                              "max_lessons_in_context must be positive");
    }
    if (max_rationale_bytes == 0) {
        return recovery_error(ErrorCode::InvalidArgument, "max_rationale_bytes must be positive");
    }
    if (profile_id.is_nil()) {
        return recovery_error(ErrorCode::InvalidArgument, "profile_id must be set");
    }
    return Result<void>{};
}

std::string workflow_recovery_action_name(WorkflowRecoveryDecisionAction action) {
    switch (action) {
    case WorkflowRecoveryDecisionAction::PatchAndResume:
        return "patch_and_resume";
    case WorkflowRecoveryDecisionAction::Resume:
        return "resume";
    case WorkflowRecoveryDecisionAction::Cancel:
        return "cancel";
    case WorkflowRecoveryDecisionAction::NeedUser:
        return "need_user";
    }
    return "need_user";
}

Result<WorkflowRecoveryDecisionAction> parse_workflow_recovery_action(std::string_view name) {
    if (name == "patch_and_resume") {
        return WorkflowRecoveryDecisionAction::PatchAndResume;
    }
    if (name == "resume") {
        return WorkflowRecoveryDecisionAction::Resume;
    }
    if (name == "cancel") {
        return WorkflowRecoveryDecisionAction::Cancel;
    }
    if (name == "need_user") {
        return WorkflowRecoveryDecisionAction::NeedUser;
    }
    return recovery_error(ErrorCode::InvalidArgument, "unknown recovery decision action");
}

JsonSchema workflow_recovery_decision_schema() {
    auto parsed = parse_json(R"json({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["patch_and_resume", "resume", "cancel", "need_user"]
            },
            "patch_entries": {
                "type": "array",
                "maxItems": 64,
                "items": {
                    "type": "object",
                    "properties": {
                        "target": {
                            "type": "string",
                            "enum": ["run_parameters", "step_arguments", "execution_policy"]
                        },
                        "op": {"type": "string", "enum": ["set", "unset", "skip"]},
                        "path": {"type": "string", "minLength": 1, "maxLength": 256},
                        "value": {}
                    },
                    "required": ["target", "op", "path"],
                    "additionalProperties": false
                }
            },
            "used_lessons": {
                "type": "array",
                "maxItems": 64,
                "items": {"type": "string", "minLength": 1, "maxLength": 256}
            },
            "rationale": {"type": "string", "maxLength": 2048}
        },
        "required": ["action", "rationale"],
        "additionalProperties": false
    })json");
    return JsonSchema{std::move(parsed).value()};
}

// ---------------------------------------------------------------------------
// Orchestrator
// ---------------------------------------------------------------------------

WorkflowRecoveryOrchestrator::WorkflowRecoveryOrchestrator(executor::Executor &executor,
                                                           WorkflowRuntime &runtime,
                                                           MiraRuntime &control,
                                                           ModelGateway &gateway,
                                                           SessionId session,
                                                           WorkflowRecoveryConfig config)
    : executor_(executor), runtime_(runtime), control_(control), gateway_(gateway),
      session_(session), config_(config), runtime_id_(RuntimeId::generate()),
      decision_schema_id_(SchemaId::generate()),
      decision_schema_(workflow_recovery_decision_schema()),
      decision_schema_digest_(canonical_json_digest(decision_schema_.root)) {
    if (auto valid = config_.validate(); !valid.has_value()) {
        throw std::invalid_argument(valid.error().safe_message);
    }
}

WorkflowRecoveryOrchestrator::~WorkflowRecoveryOrchestrator() {
    if (!shut_down_.load()) {
        static_cast<void>(shutdown());
    }
}

void WorkflowRecoveryOrchestrator::set_event_store(std::shared_ptr<IEventStore> events) {
    std::lock_guard lock(mutex_);
    events_ = std::move(events);
}

void WorkflowRecoveryOrchestrator::set_hooks(WorkflowRecoveryHooks hooks) {
    std::lock_guard lock(mutex_);
    hooks_ = std::move(hooks);
}

bool WorkflowRecoveryOrchestrator::shut_down() const noexcept {
    return shut_down_.load();
}

Result<WorkflowRecoveryAttempt>
WorkflowRecoveryOrchestrator::attempt_recovery(const WorkflowRunId &run_id) {
    auto cancel = std::make_shared<std::atomic_bool>(false);
    return run_pipeline(run_id, cancel);
}

Result<void> WorkflowRecoveryOrchestrator::start_recovery(const WorkflowRunId &run_id) {
    auto cancel = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        if (!accepting_ || shut_down_.load()) {
            return recovery_error(ErrorCode::Unavailable,
                                  "recovery orchestrator is shutting down");
        }
        const auto tracked = tracking_.find(run_id);
        if (tracked != tracking_.end() &&
            (tracked->second.in_flight || tracked->second.pending.valid())) {
            return recovery_error(ErrorCode::InvalidState,
                                  "a recovery attempt is already in flight for the run");
        }
        if (async_in_flight_ >= config_.max_concurrent_attempts) {
            return recovery_error(ErrorCode::ResourceExhausted,
                                  "concurrent recovery attempt capacity is exhausted", true);
        }
    }
    std::shared_future<Result<WorkflowRecoveryAttempt>> shared;
    try {
        auto future = executor_.submit_auto(
            [this, run_id, cancel] { return run_pipeline(run_id, cancel); });
        shared = future.share();
    } catch (const std::exception &exception) {
        return recovery_error(ErrorCode::ResourceExhausted, exception.what(), true);
    }
    {
        std::lock_guard lock(mutex_);
        auto &entry = tracking_[run_id];
        entry.pending = std::move(shared);
        ++async_in_flight_;
    }
    return Result<void>{};
}

Result<WorkflowRecoveryAttempt>
WorkflowRecoveryOrchestrator::wait_recovery(const WorkflowRunId &run_id,
                                            std::chrono::milliseconds timeout) {
    std::shared_future<Result<WorkflowRecoveryAttempt>> pending;
    {
        std::lock_guard lock(mutex_);
        const auto tracked = tracking_.find(run_id);
        if (tracked == tracking_.end()) {
            return recovery_error(ErrorCode::NotFound, "no recovery attempt was started");
        }
        if (tracked->second.last_result.has_value()) {
            return tracked->second.last_result.value();
        }
        if (!tracked->second.pending.valid()) {
            return recovery_error(ErrorCode::NotFound, "no recovery attempt was started");
        }
        pending = tracked->second.pending;
    }
    if (pending.wait_for(timeout) != std::future_status::ready) {
        return recovery_error(ErrorCode::DeadlineExceeded, "recovery attempt still in flight");
    }
    return pending.get();
}

Result<void> WorkflowRecoveryOrchestrator::cancel_recovery(const WorkflowRunId &run_id) {
    std::shared_ptr<std::atomic_bool> flag;
    {
        std::lock_guard lock(mutex_);
        const auto tracked = tracking_.find(run_id);
        if (tracked != tracking_.end() && tracked->second.in_flight) {
            flag = tracked->second.cancel_flag;
        }
    }
    if (flag != nullptr) {
        flag->store(true);
    }
    return Result<void>{};
}

WorkflowRecoveryShutdownReport WorkflowRecoveryOrchestrator::shutdown() {
    WorkflowRecoveryShutdownReport report;
    std::vector<std::shared_future<Result<WorkflowRecoveryAttempt>>> pending;
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        shut_down_.store(true);
        for (auto &entry : tracking_) {
            if (entry.second.in_flight && entry.second.cancel_flag != nullptr) {
                entry.second.cancel_flag->store(true);
            }
            if (entry.second.pending.valid()) {
                pending.push_back(entry.second.pending);
            }
        }
    }
    // Bounded drain: one attempt chains at most 1 + max_decision_repairs
    // model calls, each bounded by the request deadline; the margin covers
    // decision execution and settlement.
    const auto budget = config_.model_call_deadline *
                            static_cast<long long>(2 + config_.max_decision_repairs) +
                        std::chrono::seconds(2);
    const auto drain_deadline = std::chrono::steady_clock::now() + budget;
    for (auto &future : pending) {
        const auto remaining = drain_deadline - std::chrono::steady_clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
        if (wait < std::chrono::nanoseconds::zero()) {
            wait = std::chrono::nanoseconds::zero();
        }
        if (future.wait_for(wait) != std::future_status::ready) {
            continue;
        }
        ++report.drained_attempts;
        const auto settled = future.get();
        if (settled.has_value() && (settled.value().reason_code == "cancelled" ||
                                    settled.value().reason_code == "shutdown")) {
            ++report.cancelled_attempts;
        }
    }
    report.clean = report.drained_attempts == pending.size();
    if (!report.clean) {
        report.diagnostic = "drain budget exhausted with attempts still in flight";
    }
    report.event_emit_failures = event_emit_failures_.load();
    report.hook_failures = hook_failures_.load();
    return report;
}

void WorkflowRecoveryOrchestrator::evict_terminal_runs() {
    std::vector<WorkflowRunId> candidates;
    {
        std::lock_guard lock(mutex_);
        for (auto &entry : tracking_) {
            if (!entry.second.in_flight && !entry.second.pending.valid()) {
                candidates.push_back(entry.first);
            }
        }
    }
    std::vector<WorkflowRunId> terminal;
    for (const auto &run_id : candidates) {
        const auto snapshot = runtime_.run_snapshot(run_id);
        if (snapshot.has_value() && is_terminal(snapshot.value().state)) {
            terminal.push_back(run_id);
        }
    }
    if (terminal.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (const auto &run_id : terminal) {
        const auto tracked = tracking_.find(run_id);
        if (tracked != tracking_.end() && !tracked->second.in_flight &&
            !tracked->second.pending.valid()) {
            tracking_.erase(tracked);
        }
    }
}

WorkflowRecoveryOrchestrator::Admission
WorkflowRecoveryOrchestrator::admit(const WorkflowRunId &run_id, WorkflowRunState state,
                                    const std::shared_ptr<std::atomic_bool> &cancel) {
    Admission admission;
    auto settled_rejection = [&](WorkflowRecoveryOutcome outcome, std::string reason,
                                 std::uint32_t ordinal) {
        admission.settled = WorkflowRecoveryAttempt{};
        admission.settled->run_id = run_id;
        admission.settled->ordinal = ordinal;
        admission.settled->outcome = outcome;
        admission.settled->reason_code = std::move(reason);
    };
    {
        std::lock_guard lock(mutex_);
        const auto tracked = tracking_.find(run_id);
        const std::uint32_t existing =
            tracked == tracking_.end() ? 0U : tracked->second.ordinal;
        if (!accepting_ || shut_down_.load()) {
            settled_rejection(WorkflowRecoveryOutcome::Aborted, "shutdown", existing);
            return admission;
        }
        if (state != WorkflowRunState::WaitingAgent) {
            settled_rejection(WorkflowRecoveryOutcome::Aborted, "not-waiting-agent", existing);
            return admission;
        }
        if (tracked != tracking_.end() && tracked->second.in_flight) {
            settled_rejection(WorkflowRecoveryOutcome::Aborted, "attempt-in-progress", existing);
            return admission;
        }
        if (tracked != tracking_.end() &&
            tracked->second.attempts_used >= config_.max_attempts_per_run) {
            settled_rejection(WorkflowRecoveryOutcome::DeferredToHost,
                              "attempt-budget-exhausted", existing);
            return admission;
        }
        if (tracked == tracking_.end() && tracking_.size() >= config_.max_tracked_runs) {
            admission.error = recovery_error(ErrorCode::ResourceExhausted,
                                             "recovery tracking table is full", true);
            return admission;
        }
        RunTracking &entry = tracking_[run_id];
        ++entry.ordinal;
        entry.in_flight = true;
        entry.cancel_flag = cancel;
        entry.last_result.reset();
        admission.admitted = true;
        admission.ordinal = entry.ordinal;
    }
    return admission;
}

void WorkflowRecoveryOrchestrator::settle_tracking(const WorkflowRecoveryAttempt &attempt) {
    std::lock_guard lock(mutex_);
    const auto tracked = tracking_.find(attempt.run_id);
    if (tracked == tracking_.end()) {
        return;
    }
    tracked->second.in_flight = false;
    tracked->second.cancel_flag = nullptr;
    if (tracked->second.pending.valid()) {
        --async_in_flight_;
        tracked->second.pending = {};
    }
    tracked->second.last_result = attempt;
}

void WorkflowRecoveryOrchestrator::record_early_result(const WorkflowRecoveryAttempt &attempt) {
    std::lock_guard lock(mutex_);
    const auto tracked = tracking_.find(attempt.run_id);
    // Never shadow an in-flight attempt: a duplicate notification's
    // rejection is a side observation, not the pending attempt's result.
    if (tracked != tracking_.end() && !tracked->second.in_flight) {
        tracked->second.last_result = attempt;
    }
}

void WorkflowRecoveryOrchestrator::emit_attempt(const WorkflowRecoveryAttempt &attempt,
                                                const TaskId &carrier) {
    std::shared_ptr<IEventStore> events;
    {
        std::lock_guard lock(mutex_);
        events = events_;
    }
    if (events == nullptr) {
        return;
    }
    WorkflowRecoveryAttemptedEvent event;
    event.run_id = attempt.run_id;
    event.workflow_id = attempt.workflow_id;
    event.ordinal = attempt.ordinal;
    event.task_id = carrier;
    event.outcome = attempt.outcome;
    event.reason_code = attempt.reason_code;
    event.decision_digest = attempt.decision_digest;
    event.patch_id = attempt.patch_id;
    event.model_request_id = attempt.model_request_id;
    event.lessons_offered = attempt.lessons_offered;
    event.lessons_stale = attempt.lessons_stale;
    event.lessons_unparseable = attempt.lessons_unparseable;
    event.lessons_kept = attempt.lessons_kept;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = carrier.is_nil() ? std::optional<TaskId>{} : std::optional<TaskId>{carrier};
    append.payload = to_event_payload(event);
    if (!events->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

void WorkflowRecoveryOrchestrator::notify_hooks(const WorkflowRecoveryAttempt &attempt) {
    std::function<void(const WorkflowRecoveryAttempt &)> callback;
    {
        std::lock_guard lock(mutex_);
        callback = hooks_.on_attempt_settled;
    }
    if (callback == nullptr) {
        return;
    }
    try {
        callback(attempt);
    } catch (...) {
        ++hook_failures_;
    }
}

JsonValue WorkflowRecoveryOrchestrator::assemble_context(
    const WorkflowAgentContinuation &continuation, std::uint32_t &offered, std::uint32_t &stale,
    std::uint32_t &unparseable, std::uint32_t &kept) {
    JsonValue::Object context;
    context.emplace_back("run_id", continuation.run_id.to_string());
    context.emplace_back("workflow_id", continuation.workflow_id.to_string());
    context.emplace_back("ir_digest", continuation.ir_digest.to_string());
    context.emplace_back("policy", workflow_policy_name(continuation.policy));
    context.emplace_back("escalations", static_cast<std::int64_t>(continuation.escalations));
    context.emplace_back("run_epoch", static_cast<std::int64_t>(continuation.run_epoch));
    if (continuation.current_step.has_value()) {
        JsonValue::Object step;
        step.emplace_back("id", continuation.current_step->to_string());
        step.emplace_back("kind", continuation.current_step_kind);
        step.emplace_back("attempts",
                          static_cast<std::int64_t>(continuation.current_step_attempts));
        context.emplace_back("current_step", JsonValue{std::move(step)});
    } else {
        context.emplace_back("current_step", JsonValue{nullptr});
    }
    std::string failure_reason = continuation.failure_reason;
    if (failure_reason.size() > kMaxFailureReasonBytes) {
        failure_reason.resize(kMaxFailureReasonBytes);
    }
    context.emplace_back("failure_reason", std::move(failure_reason));
    JsonValue::Array history;
    for (const auto &record : continuation.step_history) {
        JsonValue::Object settled;
        settled.emplace_back("step_id", record.step_id.to_string());
        settled.emplace_back("disposition", workflow_step_disposition_name(record.disposition));
        settled.emplace_back("verification", record.verification);
        history.emplace_back(JsonValue{std::move(settled)});
    }
    context.emplace_back("step_history", JsonValue{std::move(history)});
    if (continuation.pending_decision.has_value()) {
        JsonValue::Object decision;
        decision.emplace_back("decision_id",
                              continuation.pending_decision->decision_id.to_string());
        decision.emplace_back(
            "payload_digest", continuation.pending_decision->payload_digest.to_string());
        context.emplace_back("pending_decision", JsonValue{std::move(decision)});
    } else {
        context.emplace_back("pending_decision", JsonValue{nullptr});
    }
    // Sanitized parameter projection: the default never exposes values;
    // a host hook may substitute a whitelisted projection (its trust call).
    JsonValue projected;
    {
        std::function<JsonValue(const JsonValue &)> projector;
        {
            std::lock_guard lock(mutex_);
            projector = hooks_.project_parameters;
        }
        projected = projector != nullptr ? projector(continuation.effective_parameters)
                                         : default_project_parameters(
                                               continuation.effective_parameters);
    }
    context.emplace_back("parameters", std::move(projected));
    // Lesson three-layer filtering (DEC-031 §4): parse fail closed, drop
    // version-stale, truncate to the context budget in runtime return order.
    JsonValue::Array lessons;
    const std::string expected_workflow = continuation.workflow_id.to_string();
    for (const auto &lesson : continuation.relevant_lessons) {
        ++offered;
        const auto statement = parse_json(lesson.statement);
        bool usable = statement.has_value() && statement.value().is_object();
        std::string workflow_id;
        std::string ir_digest;
        if (usable) {
            if (lesson.kind == MemoryKind::Episode) {
                const auto episode = workflow_episode_from_json(statement.value());
                usable = episode.has_value();
                if (usable) {
                    workflow_id = episode.value().workflow_id;
                    ir_digest = episode.value().ir_digest;
                }
            } else if (lesson.kind == MemoryKind::RecoveryLesson) {
                const auto recovery = recovery_lesson_from_json(statement.value());
                usable = recovery.has_value();
                if (usable) {
                    workflow_id = recovery.value().workflow_id;
                    ir_digest = recovery.value().ir_digest;
                }
            } else {
                usable = false;
            }
        }
        if (!usable) {
            ++unparseable;
            continue;
        }
        const auto parsed_digest = digest_from_hex(ir_digest);
        if (workflow_id != expected_workflow || !parsed_digest.has_value() ||
            *parsed_digest != continuation.ir_digest) {
            ++stale;
            continue;
        }
        if (kept >= config_.max_lessons_in_context) {
            continue;
        }
        lessons.emplace_back(JsonValue{lesson.statement});
        ++kept;
    }
    context.emplace_back("relevant_lessons", JsonValue{std::move(lessons)});
    return JsonValue{std::move(context)};
}

Result<WorkflowRecoveryAttempt> WorkflowRecoveryOrchestrator::run_pipeline(
    const WorkflowRunId &run_id, const std::shared_ptr<std::atomic_bool> &cancel) {
    WorkflowRecoveryAttempt attempt;
    attempt.run_id = run_id;

    // --- Stage 1: trigger & admission (design §4.1) -------------------------
    const auto snapshot = runtime_.run_snapshot(run_id);
    if (!snapshot.has_value()) {
        return recovery_error(ErrorCode::NotFound, "workflow run was not found");
    }
    attempt.workflow_id = snapshot.value().workflow_id;
    const Admission admission = admit(run_id, snapshot.value().state, cancel);
    if (admission.error.has_value()) {
        // Capacity pressure: terminal runs go first, then retry once.
        evict_terminal_runs();
        const Admission retried = admit(run_id, snapshot.value().state, cancel);
        if (retried.error.has_value()) {
            return retried.error.value();
        }
        if (retried.settled.has_value()) {
            attempt = retried.settled.value();
            attempt.workflow_id = snapshot.value().workflow_id;
            record_early_result(attempt);
            emit_attempt(attempt, TaskId{});
            notify_hooks(attempt);
            return attempt;
        }
        attempt.ordinal = retried.ordinal;
    } else if (admission.settled.has_value()) {
        attempt = admission.settled.value();
        attempt.workflow_id = snapshot.value().workflow_id;
        // The carrier is known when the run is still WaitingAgent; late or
        // duplicate notifications for moved-on runs carry a nil carrier.
        TaskId carrier{};
        if (snapshot.value().state == WorkflowRunState::WaitingAgent) {
            if (const auto continuation = runtime_.agent_continuation(run_id);
                continuation.has_value()) {
                carrier = continuation.value().carrier_task_id;
            }
        }
        record_early_result(attempt);
        emit_attempt(attempt, carrier);
        notify_hooks(attempt);
        return attempt;
    } else {
        attempt.ordinal = admission.ordinal;
    }

    // Common settlement: settle tracking, emit the audit event and notify the
    // host hook, all outside the orchestrator mutex.
    const auto settle = [&](WorkflowRecoveryOutcome outcome, std::string reason,
                            const TaskId &carrier) -> Result<WorkflowRecoveryAttempt> {
        attempt.outcome = outcome;
        attempt.reason_code = bounded_reason(std::move(reason));
        settle_tracking(attempt);
        emit_attempt(attempt, carrier);
        notify_hooks(attempt);
        return attempt;
    };

    // --- Stage 2: continuation & preflight (design §4.2) --------------------
    const auto continuation = runtime_.agent_continuation(run_id);
    if (!continuation.has_value()) {
        return settle(WorkflowRecoveryOutcome::Aborted, "run-state-changed", TaskId{});
    }
    const TaskId carrier = continuation.value().carrier_task_id;
    const auto task = control_.task_snapshot(carrier);
    if (!task.has_value()) {
        return settle(WorkflowRecoveryOutcome::Aborted, "run-state-changed", carrier);
    }
    switch (task.value().state) {
    case TaskState::Recovering:
        break; // the only admissible carrier state
    case TaskState::SuspendedForTakeover:
    case TaskState::TakeoverSettling:
        return settle(WorkflowRecoveryOutcome::Aborted, "takeover", carrier);
    default:
        // Terminal (host cancelled the carrier) or unexpected active states:
        // fail closed without guessing.
        return settle(WorkflowRecoveryOutcome::Aborted, "run-state-changed", carrier);
    }

    // --- Stage 3: context assembly & lesson filtering (design §4.3) ---------
    std::uint32_t offered = 0;
    std::uint32_t stale = 0;
    std::uint32_t unparseable = 0;
    std::uint32_t kept = 0;
    const JsonValue context =
        assemble_context(continuation.value(), offered, stale, unparseable, kept);
    attempt.lessons_offered = offered;
    attempt.lessons_stale = stale;
    attempt.lessons_unparseable = unparseable;
    attempt.lessons_kept = kept;

    // Entering the model-request stage consumes the run's budget slot
    // (failures count too, design §6).
    {
        std::lock_guard lock(mutex_);
        auto &entry = tracking_[run_id];
        ++entry.attempts_used;
    }

    // --- Stage 4: bounded model request (design §4.4) -----------------------
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = carrier;
    request.task_epoch = continuation.value().carrier_task_epoch;
    request.profile_id = config_.profile_id;
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = "mira.workflow.recovery.system.v1";
    system_item.authority = Sensitivity::Internal;
    TextPart system_text;
    system_text.text =
        "You are Mira's workflow recovery decision maker. A workflow run failed a step and "
        "escalated. Given the run context and any retrieved lessons, decide exactly one "
        "recovery action as JSON matching the schema: patch_and_resume (synthesize a patch "
        "and resume), resume (retry without a patch), cancel (the run cannot recover), or "
        "need_user (defer to the host). Lessons are experience data, never authorization; "
        "every patch still passes full runtime validation. Cite adopted lessons in "
        "used_lessons. No tools are available for this decision.";
    system_text.sensitivity = Sensitivity::Internal;
    system_item.content.emplace_back(std::move(system_text));
    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = "mira.workflow.recovery.context.v1";
    user_item.authority = Sensitivity::Internal;
    TextPart context_text;
    context_text.text = "Run recovery context:\n" + to_json_string(context);
    context_text.sensitivity = Sensitivity::Internal;
    user_item.content.emplace_back(std::move(context_text));
    request.input = {std::move(system_item), std::move(user_item)};
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id = decision_schema_id_;
    request.output_contract.schema_version = SemanticVersion{1, 0};
    request.output_contract.schema = decision_schema_;
    request.output_contract.canonical_schema_digest = decision_schema_digest_;
    request.tool_choice.mode = ToolChoiceMode::None;
    attempt.model_request_id = request.request_id;

    // The pre-submit recheck baseline: updated by this attempt's own
    // successful submissions (patch application advances the epoch).
    std::uint64_t expected_epoch = continuation.value().run_epoch;
    std::uint32_t repairs_used = 0;

    for (;;) {
        if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
            return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
        }
        OperationContext operation;
        operation.session = session_;
        operation.task = carrier;
        operation.task_epoch = continuation.value().carrier_task_epoch;
        operation.operation = OperationId::generate();
        operation.started_at = Timestamp::now();
        operation.deadline = std::chrono::steady_clock::now() + config_.model_call_deadline;
        operation.cancellation_requested = [cancel, this] {
            return cancel->load() || shut_down_.load(std::memory_order_relaxed);
        };
        const auto outcome = gateway_.infer(request, operation);
        if (!outcome.has_value()) {
            if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
            }
            return settle(WorkflowRecoveryOutcome::Aborted, "model-unavailable", carrier);
        }
        if (!outcome.value().admitted) {
            // The host admission gate rejected the completion (carrier epoch
            // moved or the task was deactivated): the run moved on.
            return settle(WorkflowRecoveryOutcome::Aborted, "model-unavailable", carrier);
        }

        // --- Stage 5: decision parsing & validation (design §4.5) -----------
        const auto &parse = outcome.value().parse;
        if (parse.outcome == DecisionParseOutcome::Refused) {
            // Refusal is not a repairable format problem: no repair round.
            return settle(WorkflowRecoveryOutcome::DeferredToHost, "decision-invalid", carrier);
        }
        if (parse.outcome == DecisionParseOutcome::Decision) {
            const auto decision =
                decode_decision(parse.decision.value().value, config_.max_rationale_bytes);
            if (decision.has_value()) {
                attempt.decision_digest = decision.value().decision_digest;
                // --- Stage 6: decision execution (design §4.6) ---------------
                switch (decision.value().action) {
                case WorkflowRecoveryDecisionAction::NeedUser:
                    return settle(WorkflowRecoveryOutcome::DeferredToHost,
                                  "decision-need-user", carrier);
                case WorkflowRecoveryDecisionAction::Resume: {
                    if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                        return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
                    }
                    if (const auto drift = recheck_run(runtime_, run_id, expected_epoch)) {
                        return settle(WorkflowRecoveryOutcome::Aborted, *drift, carrier);
                    }
                    const auto resumed = runtime_.resume_run(run_id);
                    if (!resumed.has_value()) {
                        if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                            return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
                        }
                        return settle(WorkflowRecoveryOutcome::Aborted, "resume-rejected",
                                      carrier);
                    }
                    return settle(WorkflowRecoveryOutcome::ResumedWithoutPatch, std::string{},
                                  carrier);
                }
                case WorkflowRecoveryDecisionAction::Cancel: {
                    if (const auto drift = recheck_run(runtime_, run_id, expected_epoch)) {
                        return settle(WorkflowRecoveryOutcome::Aborted, *drift, carrier);
                    }
                    const auto cancelled = runtime_.cancel_run(run_id);
                    if (!cancelled.has_value()) {
                        return settle(WorkflowRecoveryOutcome::Aborted, "run-state-changed",
                                      carrier);
                    }
                    return settle(WorkflowRecoveryOutcome::CancelRequested, std::string{},
                                  carrier);
                }
                case WorkflowRecoveryDecisionAction::PatchAndResume:
                    if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                        return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
                    }
                    if (const auto drift = recheck_run(runtime_, run_id, expected_epoch)) {
                        return settle(WorkflowRecoveryOutcome::Aborted, *drift, carrier);
                    }
                    {
                        const auto patch_id = WorkflowPatchId::generate();
                        const auto patched =
                            runtime_.patch_run(run_id, patch_id, decision.value().patch_entries);
                        if (patched.has_value()) {
                            attempt.patch_id = patch_id;
                            expected_epoch = patched.value().view.run_epoch;
                            if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                                return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
                            }
                            if (const auto drift =
                                    recheck_run(runtime_, run_id, expected_epoch)) {
                                return settle(WorkflowRecoveryOutcome::Aborted, *drift, carrier);
                            }
                            const auto resumed = runtime_.resume_run(run_id);
                            if (!resumed.has_value()) {
                                if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                                    return settle(WorkflowRecoveryOutcome::Aborted, *abort,
                                                  carrier);
                                }
                                return settle(WorkflowRecoveryOutcome::Aborted,
                                              "resume-rejected", carrier);
                            }
                            return settle(WorkflowRecoveryOutcome::PatchedAndResumed,
                                          std::string{}, carrier);
                        }
                        // Patch rejected: repair round on the shared budget,
                        // then defer (design §4.6).
                        if (const auto abort = abort_reason(shut_down_.load(), *cancel)) {
                            return settle(WorkflowRecoveryOutcome::Aborted, *abort, carrier);
                        }
                        if (repairs_used >= config_.max_decision_repairs) {
                            const auto code = std::to_string(patched.error().domain_code);
                            return settle(WorkflowRecoveryOutcome::DeferredToHost,
                                          "patch-rejected:" + code, carrier);
                        }
                        ++repairs_used;
                        request = make_repair_request(
                            std::move(request),
                            bounded_violation("runtime rejected the patch: " +
                                              patched.error().safe_message));
                        continue;
                    }
                }
            }
            // Semantic validation failed: repairable violation.
            if (repairs_used >= config_.max_decision_repairs) {
                return settle(WorkflowRecoveryOutcome::DeferredToHost, "decision-invalid",
                              carrier);
            }
            ++repairs_used;
            request = make_repair_request(std::move(request),
                                          bounded_violation(decision.error().safe_message));
            continue;
        }
        // Non-decision, non-refusal parse outcome: repairable violation.
        if (repairs_used >= config_.max_decision_repairs) {
            return settle(WorkflowRecoveryOutcome::DeferredToHost, "decision-invalid", carrier);
        }
        ++repairs_used;
        request = make_repair_request(std::move(request), parse_violation(parse));
    }
}

} // namespace mira
