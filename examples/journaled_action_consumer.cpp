#include "journaled_simulator_host.hpp"
// Offline host composition: durable input journal + exclusive host lease +
// Executor-owned bounded dispatch. Only the simulator receives input.
#include <mira/action_dispatcher.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/task_recovery.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>

namespace {
using namespace mira;
using namespace mira::adapters::simulator;

int run(kairo::Executor &executor, const std::filesystem::path &root) {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto environment = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
    auto store = std::make_shared<FileEventStore>(root);
    if (!store->recover({})) {
        return 1;
    }
    // This minimal host reserves one simulated input resource for a fixed
    // task/epoch. A real host must integrate its authority, revocation,
    // cancellation + interrupt bridge and task lifecycle here.
    auto resource = std::make_shared<std::mutex>();
    std::atomic<bool> cancelled{false};
    auto dispatcher = mira::examples::simulator_dispatcher(runtime, session, task, 1, environment,
                                                           store, resource);
    OperationContext context;
    context.session = session;
    context.task = task;
    context.task_epoch = 1;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + std::chrono::seconds(5);
    context.cancellation_requested = [&] { return cancelled.load(); };
    InputSequence input;
    input.events.push_back({"back", "", 0});
    auto future = executor.submit_auto([&] {
        return dispatcher->dispatch(*environment, input, context, environment->environment_epoch());
    });
    auto receipt = future.get();
    if (!receipt || receipt.value().status != ExecutionStatus::Completed ||
        environment->executed_inputs().size() != 1) {
        return 1;
    }
    // Reopen the file store through a second reader after committed writes.
    // No Verify event was appended: recovery must request fresh verification,
    // and must not execute another simulated input.
    FileEventStore reopened(root);
    if (!reopened.recover({})) {
        return 1;
    }
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(reopened, checkpoints);
    auto recovery = planner.plan(task, session, Timestamp::now());
    if (!recovery || recovery.value().action != RecoveryAction::ResumeVerifying ||
        environment->executed_inputs().size() != 1) {
        return 1;
    }
    std::cout << "journaled input: one simulated dispatch; recovery requires verification\n";
    return 0;
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-journaled-consumer-" + mira::SessionId::generate().to_string());
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config)) {
        return 1;
    }
    int result = 1;
    try {
        result = run(executor, root);
    } catch (...) {
        std::cerr << "journaled input host failed\n";
    }
    (void)executor.shutdown(true);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}
