// M13-04/M13-05/M13-06/M13-08 (runtime half): the learning loop inside the
// WorkflowRuntime (DEC-030). Settlement records episodes for non-DryRun
// terminals and survives memory failures; failure-driven escalations attach
// retrieved lessons to the agent continuation; recovery lessons derive from
// completed recoveries and stay idempotent; the end-to-end loop feeds run C
// with run A's episode and run B's lesson.

#include "support/m13_support.hpp"

#include <mira/workflow_learning.hpp>

#include <thread>

namespace {

using namespace mira;
using namespace mira::testing;

// One failing ToolCall step, no local recovery hook: under Recoverable the
// failure escalates to WaitingAgent (DEC-023 §1); under Strict it settles
// Failed.
WorkflowDefinition escalate_definition(WorkflowPolicy policy) {
    auto definition = base_definition("m13-learn");
    definition.default_policy = policy;
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

// One shared admission helper: installs the context on a throwaway runtime.
Result<void> install_context(std::shared_ptr<IMemory> memory, MemoryScope scope,
                              WorkflowLearningLimits limits = WorkflowLearningLimits{}) {
    WorkflowFixture fixture;
    auto workflow = fixture.make_workflow();
    return workflow->set_learning_context(std::move(memory), std::move(scope), limits);
}

int learning_context_admission_fails_closed() {
    auto memory = std::make_shared<FakeLearningMemory>();
    MIRA_CHECK(!install_context(nullptr, learning_scope()).has_value());
    auto user = learning_scope();
    user.kind = MemoryScopeKind::User;
    MIRA_CHECK(!install_context(memory, user).has_value());
    auto subjectless = learning_scope();
    subjectless.kind = MemoryScopeKind::Session;
    subjectless.subject_id.clear();
    MIRA_CHECK(!install_context(memory, subjectless).has_value());
    // Session with a subject stays admissible.
    auto session_scoped = learning_scope();
    session_scoped.kind = MemoryScopeKind::Session;
    MIRA_CHECK(install_context(memory, session_scoped).has_value());
    WorkflowLearningLimits broken = kDefaultWorkflowLearningLimits;
    broken.max_retrieval_results = 0;
    MIRA_CHECK(!install_context(memory, learning_scope(), broken).has_value());
    MIRA_CHECK(install_context(memory, learning_scope()).has_value());
    return 0;
}

int settlement_records_episodes_for_non_dryrun_terminals() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(
        workflow->set_learning_context(memory, learning_scope()).has_value());

    // Completed: dispatch 1 would fail, but this is a fresh tool call that
    // succeeds (the script fails dispatch 1 — so use a second instance).
    ScriptedTool reliable{std::vector<int>{}};
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     reliable.registration("reliable")).has_value());
    auto ok_definition = escalate_definition(WorkflowPolicy::Strict);
    ok_definition.steps[0].arguments = JsonValue{JsonValue::Object{
        std::pair{"tool", JsonValue{"reliable"}}, std::pair{"payload", JsonValue{"p"}}}};
    const auto ok_run = workflow->create_run(ok_definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(ok_run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto completed = workflow->execute_run(ok_run.value().run_id, context);
    MIRA_CHECK(completed.has_value());
    MIRA_CHECK(completed.value().state == WorkflowRunState::Completed);

    // Failed under Strict: dispatch 1 fails, no escalation path.
    const auto failed_definition = escalate_definition(WorkflowPolicy::Strict);
    const auto failed_run = workflow->create_run(failed_definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(failed_run.has_value());
    const auto failed = workflow->execute_run(failed_run.value().run_id, context);
    MIRA_CHECK(failed.has_value());
    MIRA_CHECK(failed.value().state == WorkflowRunState::Failed);

    // Cancelled straight from Created.
    auto cancel_definition = escalate_definition(WorkflowPolicy::Strict);
    cancel_definition.steps[0].arguments = JsonValue{JsonValue::Object{
        std::pair{"tool", JsonValue{"reliable"}}, std::pair{"payload", JsonValue{"p"}}}};
    const auto cancel_run =
        workflow->create_run(cancel_definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(cancel_run.has_value());
    const auto cancelled = workflow->cancel_run(cancel_run.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(cancelled.value().state == WorkflowRunState::Cancelled);

    const auto records = memory->snapshot();
    MIRA_CHECK(records.size() == 3);
    std::size_t outcomes = 0;
    for (const auto &record : records) {
        MIRA_CHECK(record.kind == MemoryKind::Episode);
        MIRA_CHECK(record.verification == MemoryVerification::Verified);
        MIRA_CHECK(record.confidence == 1.0F);
        MIRA_CHECK(record.scope == learning_scope());
        MIRA_CHECK(record.provenance.size() == 1);
        auto episode = workflow_episode_from_json(parse_json(record.statement).value());
        MIRA_CHECK(episode.has_value());
        if (episode.value().outcome == "completed") {
            outcomes |= 1U;
        } else if (episode.value().outcome == "failed") {
            outcomes |= 2U;
            MIRA_CHECK(episode.value().failed_step_id.has_value());
            MIRA_CHECK(episode.value().failure_reason_code.has_value());
        } else if (episode.value().outcome == "cancelled") {
            outcomes |= 4U;
        }
    }
    MIRA_CHECK(outcomes == 7U);
    static_cast<void>(workflow->shutdown());
    return 0;
}

int dry_run_and_missing_context_record_nothing() {
    // DryRun completion leaves the learning memory untouched (design skip).
    {
        WorkflowFixture fixture;
        auto memory = std::make_shared<FakeLearningMemory>();
        ScriptedTool tool{std::vector<int>{}};
        MIRA_CHECK(
            register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
        auto workflow = fixture.make_workflow();
        MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());
        const auto run = workflow->create_run(escalate_definition(WorkflowPolicy::DryRun),
                                              JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        OperationContext context;
        context.session = fixture.session_id_;
        context.started_at = Timestamp::now();
        const auto settled = workflow->execute_run(run.value().run_id, context);
        MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
        MIRA_CHECK(memory->stored_records() == 0);
        // And no audit event was emitted for the by-design skip.
        MIRA_CHECK(learning_event_outcomes(*fixture.events_, fixture.session_id_,
                                           "WorkflowEpisodeRecorded")
                       .empty());
        static_cast<void>(workflow->shutdown());
    }
    // Without a learning context the M12 behavior is unchanged: no writes,
    // no events, runs settle normally.
    {
        WorkflowFixture fixture;
        auto memory = std::make_shared<FakeLearningMemory>();
        ScriptedTool tool{std::vector<int>{1}};
        MIRA_CHECK(
            register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
        auto workflow = fixture.make_workflow();
        const auto run = workflow->create_run(escalate_definition(WorkflowPolicy::Strict),
                                              JsonValue{}, std::nullopt);
        MIRA_CHECK(run.has_value());
        OperationContext context;
        context.session = fixture.session_id_;
        context.started_at = Timestamp::now();
        const auto settled = workflow->execute_run(run.value().run_id, context);
        MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Failed);
        MIRA_CHECK(memory->stored_records() == 0);
        MIRA_CHECK(learning_event_outcomes(*fixture.events_, fixture.session_id_,
                                           "WorkflowEpisodeRecorded")
                       .empty());
        static_cast<void>(workflow->shutdown());
    }
    return 0;
}

int memory_write_failure_never_breaks_settlement() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    memory->fail_applies = true;
    ScriptedTool tool{std::vector<int>{}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());
    auto definition = escalate_definition(WorkflowPolicy::Strict);
    const auto run = workflow->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto settled = workflow->execute_run(run.value().run_id, context);
    // The run completed; the memory failure is disclosed through the audit
    // event, not by un-settling the terminal state.
    MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(memory->stored_records() == 0);
    const auto outcomes = learning_event_outcomes(*fixture.events_, fixture.session_id_,
                                                  "WorkflowEpisodeRecorded");
    MIRA_CHECK(outcomes.size() == 1 && outcomes[0] == "failed");
    static_cast<void>(workflow->shutdown());
    return 0;
}

int escalation_retrieval_feeds_the_agent_continuation() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    // Build the definition first so the seeded episode can carry the exact
    // step identity the retrieval signature will name.
    const auto definition = escalate_definition(WorkflowPolicy::Recoverable);
    const std::string step_id = definition.steps[0].id.to_string();
    WorkflowEpisodeRecord seeded;
    seeded.run_id = WorkflowRunId::generate().to_string();
    seeded.workflow_id = definition.workflow_id.to_string();
    seeded.ir_digest = digest_string("seed").to_string();
    seeded.policy = "strict";
    seeded.outcome = "failed";
    seeded.failed_step_id = step_id;
    seeded.failure_reason_code = "mira.workflow:6";
    seeded.recorded_at_ms = 1;
    const auto record =
        episode_to_memory_record(seeded, learning_scope(), {EventId::generate()},
                                 std::chrono::system_clock::now());
    {
        MemoryMutation seed;
        seed.id = MutationId::generate();
        seed.type = MemoryMutationType::Add;
        seed.scope = learning_scope();
        seed.proposed = record;
        seed.evidence = {EventId::generate()};
        seed.reason = MutationReasonCode::VerifiedEvent;
        MIRA_CHECK(memory->apply(seed).has_value());
    }

    const auto run = workflow->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto escalated = workflow->execute_run(run.value().run_id, context);
    MIRA_CHECK(escalated.has_value() &&
               escalated.value().state == WorkflowRunState::WaitingAgent);
    const auto continuation = workflow->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value());
    MIRA_CHECK(continuation.value().relevant_lessons.size() == 1);
    MIRA_CHECK(continuation.value().relevant_lessons[0].kind == MemoryKind::Episode);
    MIRA_CHECK(continuation.value().relevant_lessons[0].statement == record.statement);
    static_cast<void>(workflow->shutdown());
    return 0;
}

int retrieval_degradation_keeps_the_escalation_path() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    memory->fail_queries = true;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());
    const auto run = workflow->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                          JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto escalated = workflow->execute_run(run.value().run_id, context);
    MIRA_CHECK(escalated.has_value() &&
               escalated.value().state == WorkflowRunState::WaitingAgent);
    const auto continuation = workflow->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value());
    MIRA_CHECK(continuation.value().relevant_lessons.empty());
    MIRA_CHECK(memory->queries.load() >= 1);
    static_cast<void>(workflow->shutdown());
    return 0;
}

