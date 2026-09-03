#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/task_checkpoint.hpp>
#include <mira/task_recovery.hpp>

#include <executor/executor.hpp>

#include <memory>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

int no_state_when_nothing_is_durable() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(events, checkpoints);
    auto outcome = planner.plan(TaskId::generate(), SessionId::generate(), Timestamp::now());
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::NoState);
    MIRA_CHECK(!outcome.value().checkpoint.has_value());
    return 0;
}

int crash_after_checkpoint_replays_increments() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "long running goal"}});
    log.add("TaskConstraintAdded",
            JsonValue::Object{{"key", "safety"}, {"requirement", "stay in app"}, {"safety", true}});
    log.add("TaskObjectiveAdded", JsonValue::Object{{"objective", "phase one"}});
    const std::size_t checkpoint_boundary = log.envelopes().size();

    // Events after the checkpoint boundary arrive only in the EventStore.
    log.add("TaskFactVerified", JsonValue::Object{{"key", "page"}, {"value", "home"}});
    log.add("TaskStepCompleted", JsonValue::Object{{"step_id", StepId::generate().to_string()},
                                                   {"summary", "opened drawer"}});
    log.add("TaskEpochAdvanced", JsonValue::Object{{"epoch", 3}});
    log.append_to(events);

    CheckpointBuilder builder;
    auto stored = builder.build(
        std::nullopt, log.up_to(static_cast<std::uint64_t>(checkpoint_boundary)), Timestamp::now());
    MIRA_CHECK(checkpoints.put(stored.checkpoint).has_value());

    RecoveryPlanner planner(events, checkpoints);
    auto outcome = planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::ResumeObserving);
    MIRA_CHECK(outcome.value().checkpoint.has_value());
    // The projection includes both the checkpoint base and the increments.
    MIRA_CHECK(outcome.value().checkpoint->goal_statement == "long running goal");
    MIRA_CHECK(outcome.value().checkpoint->constraints.size() == 1);
    MIRA_CHECK(outcome.value().checkpoint->pending_objectives.size() == 1);
    MIRA_CHECK(outcome.value().checkpoint->verified_facts.size() == 1);
    MIRA_CHECK(outcome.value().checkpoint->completed_steps.size() == 1);
    MIRA_CHECK(outcome.value().task_epoch == 3);
    MIRA_CHECK(outcome.value().durable_sequence == log.envelopes().size());
    MIRA_CHECK(outcome.value().stats.applied >= 3);
    return 0;
}

int uncertain_side_effects_resume_to_observe_first() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    const auto action = ActionId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "verify dispatch outcome"}});
    log.add_pipe("ActionDispatchStarted", action.to_string() + "|" + task.to_string() + "|1|1|" +
                                              digest_string("a").to_string() + "|ui|node|screen");
    log.add_pipe("ActionExecutionUncertain", action.to_string() + "|" + task.to_string() + "|1|1|" +
                                                 digest_string("a").to_string() +
                                                 "|ui|node|screen|receipt lost");
    log.append_to(events);

    RecoveryPlanner planner(events, checkpoints);
    auto outcome = planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::ResumeObserving);
    MIRA_CHECK(outcome.value().pending_side_effects.size() == 1);
    MIRA_CHECK(outcome.value().pending_side_effects.front().action_id == action);
    MIRA_CHECK(outcome.value().checkpoint->uncertain_side_effects.size() == 1);
    return 0;
}

int awaiting_verification_resumes_to_verifying() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    const auto action = ActionId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "check result"}});
    // Dispatch started then receipt received, but no VerificationResult ran.
    log.add_pipe("ActionDispatchStarted", action.to_string() + "|" + task.to_string() + "|1|1|" +
                                              digest_string("a").to_string() + "|ui|node|screen");
    log.add_pipe("ActionReceipt", action.to_string() + "|" + task.to_string() + "|1|1|" +
                                      digest_string("a").to_string() + "|ui|node|screen|completed");
    log.append_to(events);

    RecoveryPlanner planner(events, checkpoints);
    auto outcome = planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::ResumeVerifying);
    MIRA_CHECK(outcome.value().pending_side_effects.empty());
    return 0;
}

int terminal_tasks_stay_terminal() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "finish"}});
    log.add("TaskStateChanged", JsonValue::Object{{"from", "Verifying"}, {"to", "Completed"}});
    log.append_to(events);

    RecoveryPlanner planner(events, checkpoints);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto outcome = planner.plan(task, session, Timestamp::now());
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().action == RecoveryAction::AlreadyTerminal);
        MIRA_CHECK(outcome.value().checkpoint->terminal_state ==
                   std::optional<TaskState>(TaskState::Completed));
    }
    return 0;
}

