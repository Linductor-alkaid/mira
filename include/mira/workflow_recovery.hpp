#pragma once

#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_gateway.hpp>
#include <mira/runtime.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_run.hpp>
#include <mira/workflow_runtime.hpp>
#include <mira/workflow_tools.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// ---------------------------------------------------------------------------
// Workflow recovery orchestration (DEC-031, M14): the agent-side bounded
// recovery loop for WaitingAgent runs. This component owns "one escalation ->
// one bounded model request -> one structured decision -> one submission
// through the existing runtime exits -> one audit event". It never verifies
// the repair (the runtime's step verification does), never writes memory
// (record_recovery_lesson stays host-only, DEC-030 §4) and never dispatches
// environment actions.
// ---------------------------------------------------------------------------

// Fail-closed configuration (design §5.1). Zero/negative values, an empty
// model profile or a zero concurrent-attempt capacity are rejected by
// validate(), which the constructor calls.
struct WorkflowRecoveryConfig final {
    // Per-run recovery attempt ceiling (RULE-08); exhausting it defers to the
    // host instead of settling the run.
    std::uint32_t max_attempts_per_run = 8;
    // Deadline handed to every recovery model request through the
    // OperationContext; also bounds the shutdown drain budget.
    std::chrono::milliseconds model_call_deadline{30'000};
    // Repair rounds per attempt; decision-parse failures and patch
    // rejections share this budget (DEC-031 §5).
    std::uint32_t max_decision_repairs = 1;
    // Lessons kept in the model context after filtering (runtime return
    // order preserved; aligned with DEC-030 retrieval bounds).
    std::size_t max_lessons_in_context = 8;
    // Bound on the decision rationale accepted from the model; the rationale
    // never enters events regardless.
    std::size_t max_rationale_bytes = 2'048;
    // Capacity for concurrent asynchronous attempts (start_recovery); each
    // occupies one submit_auto task until settled.
    std::size_t max_concurrent_attempts = 1;
    // Tracking-table ceiling (aligned with WorkflowRuntimeConfig
    // max_active_runs); terminal runs are evicted first.
    std::size_t max_tracked_runs = 32;
    // Profile routing the recovery decision request; required.
    ModelProfileId profile_id;

    [[nodiscard]] Result<void> validate() const;
};

// The settled result of one recovery attempt (design §5.2). The shared
// invariant of DeferredToHost and Aborted: the run is left in WaitingAgent —
// the orchestrator changed nothing.
struct WorkflowRecoveryAttempt final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    // Orchestrator-side per-run attempt ordinal (1-based, assigned to every
    // audited attempt; admission rejections do not consume the attempt
    // budget but still carry an ordinal for correlation).
    std::uint32_t ordinal = 0;
    WorkflowRecoveryOutcome outcome = WorkflowRecoveryOutcome::Aborted;
    // Closed-set reason code ("not-waiting-agent", "attempt-in-progress",
    // "attempt-budget-exhausted", "takeover", "run-state-changed",
    // "epoch-advanced", "cancelled", "shutdown", "model-unavailable",
    // "decision-invalid", "decision-need-user", "patch-rejected:<code>",
    // "resume-rejected"); empty when the outcome itself carries the cause.
    std::string reason_code;
    // Canonical digest of the accepted model decision, when one was parsed.
    std::optional<Sha256Digest> decision_digest;
    // The patch applied by a PatchedAndResumed attempt.
    std::optional<WorkflowPatchId> patch_id;
    // The first model request of this attempt (audit correlation; repair
    // requests get fresh ids from the repair builder).
    std::optional<ModelRequestId> model_request_id;
    std::uint32_t lessons_offered = 0;
    std::uint32_t lessons_stale = 0;
    std::uint32_t lessons_unparseable = 0;
    std::uint32_t lessons_kept = 0;
};

// The four-action closed set of mira.workflow.recovery-decision.v1
// (DEC-031 §3).
enum class WorkflowRecoveryDecisionAction : std::uint8_t {
    PatchAndResume,
    Resume,
    Cancel,
    NeedUser,
};

[[nodiscard]] std::string workflow_recovery_action_name(WorkflowRecoveryDecisionAction action);
[[nodiscard]] Result<WorkflowRecoveryDecisionAction>
parse_workflow_recovery_action(std::string_view name);

