#pragma once

// Shared fixtures for the M9 WorkflowRuntime tests: one executor plus one
// MiraRuntime session over the simulator environment, counting/gating BuiltIn
// tools and definition builders. Consumers: tests/m9/*.cpp.

#include "test.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/event_store.hpp>
#include <mira/runtime.hpp>
#include <mira/tool_executor.hpp>
#include <mira/workflow_runtime.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace mira::testing {

using mira::adapters::simulator::SimulatorEnvironment;
using mira::adapters::simulator::SimulatorSetup;

// register_tool takes the spec and handler separately; the tests hold
// registrations as one value.
[[nodiscard]] inline Result<void>
register_registration(BuiltinToolRegistry &registry, BuiltinToolRegistration registration) {
    return registry.register_tool(std::move(registration.spec), std::move(registration.handler));
}

// Host-owned executor + control plane + simulator session + registry. The
// shutdown order is the documented one: WorkflowRuntime::shutdown (done by
// each scenario), MiraRuntime stop, executor shutdown from the host thread.
class WorkflowFixture final {
  public:
    WorkflowFixture() {
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 4;
        executor_config.max_threads = 4;
        executor_config.queue_capacity = 64;
        (void)executor_.initialize(executor_config);
        runtime_ = std::make_unique<MiraRuntime>(RuntimeConfig{2, 16, 64});
        (void)runtime_->initialize();
        environment_ = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
        const auto opened = runtime_->open_session(environment_);
        if (opened.has_value()) {
            (void)opened.value().command.outcome(std::chrono::seconds(2));
            session_id_ = opened.value().id;
        }
        registry_ = std::make_shared<BuiltinToolRegistry>();
        events_ = std::make_shared<MemoryEventStore>();
    }

    ~WorkflowFixture() {
        const auto shutdown = runtime_->request_shutdown();
        if (shutdown.has_value()) {
            (void)shutdown.value().outcome(std::chrono::seconds(5));
        }
        (void)runtime_->finish_shutdown();
        (void)executor_.shutdown(true);
    }

    [[nodiscard]] std::unique_ptr<WorkflowRuntime>
    make_workflow(WorkflowRuntimeConfig config = WorkflowRuntimeConfig{}) {
        auto workflow = std::make_unique<WorkflowRuntime>(executor_, *runtime_, session_id_,
                                                          environment_, config);
        workflow->set_event_store(events_);
        workflow->set_tool_registry(registry_);
        return workflow;
    }

    executor::Executor executor_;
    std::unique_ptr<MiraRuntime> runtime_;
    std::shared_ptr<SimulatorEnvironment> environment_;
    std::shared_ptr<BuiltinToolRegistry> registry_;
    std::shared_ptr<MemoryEventStore> events_;
    SessionId session_id_;
};

// Counts dispatches and answers a scalar result so verification predicates
// can compare it directly ("sent" or the integer count). The first
// `failures_first` dispatches return failed records instead.
struct CountingTool final {
    std::atomic<int> dispatches{0};
    int failures_first = 0;
    bool side_effects = false;

    [[nodiscard]] BuiltinToolRegistration registration(const std::string &wire_name = "counter",
                                                       const std::string &result = "sent") {
        ++serial;
        BuiltinToolRegistration registration;
        registration.spec.wire_name = wire_name;
        registration.spec.description = "m9 counting tool";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        registration.spec.has_side_effects = side_effects;
        registration.handler = [this, result](const JsonValue &,
                                              const OperationContext &) -> Result<JsonValue> {
            const auto count = ++dispatches;
            if (count <= failures_first) {
                Error error;
                error.code = ErrorCode::PlatformError;
                error.domain = "mira.test";
                error.safe_message = "scripted tool failure";
                return error;
            }
            if (result == "count") {
                return JsonValue{static_cast<std::int64_t>(count - failures_first)};
            }
            return JsonValue{result};
        };
        return registration;
    }

  private:
    int serial = 0;
};

// Blocks until released while polling the cancellation probe; settles
// Cancelled when the control plane withdraws first.
struct GatedTool final {
    std::atomic<bool> release{false};
    std::atomic<int> entries{0};
    bool side_effects = false;