int checkpoint_handoffs_do_not_retrieve() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    ScriptedTool tool{std::vector<int>{}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    // AgentAssisted with an AgentEscalation hook: arrival-driven checkpoint
    // handoff, not a failure.
    auto definition = base_definition("m13-learn");
    definition.default_policy = WorkflowPolicy::AgentAssisted;
    definition.allowed_policies.push_back(WorkflowPolicy::AgentAssisted);
    auto step = tool_step("scripted", std::nullopt);
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::AgentEscalation;
    step.recovery = hook;
    definition.steps.push_back(std::move(step));

    // Seed a matching episode anyway: the handoff must still not retrieve.
    WorkflowEpisodeRecord seeded;
    seeded.run_id = WorkflowRunId::generate().to_string();
    seeded.workflow_id = definition.workflow_id.to_string();
    seeded.ir_digest = digest_string("seed").to_string();
    seeded.policy = "agent-assisted";
    seeded.outcome = "failed";
    seeded.failed_step_id = definition.steps[0].id.to_string();
    seeded.failure_reason_code = "mira.workflow:6";
    seeded.recorded_at_ms = 1;
    {
        MemoryMutation seed;
        seed.id = MutationId::generate();
        seed.type = MemoryMutationType::Add;
        seed.scope = learning_scope();
        seed.proposed = episode_to_memory_record(seeded, learning_scope(),
                                                 {EventId::generate()},
                                                 std::chrono::system_clock::now());
        seed.evidence = {EventId::generate()};
        seed.reason = MutationReasonCode::VerifiedEvent;
        MIRA_CHECK(memory->apply(seed).has_value());
    }
    const auto run = workflow->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto handed = workflow->execute_run(run.value().run_id, context);
    MIRA_CHECK(handed.has_value() && handed.value().state == WorkflowRunState::WaitingAgent);
    const auto continuation = workflow->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value());
    // Checkpoint handoffs are not failures: no retrieval, no attachment.
    MIRA_CHECK(continuation.value().relevant_lessons.empty());
    MIRA_CHECK(memory->queries.load() == 0);
    static_cast<void>(workflow->shutdown());
    return 0;
}

