#include <mira/adapters/net/socket_transport.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/workflow_ir.hpp>
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
    return 0;
}