// One accepted model decision after schema and shape validation.
// used_lessons is an audit reference only (never authorization, RULE-09);
// rationale is bounded and never enters events.
struct WorkflowRecoveryDecision final {
    WorkflowRecoveryDecisionAction action = WorkflowRecoveryDecisionAction::NeedUser;
    std::vector<WorkflowPatchEntry> patch_entries;
    std::vector<std::string> used_lessons;
    std::string rationale;
    Sha256Digest decision_digest{};
};

// The frozen decision schema (mira.workflow.recovery-decision.v1, draft
// subset of the JSON Schema gate; shape per DEC-031 §3).
[[nodiscard]] JsonSchema workflow_recovery_decision_schema();

// Host-installed hooks (design §5.3). Both run without the orchestrator
// mutex; they must be cheap and non-blocking. Exceptions from
// on_attempt_settled are isolated into a diagnostic counter.
struct WorkflowRecoveryHooks final {
    // Projects the run's effective parameters into the model context. The
    // default projection exposes names, value types and a SHA-256 content
    // digest (first 16 hex) — never values (DEC-031 §4.3 sanitization). A
    // host may return a whitelisted projection instead; returning values is
    // the host's explicit trust decision.
    std::function<JsonValue(const JsonValue &parameters)> project_parameters;
    // Settled-attempt observer, called after the audit event emission.
    std::function<void(const WorkflowRecoveryAttempt &)> on_attempt_settled;
};

// Bounded shutdown report (design §7).
struct WorkflowRecoveryShutdownReport final {
    // True when no attempt was in flight when shutdown began and every
    // in-flight future drained within the budget.
    bool clean = false;
    std::size_t cancelled_attempts = 0;
    std::size_t drained_attempts = 0;
    std::uint32_t event_emit_failures = 0;
    std::uint32_t hook_failures = 0;
    std::string diagnostic;
};

// The recovery orchestrator (design §5.3). One instance serves one session's
// WorkflowRuntime; the host assembles it with the same executor, control
// plane and model gateway the runtime uses.
//
// Concurrency: the internal mutex is a leaf. Runtime, gateway, control-plane
// and event-store calls happen outside it. Each attempt carries a
// cooperative cancel flag (cancel_recovery / shutdown set it); model calls
// unblock through the OperationContext cancellation probe and the request
// deadline. Late model responses are discarded by the pre-submit epoch and
// state recheck (RULE-03): a decision is never applied to a run that moved
// on.
//
// Shutdown order (AGENTS.md #6): this orchestrator produces runtime work
// (patch/resume/cancel submissions), so shutdown() must complete before
// WorkflowRuntime::shutdown(); the executor itself stays owned by the host.
class WorkflowRecoveryOrchestrator final {
  public:
    WorkflowRecoveryOrchestrator(executor::Executor &executor, WorkflowRuntime &runtime,
                                 MiraRuntime &control, ModelGateway &gateway, SessionId session,
                                 WorkflowRecoveryConfig config);
    ~WorkflowRecoveryOrchestrator();
    WorkflowRecoveryOrchestrator(const WorkflowRecoveryOrchestrator &) = delete;
    WorkflowRecoveryOrchestrator &operator=(const WorkflowRecoveryOrchestrator &) = delete;

    // Audit sink for WorkflowRecoveryAttempted events; without one, attempts
    // still run and emission failures would be silent, so hosts that need the
    // audit trail install the same event store as the runtime.
    void set_event_store(std::shared_ptr<IEventStore> events);
    void set_hooks(WorkflowRecoveryHooks hooks);

    // Synchronous entry point: runs the whole seven-stage pipeline on the
    // calling thread (deterministic tests, synchronous hosts). Admission
    // failures settle as Aborted attempts (audited), not transport errors;
    // capacity rejections return ResourceExhausted errors.
    [[nodiscard]] Result<WorkflowRecoveryAttempt>
    attempt_recovery(const WorkflowRunId &run_id);

    // Asynchronous entry point: submits one bounded task per attempt and
    // returns after submission. Rejected while shutting down, when an
    // attempt is already in flight for the run, or when the concurrent /
    // tracking capacity is exceeded (ResourceExhausted).
    [[nodiscard]] Result<void> start_recovery(const WorkflowRunId &run_id);

