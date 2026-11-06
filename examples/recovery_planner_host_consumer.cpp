#include "journaled_simulator_host.hpp"
#include <kairo/executor.hpp>
#include <mira/state_store.hpp>
#include <mira/task_recovery.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>

namespace {
using namespace mira;
using namespace std::chrono_literals;
enum class Mode {
    StartedWithoutReceipt,
    Uncertain,
    CompletedWithoutVerify,
    Terminal,
    Empty,
    Clean
};

bool check(bool value, const char *label) {
    if (!value)
        std::cerr << "recovery host failed: " << label << '\n';
    return value;
}

bool append(IEventStore &events, RuntimeId runtime, SessionId session, TaskId task,
            std::string type, JsonValue data) {
    auto receipt = events.append({EventId::generate(),
                                  runtime,
                                  session,
                                  task,
                                  {std::move(type), to_json_string(data), EventClass::State},
                                  0,
                                  Durability::ProcessCrash});
    return receipt && (receipt.value().achieved == Durability::ProcessCrash ||
                       receipt.value().achieved == Durability::PowerLoss);
}

bool run_case(kairo::Executor &executor, const std::filesystem::path &root, Mode mode) {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>(
        adapters::simulator::SimulatorSetup::single_display());
    SqliteStoreOptions options;
    options.path = root / "checkpoints.db";
    std::uint64_t base_sequence = 0;
    {
        auto events = std::make_shared<FileEventStore>(root / "events");
        if (!check(events->recover({}).has_value(), "seed event store"))
            return false;
        auto checkpoints = SqliteCheckpointStore::open(executor, options);
        if (!check(checkpoints.has_value(), "seed checkpoint store"))
            return false;
        auto seeded = executor.submit_auto([&] {
            if (mode == Mode::Empty)
                return true;
            if (!append(
                    *events, runtime, session, task, "TaskGoalSet",
                    JsonValue::Object{{"statement", "inspect before considering any replay"}}) ||
                !append(*events, runtime, session, task, "TaskEpochAdvanced",
                        JsonValue::Object{{"epoch", 1}}) ||
                !append(*events, runtime, session, task, "EnvironmentEpochChanged",
                        JsonValue::Object{{"epoch", static_cast<std::int64_t>(
                                                        environment->environment_epoch())}}))
                return false;
            auto page = events->read({session, std::nullopt, 16});
            if (!page)
                return false;
            auto base =
                CheckpointBuilder{}.build(std::nullopt, page.value().events, Timestamp::now());
            base_sequence = base.checkpoint.through_event_sequence;
            if (!checkpoints.value()->put(base.checkpoint))
                return false;
            OperationContext context;
            context.session = session;
            context.task = task;
            context.task_epoch = 1;
            context.operation = OperationId::generate();
            context.started_at = Timestamp::now();
            context.deadline = context.started_at.monotonic + 5s;
            if (mode == Mode::CompletedWithoutVerify) {
                // Real simulator dispatch through the existing journal boundary.
                auto dispatcher = mira::examples::simulator_dispatcher(
                    runtime, session, task, 1, environment, events, std::make_shared<std::mutex>());
                InputSequence input;
                input.events.push_back({"back", "", 0});
                auto receipt = dispatcher->dispatch(*environment, input, context,
                                                    environment->environment_epoch());
                return receipt && receipt.value().status == ExecutionStatus::Completed;
            }
            if (mode == Mode::Clean)
                return true;
            // Fault fixture: durable start, then stop before the actual input.
            // A reader cannot infer from the absent receipt whether input ran.
            ActionIntent intent{ActionId::generate(),
                                runtime,
                                session,
                                task,
                                1,
                                environment->environment_epoch(),
                                digest_string("fixture-back"),
                                {"simulator", "primary", "fixture"}};
            ActionJournal journal(runtime, *events);
            if (!journal.prepare(intent) || !journal.dispatch_started(intent))
                return false;
            if (mode == Mode::StartedWithoutReceipt)
                return true;
            if (!journal.execution_uncertain(intent, "fixture interruption without receipt"))
                return false;
            if (mode == Mode::Terminal)
                return append(*events, runtime, session, task, "TaskStateChanged",
                              JsonValue::Object{{"from", "Cancelling"}, {"to", "Cancelled"}});
            return true;
        });
        if (!check(seeded.get(), "seed durable prefix") ||
            !check(checkpoints.value()->close().has_value(), "close seed checkpoint store"))
            return false;
    } // Close both stores before reopening; this is a file reopen fixture, not an OS kill.

    FileEventStore events(root / "events");
    if (!check(events.recover({}).has_value(), "reopen event store"))
        return false;
    auto checkpoints = SqliteCheckpointStore::open(executor, options);
    if (!check(checkpoints.has_value(), "reopen checkpoint store"))
        return false;
    auto consumed = executor.submit_auto([&] {
        RecoveryPlanner planner(events, *checkpoints.value());
        auto recovery = planner.plan(task, session, Timestamp::now());
        if (!check(recovery.has_value(), "planner after reopen"))
            return false;
        if (recovery.value().action == RecoveryAction::NoState)
            return mode == Mode::Empty && environment->executed_inputs().empty();
        if (recovery.value().action == RecoveryAction::AlreadyTerminal)
            return mode == Mode::Terminal && recovery.value().checkpoint &&
                   recovery.value().checkpoint->terminal_state == TaskState::Cancelled &&
                   recovery.value().pending_side_effects.size() == 1 &&
                   environment->executed_inputs().empty(); // Never observe/execute a terminal task.
        if (!check(recovery.value().checkpoint &&
                       recovery.value().durable_sequence >= base_sequence,
                   "recovered checkpoint boundary"))
            return false;
        const bool uncertain = mode == Mode::Uncertain;
        const bool verifying =
            mode == Mode::CompletedWithoutVerify || mode == Mode::StartedWithoutReceipt;
        if (!check(recovery.value().pending_side_effects.size() == (uncertain ? 1U : 0U) &&
                       recovery.value().action == (verifying ? RecoveryAction::ResumeVerifying
                                                             : RecoveryAction::ResumeObserving),
                   "recovery action and pin"))
            return false;
        const auto inputs_before = environment->executed_inputs().size();
        OperationContext context;
        context.session = session;
        context.task = task;
        context.task_epoch = recovery.value().task_epoch;
        context.operation =
            OperationId::generate(); // New read-only operation, not old authorization.
        context.started_at = Timestamp::now();
        context.deadline = context.started_at.monotonic + 5s;
        ObservationRequest request;
        request.required.screen = true;
        auto observation = environment->observe(request, context);
        if (!observation || !observation.value().screen)
            return false;
        // A screenshot exists, but does not prove the prior 'back' goal. Keep
        // AwaitingExternalVerification for both uncertainty and missing Verify.
        // In particular, do NOT emit VerificationResult: it would clear pins.
        const auto metadata = to_json_string(JsonValue::Object{
            {"width", static_cast<std::int64_t>(observation.value().screen->value.width_pixels)},
            {"height",
             static_cast<std::int64_t>(observation.value().screen->value.height_pixels)}});
        if (!append(events, runtime, session, task, "ObservationRecorded",
                    JsonValue::Object{{"observation_id", observation.value().id.to_string()},
                                      {"digest", digest_string(metadata).to_string()}}))
            return false;
        auto after = planner.plan(task, session, Timestamp::now());
        if (!after || !after.value().checkpoint ||
            after.value().pending_side_effects.size() !=
                recovery.value().pending_side_effects.size() ||
            after.value().checkpoint->verification_pending !=
                recovery.value().checkpoint->verification_pending ||
            !after.value().checkpoint->current_observation ||
            after.value().durable_sequence <= recovery.value().durable_sequence ||
            environment->executed_inputs().size() != inputs_before ||
            inputs_before != (mode == Mode::CompletedWithoutVerify ? 1U : 0U))
            return false;
        if (!checkpoints.value()->put(*after.value().checkpoint))
            return false;
        auto persisted = checkpoints.value()->latest(task);
        if (!persisted || !persisted.value() ||
            persisted.value()->projection_digest() != after.value().checkpoint->projection_digest())
            return false;
        std::cout << "recovery host: " << recovery_action_name(recovery.value().action)
                  << " -> fresh Observe -> "
                  << (uncertain || verifying ? "AwaitingExternalVerification" : "ReadyForNewPlan")
                  << "; no input replay\n";
        return true;
    });
    const bool ok = consumed.get();
    return check(ok, "recovery disposition consumed") &&
           check(checkpoints.value()->close().has_value(), "close recovered checkpoint store");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-recovery-host-" + SessionId::generate().to_string());
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    if (!executor.initialize(config))
        return 1;
    bool ok = true;
    try {
        for (const auto &[mode, name] :
             {std::pair{Mode::StartedWithoutReceipt, "started"},
              std::pair{Mode::Uncertain, "uncertain"},
              std::pair{Mode::CompletedWithoutVerify, "receipt"},
              std::pair{Mode::Terminal, "terminal"}, std::pair{Mode::Empty, "empty"},
              std::pair{Mode::Clean, "clean"}}) {
            if (!run_case(executor, root / name, mode)) {
                ok = false;
                break;
            }
        }
    } catch (...) {
        std::cerr << "recovery host task/submission failed\n";
        ok = false;
    }
    if (executor.shutdown(true) != kairo::ShutdownResult::Completed)
        ok = false;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return ok && !error ? 0 : 1;
}
