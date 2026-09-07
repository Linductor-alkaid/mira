// Single-system harness integration: one MiraRuntime session hosts an
// AgentLoop that runs as an Executor-managed task, executes a BuiltIn tool,
// injects a mid-session user message and settles into a terminal task state,
// with every conversation turn reconstructable from the event store
// (MNT-202609-16). A second scenario pins the user-withdrawal semantics
// (MNT-202609-19, DEC-018): takeover releases platform input and paused or
// taken-over tasks admit no new operations.

#include "support/harness_support.hpp"

#include "support/test.hpp"

#include <mira/agent_loop.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/conversation_log.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/runtime.hpp>
#include <mira/tool_executor.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace mira;
using namespace mira::adapters::simulator;
using namespace mira::testing;

[[nodiscard]] std::unordered_set<std::string> session_event_types(const IEventStore &store,
                                                                 const SessionId &session) {
    std::unordered_set<std::string> types;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            types.insert(envelope.payload.type);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return types;
}

[[nodiscard]] bool loop_cancels_when_runtime_withdraws(const MiraRuntime &runtime,
                                                       const TaskId &task) {
    const auto snapshot = runtime.task_snapshot(task);
    if (!snapshot) {
        return true; // The task is gone; nothing may keep acting for it.
    }
    switch (snapshot.value().state) {
    case TaskState::Cancelling:
    case TaskState::Pausing:
    case TaskState::TakeoverSettling:
    case TaskState::SuspendedForTakeover:
    case TaskState::Cancelled:
    case TaskState::Completed:
    case TaskState::Failed:
        return true;
    default:
        return false;
    }
}

} // namespace