int recovery_lessons_derive_and_stay_idempotent() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    // Fails dispatch 1 (escalation), succeeds dispatch 2 (recovery).
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    // Admission: before completion the run is WaitingAgent.
    const auto run = workflow->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                          JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    MIRA_CHECK(workflow->execute_run(run.value().run_id, context).has_value());
    MIRA_CHECK(!workflow->record_recovery_lesson(run.value().run_id).has_value());

    // Resume without a patch: the recovered lesson carries the bare resume
    // mark.
    MIRA_CHECK(workflow->resume_run(run.value().run_id).has_value());
    const auto settled = workflow->wait_run(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
    const auto lesson = workflow->record_recovery_lesson(run.value().run_id);
    MIRA_CHECK(lesson.has_value());
    MIRA_CHECK(lesson.value().recovered_run_id == run.value().run_id.to_string());
    MIRA_CHECK(lesson.value().failure.workflow_id == run.value().workflow_id.to_string());
    MIRA_CHECK(lesson.value().failure.step_id.has_value());
    MIRA_CHECK(lesson.value().resumed_without_patch);
    MIRA_CHECK(lesson.value().recovery.empty());
    MIRA_CHECK(lesson.value().outcome == "recovered");
    const auto applies_after_first = memory->applies.load();

    // Idempotent replay: same lesson, no second write.
    const auto replayed = workflow->record_recovery_lesson(run.value().run_id);
    MIRA_CHECK(replayed.has_value() && replayed.value() == lesson.value());
    MIRA_CHECK(memory->applies.load() == applies_after_first);
    MIRA_CHECK(memory->stored_records() == 2); // episode + lesson

    // The lesson record round-trips through the memory statement.
    const auto records = memory->snapshot();
    std::size_t lessons = 0;
    for (const auto &record : records) {
        if (record.kind == MemoryKind::RecoveryLesson) {
            auto parsed = recovery_lesson_from_record(record);
            MIRA_CHECK(parsed.has_value() && parsed.value() == lesson.value());
            ++lessons;
        }
    }
    MIRA_CHECK(lessons == 1);

    // DryRun completions and escalation-free runs never admit lessons.
    ScriptedTool reliable{std::vector<int>{}};
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     reliable.registration("reliable")).has_value());
    auto dry = escalate_definition(WorkflowPolicy::DryRun);
    const auto dry_run = workflow->create_run(dry, JsonValue{}, std::nullopt);
    MIRA_CHECK(dry_run.has_value());
    MIRA_CHECK(workflow->execute_run(dry_run.value().run_id, context).has_value());
    MIRA_CHECK(!workflow->record_recovery_lesson(dry_run.value().run_id).has_value());
    auto clean = escalate_definition(WorkflowPolicy::Strict);
    clean.steps[0].arguments = JsonValue{JsonValue::Object{
        std::pair{"tool", JsonValue{"reliable"}}, std::pair{"payload", JsonValue{"p"}}}};
    const auto clean_run = workflow->create_run(clean, JsonValue{}, std::nullopt);
    MIRA_CHECK(clean_run.has_value());
    MIRA_CHECK(workflow->execute_run(clean_run.value().run_id, context).has_value());
    MIRA_CHECK(!workflow->record_recovery_lesson(clean_run.value().run_id).has_value());
    static_cast<void>(workflow->shutdown());
    return 0;
}

