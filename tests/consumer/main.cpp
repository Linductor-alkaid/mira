#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>

#include <chrono>
#include <filesystem>
#include <system_error>

#include <executor/executor.hpp>

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
    return 0;
}