int full_harness_session() {
    // The host owns one Executor for loop work; the runtime owns its own
    // serial control plane (DEC-001).
    executor::Executor executor;
    executor::ExecutorConfig executor_config;
    executor_config.min_threads = 2;
    executor_config.max_threads = 2;
    executor_config.queue_capacity = 32;
    MIRA_CHECK(executor.initialize(executor_config));

    auto environment = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
    auto events = std::make_shared<MemoryEventStore>();

    MiraRuntime runtime({2, 16, 64});
    MIRA_CHECK(runtime.initialize());
    const auto session = runtime.open_session(environment);
    MIRA_CHECK(session);
    MIRA_CHECK(session.value().command.receipt(std::chrono::seconds(2)));
    MIRA_CHECK(session.value().command.outcome(std::chrono::seconds(2)));

    const auto task = runtime.submit_task(session.value().id, TaskSpec{"send the daily report"});
    MIRA_CHECK(task);
    const auto submitted = task.value().command.outcome(std::chrono::seconds(2));
    MIRA_CHECK(submitted && submitted.value().task.has_value());
    const auto task_id = task.value().id;
    const auto task_epoch = submitted.value().task->epoch;

    // Harness assembly: gateway + scripted provider, BuiltIn tool registry,
    // conversation-capable loop, admission tied to the runtime task epoch.
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(executor, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);

    auto registry = std::make_shared<BuiltinToolRegistry>();
    const auto wait_registration = make_wait_tool();
    const auto wait_spec = wait_registration.spec;
    MIRA_CHECK(registry->register_tool(wait_registration.spec, wait_registration.handler));
    gateway.register_provider(std::make_shared<RecordingProvider>(
        profile, std::vector<ModelResponse>{
                     tool_call_response(wait_spec, R"json({"duration_ms": 10})json"),
                     text_response(R"json({"action":"done","reason":"report sent"})json"),
                 }));

    AgentLoopSpec loop_spec;
    loop_spec.task_id = task_id;
    loop_spec.session_id = session.value().id;
    loop_spec.task_epoch = task_epoch;
    loop_spec.profile_id = profile->id;
    loop_spec.goal = "send the daily report";
    AgentLoop loop(environment, gateway);
    const auto runtime_id = RuntimeId::generate();
    loop.set_event_store(events, runtime_id, session.value().id);
    loop.set_tool_registry(registry);
    MIRA_CHECK(loop.enqueue_user_message("send it over DingTalk"));
    admission->activate(task_id, task_epoch);

    const auto operation = runtime.begin_operation(task_id, StepId::generate());
    MIRA_CHECK(operation);

    // The cancellation probe runs while environment locks are held, so it
    // must not call back into the runtime (that would invert the control
    // plane's own lock order: close/takeover take the runtime lock and then
    // call environment->interrupt()). A monitor task bridges the two: it
    // polls the runtime snapshot on its own thread and publishes the
    // withdrawal through an atomic flag.
    std::atomic<bool> runtime_withdrew{false};
    auto monitor = executor.submit_auto([&runtime, &task_id, &runtime_withdrew] {
        while (!runtime_withdrew.load(std::memory_order_relaxed)) {
            if (loop_cancels_when_runtime_withdraws(runtime, task_id)) {
                runtime_withdrew.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    OperationContext loop_context;
    loop_context.session = session.value().id;
    loop_context.task = task_id;
    loop_context.step = operation.value().step_id;
    loop_context.task_epoch = task_epoch;
    loop_context.started_at = Timestamp::now();
    loop_context.cancellation_requested = [&runtime_withdrew] {
        return runtime_withdrew.load(std::memory_order_relaxed);
    };

    ModelDoneVerifier verifier;
    auto future = executor.submit_auto(
        [&loop, &loop_spec, &loop_context, &verifier] { return loop.run(loop_spec, loop_context, verifier); });
    const auto loop_result = future.get();
    runtime_withdrew.store(true, std::memory_order_relaxed); // stop the monitor
    (void)monitor.get();
    MIRA_CHECK(loop_result);
    MIRA_CHECK(loop_result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(loop_result.value().steps.front().action_summary == "tool:wait");

    const auto completion = runtime.admit_operation_completion(operation.value());
    MIRA_CHECK(completion && completion.value().outcome(std::chrono::seconds(2)));

    // The loop verified the goal, so the task settles Completed through the
    // control plane; repeating the settlement is an idempotent NoOp and a
    // conflicting terminal state is rejected (RULE-03).
    const auto complete =
        runtime.complete_task(task_id, TaskOutcome{TaskState::Completed, std::nullopt});
    MIRA_CHECK(complete);
    MIRA_CHECK(complete.value().outcome(std::chrono::seconds(2)).value().status ==
               SettlementStatus::Applied);
    const auto settled = runtime.task_snapshot(task_id);
    MIRA_CHECK(settled && settled.value().state == TaskState::Completed);
    MIRA_CHECK(is_terminal(settled.value().state));
    const auto repeat =
        runtime.complete_task(task_id, TaskOutcome{TaskState::Completed, std::nullopt});
    MIRA_CHECK(repeat);
    MIRA_CHECK(repeat.value().outcome(std::chrono::seconds(2)).value().status ==
               SettlementStatus::NoOp);
    const auto conflict =
        runtime.complete_task(task_id, TaskOutcome{TaskState::Failed, std::nullopt});
    MIRA_CHECK(conflict);
    MIRA_CHECK(conflict.value().outcome(std::chrono::seconds(2)).value().status ==
               SettlementStatus::Failed);

    // The whole harness left reconstructable evidence behind: tool execution,
    // user turn and loop outcome, in one event store, for one session.
    const auto types = session_event_types(*events, session.value().id);
    MIRA_CHECK(types.count("ToolExecuted") == 1);
    MIRA_CHECK(types.count("UserMessageInjected") == 1);
    MIRA_CHECK(types.count("LoopSettled") == 1);
    const auto view = build_conversation_view(*events, session.value().id);
    MIRA_CHECK(view);
    MIRA_CHECK(view.value().size() == 2);
    MIRA_CHECK(view.value().front().kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value().front().text == "send it over DingTalk");
    MIRA_CHECK(view.value().back().kind == ConversationEntry::Kind::LoopOutcome);

    const auto close = runtime.close_session(session.value().id);
    MIRA_CHECK(close && close.value().outcome(std::chrono::seconds(2)));
    const auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.finish_shutdown().clean);
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// Counts best-effort platform releases so the scenario can assert that
// takeover converges in-flight input without trusting the simulator.
class InterruptCountingEnvironment final : public IEnvironment {
  public:
    explicit InterruptCountingEnvironment(std::shared_ptr<IEnvironment> inner)
        : inner_(std::move(inner)) {}

    EnvironmentCapabilities capabilities() const override { return inner_->capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        return inner_->observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        return inner_->execute(input, context);
    }
    Result<void> interrupt(const OperationContext &context) override {
        ++releases_;
        return inner_->interrupt(context);
    }
    [[nodiscard]] int releases() const { return releases_.load(); }

  private:
    std::shared_ptr<IEnvironment> inner_;
    std::atomic<int> releases_{0};
};

int takeover_releases_input_and_blocks_operations() {
    const auto environment = std::make_shared<InterruptCountingEnvironment>(
        std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display()));
    MiraRuntime runtime({2, 16, 64});
    MIRA_CHECK(runtime.initialize());
    const auto session = runtime.open_session(environment);
    MIRA_CHECK(session);
    MIRA_CHECK(session.value().command.receipt(std::chrono::seconds(2)));
    MIRA_CHECK(session.value().command.outcome(std::chrono::seconds(2)));

    const auto task = runtime.submit_task(session.value().id, TaskSpec{"hold the device"});
    MIRA_CHECK(task);
    MIRA_CHECK(task.value().command.outcome(std::chrono::seconds(2)));
    const auto task_id = task.value().id;

    // Active tasks admit operations.
    const auto admitted = runtime.begin_operation(task_id, StepId::generate());
    MIRA_CHECK(admitted);

    // Paused tasks admit none: the user withdrew the environment.
    const auto paused = runtime.pause_task(task_id);
    MIRA_CHECK(paused && paused.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.task_snapshot(task_id).value().state == TaskState::Paused);
    MIRA_CHECK(!runtime.begin_operation(task_id, StepId::generate()));
    // The operation opened before the pause settles stale (epoch advanced).
    const auto late = runtime.admit_operation_completion(admitted.value());
    MIRA_CHECK(late && late.value().outcome(std::chrono::seconds(2)).value().status ==
                           SettlementStatus::NoOp);

    const auto resumed = runtime.resume_task(task_id);
    MIRA_CHECK(resumed && resumed.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.task_snapshot(task_id).value().state == TaskState::Observing);
    MIRA_CHECK(runtime.begin_operation(task_id, StepId::generate()));

    // Takeover converges autonomous activity: platform input is released and
    // the taken-over task admits no new operations (DEC-018).
    const auto before_takeover = environment->releases();
    const auto takeover = runtime.request_human_takeover(session.value().id);
    MIRA_CHECK(takeover && takeover.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(environment->releases() == before_takeover + 1);
    MIRA_CHECK(runtime.task_snapshot(task_id).value().state == TaskState::SuspendedForTakeover);
    MIRA_CHECK(!runtime.begin_operation(task_id, StepId::generate()));

    // Release re-opens the environment for autonomous work: the task re-observes.
    const auto release = runtime.release_human_takeover(session.value().id);
    MIRA_CHECK(release && release.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.task_snapshot(task_id).value().state == TaskState::Observing);
    MIRA_CHECK(runtime.begin_operation(task_id, StepId::generate()));

    const auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(std::chrono::seconds(2)));
    MIRA_CHECK(runtime.finish_shutdown().clean);
    return 0;
}

int main() {
    if (const int code = full_harness_session(); code != 0) {
        return code;
    }
    return takeover_releases_input_and_blocks_operations();
}
