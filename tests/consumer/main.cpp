#include <mira/adapters/net/socket_transport.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_runtime.hpp>
#include <mira/workflow_tools.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>

#include <executor/executor.hpp>

#ifdef MIRA_CONSUMER_HAS_MBEDTLS
#include <mira/adapters/net/mbedtls_tls.hpp>
#endif

int main() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::single_display()};

    mira::ObservationRequest request;
    request.required.screen = true;
    mira::OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    const auto observation = environment.observe(request, context);
    if (!observation.has_value() || !observation.value().screen.has_value()) {
        return 1;
    }
    mira::RuntimeBaseline runtime;
    if (!runtime.initialize()) {
        return 2;
    }
    const auto submission = runtime.submit({1, 1, 0, mira::BaselineCommandKind::Command});
    if (!submission.admitted) {
        return 3;
    }
    const auto result = runtime.wait(1, std::chrono::seconds(2));
    if (!runtime.request_shutdown()) {
        return 5;
    }
    runtime.finish_shutdown();
    if (result.code != mira::BaselineResultCode::Applied) {
        return 4;
    }

    // Durable state stores: the installed package must export Mira::state_store
    // (with its vendored SQLite closure), not just the public headers.
    executor::Executor store_exec;
    if (!store_exec.initialize(executor::ExecutorConfig{})) {
        return 6;
    }
    {
        std::error_code fs_error;
        const auto root =
            std::filesystem::temp_directory_path(fs_error) / "mira-installed-consumer";
        std::filesystem::create_directories(root, fs_error);

        mira::SqliteStoreOptions checkpoint_options;
        checkpoint_options.path = root / "consumer-checkpoints.db";
        auto checkpoint_store = mira::SqliteCheckpointStore::open(store_exec, checkpoint_options);
        if (!checkpoint_store) {
            return 7;
        }

        mira::SqliteMemoryStoreOptions memory_options;
        memory_options.path = root / "consumer-memory.db";
        auto memory_store = mira::SqliteMemoryStore::open(store_exec, memory_options);
        if (!memory_store) {
            return 8;
        }

        (void)memory_store.value()->close();
        (void)checkpoint_store.value()->close();
        std::filesystem::remove_all(root, fs_error);
    }
    if (store_exec.shutdown(true) != executor::ShutdownResult::Completed) {
        return 9;
    }

    // Network transports: the installed package must export the adapter
    // headers and libraries so a package-only consumer can construct the
    // official production transport stack (GitHub #14). A PEM CA bundle is
    // the caller's responsibility; a missing file must fail closed at
    // initialize() rather than at first use.
    {
        executor::Executor net_exec;
        if (!net_exec.initialize(executor::ExecutorConfig{})) {
            return 10;
        }
        auto secrets = std::make_shared<mira::NullSecretResolver>();
        mira::adapters::net::SocketHttpTransport transport{net_exec, secrets};
        if (!transport.start()) {
            return 11;
        }
        if (!transport.running()) {
            return 12;
        }
        transport.shutdown();
        if (transport.running()) {
            return 13;
        }
#ifdef MIRA_CONSUMER_HAS_MBEDTLS
        mira::adapters::net::MbedTlsChannelFactory tls{
            "/nonexistent/mira-consumer-ca.pem"};
        if (tls.initialize()) {
            return 14; // A missing CA bundle must not initialize.
        }
#endif
        if (net_exec.shutdown(true) != executor::ShutdownResult::Completed) {
            return 15;
        }
    }

    // Workflow contracts (M8): the installed package must export Mira::workflow
    // so a package-only consumer can parse, bind and validate IR documents and
    // workflow tool calls without any runtime execution.
    {
        const char *ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-probe",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "fedcba9876543210fedcba9876543210", "kind": "tool_call",
                 "arguments": {"text": {"$param": "contact"}}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto definition = mira::parse_workflow_definition(ir);
        if (!definition.has_value()) {
            return 16;
        }
        mira::JsonValue input{mira::JsonValue::Object{}};
        input.set("contact", mira::JsonValue{std::string("zhang san")});
        auto bindings = mira::bind_workflow_parameters(definition.value(), input);
        if (!bindings.has_value()) {
            return 17;
        }
        auto arguments = mira::resolve_step_arguments(
            bindings.value(), definition.value().steps.front().arguments);
        if (!arguments.has_value() ||
            *arguments.value().find("text")->as_string() != "zhang san") {
            return 18;
        }
        mira::JsonValue run_arguments{mira::JsonValue::Object{}};
        run_arguments.set("workflow_id",
                          mira::JsonValue{std::string("0123456789abcdef0123456789abcdef")});
        run_arguments.set(
            "ir_digest",
            mira::JsonValue{mira::workflow_definition_digest(definition.value()).to_string()});
        if (!mira::validate_workflow_operation(mira::WorkflowOperation::RunWorkflow,
                                               run_arguments)) {
            return 19;
        }
    }

    // Workflow runtime (M9): the installed package executes a DryRun closed
    // loop over its own Executor and control plane, proving that
    // Mira::workflow's executor dependency resolves for package consumers.
    {
        executor::Executor host_executor;
        executor::ExecutorConfig host_config;
        host_config.min_threads = 2;
        host_config.max_threads = 2;
        host_config.queue_capacity = 8;
        if (!host_executor.initialize(host_config)) {
            return 20;
        }
        mira::MiraRuntime runtime;
        if (!runtime.initialize()) {
            return 21;
        }
        auto environment = std::make_shared<mira::adapters::simulator::SimulatorEnvironment>(
            mira::adapters::simulator::SimulatorSetup::single_display());
        const auto session = runtime.open_session(environment);
        if (!session.has_value() ||
            !session.value().command.outcome(std::chrono::seconds(2)).has_value()) {
            return 22;
        }
        mira::WorkflowRuntime workflows(host_executor, runtime, session.value().id, environment);
        const char *ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-dry-run",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "fedcba9876543210fedcba9876543210", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "exists"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto definition = mira::parse_workflow_definition(ir);
        if (!definition.has_value()) {
            return 23;
        }
        mira::JsonValue parameters{mira::JsonValue::Object{}};
        parameters.set("contact", mira::JsonValue{std::string("zhang san")});
        const auto created = workflows.create_run(definition.value(), parameters,
                                                 mira::WorkflowPolicy::DryRun);
        if (!created.has_value()) {
            return 24;
        }
        mira::OperationContext context;
        context.started_at = mira::Timestamp::now();
        const auto result = workflows.execute_run(created.value().run_id, context);
        if (!result.has_value() ||
            result.value().state != mira::WorkflowRunState::Completed) {
            return 25;
        }

        // Workflow intervention (M10): an Interactive run parks in
        // WaitingUser on failure, accepts a run patch in the wait state and
        // resolves the decision point through the installed package.
        const char *interactive_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-interactive",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "11111111111111111111111111111111", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "eq",
                                  "value": "blocked"}},
                {"step_id": "22222222222222222222222222222222", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "exists"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "interactive"]
        })";
        auto interactive = mira::parse_workflow_definition(interactive_ir);
        if (!interactive.has_value()) {
            return 30;
        }
        const auto started = workflows.create_run(interactive.value(), parameters,
                                                  mira::WorkflowPolicy::Interactive);
        if (!started.has_value()) {
            return 31;
        }
        const auto parked = workflows.execute_run(started.value().run_id, context);
        if (!parked.has_value() ||
            parked.value().state != mira::WorkflowRunState::WaitingUser) {
            return 32;
        }
        mira::WorkflowPatchEntry entry;
        entry.target = mira::WorkflowPatchTarget::RunParameters;
        entry.op = mira::WorkflowPatchOp::Set;
        entry.path = "contact";
        entry.value = mira::JsonValue{std::string("resolved")};
        const auto patched = workflows.patch_run(started.value().run_id,
                                                 mira::WorkflowPatchId::generate(), {entry});
        if (!patched.has_value() || !patched.value().applied ||
            patched.value().view.run_patch_epoch != 1) {
            return 33;
        }
        const auto decision = workflows.pending_decision_request(started.value().run_id);
        if (!decision.has_value() ||
            decision.value().kind != mira::WorkflowDecisionKind::StepFailure) {
            return 34;
        }
        const auto resolved = workflows.resolve_decision(
            started.value().run_id, decision.value().decision_id,
            decision.value().payload_digest, mira::WorkflowDecisionResolution::Accept);
        if (!resolved.has_value()) {
            return 35;
        }
        const auto settled = workflows.wait_run(started.value().run_id, std::chrono::seconds(10));
        if (!settled.has_value() ||
            settled.value().state != mira::WorkflowRunState::Completed) {
            return 36;
        }

        const auto report = workflows.shutdown();
        if (!report.clean) {
            return 26;
        }
        const auto shutdown = runtime.request_shutdown();
        if (!shutdown.has_value() ||
            !shutdown.value().outcome(std::chrono::seconds(5)).has_value()) {
            return 27;
        }
        if (!runtime.finish_shutdown().clean) {
            return 28;
        }
        if (host_executor.shutdown(true) != executor::ShutdownResult::Completed) {
            return 29;
        }
    }
    return 0;
}
