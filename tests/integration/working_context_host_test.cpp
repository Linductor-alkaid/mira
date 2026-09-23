// Single-system Working Context host integration (M25 / DEC-045, gate HI-G5):
// one MiraRuntime session hosts an AgentLoop wired to the Working Context
// snapshot supply seam, in the agent_harness_test.cpp shape (runtime session
// + scripted provider, every turn reconstructable from the event store).
// Deterministic, no model, offline.
//
//   Main-session closed loop — the loop starts context-blind; the host
//   signal triggers curation and the commit lands between two steps, so the
//   next assembled request carries the committed snapshot through the seam;
//   a mid-run host-orchestrated subagent round (fork -> child session chain
//   -> child Loop -> delta -> merge on a strictly advancing parent
//   watermark) lands before the parent's final request, which carries the
//   merged child finding; the task boundary then awaits the flush future
//   before the terminal flag and promotes the committed snapshot through the
//   generic Deferrable submit; the whole sequence stays auditable in the
//   session event store.
//
//   Seam degradation in the closed loop — a scripted supplier failure on the
//   first step leaves that request seam-free with exactly one visible
//   diagnostic, and the loop continues and injects on the next step.

#include "m25/m25_support.hpp"

#include "../m23/m23_promotion_support.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/agent_loop.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/runtime.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::adapters::simulator;
using namespace mira::testing;

// Deterministic checkpoint for the host-reported curation input, stamped
// with the live runtime task frame so the committed snapshot passes the
// seam's identity gate for the loop consuming it.
[[nodiscard]] ConversationCheckpoint runtime_checkpoint(const SessionId &session,
                                                        const TaskId &task, std::uint64_t epoch,
                                                        std::uint64_t watermark) {
    ConversationCheckpoint checkpoint;
    checkpoint.id =
        conversation_checkpoint_id_from_seed(session.to_string() + "|" + std::to_string(watermark));
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = epoch;
    checkpoint.environment_epoch = 7;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = m25_fixed_now();
    ConversationStatement constraint;
    constraint.content = "constraint r1 confirm before sending";
    constraint.source_events = {m25_event_from_seed(watermark + 1)};
    constraint.source_sequence = watermark - 1;
    constraint.confidence = 0.9;
    checkpoint.constraints.push_back(std::move(constraint));
    ConversationStatement decision;
    decision.content = "decision r1 use batch provider";
    decision.source_events = {m25_event_from_seed(watermark + 2)};
    decision.source_sequence = watermark - 1;
    decision.confidence = 0.9;
    checkpoint.decisions.push_back(std::move(decision));
    checkpoint.summary = "revision 1";
    checkpoint.source_events = {m25_event_from_seed(watermark + 1),
                                m25_event_from_seed(watermark + 2)};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] WorkingContextIdentity runtime_identity(const TaskId &task, std::uint64_t epoch) {
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = epoch;
    identity.environment_epoch = 7;
    return identity;
}

[[nodiscard]] WorkingContextCommitState runtime_live(const SessionId &session, const TaskId &task,
                                                     std::uint64_t epoch) {
    WorkingContextCommitState live;
    live.session = session;
    live.task = task;
    live.task_epoch = epoch;
    live.environment_epoch = 7;
    return live;
}

// Handshake between a provider hook (running inside the loop's infer on an
// Executor worker) and the host driving the orchestration from the main
// thread: the hook parks until the host finished the orchestrated step, so
// request ordering is deterministic without sleeps.
class HostGate final {
  public:
    void enter_and_wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        entered_cv_.notify_all();
        done_cv_.wait(lock, [this] { return done_; });
    }

    void wait_entered() {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        done_cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable entered_cv_;
    std::condition_variable done_cv_;
    bool entered_ = false;
    bool done_ = false;
};

