#include <kairo/executor.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/workflow_runtime.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

namespace {
using namespace mira;
using namespace std::chrono_literals;

bool check(bool value, const char *label) {
    if (!value)
        std::cerr << "workflow host failed: " << label << '\n';
    return value;
}

bool run(kairo::Executor &executor) {
    MiraRuntime runtime;
    if (!check(runtime.initialize().has_value(), "runtime initialize"))
        return false;
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>(
        adapters::simulator::SimulatorSetup::single_display());
    auto session = runtime.open_session(environment);
    if (!check(session && session.value().command.outcome(2s), "session"))
        return false;
    auto events = std::make_shared<MemoryEventStore>(); // Process-local only.
    auto tools = std::make_shared<BuiltinToolRegistry>();
    std::atomic<unsigned> reads{0};
    BuiltinToolSpec spec;
    spec.wire_name = "fixture.screen_width";
    spec.description = "Read the simulator screen width without input";
    spec.has_side_effects = false;
    spec.parameters_schema = JsonSchema{JsonValue::Object{
        {"type", "object"}, {"properties", JsonValue::Object{}}, {"additionalProperties", false}}};
    auto registered = tools->register_tool(
        spec, [&](const JsonValue &, const OperationContext &context) -> Result<JsonValue> {
            ObservationRequest request;
            request.required.screen = true;
            auto observation = environment->observe(request, context);
            if (!observation)
                return observation.error();
            if (!observation.value().screen) {
                Error error;
                error.code = ErrorCode::InvalidObservation;
                return error;
            }
            ++reads;
            return JsonValue{
                static_cast<std::int64_t>(observation.value().screen->value.width_pixels)};
        });
    if (!check(registered.has_value(), "register read-only tool"))
        return false;
    WorkflowRuntimeConfig config;
    config.max_concurrent_async_drives = 1;
    config.max_step_executions_per_run = 4;
    WorkflowRuntime workflow(executor, runtime, session.value().id, environment, config);
    workflow.set_tool_registry(tools);
    workflow.set_event_store(events);
    WorkflowDefinition definition;
    definition.workflow_id = WorkflowId::generate();
    definition.name = "offline-native-width";
    definition.summary = "Read-only native result consumed by a verification step";
    definition.allowed_policies = {WorkflowPolicy::Strict};
    WorkflowStep read;
    read.id = StepId::generate();
    read.name = "read-width";
    read.kind = WorkflowStepKind::ToolCall;
    read.arguments = JsonValue::Object{{"tool", spec.wire_name}};
    WorkflowStep verify;
    verify.id = StepId::generate();
    verify.name = "verify-width";
    verify.kind = WorkflowStepKind::Verify;
    verify.verification = WorkflowPredicate{"step_result:" + read.id.to_string(),
                                            WorkflowPredicateOp::Eq, JsonValue{240}};
    definition.steps = {read, verify};
    auto created = workflow.create_run(definition, JsonValue::Object{}, WorkflowPolicy::Strict);
    if (!check(created.has_value(), "create"))
        return false;
    if (!check(workflow.start_run(created.value().run_id).has_value(), "start"))
        return false;
    auto result = workflow.wait_run(created.value().run_id, 5s);
    if (!check(result && result.value().state == WorkflowRunState::Completed &&
                   result.value().steps.size() == 2 &&
                   result.value().steps.back().verification == "satisfied" && reads == 1 &&
                   environment->executed_inputs().empty(),
               "result consumed"))
        return false;
    auto page = events->read({session.value().id, std::nullopt, 64});
    if (!check(page.has_value() && !page.value().has_more, "bounded events"))
        return false;
    unsigned terminal_events = 0;
    for (const auto &event : page.value().events) {
        if (event.payload.type != "WorkflowRunSettled")
            continue;
        auto settled = parse_workflow_run_settled(event.payload);
        if (!check(settled && settled.value().run_id == created.value().run_id &&
                       settled.value().terminal_state == result.value().state,
                   "terminal event reader"))
            return false;
        ++terminal_events;
    }
    if (!check(terminal_events == 1, "one terminal event") ||
        !check(!workflow.start_run(created.value().run_id), "terminal run cannot restart"))
        return false;
    const auto stopped = workflow.shutdown();
    auto shutdown = runtime.request_shutdown();
    if (!check(stopped.clean && shutdown && shutdown.value().outcome(2s) &&
                   runtime.finish_shutdown().clean,
               "shutdown"))
        return false;
    std::cout << "workflow host: read-only tool -> native step result -> verified terminal event\n";
    return true;
}
} // namespace

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 4; // One drive waits for bounded step futures.
    if (!executor.initialize(config))
        return 1;
    bool ok = false;
    try {
        ok = run(executor);
    } catch (...) {
        std::cerr << "workflow host task/submission failed\n";
    }
    if (executor.shutdown(true) != kairo::ShutdownResult::Completed)
        ok = false;
    return ok ? 0 : 1;
}