    [[nodiscard]] BuiltinToolRegistration registration(const std::string &wire_name = "gate") {
        BuiltinToolRegistration registration;
        registration.spec.wire_name = wire_name;
        registration.spec.description = "m9 gated tool";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        registration.spec.has_side_effects = side_effects;
        registration.handler = [this](const JsonValue &,
                                      const OperationContext &context) -> Result<JsonValue> {
            ++entries;
            while (!release.load(std::memory_order_relaxed)) {
                if (context.cancelled()) {
                    Error error;
                    error.code = ErrorCode::Cancelled;
                    error.domain = "mira.test";
                    error.safe_message = "gate released by cancellation";
                    return error;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return JsonValue{"released"};
        };
        return registration;
    }
};

// Wraps an environment and counts observations so tests can pin the W-02
// observation-before-verification and resume re-observation behavior.
class ObservationCountingEnvironment final : public IEnvironment {
  public:
    explicit ObservationCountingEnvironment(std::shared_ptr<IEnvironment> inner)
        : inner_(std::move(inner)) {}

    EnvironmentCapabilities capabilities() const override { return inner_->capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        ++observations_;
        return inner_->observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        return inner_->execute(input, context);
    }
    Result<void> interrupt(const OperationContext &context) override {
        return inner_->interrupt(context);
    }
    [[nodiscard]] int observations() const { return observations_.load(); }

  private:
    std::shared_ptr<IEnvironment> inner_;
    std::atomic<int> observations_{0};
};

// --- definition builders ----------------------------------------------------

[[nodiscard]] inline WorkflowDefinition base_definition(std::string name = "m9-flow",
                                                        bool with_parameters = true) {
    WorkflowDefinition definition;
    definition.schema_version = SchemaVersion{1, 0};
    definition.workflow_id = WorkflowId::generate();
    definition.name = std::move(name);
    definition.summary = "m9 fixture workflow";
    definition.default_policy = WorkflowPolicy::Strict;
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::DryRun,
                                   WorkflowPolicy::Recoverable, WorkflowPolicy::Interactive};
    if (with_parameters) {
        WorkflowParameterSpec parameter;
        parameter.name = "mode";
        parameter.type = WorkflowParameterType::String;
        parameter.required = false;
        parameter.default_value = JsonValue{"run"};
        parameter.max_length = 16;
        definition.parameters.push_back(parameter);
    }
    return definition;
}

[[nodiscard]] inline WorkflowStep tool_step(const std::string &wire,
                                            std::optional<WorkflowPredicate> verification,
                                            std::optional<WorkflowRecoveryHook> recovery = {},
                                            bool loop_head = false,
                                            std::uint32_t max_attempts = 1) {
    WorkflowStep step;
    step.id = StepId::generate();
    step.name = "call-" + wire;
    step.kind = WorkflowStepKind::ToolCall;
    step.loop_head = loop_head;
    JsonValue::Object arguments;
    arguments.emplace_back("tool", wire);
    arguments.emplace_back("payload", "payload");
    step.arguments = JsonValue{std::move(arguments)};
    step.verification = std::move(verification);
    step.recovery = std::move(recovery);
    step.max_attempts = max_attempts;
    return step;
}

[[nodiscard]] inline WorkflowStep verify_step(WorkflowPredicate verification) {
    WorkflowStep step;
    step.id = StepId::generate();
    step.name = "verify";
    step.kind = WorkflowStepKind::Verify;
    step.verification = std::move(verification);
    return step;
}

[[nodiscard]] inline WorkflowStep control_step(const StepId &target, std::uint32_t max_iterations,
                                               std::optional<WorkflowPredicate> precondition = {}) {
    WorkflowStep step;
    step.id = StepId::generate();
    step.name = "loop";
    step.kind = WorkflowStepKind::Control;
    step.jump_to = target;
    step.max_iterations = max_iterations;
    step.precondition = std::move(precondition);
    return step;
}

[[nodiscard]] inline WorkflowStep navigate_step(const std::string &target) {
    WorkflowStep step;
    step.id = StepId::generate();
    step.name = "navigate";
    step.kind = WorkflowStepKind::Navigate;
    JsonValue::Object arguments;
    arguments.emplace_back("target", target);
    step.arguments = JsonValue{std::move(arguments)};
    return step;
}

[[nodiscard]] inline WorkflowPredicate step_result_predicate(const StepId &step,
                                                             WorkflowPredicateOp op,
                                                             JsonValue value) {
    WorkflowPredicate predicate;
    predicate.signal = "step_result:" + step.to_string();
    predicate.op = op;
    predicate.value = std::move(value);
    return predicate;
}

[[nodiscard]] inline WorkflowPredicate parameter_predicate(const std::string &name,
                                                           WorkflowPredicateOp op,
                                                           JsonValue value) {
    WorkflowPredicate predicate;
    predicate.signal = "run_parameter:" + name;
    predicate.op = op;
    predicate.value = std::move(value);
    return predicate;
}

[[nodiscard]] inline OperationContext drive_context() {
    OperationContext context;
    context.started_at = Timestamp::now();
    return context;
}

// Collects the recorded event types for one session (paged read).
[[nodiscard]] inline std::vector<std::string> session_event_types(const IEventStore &store,
                                                                  const SessionId &session) {
    std::vector<std::string> types;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            types.push_back(envelope.payload.type);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return types;
}

} // namespace mira::testing
