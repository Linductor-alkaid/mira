#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/task_checkpoint.hpp>

#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] TaskEventLog seeded_log(const RuntimeId &runtime, const SessionId &session,
                                      const TaskId &task) {
    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "open settings"},
                                             {"success_criterion", "settings home visible"}});
    log.add("TaskConstraintAdded", JsonValue::Object{{"key", "no-secrets"},
                                                     {"requirement", "never type credentials"},
                                                     {"safety", true}});
    log.add("TaskObjectiveAdded", JsonValue::Object{{"objective", "navigate to settings"}});
    log.add("TaskFactVerified", JsonValue::Object{{"key", "foreground"}, {"value", "launcher"}});
    log.add("TaskIssueRaised", JsonValue::Object{{"issue", "button occluded"}});
    log.add("TaskStepCompleted", JsonValue::Object{{"step_id", StepId::generate().to_string()},
                                                   {"summary", "scrolled to bottom"}});
    log.add("TaskEpochAdvanced", JsonValue::Object{{"epoch", 2}});
    log.add("EnvironmentEpochChanged", JsonValue::Object{{"epoch", 6}});
    log.add("ObservationRecorded",
            JsonValue::Object{{"observation_id", ObservationId::generate().to_string()},
                              {"digest", digest_string("frame").to_string()}});
    return log;
}

[[nodiscard]] std::string pipe_payload(const TaskId &task, const ActionId &action,
                                       std::uint64_t task_epoch, std::uint64_t environment_epoch,
                                       std::string suffix = {}) {
    std::string data = action.to_string() + "|" + task.to_string() + "|" +
                       std::to_string(task_epoch) + "|" + std::to_string(environment_epoch) + "|" +
                       digest_string("action").to_string() + "|ui|node-1|screen";
    if (!suffix.empty()) {
        data += "|" + suffix;
    }
    return data;
}

int reducer_folds_the_full_vocabulary() {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    const auto action = ActionId::generate();
    log.add_pipe("ActionDispatchStarted", pipe_payload(task, action, 2, 6));
    log.add_pipe("ActionExecutionUncertain", pipe_payload(task, action, 2, 6, "transport lost"));
    log.add("ActionDispatched",
            JsonValue::Object{{"step", 1},
                              {"action", "tap(0.5,0.5)"},
                              {"decision_digest", digest_string("d").to_string()}});
    log.add("VerificationResult", JsonValue::Object{{"step", 1}, {"verdict", "not-satisfied"}});
    log.add("TaskIssueResolved", JsonValue::Object{{"issue", "button occluded"}});
    log.add("TaskObjectiveCompleted", JsonValue::Object{{"objective", "navigate to settings"}});
    log.add("LoopSettled", JsonValue::Object{{"outcome", "Completed"}, {"steps", 1}});

    CheckpointBuilder builder;
    auto built = builder.build(std::nullopt, log.envelopes(), Timestamp::now());
    const auto &checkpoint = built.checkpoint;

    MIRA_CHECK(checkpoint.goal_statement == "open settings");
    MIRA_CHECK(checkpoint.success_criterion == std::optional<std::string>("settings home visible"));
    MIRA_CHECK(checkpoint.constraints.size() == 1);
    MIRA_CHECK(checkpoint.constraints.front().safety);
    MIRA_CHECK(checkpoint.constraints.front().key == "no-secrets");
    MIRA_CHECK(checkpoint.verified_facts.size() == 1);
    MIRA_CHECK(checkpoint.verified_facts.front().key == "foreground");
    MIRA_CHECK(checkpoint.verified_facts.front().value == "launcher");
    MIRA_CHECK(checkpoint.completed_steps.size() == 1);
    MIRA_CHECK(checkpoint.pending_objectives.empty());
    MIRA_CHECK(checkpoint.unresolved_issues.empty());
    MIRA_CHECK(checkpoint.task_epoch == 2);
    MIRA_CHECK(checkpoint.environment_epoch == 6);
    MIRA_CHECK(checkpoint.current_observation.has_value());
    // The uncertain side effect was settled by the later VerificationResult.
    MIRA_CHECK(checkpoint.uncertain_side_effects.empty());
    MIRA_CHECK(checkpoint.recent_actions.size() == 2);
    MIRA_CHECK(checkpoint.terminal_state == std::optional<TaskState>(TaskState::Completed));
    MIRA_CHECK(checkpoint.through_event_sequence ==
               static_cast<std::uint64_t>(log.envelopes().size()));
    MIRA_CHECK(checkpoint.validate().has_value());
    MIRA_CHECK(built.stats.applied == log.envelopes().size());
    MIRA_CHECK(built.stats.ignored == 0);
    MIRA_CHECK(built.stats.malformed == 0);
    return 0;
}

