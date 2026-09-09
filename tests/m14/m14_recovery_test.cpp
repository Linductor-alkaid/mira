// M14 (DEC-031): the WorkflowRecoveryOrchestrator test matrix — decision
// matrix, lesson filtering matrix, race matrix, budget matrix, shutdown
// matrix, the end-to-end lesson-reuse scenario and the security negatives.

#include "support/m14_support.hpp"

#include <mira/workflow_recovery.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] OperationContext run_context(const RecoveryFixture &fixture) {
    OperationContext context;
    context.session = fixture.fixture_.session_id_;
    context.started_at = Timestamp::now();
    return context;
}

// One Recoverable definition whose single ToolCall step fails the given
// dispatches of the shared scripted tool.
[[nodiscard]] WorkflowDefinition escalate_definition(WorkflowPolicy policy) {
    auto definition = base_definition("m14-recovery");
    definition.default_policy = policy;
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

[[nodiscard]] const std::string &context_text(const ModelRequest &request) {
    return std::get<TextPart>(request.input.back().content.back()).text;
}

// Extracts the JSON context the orchestrator assembled (after the fixed
// prefix of the user turn).
[[nodiscard]] JsonValue parse_context(const ModelRequest &request) {
    const auto &text = context_text(request);
    const auto marker = text.find('\n');
    MIRA_CHECK(marker != std::string::npos);
    auto parsed = parse_json(text.substr(marker + 1));
    MIRA_CHECK(parsed.has_value());
    return parsed.value();
}

int continuation_exposes_recovery_identity() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                     tool.registration("scripted"))
                   .has_value());
    auto definition = escalate_definition(WorkflowPolicy::Recoverable);
    const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    const auto continuation = fixture.workflow_->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value());
    // Incremental identity fields (DEC-031 §7) surface the run-record state.
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    MIRA_CHECK(snapshot.has_value());
    MIRA_CHECK(continuation.value().run_epoch == snapshot.value().run_epoch);
    MIRA_CHECK(continuation.value().escalations >= 1);
    const auto task =
        fixture.fixture_.runtime_->task_snapshot(continuation.value().carrier_task_id);
    MIRA_CHECK(task.has_value());
    MIRA_CHECK(task.value().state == TaskState::Recovering);
    MIRA_CHECK(continuation.value().carrier_task_epoch == task.value().epoch);
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int decision_matrix_four_actions_reach_the_runtime_exits() {
    // patch_and_resume
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        const auto continuation = fixture.workflow_->agent_continuation(run.value().run_id);
        MIRA_CHECK(continuation.has_value());
        fixture.provider_->add_response(
            text_response(skip_step_decision(continuation.value().current_step->to_string(),
                                             "skip the failing step")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::PatchedAndResumed);
        MIRA_CHECK(attempt.value().patch_id.has_value());
        MIRA_CHECK(attempt.value().decision_digest.has_value());
        MIRA_CHECK(attempt.value().model_request_id.has_value());
        const auto settled =
            fixture.workflow_->wait_run(run.value().run_id, std::chrono::seconds(10));
        MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // resume (the second dispatch must succeed for the run to complete)
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(resume_decision("retry")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::ResumedWithoutPatch);
        MIRA_CHECK(!attempt.value().patch_id.has_value());
        const auto settled =
            fixture.workflow_->wait_run(run.value().run_id, std::chrono::seconds(10));
        MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // cancel
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        const auto run = fixture.workflow_->create_run(escalate_definition(
                                                           WorkflowPolicy::Recoverable),
                                                       JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(cancel_decision("unrecoverable")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::CancelRequested);
        const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
        MIRA_CHECK(snapshot.has_value() &&
                   snapshot.value().state == WorkflowRunState::Cancelled);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // need_user
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        const auto run = fixture.workflow_->create_run(escalate_definition(
                                                           WorkflowPolicy::Recoverable),
                                                       JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(need_user_decision("host must decide")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::DeferredToHost);
        MIRA_CHECK(attempt.value().reason_code == "decision-need-user");
        const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
        MIRA_CHECK(snapshot.has_value() &&
                   snapshot.value().state == WorkflowRunState::WaitingAgent);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    return 0;
}

int malformed_decisions_repair_then_defer() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    // First response malformed, repair round answers legally.
    fixture.provider_->add_response(text_response("this is not json"));
    const auto continuation = fixture.workflow_->agent_continuation(run.value().run_id);
    fixture.provider_->add_response(
        text_response(resume_decision("recovered after repair")));
    auto orchestrator = fixture.make_orchestrator();
    const auto repaired = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(repaired.has_value());
    MIRA_CHECK(repaired.value().outcome == WorkflowRecoveryOutcome::ResumedWithoutPatch);
    MIRA_CHECK(fixture.provider_->consumed() == 2);
    static_cast<void>(orchestrator->shutdown());

    // Budget 1 exhausted: two malformed responses defer with one repair.
    RecoveryFixture second;
    ScriptedTool again{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*second.fixture_.registry_, again.registration("scripted"))
                   .has_value());
    const auto run_b =
        second.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                     JsonValue{}, std::nullopt);
    MIRA_CHECK(run_b.has_value());
    MIRA_CHECK(second.workflow_
                   ->execute_run(run_b.value().run_id, run_context(second))
                   .has_value());
    second.provider_->add_response(text_response("still not json"));
    second.provider_->add_response(text_response("{\"action\":\"resume\"}")); // no rationale
    auto exhausted = second.make_orchestrator();
    const auto deferred = exhausted->attempt_recovery(run_b.value().run_id);
    MIRA_CHECK(deferred.has_value());
    MIRA_CHECK(deferred.value().outcome == WorkflowRecoveryOutcome::DeferredToHost);
    MIRA_CHECK(deferred.value().reason_code == "decision-invalid");
    MIRA_CHECK(second.provider_->consumed() == 2);
    const auto snapshot = second.workflow_->run_snapshot(run_b.value().run_id);
    MIRA_CHECK(snapshot.has_value() &&
               snapshot.value().state == WorkflowRunState::WaitingAgent);
    static_cast<void>(exhausted->shutdown());
    static_cast<void>(second.workflow_->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int refused_decisions_never_repair() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->add_response(refused_response());
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(attempt.has_value());
    MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::DeferredToHost);
    MIRA_CHECK(attempt.value().reason_code == "decision-invalid");
    MIRA_CHECK(fixture.provider_->consumed() == 1);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int unavailable_models_abort_without_touching_the_run() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    // No scripted responses: the provider is exhausted (model unavailable).
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(attempt.has_value());
    MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(attempt.value().reason_code == "model-unavailable");
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    MIRA_CHECK(snapshot.has_value() &&
               snapshot.value().state == WorkflowRunState::WaitingAgent);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int lesson_matrix_counts_filtering_layers() {
    // Empty retrieval (no learning context): the attempt proceeds normally.
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        const auto run = fixture.workflow_->create_run(escalate_definition(
                                                           WorkflowPolicy::Recoverable),
                                                       JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(resume_decision("retry")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().lessons_offered == 0);
        MIRA_CHECK(attempt.value().lessons_kept == 0);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // Stale (version drift) and unparseable lessons drop with counters;
    // valid ones keep their statement byte-identical in the context.
    {
        RecoveryFixture fixture;
        auto memory = std::make_shared<FakeLearningMemory>();
        MIRA_CHECK(
            fixture.workflow_->set_learning_context(memory, learning_scope()).has_value());
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const std::string step_id = definition.steps[0].id.to_string();

        // Stale: same workflow id, different ir digest.
        WorkflowEpisodeRecord stale;
        stale.run_id = WorkflowRunId::generate().to_string();
        stale.workflow_id = definition.workflow_id.to_string();
        stale.ir_digest = digest_string("stale").to_string();
        stale.policy = "recoverable";
        stale.outcome = "failed";
        stale.failed_step_id = step_id;
        stale.failure_reason_code = "mira.test:77";
        stale.recorded_at_ms = 1;
        MIRA_CHECK(memory
                       ->apply(mutation_of(episode_to_memory_record(
                                    stale, learning_scope(), {EventId::generate()},
                                    std::chrono::system_clock::now())))
                       .has_value());

        // Unparseable: a structurally valid record whose statement is not
        // a learning contract (the retrieval terms still match).
        WorkflowEpisodeRecord unparseable;
        unparseable.run_id = WorkflowRunId::generate().to_string();
        unparseable.workflow_id = definition.workflow_id.to_string();
        unparseable.ir_digest = digest_string("stale").to_string();
        unparseable.policy = "recoverable";
        unparseable.outcome = "failed";
        unparseable.failed_step_id = step_id;
        unparseable.failure_reason_code = "mira.test:77";
        unparseable.recorded_at_ms = 1;
        auto garbage = episode_to_memory_record(unparseable, learning_scope(),
                                                {EventId::generate()},
                                                std::chrono::system_clock::now());
        garbage.statement = "{\"workflow_id\":\"" + definition.workflow_id.to_string() +
                            "\",\"step_id\":\"" + step_id + "\",\"junk\":true}";
        MIRA_CHECK(memory->apply(mutation_of(std::move(garbage))).has_value());

        // Valid episode with the exact current digest: the test seeds it
        // after creation, so the digest matches by construction below.
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        const std::string live_digest = run.value().ir_digest.to_string();
        WorkflowEpisodeRecord fresh;
        fresh.run_id = WorkflowRunId::generate().to_string();
        fresh.workflow_id = definition.workflow_id.to_string();
        fresh.ir_digest = live_digest;
        fresh.policy = "recoverable";
        fresh.outcome = "failed";
        fresh.failed_step_id = step_id;
        fresh.failure_reason_code = "mira.test:77";
        fresh.recorded_at_ms = 2;
        const auto fresh_record = episode_to_memory_record(
            fresh, learning_scope(), {EventId::generate()}, std::chrono::system_clock::now());
        MIRA_CHECK(memory->apply(mutation_of(fresh_record)).has_value());

        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(resume_decision("retry")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().lessons_offered >= 3);
        MIRA_CHECK(attempt.value().lessons_stale == 1);
        MIRA_CHECK(attempt.value().lessons_unparseable == 1);
        MIRA_CHECK(attempt.value().lessons_kept == 1);
        // Kept statement is byte-identical to the original record.
        const auto requests = fixture.provider_->requests();
        MIRA_CHECK(!requests.empty());
        const auto context = parse_context(requests.front());
        const auto *lessons = context.find("relevant_lessons");
        MIRA_CHECK(lessons != nullptr && lessons->is_array());
        MIRA_CHECK(lessons->as_array()->size() == 1);
        MIRA_CHECK(*lessons->as_array()->front().as_string() == fresh_record.statement);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // Budget truncation keeps runtime return order.
    {
        RecoveryFixture fixture;
        auto memory = std::make_shared<FakeLearningMemory>();
        MIRA_CHECK(
            fixture.workflow_->set_learning_context(memory, learning_scope()).has_value());
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const std::string step_id = definition.steps[0].id.to_string();
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        for (int serial = 0; serial < 2; ++serial) {
            WorkflowEpisodeRecord episode;
            episode.run_id = WorkflowRunId::generate().to_string();
            episode.workflow_id = definition.workflow_id.to_string();
            episode.ir_digest = run.value().ir_digest.to_string();
            episode.policy = "recoverable";
            episode.outcome = "failed";
            episode.failed_step_id = step_id;
            episode.failure_reason_code = "mira.test:77";
            episode.recorded_at_ms = static_cast<std::uint64_t>(serial + 1);
            MIRA_CHECK(memory
                           ->apply(mutation_of(episode_to_memory_record(
                                        episode, learning_scope(), {EventId::generate()},
                                        std::chrono::system_clock::now())))
                           .has_value());
        }
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(text_response(resume_decision("retry")));
        auto orchestrator = fixture.make_orchestrator([](WorkflowRecoveryConfig &config) {
            config.max_lessons_in_context = 1;
        });
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().lessons_offered == 2);
        MIRA_CHECK(attempt.value().lessons_kept == 1);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    return 0;
}

int cancellation_mid_request_aborts_and_keeps_the_run() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->block();
    auto orchestrator = fixture.make_orchestrator();
    MIRA_CHECK(orchestrator->start_recovery(run.value().run_id).has_value());
    fixture.provider_->wait_entered();
    // A duplicate notification while the attempt is in flight is absorbed.
    const auto duplicate = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(duplicate.has_value());
    MIRA_CHECK(duplicate.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(duplicate.value().reason_code == "attempt-in-progress");
    MIRA_CHECK(orchestrator->cancel_recovery(run.value().run_id).has_value());
    fixture.provider_->release();
    const auto settled = orchestrator->wait_recovery(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(settled.value().reason_code == "cancelled");
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    MIRA_CHECK(snapshot.has_value() &&
               snapshot.value().state == WorkflowRunState::WaitingAgent);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int host_cancellation_drift_discards_the_late_decision() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->block();
    fixture.provider_->add_response(text_response(resume_decision("too late")));
    auto orchestrator = fixture.make_orchestrator();
    MIRA_CHECK(orchestrator->start_recovery(run.value().run_id).has_value());
    fixture.provider_->wait_entered();
    // The host settles the run while the model request is in flight.
    MIRA_CHECK(fixture.workflow_->cancel_run(run.value().run_id).has_value());
    fixture.provider_->release();
    const auto settled = orchestrator->wait_recovery(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(settled.value().reason_code == "run-state-changed");
    // The late decision is discarded: no resume, terminal state not revived.
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    MIRA_CHECK(snapshot.has_value() &&
               snapshot.value().state == WorkflowRunState::Cancelled);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int host_patch_in_flight_does_not_invalidate_a_valid_decision() {
    // run_epoch advances on state transitions only, so a host patch on the
    // WaitingAgent run (immediate application, run_patch_epoch bump) leaves
    // the pre-submit recheck satisfied: the still-valid decision executes.
    // The epoch-advanced exit stays in force as a fail-closed guard for any
    // future path that re-enters WaitingAgent at a new epoch; state drift
    // (the previous scenario) covers the observable race.
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->block();
    fixture.provider_->add_response(text_response(resume_decision("retry")));
    auto orchestrator = fixture.make_orchestrator();
    MIRA_CHECK(orchestrator->start_recovery(run.value().run_id).has_value());
    fixture.provider_->wait_entered();
    const auto continuation = fixture.workflow_->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value() && continuation.value().current_step.has_value());
    WorkflowPatchEntry skip;
    skip.target = WorkflowPatchTarget::StepArguments;
    skip.op = WorkflowPatchOp::Skip;
    skip.path = continuation.value().current_step->to_string();
    const auto patched =
        fixture.workflow_->patch_run(run.value().run_id, WorkflowPatchId::generate(), {skip});
    MIRA_CHECK(patched.has_value() && patched.value().applied);
    fixture.provider_->release();
    const auto settled = orchestrator->wait_recovery(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().outcome == WorkflowRecoveryOutcome::ResumedWithoutPatch);
    MIRA_CHECK(!settled.value().patch_id.has_value());
    const auto completed =
        fixture.workflow_->wait_run(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(completed.has_value() &&
               completed.value().state == WorkflowRunState::Completed);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int takeover_blocks_recovery_admission() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    const auto takeover =
        fixture.fixture_.runtime_->request_human_takeover(fixture.fixture_.session_id_);
    MIRA_CHECK(takeover.has_value());
    MIRA_CHECK(takeover.value().outcome(std::chrono::seconds(2)).has_value());
    fixture.provider_->add_response(text_response(resume_decision("never happens")));
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(attempt.has_value());
    MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(attempt.value().reason_code == "takeover");
    MIRA_CHECK(fixture.provider_->consumed() == 0);
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    MIRA_CHECK(snapshot.has_value() &&
               snapshot.value().state == WorkflowRunState::WaitingAgent);
    static_cast<void>(orchestrator->shutdown());
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int attempt_budget_and_tracking_capacity_bound_the_loop() {
    // attempt budget 1: the failed (model-unavailable) attempt consumes the
    // slot; the next notification defers to the host.
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        const auto run = fixture.workflow_->create_run(escalate_definition(
                                                           WorkflowPolicy::Recoverable),
                                                       JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        auto orchestrator = fixture.make_orchestrator([](WorkflowRecoveryConfig &config) {
            config.max_attempts_per_run = 1;
        });
        const auto first = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(first.has_value() &&
                   first.value().reason_code == "model-unavailable");
        const auto second = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(second.has_value());
        MIRA_CHECK(second.value().outcome == WorkflowRecoveryOutcome::DeferredToHost);
        MIRA_CHECK(second.value().reason_code == "attempt-budget-exhausted");
        const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
        MIRA_CHECK(snapshot.has_value() &&
                   snapshot.value().state == WorkflowRunState::WaitingAgent);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // Concurrent capacity 1: the second asynchronous attempt on another run
    // is rejected with ResourceExhausted.
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1, 2}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const auto run_a = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        const auto run_b = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run_a.has_value() && run_b.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run_a.value().run_id, run_context(fixture))
                       .has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run_b.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->block();
        auto orchestrator = fixture.make_orchestrator();
        MIRA_CHECK(orchestrator->start_recovery(run_a.value().run_id).has_value());
        fixture.provider_->wait_entered();
        const auto rejected = orchestrator->start_recovery(run_b.value().run_id);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::ResourceExhausted);
        fixture.provider_->release();
        const auto settled =
            orchestrator->wait_recovery(run_a.value().run_id, std::chrono::seconds(10));
        MIRA_CHECK(settled.has_value() &&
                   settled.value().reason_code == "model-unavailable");
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // Tracking capacity 2 with eviction: terminal runs free their slots; a
    // table full of live (attempted, still WaitingAgent) runs rejects.
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1, 2, 3, 4}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        auto orchestrator = fixture.make_orchestrator([](WorkflowRecoveryConfig &config) {
            config.max_tracked_runs = 2;
        });
        const auto escalate = [&](const char *label) -> Result<WorkflowRunView> {
            auto created =
                fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
            if (!created.has_value()) {
                return created;
            }
            const auto driven = fixture.workflow_->execute_run(created.value().run_id,
                                                               run_context(fixture));
            if (!driven.has_value() ||
                driven.value().state != WorkflowRunState::WaitingAgent) {
                Error error;
                error.domain = "mira.test";
                error.safe_message = label;
                return error;
            }
            return created;
        };
        // Slot 1: a run that recovers and completes (terminal, evictable).
        const auto run_a = escalate("a");
        MIRA_CHECK(run_a.has_value());
        const auto continuation = fixture.workflow_->agent_continuation(run_a.value().run_id);
        MIRA_CHECK(continuation.has_value());
        fixture.provider_->add_response(
            text_response(skip_step_decision(continuation.value().current_step->to_string(), "fix")));
        const auto recovered = orchestrator->attempt_recovery(run_a.value().run_id);
        MIRA_CHECK(recovered.has_value() &&
                   recovered.value().outcome == WorkflowRecoveryOutcome::PatchedAndResumed);
        MIRA_CHECK(fixture.workflow_
                       ->wait_run(run_a.value().run_id, std::chrono::seconds(10))
                       .has_value());
        // Slot 2: a live run whose attempt aborted (model unavailable).
        const auto run_b = escalate("b");
        MIRA_CHECK(run_b.has_value());
        const auto aborted = orchestrator->attempt_recovery(run_b.value().run_id);
        MIRA_CHECK(aborted.has_value() &&
                   aborted.value().reason_code == "model-unavailable");
        // The terminal run A is evicted to make room for run C.
        const auto run_c = escalate("c");
        MIRA_CHECK(run_c.has_value());
        const auto evicted = orchestrator->attempt_recovery(run_c.value().run_id);
        MIRA_CHECK(evicted.has_value() &&
                   evicted.value().reason_code == "model-unavailable");
        // The table now holds two live runs: run D is rejected outright.
        const auto run_d = escalate("d");
        MIRA_CHECK(run_d.has_value());
        const auto full = orchestrator->attempt_recovery(run_d.value().run_id);
        MIRA_CHECK(!full.has_value());
        MIRA_CHECK(full.error().code == ErrorCode::ResourceExhausted);
        // Cancelling run C frees a slot through terminal eviction.
        MIRA_CHECK(fixture.workflow_->cancel_run(run_c.value().run_id).has_value());
        const auto admitted = orchestrator->attempt_recovery(run_d.value().run_id);
        MIRA_CHECK(admitted.has_value() &&
                   admitted.value().reason_code == "model-unavailable");
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    return 0;
}

int shutdown_matrix_drains_and_rejects() {
    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->block();
    auto orchestrator = fixture.make_orchestrator();
    MIRA_CHECK(orchestrator->start_recovery(run.value().run_id).has_value());
    fixture.provider_->wait_entered();
    const auto report = orchestrator->shutdown();
    MIRA_CHECK(report.drained_attempts == 1);
    MIRA_CHECK(report.cancelled_attempts == 1);
    MIRA_CHECK(report.clean);
    // Notifications after shutdown are rejected.
    const auto rejected = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(rejected.has_value());
    MIRA_CHECK(rejected.value().outcome == WorkflowRecoveryOutcome::Aborted);
    MIRA_CHECK(rejected.value().reason_code == "shutdown");
    MIRA_CHECK(!orchestrator->start_recovery(run.value().run_id).has_value());
    MIRA_CHECK(orchestrator->shut_down());
    // Producer-first order holds: the runtime still shuts down cleanly.
    const auto runtime_report = fixture.workflow_->shutdown();
    MIRA_CHECK(runtime_report.clean);
    return 0;
}

int event_emission_failure_never_breaks_the_attempt() {
    class FailingEventStore final : public IEventStore {
      public:
        Result<AppendReceipt> append(const AppendRequest &) override {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.test";
            error.safe_message = "event store down";
            return error;
        }
        Result<std::vector<AppendReceipt>>
        append_batch(std::span<const AppendRequest> requests) override {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.test";
            error.safe_message = "event store down";
            static_cast<void>(requests);
            return error;
        }
        Result<EventPage> read(const EventQuery &) const override { return EventPage{}; }
        Result<StoreRecoveryReport> recover(const RecoveryOptions &) override {
            return StoreRecoveryReport{};
        }
        Result<void> flush(Durability) override { return Result<void>{}; }
    };

    RecoveryFixture fixture;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    const auto run = fixture.workflow_->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                                   JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run.value().run_id, run_context(fixture)).has_value());
    fixture.provider_->add_response(text_response(resume_decision("retry")));
    auto orchestrator = fixture.make_orchestrator();
    orchestrator->set_event_store(std::make_shared<FailingEventStore>());
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    MIRA_CHECK(attempt.has_value());
    MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::ResumedWithoutPatch);
    const auto report = orchestrator->shutdown();
    MIRA_CHECK(report.event_emit_failures >= 1);
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int end_to_end_lesson_reuse_with_full_event_correlation() {
    RecoveryFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    MIRA_CHECK(fixture.workflow_->set_learning_context(memory, learning_scope()).has_value());
    ScriptedTool tool{std::vector<int>{1, 2, 3}};
    MIRA_CHECK(
        register_registration(*fixture.fixture_.registry_, tool.registration("scripted"))
            .has_value());
    auto definition = escalate_definition(WorkflowPolicy::Recoverable);

    // Run 0: a terminal Strict failure seeds the episodic half of the
    // retrieval (DEC-030 §2: completed runs carry no failed_step_id, so the
    // signature-matching episode is the failed one; a successful recovery
    // serves retrieval through its lesson).
    const auto run_zero =
        fixture.workflow_->create_run(definition, JsonValue{}, WorkflowPolicy::Strict);
    MIRA_CHECK(run_zero.has_value());
    const auto failed_zero =
        fixture.workflow_->execute_run(run_zero.value().run_id, run_context(fixture));
    MIRA_CHECK(failed_zero.has_value() && failed_zero.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(memory->stored_records() == 1);

    // Run A: escalates (retrieval already sees run 0's episode), the
    // orchestrator repairs by skipping the failing step, the run completes
    // and the host records the lesson.
    const auto run_a = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run_a.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run_a.value().run_id, run_context(fixture)).has_value());
    const auto mid = fixture.workflow_->agent_continuation(run_a.value().run_id);
    MIRA_CHECK(mid.has_value());
    MIRA_CHECK(mid.value().relevant_lessons.size() == 1);
    MIRA_CHECK(mid.value().relevant_lessons.front().kind == MemoryKind::Episode);
    fixture.provider_->add_response(
        text_response(skip_step_decision(mid.value().current_step->to_string(), "run A repair")));
    auto orchestrator = fixture.make_orchestrator();
    const auto first = orchestrator->attempt_recovery(run_a.value().run_id);
    MIRA_CHECK(first.has_value() &&
               first.value().outcome == WorkflowRecoveryOutcome::PatchedAndResumed);
    const auto settled_a =
        fixture.workflow_->wait_run(run_a.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled_a.has_value() && settled_a.value().state == WorkflowRunState::Completed);
    const auto lesson = fixture.workflow_->record_recovery_lesson(run_a.value().run_id);
    MIRA_CHECK(lesson.has_value());

    // Run B: the same failure retrieves run A's episode and lesson; the
    // model cites the lesson and synthesizes a fresh patch.
    const auto run_b = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run_b.has_value());
    MIRA_CHECK(
        fixture.workflow_->execute_run(run_b.value().run_id, run_context(fixture)).has_value());
    const auto continuation = fixture.workflow_->agent_continuation(run_b.value().run_id);
    MIRA_CHECK(continuation.has_value());
    std::size_t episodes = 0;
    std::size_t lessons = 0;
    for (const auto &attached : continuation.value().relevant_lessons) {
        episodes += attached.kind == MemoryKind::Episode ? 1U : 0U;
        lessons += attached.kind == MemoryKind::RecoveryLesson ? 1U : 0U;
    }
    MIRA_CHECK(episodes >= 1);
    MIRA_CHECK(lessons == 1);
    fixture.provider_->add_response(
        text_response("{\"action\":\"patch_and_resume\",\"patch_entries\":[{\"target\":"
                      "\"step_arguments\",\"op\":\"skip\",\"path\":\"" +
                      continuation.value().current_step->to_string() + "\"}],\"used_lessons\":[\"" +
                      lesson.value().lesson_id + "\"],\"rationale\":\"run B repair\"}"));
    const auto second = orchestrator->attempt_recovery(run_b.value().run_id);
    MIRA_CHECK(second.has_value() &&
               second.value().outcome == WorkflowRecoveryOutcome::PatchedAndResumed);
    const auto settled_b =
        fixture.workflow_->wait_run(run_b.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled_b.has_value() && settled_b.value().state == WorkflowRunState::Completed);

    // The provider really received the retrieved context on run B's attempt:
    // both statements, byte-identical, in the assembled context.
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    MIRA_CHECK(requests.back().task_id == continuation.value().carrier_task_id);
    MIRA_CHECK(requests.back().task_epoch == continuation.value().carrier_task_epoch);
    MIRA_CHECK(requests.back().tools.empty());
    MIRA_CHECK(requests.back().tool_choice.mode == ToolChoiceMode::None);
    const auto context_b = parse_context(requests.back());
    const auto *kept = context_b.find("relevant_lessons");
    MIRA_CHECK(kept != nullptr && kept->is_array());
    bool saw_lesson = false;
    for (const auto &entry : *kept->as_array()) {
        if (entry.as_string()->find("\"outcome\":\"recovered\"") != std::string::npos) {
            saw_lesson = true;
        }
    }
    MIRA_CHECK(saw_lesson);
    static_cast<void>(orchestrator->shutdown());

    // Full-chain correlation: the recovery attempt event links the model
    // request, the decision digest and the applied patch.
    const auto attempts =
        recovery_attempt_events(*fixture.fixture_.events_, fixture.fixture_.session_id_);
    MIRA_CHECK(attempts.size() == 2);
    MIRA_CHECK(attempts.back().run_id == run_b.value().run_id);
    MIRA_CHECK(attempts.back().outcome == WorkflowRecoveryOutcome::PatchedAndResumed);
    MIRA_CHECK(attempts.back().model_request_id.has_value());
    MIRA_CHECK(attempts.back().patch_id.has_value());
    MIRA_CHECK(attempts.back().decision_digest.has_value());
    MIRA_CHECK(attempts.back().lessons_offered >= 2);
    MIRA_CHECK(attempts.back().lessons_kept >= 2);
    MIRA_CHECK(attempts.back().task_id == continuation.value().carrier_task_id);
    bool request_event = false;
    for (const auto &payload :
         event_payloads_of_type(*fixture.fixture_.events_, fixture.fixture_.session_id_,
                                "ModelRequestPrepared")) {
        if (payload.find(attempts.back().model_request_id->to_string()) !=
            std::string::npos) {
            request_event = true;
        }
    }
    MIRA_CHECK(request_event);
    bool patch_event = false;
    for (const auto &payload :
         event_payloads_of_type(*fixture.fixture_.events_, fixture.fixture_.session_id_,
                                "WorkflowPatchApplied")) {
        if (payload.find(attempts.back().patch_id->to_string()) != std::string::npos) {
            patch_event = true;
        }
    }
    MIRA_CHECK(patch_event);
    const auto run_settled =
        event_payloads_of_type(*fixture.fixture_.events_, fixture.fixture_.session_id_,
                               "WorkflowRunSettled");
    MIRA_CHECK(run_settled.size() >= 2);
    static_cast<void>(fixture.workflow_->shutdown());
    return 0;
}