int recovery_lesson_patch_form_records_the_recovery_actions() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());
    const auto run = workflow->create_run(escalate_definition(WorkflowPolicy::Recoverable),
                                          JsonValue{}, std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    MIRA_CHECK(workflow->execute_run(run.value().run_id, context).has_value());

    // Repair by skipping the failing step, then resume and complete.
    const auto before_patch = workflow->agent_continuation(run.value().run_id);
    MIRA_CHECK(before_patch.has_value());
    const auto step_id = before_patch.value().current_step;
    MIRA_CHECK(step_id.has_value());
    WorkflowPatchEntry skip;
    skip.target = WorkflowPatchTarget::StepArguments;
    skip.op = WorkflowPatchOp::Skip;
    skip.path = step_id->to_string();
    const auto patched =
        workflow->patch_run(run.value().run_id, WorkflowPatchId::generate(), {skip});
    MIRA_CHECK(patched.has_value() && patched.value().applied);
    MIRA_CHECK(workflow->resume_run(run.value().run_id).has_value());
    const auto settled = workflow->wait_run(run.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Completed);
    const auto lesson = workflow->record_recovery_lesson(run.value().run_id);
    MIRA_CHECK(lesson.has_value());
    MIRA_CHECK(!lesson.value().resumed_without_patch);
    MIRA_CHECK(lesson.value().recovery.size() == 1);
    MIRA_CHECK(lesson.value().recovery[0].targets.size() == 1);
    MIRA_CHECK(lesson.value().recovery[0].targets[0] == "skip");
    static_cast<void>(workflow->shutdown());
    return 0;
}