int reducer_keeps_uncertain_side_effects_pinned() {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    const auto action = ActionId::generate();
    log.add_pipe("ActionDispatchStarted", pipe_payload(task, action, 2, 6));
    log.add_pipe("ActionExecutionUncertain", pipe_payload(task, action, 2, 6, "lost receipt"));
    // No VerificationResult follows: the uncertainty stays pinned.
    log.add("TaskStateChanged", JsonValue::Object{{"from", "Acting"}, {"to", "Verifying"}});

    CheckpointBuilder builder;
    auto built = builder.build(std::nullopt, log.envelopes(), Timestamp::now());
    MIRA_CHECK(built.checkpoint.uncertain_side_effects.size() == 1);
    MIRA_CHECK(built.checkpoint.uncertain_side_effects.front().action_id == action);
    MIRA_CHECK(built.checkpoint.uncertain_side_effects.front().reason == "lost receipt");
    MIRA_CHECK(built.checkpoint.verification_pending);
    MIRA_CHECK(!built.checkpoint.terminal_state.has_value());
    return 0;
}

int reducer_skips_unknown_malformed_and_foreign_events() {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    log.add_raw("DiagnosticPing", R"({"note":"not consumed"})");
    log.add_raw("TaskGoalSet", "{not json");
    // An event from another task in the same session is skipped.
    EventEnvelope foreign;
    foreign.event_id = EventId::generate();
    foreign.runtime_id = runtime;
    foreign.session_id = session;
    foreign.task_id = TaskId::generate();
    foreign.session_sequence = 999;
    foreign.timestamp = Timestamp::now();
    foreign.payload =
        EventPayload{"TaskGoalSet", R"({"statement":"other task"})", EventClass::State};
    auto envelopes = log.envelopes();
    envelopes.push_back(foreign);

    CheckpointBuilder builder;
    auto built = builder.build(std::nullopt, envelopes, Timestamp::now());
    MIRA_CHECK(built.stats.ignored == 1);
    MIRA_CHECK(built.stats.malformed == 1);
    MIRA_CHECK(built.stats.stale_skipped == 1);
    MIRA_CHECK(built.checkpoint.goal_statement == "open settings");
    MIRA_CHECK(built.checkpoint.through_event_sequence == envelopes.size() - 1);
    return 0;
}

int incremental_matches_full_rebuild() {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    for (int index = 0; index < 6; ++index) {
        log.add("TaskFactVerified", JsonValue::Object{{"key", "k" + std::to_string(index)},
                                                      {"value", "v" + std::to_string(index)}});
    }

    CheckpointBuilder builder;
    const auto &all = log.envelopes();
    const auto split = all.size() / 2;
    auto first = builder.build(
        std::nullopt, std::vector(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(split)),
        Timestamp::now());
    MIRA_CHECK(first.stats.stale_skipped == 0);
    auto second =
        builder.build(std::optional<TaskCheckpoint>(first.checkpoint), all, Timestamp::now());
    auto full = builder.build(std::nullopt, all, Timestamp::now());

    MIRA_CHECK(second.checkpoint.projection_digest() == full.checkpoint.projection_digest());
    MIRA_CHECK(second.stats.stale_skipped >= split);
    MIRA_CHECK(full.stats.stale_skipped == 0);
    MIRA_CHECK(full.checkpoint.verified_facts.size() == second.checkpoint.verified_facts.size());
    return 0;
}

int memory_store_orders_bounded_and_erases() {
    MemoryCheckpointStore store(4);
    const auto task = TaskId::generate();

    for (int round = 0; round < 6; ++round) {
        TaskCheckpoint checkpoint;
        checkpoint.id = CheckpointId::generate();
        checkpoint.task_id = task;
        checkpoint.session_id = SessionId::generate();
        checkpoint.through_event_sequence = static_cast<std::uint64_t>(round + 1);
        checkpoint.goal_statement = "g" + std::to_string(round);
        MIRA_CHECK(store.put(checkpoint).has_value());
    }
    auto count = store.count(task);
    MIRA_CHECK(count.has_value() && count.value() == 4); // bounded retention

    auto latest = store.latest(task);
    MIRA_CHECK(latest.has_value() && latest.value().has_value());
    MIRA_CHECK(latest.value()->through_event_sequence == 6);

    auto bounded = store.latest_at_or_before(task, 3);
    MIRA_CHECK(bounded.has_value() && bounded.value().has_value());
    MIRA_CHECK(bounded.value()->through_event_sequence == 3);

    auto none = store.latest_at_or_before(task, 0);
    MIRA_CHECK(none.has_value() && !none.value().has_value());

    // Idempotent put of the same id replaces instead of duplicating.
    auto duplicate = latest.value();
    const auto put_again = store.put(duplicate.value());
    MIRA_CHECK(put_again.has_value());
    auto recount = store.count(task);
    MIRA_CHECK(recount.has_value() && recount.value() == 4);

    auto erased = store.erase_task(task, "retention");
    MIRA_CHECK(erased.has_value() && erased.value() == 4);
    MIRA_CHECK(store.erasures() == 1);
    auto empty = store.latest(task);
    MIRA_CHECK(empty.has_value() && !empty.value().has_value());

    TaskCheckpoint invalid;
    invalid.task_id = task;
    MIRA_CHECK(!store.put(invalid).has_value());
    return 0;
}