int security_negatives_fail_closed() {
    // Lesson-guided invalid patch: the runtime's deterministic rejection
    // defers to the host after the repair budget is spent.
    {
        RecoveryFixture fixture;
        auto memory = std::make_shared<FakeLearningMemory>();
        MIRA_CHECK(
            fixture.workflow_->set_learning_context(memory, learning_scope()).has_value());
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        // An unknown run parameter: entry shape is valid, the runtime
        // binding fails deterministically.
        fixture.provider_->add_response(text_response(
            "{\"action\":\"patch_and_resume\",\"patch_entries\":[{\"target\":"
            "\"run_parameters\",\"op\":\"set\",\"path\":\"no_such_parameter\",\"value\":1}],"
            "\"used_lessons\":[],\"rationale\":\"malicious lesson repair\"}"));
        fixture.provider_->add_response(text_response(
            "{\"action\":\"patch_and_resume\",\"patch_entries\":[{\"target\":"
            "\"run_parameters\",\"op\":\"set\",\"path\":\"still_missing\",\"value\":1}],"
            "\"used_lessons\":[],\"rationale\":\"malicious lesson repair\"}"));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        MIRA_CHECK(attempt.value().outcome == WorkflowRecoveryOutcome::DeferredToHost);
        MIRA_CHECK(attempt.value().reason_code.find("patch-rejected:") == 0);
        const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
        MIRA_CHECK(snapshot.has_value() &&
                   snapshot.value().state == WorkflowRunState::WaitingAgent);
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    // Parameter projection default carries no values and the rationale never
    // reaches an event payload.
    {
        RecoveryFixture fixture;
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(register_registration(*fixture.fixture_.registry_,
                                         tool.registration("scripted"))
                       .has_value());
        auto definition = escalate_definition(WorkflowPolicy::Recoverable);
        definition.parameters.front().default_value = JsonValue{"secret-val"};
        const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        MIRA_CHECK(fixture.workflow_
                       ->execute_run(run.value().run_id, run_context(fixture))
                       .has_value());
        fixture.provider_->add_response(
            text_response(need_user_decision("unique-rationale-marker-2026")));
        auto orchestrator = fixture.make_orchestrator();
        const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
        MIRA_CHECK(attempt.has_value());
        const auto requests = fixture.provider_->requests();
        MIRA_CHECK(!requests.empty());
        const auto context = parse_context(requests.front());
        const auto *parameters = context.find("parameters");
        MIRA_CHECK(parameters != nullptr && parameters->is_object());
        const auto *mode = parameters->find("mode");
        MIRA_CHECK(mode != nullptr && mode->is_object());
        MIRA_CHECK(mode->find("value") == nullptr);
        MIRA_CHECK(mode->find("type") != nullptr);
        MIRA_CHECK(mode->find("digest") != nullptr);
        MIRA_CHECK(context_text(requests.front()).find("secret-val") ==
                   std::string::npos);
        for (const auto &payload :
             event_payloads_of_type(*fixture.fixture_.events_, fixture.fixture_.session_id_,
                                    "WorkflowRecoveryAttempted")) {
            MIRA_CHECK(payload.find("unique-rationale-marker-2026") == std::string::npos);
        }
        static_cast<void>(orchestrator->shutdown());
        static_cast<void>(fixture.workflow_->shutdown());
    }
    return 0;
}

int recovery_event_payload_round_trips_fail_closed() {
    MIRA_CHECK(is_workflow_event_type("WorkflowRecoveryAttempted"));
    WorkflowRecoveryAttemptedEvent event;
    event.run_id = WorkflowRunId::generate();
    event.workflow_id = WorkflowId::generate();
    event.ordinal = 3;
    event.task_id = TaskId::generate();
    event.outcome = WorkflowRecoveryOutcome::PatchedAndResumed;
    event.reason_code = "patch-rejected:12";
    event.decision_digest = digest_string("decision");
    event.patch_id = WorkflowPatchId::generate();
    event.model_request_id = ModelRequestId::generate();
    event.lessons_offered = 4;
    event.lessons_stale = 1;
    event.lessons_unparseable = 1;
    event.lessons_kept = 2;
    const auto payload = to_event_payload(event);
    MIRA_CHECK(payload.classification == EventClass::State);
    auto parsed = parse_workflow_recovery_attempted(payload);
    MIRA_CHECK(parsed.has_value());
    MIRA_CHECK(parsed.value().run_id == event.run_id);
    MIRA_CHECK(parsed.value().outcome == event.outcome);
    MIRA_CHECK(parsed.value().reason_code == event.reason_code);
    MIRA_CHECK(parsed.value().ordinal == 3);
    MIRA_CHECK(parsed.value().decision_digest == event.decision_digest);
    MIRA_CHECK(parsed.value().patch_id == event.patch_id);
    MIRA_CHECK(parsed.value().model_request_id == event.model_request_id);
    MIRA_CHECK(parsed.value().lessons_offered == 4);
    MIRA_CHECK(parsed.value().lessons_unparseable == 1);

    // Optional members round-trip through null.
    WorkflowRecoveryAttemptedEvent bare;
    bare.run_id = WorkflowRunId::generate();
    bare.workflow_id = WorkflowId::generate();
    bare.task_id = TaskId::generate();
    bare.outcome = WorkflowRecoveryOutcome::Aborted;
    auto bare_parsed = parse_workflow_recovery_attempted(to_event_payload(bare));
    MIRA_CHECK(bare_parsed.has_value());
    MIRA_CHECK(!bare_parsed.value().decision_digest.has_value());
    MIRA_CHECK(!bare_parsed.value().patch_id.has_value());
    MIRA_CHECK(!bare_parsed.value().model_request_id.has_value());

    // Unknown fields fail closed (DEC-022 §2 discipline).
    JsonValue json = parse_json(payload.data).value();
    JsonValue::Object object = *json.as_object();
    object.emplace_back("extra", JsonValue{"field"});
    EventPayload tampered = payload;
    tampered.data = to_json_string(JsonValue{std::move(object)});
    MIRA_CHECK(!parse_workflow_recovery_attempted(tampered).has_value());
    MIRA_CHECK(!parse_workflow_recovery_outcome("unknown-outcome").has_value());
    return 0;
}

int config_validation_fails_closed() {
    auto fixture = std::make_unique<RecoveryFixture>();
    ScriptedTool tool{std::vector<int>{}};
    MIRA_CHECK(
        register_registration(*fixture->fixture_.registry_, tool.registration("scripted"))
            .has_value());
    WorkflowRecoveryConfig base;
    base.profile_id = fixture->profile_->id;
    MIRA_CHECK(base.validate().has_value());
    auto broken = base;
    broken.max_attempts_per_run = 0;
    MIRA_CHECK(!broken.validate().has_value());
    broken = base;
    broken.model_call_deadline = std::chrono::milliseconds::zero();
    MIRA_CHECK(!broken.validate().has_value());
    broken = base;
    broken.max_concurrent_attempts = 0;
    MIRA_CHECK(!broken.validate().has_value());
    broken = base;
    broken.profile_id = ModelProfileId{};
    MIRA_CHECK(!broken.validate().has_value());
    bool threw = false;
    try {
        WorkflowRecoveryConfig empty;
        WorkflowRecoveryOrchestrator orchestrator(
            fixture->fixture_.executor_, *fixture->workflow_, *fixture->fixture_.runtime_,
            *fixture->gateway_, fixture->fixture_.session_id_, empty);
        static_cast<void>(orchestrator);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    MIRA_CHECK(threw);
    static_cast<void>(fixture->workflow_->shutdown());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {continuation_exposes_recovery_identity,
         decision_matrix_four_actions_reach_the_runtime_exits,
         malformed_decisions_repair_then_defer, refused_decisions_never_repair,
         unavailable_models_abort_without_touching_the_run,
         lesson_matrix_counts_filtering_layers,
         cancellation_mid_request_aborts_and_keeps_the_run,
         host_cancellation_drift_discards_the_late_decision,
         host_patch_in_flight_does_not_invalidate_a_valid_decision,
         takeover_blocks_recovery_admission,
         attempt_budget_and_tracking_capacity_bound_the_loop,
         shutdown_matrix_drains_and_rejects,
         event_emission_failure_never_breaks_the_attempt,
         end_to_end_lesson_reuse_with_full_event_correlation,
         security_negatives_fail_closed,
         recovery_event_payload_round_trips_fail_closed,
         config_validation_fails_closed});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    std::cout << "m14 recovery orchestration tests passed\n";
    return 0;
}