    // Bounded wait for the result of the most recent attempt of one run.
    // Returns the settled attempt (immediately when already settled),
    // DeadlineExceeded on timeout, NotFound when no attempt was started.
    [[nodiscard]] Result<WorkflowRecoveryAttempt>
    wait_recovery(const WorkflowRunId &run_id, std::chrono::milliseconds timeout);

    // Cooperative cancel of the in-flight attempt of one run; the attempt
    // settles Aborted("cancelled") at its next checkpoint. Idempotent and
    // safe without an in-flight attempt.
    [[nodiscard]] Result<void> cancel_recovery(const WorkflowRunId &run_id);

    // Stops accepting notifications, sets every in-flight cancel flag and
    // drains the asynchronous attempt futures within a bounded budget.
    // Call before WorkflowRuntime::shutdown() (producer-first order).
    [[nodiscard]] WorkflowRecoveryShutdownReport shutdown();

    [[nodiscard]] bool shut_down() const noexcept;

  private:
    struct RunTracking final {
        std::uint32_t ordinal = 0;        // last assigned audit ordinal
        std::uint32_t attempts_used = 0;  // budget: attempts that reached the
                                          // model-request stage (failures too)
        bool in_flight = false;
        std::optional<WorkflowRecoveryAttempt> last_result;
        // Cancel flag of the in-flight attempt (sync or async).
        std::shared_ptr<std::atomic_bool> cancel_flag;
        // Pending asynchronous attempt (absent for synchronous attempts).
        std::shared_future<Result<WorkflowRecoveryAttempt>> pending;
    };

    // Stage 1. Returns the assigned ordinal on success (the tracking entry
    // is now in-flight with the cancel flag installed), a fully settled
    // audited rejection (duplicate / late notification, exhausted budget,
    // shutdown), or a transport error (capacity).
    struct Admission final {
        bool admitted = false;
        std::uint32_t ordinal = 0;
        std::optional<Error> error;
        std::optional<WorkflowRecoveryAttempt> settled;
    };
    [[nodiscard]] Admission admit(const WorkflowRunId &run_id, WorkflowRunState state,
                                  const std::shared_ptr<std::atomic_bool> &cancel);
    // Best-effort eviction of tracking entries whose runs settled
    // terminally (design §4.1); called without the mutex.
    void evict_terminal_runs();

    [[nodiscard]] Result<WorkflowRecoveryAttempt>
    run_pipeline(const WorkflowRunId &run_id, const std::shared_ptr<std::atomic_bool> &cancel);
    void settle_tracking(const WorkflowRecoveryAttempt &attempt);
    void record_early_result(const WorkflowRecoveryAttempt &attempt);
    // carrier is the run's carrier task when known (nil for admission
    // rejections of unknown carriers).
    void emit_attempt(const WorkflowRecoveryAttempt &attempt, const TaskId &carrier);
    void notify_hooks(const WorkflowRecoveryAttempt &attempt);
    [[nodiscard]] JsonValue assemble_context(const WorkflowAgentContinuation &continuation,
                                             std::uint32_t &offered, std::uint32_t &stale,
                                             std::uint32_t &unparseable, std::uint32_t &kept);

    executor::Executor &executor_;
    WorkflowRuntime &runtime_;
    MiraRuntime &control_;
    ModelGateway &gateway_;
    SessionId session_;
    WorkflowRecoveryConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<WorkflowRunId, RunTracking, StrongIdHash<WorkflowRunId>> tracking_;
    std::size_t async_in_flight_ = 0;
    bool accepting_ = true;
    // Atomic mirror of the shutdown decision for the cooperative cancellation
    // probes (read on model-call paths without the mutex).
    std::atomic<bool> shut_down_{false};

    std::shared_ptr<IEventStore> events_;
    WorkflowRecoveryHooks hooks_;
    RuntimeId runtime_id_;
    SchemaId decision_schema_id_;
    JsonSchema decision_schema_;
    Hash decision_schema_digest_{};

    std::atomic<std::uint32_t> event_emit_failures_{0};
    std::atomic<std::uint32_t> hook_failures_{0};
};

} // namespace mira