// Drives the main-session closed loop. Returns the parent requests, the
// store and the curator stats through out-params for the assertions.
int main_session_closed_loop(MiraRuntime &runtime, const std::shared_ptr<IEnvironment> &environment,
                             executor::Executor &executor, IWorkingContextStore &store,
                             FaithfulMemory &memory, MemoryConsolidator &consolidator) {
    const auto session = runtime.open_session(environment);
    MIRA_CHECK(session);
    MIRA_CHECK(session.value().command.receipt(std::chrono::seconds(2)));
    MIRA_CHECK(session.value().command.outcome(std::chrono::seconds(2)));
    const auto session_id = session.value().id;

    const auto task = runtime.submit_task(session_id, TaskSpec{"finish the quarterly report"});
    MIRA_CHECK(task);
    const auto submitted = task.value().command.outcome(std::chrono::seconds(2));
    MIRA_CHECK(submitted && submitted.value().task.has_value());
    const auto task_id = task.value().id;
    const auto task_epoch = submitted.value().task->epoch;

    MemoryEventStore events;
    const auto runtime_id = RuntimeId::generate();
    ContextMemorySupervisor supervisor(executor, SupervisorConfig{}, &events, runtime_id,
                                       session_id);
    M25ScriptedCurator curator;
    WorkingContextAutoCurator auto_curator(supervisor, curator, store);

    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(executor, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);
    admission->activate(task_id, task_epoch);

    // Three scripted turns: the first before any snapshot exists, the second
    // after the host curation commit, the third after the subagent merge.
    auto provider = std::make_shared<RecordingProvider>(
        profile, std::vector<ModelResponse>{
                     text_response(decision_body("tap", 0.5, 0.5)),
                     text_response(decision_body("tap", 0.6, 0.6)),
                     text_response(R"json({"action":"done","reason":"goal reached"})json")});
    gateway.register_provider(provider);
    HostGate curation_gate;
    HostGate merge_gate;
    provider->on_after_response(0, [&curation_gate] { curation_gate.enter_and_wait(); });
    provider->on_after_response(1, [&merge_gate] { merge_gate.enter_and_wait(); });

    AgentLoopSpec spec;
    spec.task_id = task_id;
    spec.session_id = session_id;
    spec.task_epoch = task_epoch;
    spec.profile_id = profile->id;
    spec.goal = "finish the quarterly report";
    AgentLoop loop(environment, gateway);
    loop.set_event_store(std::shared_ptr<IEventStore>(std::shared_ptr<IEventStore>(), &events),
                         runtime_id, session_id);
    loop.set_working_context_supplier(
        [&store, session_id]() -> Result<std::optional<WorkingContextSnapshot>> {
            return store.latest(session_id);
        });

    const auto operation = runtime.begin_operation(task_id, StepId::generate());
    MIRA_CHECK(operation);

    ModelDoneVerifier verifier;
    auto loop_future = executor.submit_auto([&loop, &spec, &verifier] {
        OperationContext context;
        context.session = spec.session_id;
        context.task = spec.task_id;
        context.task_epoch = spec.task_epoch;
        context.started_at = Timestamp::now();
        return loop.run(spec, context, verifier);
    });

    // Host orchestration point 1: the session signal fires curation, whose
    // commit lands before the loop's second request.
    curation_gate.wait_entered();
    {
        const auto checkpoint = runtime_checkpoint(session_id, task_id, task_epoch, 8);
        const auto scheduled = auto_curator.on_signal(
            session_id, m25_refresh_input(checkpoint, runtime_identity(task_id, task_epoch),
                                          runtime_live(session_id, task_id, task_epoch)));
        MIRA_CHECK(scheduled.has_value());
        const auto outcome = scheduled->get();
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
    }
    curation_gate.release();

    // Host orchestration point 2: the subagent round — fork the committed
    // parent snapshot, run the child loop session on its own chain, project
    // the delta and merge it into the parent at a strictly advanced
    // watermark through the existing commit pipeline.
    SessionId child_session_id{};
    merge_gate.wait_entered();
    {
        auto parent_base = store.latest(session_id);
        MIRA_CHECK(parent_base.has_value() && parent_base.value().has_value());
        MIRA_CHECK(parent_base.value()->through_event_sequence == 8);

        const auto child_session = runtime.open_session(environment);
        MIRA_CHECK(child_session);
        MIRA_CHECK(child_session.value().command.receipt(std::chrono::seconds(2)));
        MIRA_CHECK(child_session.value().command.outcome(std::chrono::seconds(2)));
        child_session_id = child_session.value().id;
        const auto child_task =
            runtime.submit_task(child_session_id, TaskSpec{"gather the login path facts"});
        MIRA_CHECK(child_task);
        const auto child_submitted = child_task.value().command.outcome(std::chrono::seconds(2));
        MIRA_CHECK(child_submitted && child_submitted.value().task.has_value());
        const auto child_task_id = child_task.value().id;
        const auto child_epoch = child_submitted.value().task->epoch;

        WorkingContextForkSeed seed;
        seed.child_session = child_session_id;
        seed.child_identity = runtime_identity(child_task_id, child_epoch);
        seed.child_watermark = 1;
        auto baseline = fork_working_context(parent_base.value().value(), seed);
        MIRA_CHECK(baseline.has_value());
        const auto baseline_commit = commit_working_context(
            store, baseline.value(), runtime_live(child_session_id, child_task_id, child_epoch));
        MIRA_CHECK(baseline_commit.disposition == WorkingContextCommitDisposition::Committed);

        // The child chain adds one local finding at a strictly advancing
        // child watermark.
        auto child_tip = baseline.value();
        child_tip.constraints.push_back(
            m25_item("m25 child finding: the second login path works", 1100, 0.8));
        child_tip.through_event_sequence = 2;
        MIRA_CHECK(child_tip.validate().has_value());
        const auto child_commit = commit_working_context(
            store, child_tip, runtime_live(child_session_id, child_task_id, child_epoch));
        MIRA_CHECK(child_commit.disposition == WorkingContextCommitDisposition::Committed);

        // The child Loop session consumes its own chain through the seam.
        {
            auto child_profile = std::make_shared<ModelProfile>(
                make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
            ModelRouter child_router;
            child_router.register_profile(child_profile);
            ModelGateway child_gateway(executor, child_router, nullptr, PriceTable{},
                                       ModelGatewayConfig{});
            auto child_admission = std::make_shared<SimpleAdmissionGate>();
            child_gateway.set_admission_gate(child_admission);
            child_admission->activate(child_task_id, child_epoch);
            auto child_provider = std::make_shared<RecordingProvider>(
                child_profile, std::vector<ModelResponse>{text_response(R"json(
{"action":"done","reason":"facts gathered"})json")});
            child_gateway.register_provider(child_provider);

            AgentLoopSpec child_spec;
            child_spec.task_id = child_task_id;
            child_spec.session_id = child_session_id;
            child_spec.task_epoch = child_epoch;
            child_spec.profile_id = child_profile->id;
            child_spec.goal = "gather the login path facts";
            AgentLoop child_loop(environment, child_gateway);
            child_loop.set_event_store(
                std::shared_ptr<IEventStore>(std::shared_ptr<IEventStore>(), &events), runtime_id,
                child_session_id);
            child_loop.set_working_context_supplier(
                [&store, child_session_id]() -> Result<std::optional<WorkingContextSnapshot>> {
                    return store.latest(child_session_id);
                });
            ModelDoneVerifier child_verifier;
            OperationContext child_context;
            child_context.session = child_session_id;
            child_context.task = child_task_id;
            child_context.task_epoch = child_epoch;
            child_context.started_at = Timestamp::now();
            const auto child_outcome = child_loop.run(child_spec, child_context, child_verifier);
            MIRA_CHECK(child_outcome.has_value());
            MIRA_CHECK(child_outcome.value().outcome == LoopOutcome::Completed);
            // The child request carried the fork baseline (parent content)
            // plus the child finding: both chains visible through the seam.
            const auto child_requests = child_provider->requests();
            MIRA_CHECK(child_requests.size() == 1);
            const auto child_seam = m25_seam_text(child_requests.front());
            MIRA_CHECK(child_seam.find("confirm before sending") != std::string::npos);
            MIRA_CHECK(child_seam.find("the second login path works") != std::string::npos);
        }

        // Child returns: the delta folds into the parent at parent watermark
        // 9 (strict advance past 8) through the existing commit pipeline.
        auto delta = working_context_delta_from_fork(baseline.value(), child_tip);
        MIRA_CHECK(delta.has_value());
        auto merged =
            merge_working_context_delta(baseline.value(), parent_base.value().value(),
                                        delta.value(), runtime_identity(task_id, task_epoch), 9);
        MIRA_CHECK(merged.has_value());
        MIRA_CHECK(merged.value().additions_appended == 1);
        const auto merged_commit = commit_working_context(
            store, merged.value().merged, runtime_live(session_id, task_id, task_epoch));
        MIRA_CHECK(merged_commit.disposition == WorkingContextCommitDisposition::Committed);
    }
    merge_gate.release();

    const auto loop_outcome = loop_future.get();
    MIRA_CHECK(loop_outcome.has_value());
    MIRA_CHECK(loop_outcome.value().outcome == LoopOutcome::Completed);

    // Request-level audit: request 1 context-blind, request 2 carries the
    // committed snapshot, request 3 carries the merged child finding.
    const auto requests = provider->requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(m25_seam_item_indices(requests[0]).empty());
    MIRA_CHECK(m25_seam_item_indices(requests[1]).size() == 1);
    const auto second_seam = m25_seam_text(requests[1]);
    MIRA_CHECK(second_seam.find("confirm before sending") != std::string::npos);
    MIRA_CHECK(m25_seam_item_indices(requests[2]).size() == 1);
    const auto third_seam = m25_seam_text(requests[2]);
    MIRA_CHECK(third_seam.find("the second login path works") != std::string::npos);
    MIRA_CHECK(m25_find_item_source(requests[2], "mira.agent-loop.working-context.v1").has_value());

    // Task boundary: the flush future is bounded-awaited before the terminal
    // flag, then the runtime settles the task.
    {
        const auto checkpoint = runtime_checkpoint(session_id, task_id, task_epoch, 8);
        auto live = runtime_live(session_id, task_id, task_epoch);
        const auto boundary = auto_curator.flush(
            session_id, m25_refresh_input(checkpoint, runtime_identity(task_id, task_epoch), live));
        MIRA_CHECK(boundary.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        MIRA_CHECK(boundary.get().has_value());
    }
    const auto completion = runtime.admit_operation_completion(operation.value());
    MIRA_CHECK(completion && completion.value().outcome(std::chrono::seconds(2)));
    const auto complete =
        runtime.complete_task(task_id, TaskOutcome{TaskState::Completed, std::nullopt});
    MIRA_CHECK(complete && complete.value().outcome(std::chrono::seconds(2)).value().status ==
                               SettlementStatus::Applied);

    // Terminal boundary: the host promotes the committed snapshot through
    // the generic Deferrable route and consumes the future.
    {
        auto promoted = store.latest(session_id);
        MIRA_CHECK(promoted.has_value() && promoted.value().has_value());
        auto future = supervisor.submit<WorkingContextPromotionReport>(
            "working-context-promotion", SupervisedOpClass::Deferrable, [&](SupervisorToken) {
                return promote_working_context_to_memory(consolidator, memory,
                                                         promoted.value().value(),
                                                         m23_env_scope("env-42"), m25_fixed_now());
            });
        const auto report = future.get();
        MIRA_CHECK(report.has_value());
        MIRA_CHECK(memory.stored_records() == 1);
    }

    // Event audit: the closed loop left reconstructable evidence in one
    // session event store.
    const auto types = m25_session_event_types(events, session_id);
    bool saw_action = false;
    bool saw_verification = false;
    bool saw_settled = false;
    for (const auto &type : types) {
        saw_action = saw_action || type == "ActionDispatched";
        saw_verification = saw_verification || type == "VerificationResult";
        saw_settled = saw_settled || type == "LoopSettled";
    }
    MIRA_CHECK(saw_action && saw_verification && saw_settled);

    const auto close = runtime.close_session(session_id);
    MIRA_CHECK(close && close.value().outcome(std::chrono::seconds(2)));
    const auto child_close = runtime.close_session(child_session_id);
    MIRA_CHECK(child_close && child_close.value().outcome(std::chrono::seconds(2)));
    return 0;
}

// ---------------------------------------------------------------------------
// Seam degradation stays visible inside the closed loop: the failed supply
// degrades to a seam-free request plus exactly one diagnostic, and the next
// step injects again.
// ---------------------------------------------------------------------------

int seam_degradation_visible_in_closed_loop(MiraRuntime &runtime,
                                            const std::shared_ptr<IEnvironment> &environment,
                                            executor::Executor &executor,
                                            IWorkingContextStore &store) {
    const auto session = runtime.open_session(environment);
    MIRA_CHECK(session);
    MIRA_CHECK(session.value().command.receipt(std::chrono::seconds(2)));
    MIRA_CHECK(session.value().command.outcome(std::chrono::seconds(2)));
    const auto session_id = session.value().id;

    const auto task = runtime.submit_task(session_id, TaskSpec{"send the daily report"});
    MIRA_CHECK(task);
    const auto submitted = task.value().command.outcome(std::chrono::seconds(2));
    MIRA_CHECK(submitted && submitted.value().task.has_value());
    const auto task_id = task.value().id;
    const auto task_epoch = submitted.value().task->epoch;

    MemoryEventStore events;
    const auto runtime_id = RuntimeId::generate();

    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(executor, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);
    admission->activate(task_id, task_epoch);
    auto provider = std::make_shared<RecordingProvider>(
        profile, std::vector<ModelResponse>{
                     text_response(decision_body("tap", 0.5, 0.5)),
                     text_response(R"json({"action":"done","reason":"report sent"})json")});
    gateway.register_provider(provider);

    WorkingContextSupplierHarness supplier_harness(store, session_id);
    supplier_harness.set_script(
        {WorkingContextSupplierHarness::Mode::Error, WorkingContextSupplierHarness::Mode::Store});
    // The second supply resolves an aligned committed snapshot, so the
    // recovery step visibly injects it.
    auto snapshot = m25_snapshot(session_id, task_id, task_epoch, 1200);
    snapshot.constraints.push_back(
        m25_item("m25 report constraint: confirm the audience before sending", 1201, 0.9));
    MIRA_CHECK(store.put(snapshot).has_value());

    AgentLoopSpec spec;
    spec.task_id = task_id;
    spec.session_id = session_id;
    spec.task_epoch = task_epoch;
    spec.profile_id = profile->id;
    spec.goal = "send the daily report";
    AgentLoop loop(environment, gateway);
    loop.set_event_store(std::shared_ptr<IEventStore>(std::shared_ptr<IEventStore>(), &events),
                         runtime_id, session_id);
    loop.set_working_context_supplier(supplier_harness.supplier());

    ModelDoneVerifier verifier;
    OperationContext context;
    context.session = session_id;
    context.task = task_id;
    context.task_epoch = task_epoch;
    context.started_at = Timestamp::now();
    const auto outcome = loop.run(spec, context, verifier);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);

    const auto requests = provider->requests();
    MIRA_CHECK(requests.size() == 2);
    MIRA_CHECK(m25_seam_item_indices(requests[0]).empty());
    MIRA_CHECK(m25_seam_item_indices(requests[1]).size() == 1);
    MIRA_CHECK(supplier_harness.calls() == 2);
    // Degradation is visible exactly once on the session event surface, and
    // the loop's own settlement is in the same audit trail.
    MIRA_CHECK(m25_working_context_diagnostic_count(events, session_id) == 1);
    bool saw_settled = false;
    for (const auto &type : m25_session_event_types(events, session_id)) {
        saw_settled = saw_settled || type == "LoopSettled";
    }
    MIRA_CHECK(saw_settled);

    const auto close = runtime.close_session(session_id);
    MIRA_CHECK(close && close.value().outcome(std::chrono::seconds(2)));
    return 0;
}

} // namespace

int main() {
    executor::Executor executor;
    executor::ExecutorConfig executor_config;
    executor_config.min_threads = 2;
    executor_config.max_threads = 4;
    executor_config.queue_capacity = 64;
    MIRA_CHECK(executor.initialize(executor_config));

    auto environment = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
    InMemoryWorkingContextStore store;
    FaithfulMemory memory;
    MemoryConsolidator consolidator;

    MiraRuntime runtime({2, 16, 64});
    MIRA_CHECK(runtime.initialize());

    if (const int code =
            main_session_closed_loop(runtime, environment, executor, store, memory, consolidator);
        code != 0) {
        return code;
    }
    if (const int code =
            seam_degradation_visible_in_closed_loop(runtime, environment, executor, store);
        code != 0) {
        return code;
    }

    const auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.finish_shutdown().clean);
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}