int checkpoints_newer_than_durable_are_ignored() {
    // The checkpoint store saw events that the recovered EventStore no longer
    // has; only projections at or below the durable sequence are usable.
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    TaskEventLog full_log(runtime, session, task);
    full_log.add("TaskGoalSet", JsonValue::Object{{"statement", "durable boundary"}});
    full_log.add("TaskFactVerified", JsonValue::Object{{"key", "a"}, {"value", "1"}});
    full_log.add("TaskFactVerified", JsonValue::Object{{"key", "b"}, {"value", "2"}});
    CheckpointBuilder builder;
    auto ahead = builder.build(std::nullopt, full_log.envelopes(), Timestamp::now());

    MemoryEventStore truncated_events;
    TaskEventLog truncated_log(runtime, session, task);
    truncated_log.add("TaskGoalSet", JsonValue::Object{{"statement", "durable boundary"}});
    truncated_log.add("TaskFactVerified", JsonValue::Object{{"key", "a"}, {"value", "1"}});
    truncated_log.append_to(truncated_events);

    MemoryCheckpointStore checkpoints;
    MIRA_CHECK(checkpoints.put(ahead.checkpoint).has_value());

    RecoveryPlanner planner(truncated_events, checkpoints);
    auto outcome = planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::ResumeObserving);
    MIRA_CHECK(outcome.value().durable_sequence == 2);
    // The projection was rebuilt from the durable prefix, not from the
    // checkpoint that claimed knowledge of sequence 3.
    MIRA_CHECK(outcome.value().checkpoint->through_event_sequence == 2);
    MIRA_CHECK(outcome.value().checkpoint->verified_facts.size() == 1);
    MIRA_CHECK(outcome.value().checkpoint->verified_facts.front().key == "a");
    return 0;
}

int foreign_checkpoint_projection_is_rejected() {
    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "belongs elsewhere"}});
    log.append_to(events);

    TaskCheckpoint foreign;
    foreign.id = CheckpointId::generate();
    foreign.task_id = task;
    foreign.session_id = SessionId::generate(); // different session
    foreign.through_event_sequence = 1;
    MIRA_CHECK(checkpoints.put(foreign).has_value());

    RecoveryPlanner planner(events, checkpoints);
    auto outcome = planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(!outcome.has_value());
    MIRA_CHECK(outcome.error().code == ErrorCode::InvalidState);
    return 0;
}

int recovery_and_checkpoints_run_under_executor_management() {
    // AGENTS.md: Mira-originated work runs on Executor tasks with consumed
    // futures. The planner and coordinator are synchronous components hosted
    // by those tasks; this test exercises that contract.
    executor::Executor exec;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 16;
    MIRA_CHECK(exec.initialize(config));

    MemoryEventStore events;
    MemoryCheckpointStore checkpoints;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "executor hosted"}});
    for (int index = 0; index < 8; ++index) {
        log.add("TaskFactVerified", JsonValue::Object{{"key", "k" + std::to_string(index)},
                                                      {"value", "v" + std::to_string(index)}});
    }
    log.append_to(events);

    auto build = exec.submit_auto([&events, &checkpoints, task, session]() {
        CheckpointSchedulePolicy policy;
        policy.min_event_increment = 2;
        CheckpointCoordinator coordinator(events, checkpoints, policy);
        return coordinator.checkpoint(task, session, CheckpointTrigger::Watermark,
                                      Timestamp::now());
    });
    const auto built = build.get();
    MIRA_CHECK(built.has_value() && built.value().has_value());

    auto plan = exec.submit_auto([&events, &checkpoints, task, session]() {
        RecoveryPlanner planner(events, checkpoints);
        return planner.plan(task, session, Timestamp::now());
    });
    const auto outcome = plan.get();
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().action == RecoveryAction::ResumeObserving);
    MIRA_CHECK(outcome.value().checkpoint->verified_facts.size() == 8);

    // Concurrent store writes from separate Executor tasks stay serialized.
    std::vector<std::future<Result<void>>> writers;
    for (int index = 0; index < 4; ++index) {
        writers.push_back(exec.submit_auto([&checkpoints, task, index]() {
            TaskCheckpoint checkpoint;
            checkpoint.id = CheckpointId::generate();
            checkpoint.task_id = task;
            checkpoint.session_id = SessionId::generate();
            checkpoint.through_event_sequence = static_cast<std::uint64_t>(100 + index);
            return checkpoints.put(checkpoint);
        }));
    }
    for (auto &writer : writers) {
        MIRA_CHECK(writer.get().has_value());
    }
    auto count = checkpoints.count(task);
    MIRA_CHECK(count.has_value() && count.value() == 5);

    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    if (no_state_when_nothing_is_durable() != 0) {
        return 1;
    }
    if (crash_after_checkpoint_replays_increments() != 0) {
        return 1;
    }
    if (uncertain_side_effects_resume_to_observe_first() != 0) {
        return 1;
    }
    if (awaiting_verification_resumes_to_verifying() != 0) {
        return 1;
    }
    if (terminal_tasks_stay_terminal() != 0) {
        return 1;
    }
    if (checkpoints_newer_than_durable_are_ignored() != 0) {
        return 1;
    }
    if (foreign_checkpoint_projection_is_rejected() != 0) {
        return 1;
    }
    if (recovery_and_checkpoints_run_under_executor_management() != 0) {
        return 1;
    }
    return 0;
}