int end_to_end_loop_feeds_the_next_failure() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    // Dispatch script: 1 = run A fails (Strict, terminal); 2 = run B
    // escalates; 3 = run B recovers; 4 = run C escalates again.
    ScriptedTool tool{std::vector<int>{1, 2, 4}};
    MIRA_CHECK(
        register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();

    // One shared definition: the same workflow version runs in A, B and C,
    // so failure signatures (workflow id + step id) match across runs.
    const auto definition = escalate_definition(WorkflowPolicy::Recoverable);

    // Run A: terminal failure records a failed episode with the signature.
    const auto run_a = workflow->create_run(definition, JsonValue{},
                                            WorkflowPolicy::Strict);
    MIRA_CHECK(run_a.has_value());
    const auto failed = workflow->execute_run(run_a.value().run_id, context);
    MIRA_CHECK(failed.has_value() && failed.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(memory->stored_records() == 1);

    // Run B: escalates (retrieval finds A's episode), recovers, completes
    // and records a lesson.
    const auto run_b = workflow->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run_b.has_value());
    const auto escalated = workflow->execute_run(run_b.value().run_id, context);
    MIRA_CHECK(escalated.has_value() &&
               escalated.value().state == WorkflowRunState::WaitingAgent);
    const auto mid = workflow->agent_continuation(run_b.value().run_id);
    MIRA_CHECK(mid.has_value());
    MIRA_CHECK(mid.value().relevant_lessons.size() == 1);
    MIRA_CHECK(mid.value().relevant_lessons[0].kind == MemoryKind::Episode);
    MIRA_CHECK(workflow->resume_run(run_b.value().run_id).has_value());
    const auto recovered = workflow->wait_run(run_b.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(recovered.has_value() && recovered.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(workflow->record_recovery_lesson(run_b.value().run_id).has_value());
    MIRA_CHECK(memory->stored_records() == 3); // episode A + episode B + lesson B

    // Run C: the same failure now retrieves both the episode and the lesson.
    const auto run_c = workflow->create_run(definition, JsonValue{}, std::nullopt);
    MIRA_CHECK(run_c.has_value());
    const auto c_escalated = workflow->execute_run(run_c.value().run_id, context);
    MIRA_CHECK(c_escalated.has_value() &&
               c_escalated.value().state == WorkflowRunState::WaitingAgent);
    const auto continuation = workflow->agent_continuation(run_c.value().run_id);
    MIRA_CHECK(continuation.has_value());
    std::size_t episodes = 0;
    std::size_t lessons = 0;
    for (const auto &attached : continuation.value().relevant_lessons) {
        if (attached.kind == MemoryKind::Episode) {
            ++episodes;
            MIRA_CHECK(attached.statement.find("\"outcome\":\"failed\"") !=
                       std::string::npos);
        } else if (attached.kind == MemoryKind::RecoveryLesson) {
            ++lessons;
            MIRA_CHECK(attached.statement.find("\"outcome\":\"recovered\"") !=
                       std::string::npos);
        }
    }
    MIRA_CHECK(episodes >= 1);
    MIRA_CHECK(lessons == 1);
    static_cast<void>(workflow->shutdown());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {learning_context_admission_fails_closed,
         settlement_records_episodes_for_non_dryrun_terminals,
         dry_run_and_missing_context_record_nothing,
         memory_write_failure_never_breaks_settlement,
         escalation_retrieval_feeds_the_agent_continuation,
         retrieval_degradation_keeps_the_escalation_path,
         checkpoint_handoffs_do_not_retrieve, recovery_lessons_derive_and_stay_idempotent,
         recovery_lesson_patch_form_records_the_recovery_actions,
         end_to_end_loop_feeds_the_next_failure});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    std::cout << "m13 learning tests passed\n";
    return 0;
}
