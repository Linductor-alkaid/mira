// M9-07..M9-10: run-level control over the WorkflowRuntime — pause converges
// at step boundaries with Stale settlement and cursor retention, resume
// re-observes and resolves interrupted steps (re-execute / predicate recovery
// / ExecutionUncertain), cancel is idempotent with late completions isolated,
// takeover suspends runs, terminal runs never revive, and shutdown drains
// without touching the host's executor.

#include "support/m9_support.hpp"

#include <algorithm>
#include <chrono>

namespace {

using namespace mira;
using namespace mira::testing;

// Reads the carrier task id of one run from the recorded events so tests can
// assert the two-view invariant (task terminal <=> run terminal).
[[nodiscard]] std::optional<TaskId> carrier_task(const IEventStore &store,
                                                 const SessionId &session,
                                                 const WorkflowRunId &run_id) {
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            if (envelope.payload.type != "WorkflowRunStarted" || !envelope.task_id.has_value()) {
                continue;
            }
            auto started = parse_workflow_run_started(envelope.payload);
            if (started.has_value() && started.value().run_id == run_id) {
                return envelope.task_id;
            }
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t count_events(const IEventStore &store, const SessionId &session,
                                       const std::string &type) {
    const auto types = session_event_types(store, session);
    return static_cast<std::size_t>(std::count(types.begin(), types.end(), type));
}

int async_drive_waits_to_completion() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("async");
    definition.steps = {tool_step("counter", std::nullopt),
                        tool_step("counter", std::nullopt)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    const auto result = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    // The task settled with the run: both views terminal and compatible.
    const auto task = carrier_task(*fixture.events_, fixture.session_id_, created.value().run_id);
    MIRA_CHECK(task.has_value());
    const auto snapshot = fixture.runtime_->task_snapshot(task.value());
    MIRA_CHECK(snapshot.has_value());
    MIRA_CHECK(is_terminal(snapshot.value().state));
    MIRA_CHECK(run_task_state_compatible(WorkflowRunState::Completed, snapshot.value().state));
    return 0;
}

int pause_settles_stale_and_resume_reexecutes() {
    WorkflowFixture fixture;
    GatedTool gate; // side-effect free: interrupted steps may re-execute
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto counting_environment =
        std::make_shared<ObservationCountingEnvironment>(fixture.environment_);
    WorkflowRuntime observed(fixture.executor_, *fixture.runtime_, fixture.session_id_,
                             counting_environment);
    observed.set_event_store(fixture.events_);
    observed.set_tool_registry(fixture.registry_);

    auto definition = base_definition("pause-resume");
    const auto blocked = tool_step("gate", std::nullopt);
    definition.steps = {blocked, tool_step("gate", std::nullopt)};

    const auto created =
        observed.create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(observed.start_run(created.value().run_id).has_value());

    // Wait until the first gated step is in flight, then pause: the step is
    // cut short at its cooperative safe point and settles Stale.
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto paused = observed.pause_run(created.value().run_id);
    MIRA_CHECK(paused.has_value());
    MIRA_CHECK(paused.value().state == WorkflowRunState::Paused);
    const auto while_paused = observed.run_snapshot(created.value().run_id);
    MIRA_CHECK(while_paused.value().state == WorkflowRunState::Paused);

    const auto stale = observed.wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(stale.has_value());
    MIRA_CHECK(stale.value().state == WorkflowRunState::Paused);
    MIRA_CHECK(stale.value().steps.front().disposition == WorkflowStepDisposition::Stale);

    // Resume: re-observe, then re-execute the interrupted side-effect-free
    // step and finish the run.
    const auto observations_before = counting_environment->observations();
    gate.release.store(true);
    const auto resumed = observed.resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    MIRA_CHECK(resumed.value().state == WorkflowRunState::Running);
    MIRA_CHECK(counting_environment->observations() > observations_before);
    const auto result = observed.wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    // One cancelled entry plus the re-executed step and the second step.
    MIRA_CHECK(gate.entries.load() == 3);
    return 0;
}

int resume_of_uncertain_side_effect_fails_closed() {
    WorkflowFixture fixture;
    GatedTool gate;
    gate.side_effects = true;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("uncertain");
    auto blocked = tool_step("gate", std::nullopt);
    blocked.verification =
        step_result_predicate(blocked.id, WorkflowPredicateOp::Eq, JsonValue{"released"});
    definition.steps = {blocked};

    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto paused = workflow->pause_run(created.value().run_id);
    MIRA_CHECK(paused.has_value());
    MIRA_CHECK(paused.value().state == WorkflowRunState::Paused);

    // The interrupted step dispatched a side effect with no definite result:
    // its verification cannot be evaluated and no fallback is declared, so
    // resume refuses the blind redispatch (RULE-05) and fails the run.
    gate.release.store(true);
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(!resumed.has_value());
    MIRA_CHECK(resumed.error().code == ErrorCode::ExecutionUncertain);
    const auto after = workflow->run_snapshot(created.value().run_id);
    MIRA_CHECK(after.value().state == WorkflowRunState::Failed);
    return 0;
}

int cancel_is_idempotent_and_isolates_late_completions() {
    WorkflowFixture fixture;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("cancel-flow");
    definition.steps = {tool_step("gate", std::nullopt),
                        tool_step("gate", std::nullopt)};

    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto cancelled = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(cancelled.value().state == WorkflowRunState::Cancelled);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Cancelled);
    // The in-flight step settled Stale, not Completed: late completions of a
    // withdrawn run never count.
    MIRA_CHECK(settled.value().steps.front().disposition == WorkflowStepDisposition::Stale);

    // Idempotent cancel: repeating the terminal settlement is a NoOp.
    const auto repeat = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(repeat.has_value());
    MIRA_CHECK(repeat.value().state == WorkflowRunState::Cancelled);
    MIRA_CHECK(count_events(*fixture.events_, fixture.session_id_, "WorkflowRunSettled") == 1);

    // The released gate cannot revive the terminal run.
    gate.release.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto after = workflow->run_snapshot(created.value().run_id);
    MIRA_CHECK(after.value().state == WorkflowRunState::Cancelled);
    const auto task = carrier_task(*fixture.events_, fixture.session_id_, created.value().run_id);
    MIRA_CHECK(task.has_value());
    const auto snapshot = fixture.runtime_->task_snapshot(task.value());
    MIRA_CHECK(snapshot.has_value());
    MIRA_CHECK(snapshot.value().state == TaskState::Cancelled);
    MIRA_CHECK(run_task_state_compatible(WorkflowRunState::Cancelled, snapshot.value().state));
    return 0;
}

int takeover_suspends_and_release_resumes() {
    WorkflowFixture fixture;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("takeover-flow");
    definition.steps = {tool_step("gate", std::nullopt),
                        tool_step("gate", std::nullopt)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Human takeover withdraws the environment: the carrier task suspends and
    // the run view converges into the pause family (DEC-020 §2 mapping).
    const auto takeover = fixture.runtime_->request_human_takeover(fixture.session_id_);
    MIRA_CHECK(takeover.has_value());
    MIRA_CHECK(takeover.value().outcome(std::chrono::seconds(5)).has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (workflow->run_snapshot(created.value().run_id).value().state ==
            WorkflowRunState::Paused) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().state ==
               WorkflowRunState::Paused);

    const auto release = fixture.runtime_->release_human_takeover(fixture.session_id_);
    MIRA_CHECK(release.has_value());
    MIRA_CHECK(release.value().outcome(std::chrono::seconds(5)).has_value());
    gate.release.store(true);
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto result = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    return 0;
}

int shutdown_cancels_drains_and_rejects_producers() {
    WorkflowFixture fixture;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("shutdown-flow");
    definition.steps = {tool_step("gate", std::nullopt)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto report = workflow->shutdown();
    MIRA_CHECK(report.cancelled_runs >= 1);
    MIRA_CHECK(report.drained_drives >= 1);
    MIRA_CHECK(report.clean);
    MIRA_CHECK(workflow->shut_down());
    const auto after = workflow->run_snapshot(created.value().run_id);
    MIRA_CHECK(after.value().state == WorkflowRunState::Cancelled);

    // Producers are rejected after shutdown.
    const auto rejected =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable);
    const auto no_start = workflow->start_run(created.value().run_id);
    MIRA_CHECK(!no_start.has_value());

    // The runtime never shut the host's executor down: it still executes.
    std::atomic<bool> ran{false};
    auto probe = fixture.executor_.submit_auto([&ran] {
        ran.store(true);
        return 0;
    });
    MIRA_CHECK(probe.get() == 0 && ran.load());
    gate.release.store(true);
    return 0;
}

int pause_of_unstarted_and_terminal_runs_fails_closed() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("edge-pause");
    definition.steps = {tool_step("counter", std::nullopt)};

    // Created runs cannot pause (the frozen table has no Created -> Paused).
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto paused = workflow->pause_run(created.value().run_id);
    MIRA_CHECK(!paused.has_value());
    MIRA_CHECK(paused.error().code == ErrorCode::InvalidState);

    // Cancelling the unstarted run takes the legal Created -> Cancelled edge.
    const auto cancelled = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(cancelled.value().state == WorkflowRunState::Cancelled);
    const auto resume_after_cancel = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(!resume_after_cancel.has_value());
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        async_drive_waits_to_completion,
        pause_settles_stale_and_resume_reexecutes,
        resume_of_uncertain_side_effect_fails_closed,
        cancel_is_idempotent_and_isolates_late_completions,
        takeover_suspends_and_release_resumes,
        shutdown_cancels_drains_and_rejects_producers,
        pause_of_unstarted_and_terminal_runs_fails_closed,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
