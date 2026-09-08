#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/runtime.hpp>
#include <mira/tool_executor.hpp>
#include <mira/workflow_compiler.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_navigation.hpp>
#include <mira/workflow_run.hpp>
#include <mira/workflow_tools.hpp>
#include <mira/workflow_versioning.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
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
// Workflow execution runtime, stage B (workflow_runtime_design §2/§7)
// ---------------------------------------------------------------------------

struct WorkflowRuntimeConfig final {
    // Run-table capacity (RULE-08): active, non-terminal runs.
    std::size_t max_active_runs = 32;
    // Executor-managed asynchronous drives (start_run / run_workflow tool).
    // Must stay below the executor's worker count: one drive occupies one
    // worker for the run duration while it waits on step futures.
    std::size_t max_concurrent_async_drives = 1;
    // Whole-run step execution budget (RULE-08) covering attempts and loop
    // iterations; exhausting it settles the run as Failed.
    std::uint32_t max_step_executions_per_run = 1024;
    // Deadline handed to each step's OperationContext.
    std::chrono::milliseconds step_deadline{10'000};
    // Bounded wait for control-plane command outcomes and pause convergence.
    std::chrono::milliseconds command_timeout{5'000};
    // Patches queued for the next step boundary of one running run (RULE-08).
    std::size_t max_pending_patches_per_run = 16;
    // Applied patch records retained per run for idempotency and rollback.
    std::size_t max_applied_patches_per_run = 64;
    // Failure escalations (WaitingAgent / WaitingUser) per run (RULE-08,
    // DEC-023 §1); exhausting it settles the run Failed. Checkpoint handoffs
    // are bounded by the loop and step budgets instead.
    std::uint32_t max_escalations_per_run = 32;
    // Stage E navigation (DEC-028 §1): planner weights and budgets. The
    // weight defaults are provisional pending target-platform calibration
    // (RULE-10); the budgets bound one plan_navigation call.
    NavigationCostProfile nav_cost_profile;
    std::size_t nav_max_edge_evaluations = 4096;
    std::size_t nav_max_path_edges = 64;
};

// One settled step attempt as reported in WorkflowRunResult.
struct WorkflowStepRecord final {
    StepId step_id;
    std::uint32_t attempt = 1;
    WorkflowStepDisposition disposition = WorkflowStepDisposition::Completed;
    // "satisfied" | "not_satisfied" | "not_evaluable" | "none".
    std::string verification;
    std::string safe_summary;
};

// The outcome of one drive: the run state when the drive stopped plus the
// settled step history. `state` is terminal, or the boundary the drive
// stopped at (Paused, WaitingUser, WaitingAgent).
struct WorkflowRunResult final {
    WorkflowRunId run_id;
    WorkflowRunState state = WorkflowRunState::Created;
    std::uint64_t run_epoch = 0;
    std::vector<WorkflowStepRecord> steps;
    // DryRun honesty counter (RULE-10): verifications that could not be
    // evaluated; a completed DryRun with a non-zero count never claims
    // verified execution.
    std::uint32_t unevaluable_verifications = 0;
    std::string safe_summary;
};

// The reply of one patch submission (DEC-024 §2): `applied` is false only
// for the idempotent NoOp (same patch_id + digest replay); `queued` marks a
// patch accepted on a running run whose application lands at the next step
// boundary.
struct WorkflowPatchOutcome final {
    WorkflowRunView view;
    bool applied = false;
    bool queued = false;
};

// Decision-point kinds (DEC-024 §6). StepFailure decisions are raised by the
// runtime when an Interactive run exhausts its recovery chain; AgentPrompt
// decisions are raised through the request_user_input tool.
enum class WorkflowDecisionKind : std::uint8_t { StepFailure, AgentPrompt };

// Host-visible view of one pending decision point. The payload digest is
// computed over {prompt, proposal}; answers must match both decision_id and
// the digest (DEC-022 §3).
struct WorkflowDecisionRequest final {
    WorkflowDecisionId decision_id;
    Sha256Digest payload_digest{};
    WorkflowDecisionKind kind = WorkflowDecisionKind::StepFailure;
    // StepFailure: the step whose failure raised the decision.
    std::optional<StepId> step_id;
    // Applied when the decision is accepted (W-04: not an exemption channel).
    std::vector<WorkflowPatchEntry> proposal;
    std::string safe_summary;
};

// Agent continuation context for one WaitingAgent run (DEC-023 §3, design
// §7.6 minimal form). Host-side API: effective parameters appear in clear
// only at the host boundary; model-facing serialization applies the
// existing sanitization rules.
struct WorkflowAgentContinuation final {
    WorkflowRunId run_id;
    WorkflowId workflow_id;
    Sha256Digest ir_digest{};
    WorkflowPolicy policy = WorkflowPolicy::Strict;
    std::optional<StepId> current_step;
    std::string current_step_kind;
    std::uint32_t current_step_attempts = 0;
    JsonValue effective_parameters;
    std::vector<WorkflowStepRecord> step_history;
    std::string failure_reason;
    std::optional<WorkflowPendingDecision> pending_decision;
};

struct WorkflowShutdownReport final {
    bool clean = false;
    std::size_t cancelled_runs = 0;
    std::size_t drained_drives = 0;
    std::string diagnostic;
};

// The reply of one gated publish (DEC-025 §3): the content digest of the
// appended (or already-present) version, the gate drive identity and the
// content-derived evidence digest. `idempotent` marks the NoOp replay of a
// head record with the same content and evidence.
struct WorkflowPublishOutcome final {
    Sha256Digest ir_digest{};
    bool idempotent = false;
    WorkflowRunId dry_run_id;
    Sha256Digest evidence{};
};

// FNV-1a over the digest bytes; lets digests key the runtime's library maps.
struct Sha256DigestHash final {
    std::size_t operator()(const Sha256Digest &digest) const noexcept {
        std::size_t value = 1469598103934665603ULL;
        for (const auto byte : digest.bytes) {
            value ^= byte;
            value *= 1099511628211ULL;
        }
        return value;
    }
};

// Drives WorkflowDefinition executions over the existing task lifecycle.
//
// One WorkflowRuntime serves one session. Each run is carried by one task
// submitted through the MiraRuntime control plane (DEC-020 §2): task-level
// commands (pause/resume/cancel/complete) go through MiraRuntime, and the
// run view is committed only through apply_workflow_run_transition under the
// runtime's own mutex, so the frozen table, epoch and terminal-idempotency
// rules govern every transition. No control thread is created.
//
// Routing (M9 milestone §5): steps and post-side-effect verification
// observations are submitted as bounded submit_auto tasks whose futures the
// drive consumes; DryRun predicate evaluation is synchronous; asynchronous
// drives (start_run, resume_run and the run_workflow tool) occupy one
// executor worker each and are capped by max_concurrent_async_drives.
//
// Lock order: the internal mutex is a leaf. Runtime commands, environment
// calls and event-store appends happen outside it, which keeps the step
// cancellation probe (an atomic flag published by a monitor task) safe in
// every calling context.
class WorkflowRuntime final {
  public:
    WorkflowRuntime(executor::Executor &executor, MiraRuntime &runtime, SessionId session,
                    std::shared_ptr<IEnvironment> environment,
                    WorkflowRuntimeConfig config = WorkflowRuntimeConfig{});
    ~WorkflowRuntime();
    WorkflowRuntime(const WorkflowRuntime &) = delete;
    WorkflowRuntime &operator=(const WorkflowRuntime &) = delete;

    void set_event_store(std::shared_ptr<IEventStore> events);
    // Registry used to dispatch ToolCall steps. Required before creating a
    // Strict run whose definition contains ToolCall steps.
    void set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools);

    // Stage E navigation context (DEC-027 §3, DEC-028 §3). The model must
    // pass validate_app_model (fail closed here); the provider is a host
    // callback invoked synchronously on drive and caller threads, so it must
    // be cheap and non-blocking. With a context installed, Navigate steps
    // become admissible under dispatching policies; without one the M9
    // navigate-unresolvable admission rejection stays in force.
    [[nodiscard]] Result<void> set_navigation_context(AppModel model,
                                                      ScreenStateProvider provider);
    // Reinstalls the model projection (e.g. a decayed or host-edited
    // document); the screen state provider stays. Same validation rules.
    [[nodiscard]] Result<void> set_app_model(AppModel model);
    // The confidence-evolved projection for host inspection and decay
    // workflows; nullopt when no context is installed.
    [[nodiscard]] std::optional<AppModel> app_model_snapshot() const;

    // Appends one immutable version record for the definition to the runtime's
    // library and returns its content digest. Validation defaults to
    // NotValidated: such versions are resolvable but not runnable (W-04);
    // the caller supplies DryRunPassed/Validated evidence when it has any.
    [[nodiscard]] Result<Sha256Digest>
    publish_workflow(const WorkflowDefinition &definition, const std::string &actor,
                     const std::string &reason,
                     WorkflowValidationResult validation = WorkflowValidationResult::NotValidated,
                     std::optional<Sha256Digest> evidence = std::nullopt);

    // Host-direct creation (DEC-021 §4): the host is the trust boundary, so
    // the definition is accepted after full structural validation without
    // requiring a library version record.
    [[nodiscard]] Result<WorkflowRunView> create_run(const WorkflowDefinition &definition,
                                                     JsonValue parameters,
                                                     std::optional<WorkflowPolicy> policy);

    // Library-resolved creation (run_workflow tool path): pins the
    // creation-time version by digest (W-03) and requires it to be runnable
    // (W-04: DryRunPassed or Validated).
    [[nodiscard]] Result<WorkflowRunView> create_run(const WorkflowId &workflow_id,
                                                     const Sha256Digest &ir_digest,
                                                     JsonValue parameters,
                                                     std::optional<WorkflowPolicy> policy);

    // Submits one asynchronous drive (executor-managed, capacity capped) and
    // returns after submission; poll run_snapshot or use wait_run.
    [[nodiscard]] Result<void> start_run(const WorkflowRunId &run_id);

    // Drives the run on the calling thread until a terminal state or a pause
    // boundary. Each step is a bounded unit; this call blocks.
    [[nodiscard]] Result<WorkflowRunResult> execute_run(const WorkflowRunId &run_id,
                                                        const OperationContext &context);

    // Bounded wait for the result of an asynchronous drive; returns the
    // stored result for runs already settled or synchronously driven.
    [[nodiscard]] Result<WorkflowRunResult>
    wait_run(const WorkflowRunId &run_id, std::chrono::milliseconds timeout);

    // Run control. pause converges at the next step boundary; resume
    // re-observes and continues from the cursor via a new asynchronous drive
    // (Paused and WaitingAgent runs; a WaitingUser run is resolved through
    // resolve_decision instead); cancel is idempotent.
    [[nodiscard]] Result<WorkflowRunView> pause_run(const WorkflowRunId &run_id);
    [[nodiscard]] Result<WorkflowRunView> resume_run(const WorkflowRunId &run_id);
    [[nodiscard]] Result<WorkflowRunView> cancel_run(const WorkflowRunId &run_id);

    // Patch plane (DEC-024). Host-direct submission sharing the exact
    // validation, idempotency and audit pipeline with the patch_workflow
    // tool. Admission follows the policy x state matrix; a patch accepted on
    // a running run is queued for the next step boundary.
    [[nodiscard]] Result<WorkflowPatchOutcome>
    patch_run(const WorkflowRunId &run_id, const WorkflowPatchId &patch_id,
              const std::vector<WorkflowPatchEntry> &entries);

    // Explicit rollback patch (DEC-024 §2): constructs revert entries from
    // the recorded pre-application snapshot of one applied patch and submits
    // them as a fresh patch through the same pipeline. No implicit snapshot
    // restore exists.
    [[nodiscard]] Result<WorkflowPatchOutcome>
    rollback_run_patch(const WorkflowRunId &run_id, const WorkflowPatchId &patch_id);

    // Decision points (DEC-024 §5/§6). pending_decision_request returns the
    // current decision of a WaitingUser run; resolve_decision matches on
    // decision_id plus payload digest and settles accept / reject / cancel.
    [[nodiscard]] Result<WorkflowDecisionRequest>
    pending_decision_request(const WorkflowRunId &run_id) const;
    [[nodiscard]] Result<WorkflowRunView>
    resolve_decision(const WorkflowRunId &run_id, const WorkflowDecisionId &decision_id,
                     const Sha256Digest &payload_digest,
                     WorkflowDecisionResolution resolution);

    // Agent continuation context (DEC-023 §3): WaitingAgent runs only.
    [[nodiscard]] Result<WorkflowAgentContinuation>
    agent_continuation(const WorkflowRunId &run_id) const;

    // Trajectory capture (DEC-025 §1, stage D): one structured snapshot of a
    // Completed run that actually dispatched side effects. DryRun-planned
    // completions, non-terminal, failed, cancelled and unknown runs fail
    // closed; a compile-ready draft never comes from a run that did not
    // execute (RULE-10).
    [[nodiscard]] Result<WorkflowTrajectory>
    capture_trajectory(const WorkflowRunId &run_id) const;

    // The stage-D publish gate (DEC-025 §3): structural validation, a DryRun
    // drive on the calling thread (empty parameters, defaults apply) and a
    // DryRunPassed version record with content-derived evidence. Gate
    // failures leave the library untouched and are audited through the
    // WorkflowPublishProposed/Applied/Rejected events; a head record with
    // the same content and evidence settles as an idempotent NoOp.
    [[nodiscard]] Result<WorkflowPublishOutcome>
    publish_validated(const WorkflowDefinition &definition, const std::string &actor,
                      const std::string &reason,
                      std::optional<WorkflowRunId> source_run_id = std::nullopt);

    [[nodiscard]] Result<WorkflowRunView> run_snapshot(const WorkflowRunId &run_id) const;

    // The five stage-C operation handlers (run/patch/pause/resume/cancel)
    // bound to this runtime, ready for BuiltinToolRegistry::register_tool.
    // The registry entries capture `this`: this runtime must outlive them.
    [[nodiscard]] std::vector<BuiltinToolRegistration> operation_tool_registrations();

    // The request_user_input handler (DEC-024 §4) under the same lifetime
    // constraint as the operation handlers.
    [[nodiscard]] std::vector<BuiltinToolRegistration> decision_tool_registrations();

    // Stops run producers, cancels active runs through the control plane and
    // drains drive futures within the bounded budget. Never shuts the
    // executor down: the host owns it and follows with MiraRuntime shutdown
    // and executor::Executor::shutdown(true).
    WorkflowShutdownReport shutdown();

    [[nodiscard]] bool shut_down() const noexcept;

  private:
    struct RunRecord;
    struct DriveFlags;
    struct AppliedPatch;
    struct PendingPatch;
    struct VersionAppendResult final {
        Sha256Digest digest{};
        bool idempotent = false;
    };

    [[nodiscard]] Result<RunRecord *> find_run(const WorkflowRunId &run_id, Error &error);
    // Shared library append (M8-09 semantics): creates the history on first
    // publish, chains the parent digest to the current head and stores the
    // definition content. `dedupe` short-circuits a head record with the same
    // content, validation and evidence into an idempotent NoOp (stage D).
    [[nodiscard]] Result<VersionAppendResult>
    append_version_record(const WorkflowDefinition &definition, const Sha256Digest &digest,
                          const std::string &actor, const std::string &reason,
                          WorkflowValidationResult validation,
                          std::optional<Sha256Digest> evidence, bool dedupe);
    [[nodiscard]] Result<WorkflowRunView> create_run_locked(const WorkflowDefinition &definition,
                                                            JsonValue parameters,
                                                            std::optional<WorkflowPolicy> policy);

    // Applies one run-view transition through the frozen table under the
    // runtime mutex. Returns nullopt when applied; NoOpTerminal and Rejected
    // carry the corresponding Error. Leaving WaitingUser also drops the
    // record's staged decision details.
    [[nodiscard]] std::optional<Error> commit_transition(RunRecord &run,
                                                         WorkflowRunState target);
    // Commits the terminal transition, emits WorkflowRunSettled and settles
    // the carrier task through the control plane (tolerating idempotent
    // outcomes). Returns the transition error, if any.
    [[nodiscard]] std::optional<Error>
    settle_terminal(RunRecord &run, WorkflowRunState terminal_state,
                    const std::string &safe_summary);

    void emit_run_started(const RunRecord &run);
    void emit_step_started(const RunRecord &run, const WorkflowStep &step,
                           std::uint32_t attempt);
    void emit_step_settled(const RunRecord &run, const WorkflowStep &step,
                           const WorkflowStepRecord &record);
    void emit_run_settled(const RunRecord &run, const std::string &safe_summary);
    void emit_patch_proposed(const RunRecord &run, const WorkflowPatchId &patch_id,
                             const Sha256Digest &digest, WorkflowPatchTarget target,
                             const std::string &reason_code);
    void emit_patch_applied(const RunRecord &run, const WorkflowPatchId &patch_id);
    void emit_patch_rejected(const RunRecord &run, const WorkflowPatchId &patch_id,
                             const std::string &reason_code);
    void emit_policy_switched(const RunRecord &run, WorkflowPolicy from, WorkflowPolicy to);
    void emit_decision_raised(const RunRecord &run, const WorkflowDecisionRequest &decision);
    void emit_decision_resolved(const RunRecord &run, const WorkflowDecisionId &decision_id,
                                WorkflowDecisionResolution resolution);
    // Session-scoped publish-gate audit events (DEC-025 §3): no run record
    // exists while the gate runs, so these carry the workflow identity.
    void emit_publish_proposed(const WorkflowId &workflow_id, const Sha256Digest &ir_digest,
                               const std::optional<WorkflowRunId> &source_run_id);
    void emit_publish_applied(const WorkflowId &workflow_id, const Sha256Digest &ir_digest,
                              const Sha256Digest &evidence, const WorkflowRunId &dry_run_id);
    void emit_publish_rejected(const WorkflowId &workflow_id, const Sha256Digest &ir_digest,
                               const std::string &reason_code);
    // Stage E navigation events (DEC-028 §4). Planned is emitted for both
    // DryRun and dispatching resolutions; Observed only after a real edge
    // dispatch, carrying the post-update edge confidence.
    void emit_navigation_planned(const RunRecord &run, const WorkflowStep &step,
                                 const NavigationPlan &plan);
    void emit_navigation_observed(const RunRecord &run, const WorkflowStep &step,
                                  const AppModelTransition &transition, bool success);

    // Stage E Navigate execution (DEC-028 §3): screen read, target check,
    // planning (DryRun stops here), per-edge tool dispatch with arrival
    // verification and confidence write-back. Returns nullopt on success with
    // the step record filled; `cancelled` mirrors dispatch_step semantics.
    [[nodiscard]] std::optional<Error>
    execute_navigate_step(RunRecord &run, const WorkflowStep &step, std::size_t index,
                          const OperationContext &parent, const DriveFlags &flags,
                          WorkflowStepRecord &record, bool &cancelled);
    // The run's predicate context merged with the host screen state snapshot
    // (DEC-028 §2): screen_state:<id> (true) for the current state plus
    // screen_state:current (id string); nothing injected without a snapshot.
    // The provider is invoked without holding the runtime mutex.
    [[nodiscard]] JsonValue evaluation_context(const RunRecord &run) const;
    // Confidence write-back for one traversed edge (DEC-027 §2 pure update
    // under the mutex); events are emitted outside it.
    void note_navigation_outcome(RunRecord &run, const WorkflowStep &step,
                                 const AppModelTransition &transition, bool success,
                                 std::uint64_t now_ms);
    [[nodiscard]] bool navigation_context_installed() const;

    [[nodiscard]] WorkflowRunResult drive(RunRecord &run, const OperationContext &context);
    void assemble_result(const RunRecord &run, WorkflowRunResult &result) const;
    void settle_step(RunRecord &run, const WorkflowStep &step, const WorkflowStepRecord &record);
    [[nodiscard]] std::size_t step_index(const RunRecord &run, const StepId &step_id) const;
    // Handles one failed step: records it, consults the recovery hook and
    // either redirects the cursor (returning true), escalates to a wait
    // state (returning false) or settles the run Failed (returning false).
    [[nodiscard]] bool handle_step_failure(RunRecord &run, const WorkflowStep &step,
                                           std::size_t index, const Error &failure);
    // Escalation paths (DEC-023 §1). Both return nullopt when the run is in
    // the wait state (the drive must stop) and the failure otherwise; the
    // caller decides the fallback. escalate_waiting_* consume the run's
    // escalation budget; enter_waiting_agent (checkpoint handoffs) does not.
    [[nodiscard]] std::optional<Error> enter_waiting_agent(RunRecord &run,
                                                           const std::string &reason);
    [[nodiscard]] std::optional<Error> escalate_waiting_agent(RunRecord &run,
                                                              const std::string &reason);
    [[nodiscard]] std::optional<Error> escalate_waiting_user(RunRecord &run,
                                                             WorkflowDecisionRequest decision);
    [[nodiscard]] bool step_tool_has_side_effects(const RunRecord &run, std::size_t index) const;
    [[nodiscard]] Result<void> dispatch_step(RunRecord &run, const WorkflowStep &step,
                                             std::size_t index, const OperationContext &parent,
                                             const DriveFlags &flags,
                                             WorkflowStepRecord &record, bool &cancelled);
    // One BuiltIn-boundary invocation shared by step tools and navigation
    // edge actions (stage E): bounded submit_auto dispatch with the common
    // cancellation probe, deadline and error mapping.
    [[nodiscard]] Result<ToolExecutionRecord>
    dispatch_tool_invocation(RunRecord &run, const ExposedToolSpec &tool, const JsonValue &input,
                             const std::string &call_id, const OperationContext &parent,
                             const DriveFlags &flags, bool &cancelled);
    [[nodiscard]] Result<Observation> observe_for_verification(const RunRecord &run,
                                                               const WorkflowStep &step,
                                                               const OperationContext &parent);
    [[nodiscard]] Result<void> resolve_interrupted_step(RunRecord &run);
    // Patch pipeline (DEC-024 §2/§3). submit_patch performs admission and
    // validation and either queues (running) or applies immediately;
    // apply_patch performs the atomic effective-state transition, audit
    // events and epoch advance. Both fail closed with deterministic codes.
    [[nodiscard]] Result<WorkflowPatchOutcome> submit_patch(RunRecord &run,
                                                            const WorkflowPatchId &patch_id,
                                                            const std::vector<WorkflowPatchEntry> &entries,
                                                            bool decision_confirmed);
    [[nodiscard]] std::optional<Error> apply_patch(RunRecord &run, const WorkflowPatchId &patch_id,
                                                   const Sha256Digest &digest,
                                                   const std::vector<WorkflowPatchEntry> &entries,
                                                   bool decision_confirmed,
                                                   const std::string &reason_code);
    void drain_pending_patches(RunRecord &run);
    [[nodiscard]] std::vector<WorkflowPatchEntry>
    rollback_entries(const RunRecord &run, const AppliedPatch &target) const;
    [[nodiscard]] Result<JsonValue> execute_operation(WorkflowOperation operation,
                                                      const JsonValue &arguments);
    [[nodiscard]] Result<JsonValue> execute_user_input(const JsonValue &arguments);
    // Shared continuation for resume_run and resolve_decision: resumes the
    // carrier task, re-observes, commits Running and launches the drive.
    [[nodiscard]] Result<WorkflowRunView> continue_run(RunRecord &run);
    [[nodiscard]] WorkflowRunState run_state(const RunRecord &run) const;
    [[nodiscard]] JsonValue predicate_context_copy(const RunRecord &run) const;

    executor::Executor &executor_;
    MiraRuntime &runtime_;
    SessionId session_;
    std::shared_ptr<IEnvironment> environment_;
    WorkflowRuntimeConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<WorkflowRunId, std::unique_ptr<RunRecord>, StrongIdHash<WorkflowRunId>>
        runs_;
    std::unordered_map<WorkflowId, WorkflowVersionHistory, StrongIdHash<WorkflowId>> library_;
    std::unordered_map<Sha256Digest, WorkflowDefinition, Sha256DigestHash> definitions_;
    std::size_t active_runs_ = 0;
    std::size_t async_drives_ = 0;
    bool accepting_ = true;
    bool shut_down_ = false;

    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_id_;
    std::shared_ptr<BuiltinToolRegistry> tools_;
    // Stage E navigation context (DEC-027/028): the installed App Model
    // projection and the host screen-state callback. Both are guarded by
    // mutex_; the provider is copied out and invoked without the lock.
    std::optional<AppModel> app_model_;
    ScreenStateProvider screen_state_provider_;

    std::atomic<std::uint32_t> event_emit_failures_{0};
    std::atomic<std::uint32_t> task_settlement_failures_{0};
    std::atomic<std::uint32_t> monitor_failures_{0};
};

} // namespace mira