int coordinator_schedules_by_trigger_policy() {
    MemoryEventStore events;
    MemoryCheckpointStore store;
    CheckpointSchedulePolicy policy;
    policy.min_event_increment = 4;
    CheckpointCoordinator coordinator(events, store, policy);

    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    log.append_to(events);

    // Watermark trigger admits: 11 events exceed the increment policy.
    auto first =
        coordinator.checkpoint(task, session, CheckpointTrigger::Watermark, Timestamp::now());
    MIRA_CHECK(first.has_value() && first.value().has_value());
    MIRA_CHECK(first.value()->through_event_sequence == log.envelopes().size());

    // Nothing new arrived: the watermark trigger is a no-op.
    auto idle =
        coordinator.checkpoint(task, session, CheckpointTrigger::Watermark, Timestamp::now());
    MIRA_CHECK(idle.has_value() && !idle.value().has_value());

    // One incremental event still checkpoints on the explicit Pause trigger.
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = runtime;
    request.session_id = session;
    request.task_id = task;
    request.payload =
        EventPayload{"TaskFactVerified", R"({"key":"battery","value":"low"})", EventClass::State};
    request.required = Durability::ProcessCrash;
    MIRA_CHECK(events.append(request).has_value());

    auto paused = coordinator.checkpoint(task, session, CheckpointTrigger::Pause, Timestamp::now());
    MIRA_CHECK(paused.has_value() && paused.value().has_value());
    MIRA_CHECK(paused.value()->through_event_sequence == log.envelopes().size() + 1);
    MIRA_CHECK(paused.value()->verified_facts.size() == 2);

    auto sequence = coordinator.latest_stored_sequence(task);
    MIRA_CHECK(sequence.has_value() && sequence.value() == log.envelopes().size() + 1);
    return 0;
}

int coordinator_rebuild_validates_and_repairs_projections() {
    MemoryEventStore events;
    MemoryCheckpointStore store;
    CheckpointCoordinator coordinator(events, store, CheckpointSchedulePolicy{});

    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    auto log = seeded_log(runtime, session, task);
    log.append_to(events);

    auto written =
        coordinator.checkpoint(task, session, CheckpointTrigger::Shutdown, Timestamp::now());
    MIRA_CHECK(written.has_value() && written.value().has_value());

    auto consistent = coordinator.rebuild(task, session, Timestamp::now(), false);
    MIRA_CHECK(consistent.has_value());
    MIRA_CHECK(consistent.value().consistent);
    MIRA_CHECK(!consistent.value().repaired);

    // Corrupt the stored projection with a diverging goal.
    auto diverging = written.value().value();
    diverging.goal_statement = "tampered";
    diverging.id = CheckpointId::generate();
    MIRA_CHECK(store.put(diverging).has_value());

    auto diverged = coordinator.rebuild(task, session, Timestamp::now(), false);
    MIRA_CHECK(diverged.has_value());
    MIRA_CHECK(!diverged.value().consistent);

    auto repaired = coordinator.rebuild(task, session, Timestamp::now(), true);
    MIRA_CHECK(repaired.has_value());
    MIRA_CHECK(repaired.value().repaired);
    auto latest = store.latest(task);
    MIRA_CHECK(latest.has_value() && latest.value().has_value());
    MIRA_CHECK(latest.value()->goal_statement == "open settings");
    return 0;
}

int terminal_tasks_are_not_resurrected() {
    MemoryEventStore events;
    MemoryCheckpointStore store;
    CheckpointCoordinator coordinator(events, store, CheckpointSchedulePolicy{});

    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "done soon"}});
    log.add("LoopSettled", JsonValue::Object{{"outcome", "Completed"}});
    log.append_to(events);

    auto first = coordinator.checkpoint(task, session, CheckpointTrigger::Pause, Timestamp::now());
    MIRA_CHECK(first.has_value() && first.value().has_value());
    MIRA_CHECK(first.value()->terminal_state == std::optional<TaskState>(TaskState::Completed));

    // Late runtime triggers on a terminal task produce nothing new.
    auto late = coordinator.checkpoint(task, session, CheckpointTrigger::Pause, Timestamp::now());
    MIRA_CHECK(late.has_value() && !late.value().has_value());
    auto count = store.count(task);
    MIRA_CHECK(count.has_value() && count.value() == 1);
    return 0;
}

} // namespace

int main() {
    if (reducer_folds_the_full_vocabulary() != 0) {
        return 1;
    }
    if (reducer_keeps_uncertain_side_effects_pinned() != 0) {
        return 1;
    }
    if (reducer_skips_unknown_malformed_and_foreign_events() != 0) {
        return 1;
    }
    if (incremental_matches_full_rebuild() != 0) {
        return 1;
    }
    if (memory_store_orders_bounded_and_erases() != 0) {
        return 1;
    }
    if (coordinator_schedules_by_trigger_policy() != 0) {
        return 1;
    }
    if (coordinator_rebuild_validates_and_repairs_projections() != 0) {
        return 1;
    }
    if (terminal_tasks_are_not_resurrected() != 0) {
        return 1;
    }
    return 0;
}
