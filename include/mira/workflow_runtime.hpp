#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/runtime.hpp>
#include <mira/tool_executor.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>
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
// settled step history. `state` is terminal unless the drive stopped at a
// pause boundary (Paused).
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

struct WorkflowShutdownReport final {
    bool clean = false;
    std::size_t cancelled_runs = 0;
    std::size_t drained_drives = 0;
    std::string diagnostic;
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
    // re-observes and continues from the cursor via a new asynchronous drive;
    // cancel is idempotent.
    [[nodiscard]] Result<WorkflowRunView> pause_run(const WorkflowRunId &run_id);
    [[nodiscard]] Result<WorkflowRunView> resume_run(const WorkflowRunId &run_id);
    [[nodiscard]] Result<WorkflowRunView> cancel_run(const WorkflowRunId &run_id);

    [[nodiscard]] Result<WorkflowRunView> run_snapshot(const WorkflowRunId &run_id) const;

    // The four stage-B operation handlers (run/pause/resume/cancel) bound to
    // this runtime, ready for BuiltinToolRegistry::register_tool. The
    // registry entries capture `this`: this runtime must outlive them.
    // patch_workflow stays schema-only until stage C.
    [[nodiscard]] std::vector<BuiltinToolRegistration> operation_tool_registrations();

    // Stops run producers, cancels active runs through the control plane and
    // drains drive futures within the bounded budget. Never shuts the
    // executor down: the host owns it and follows with MiraRuntime shutdown
    // and executor::Executor::shutdown(true).
    WorkflowShutdownReport shutdown();

    [[nodiscard]] bool shut_down() const noexcept;

  private:
    struct RunRecord;
    struct DriveFlags;

    [[nodiscard]] Result<RunRecord *> find_run(const WorkflowRunId &run_id, Error &error);
    [[nodiscard]] Result<WorkflowRunView> create_run_locked(const WorkflowDefinition &definition,
                                                            JsonValue parameters,
                                                            std::optional<WorkflowPolicy> policy);

    // Applies one run-view transition through the frozen table under the
    // runtime mutex. Returns nullopt when applied; NoOpTerminal and Rejected
    // carry the corresponding Error.
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

    [[nodiscard]] WorkflowRunResult drive(RunRecord &run, const OperationContext &context);
    void assemble_result(const RunRecord &run, WorkflowRunResult &result) const;
    void settle_step(RunRecord &run, const WorkflowStep &step, const WorkflowStepRecord &record);
    [[nodiscard]] std::size_t step_index(const RunRecord &run, const StepId &step_id) const;
    // Handles one failed step: records it, consults the recovery hook and
    // either redirects the cursor (returning true) or settles the run Failed
    // (returning false).
    [[nodiscard]] bool handle_step_failure(RunRecord &run, const WorkflowStep &step,
                                           std::size_t index, const Error &failure);
    [[nodiscard]] bool step_tool_has_side_effects(const RunRecord &run, std::size_t index) const;
    [[nodiscard]] Result<void> dispatch_step(RunRecord &run, const WorkflowStep &step,
                                             std::size_t index, const OperationContext &parent,
                                             const DriveFlags &flags,
                                             WorkflowStepRecord &record, bool &cancelled);
    [[nodiscard]] Result<Observation> observe_for_verification(const RunRecord &run,
                                                               const WorkflowStep &step,
                                                               const OperationContext &parent);
    [[nodiscard]] Result<void> resolve_interrupted_step(RunRecord &run);
    [[nodiscard]] Result<JsonValue> execute_operation(WorkflowOperation operation,
                                                      const JsonValue &arguments);
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

    std::atomic<std::uint32_t> event_emit_failures_{0};
    std::atomic<std::uint32_t> task_settlement_failures_{0};
    std::atomic<std::uint32_t> monitor_failures_{0};
};

} // namespace mira
