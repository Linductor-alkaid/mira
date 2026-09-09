// MNT-202609-25 (M13 stage-F persistence evidence): the learning loop against
// the real SQLite IMemory and the DEC-030 §5 event-rebuild recipe.
//
// Three evidence groups:
//  1. Episodes and recovery lessons written by WorkflowRuntime through a real
//     SqliteMemoryStore survive closing and rebuilding the whole owner stack
//     (executor + runtime + store): same-scope retrieval is bit-identical,
//     foreign scopes read nothing, and deterministic mutation ids replay as
//     idempotent NoOps.
//  2. The DEC-030 §5 rebuild recipe (RunStarted + StepSettled + RunSettled +
//     PatchApplied + the two audit events) is exercised field by field against
//     the records the direct path wrote. Recoverable fields are asserted
//     equal; fields the recipe payloads cannot carry are asserted absent and
//     collected into the gap list this test freezes (any future payload
//     extension flips these checks and forces the list to be re-audited).
//  3. Slow and failing memory stores keep cancel and shutdown closed: slow
//     synchronous facades delay but never break escalation, cancellation,
//     settlement or drain; failing stores disclose through audit events
//     without un-settling terminals.

#include "support/m13_support.hpp"

#include <mira/event_store.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/workflow_learning.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <executor/executor.hpp>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] std::filesystem::path temp_dir(const std::string &tag) {
    auto base = std::filesystem::temp_directory_path();
    std::random_device device;
    auto path = base / ("mira-mnt25-" + tag + "-" + std::to_string(device()));
    std::filesystem::create_directories(path);
    return path;
}

[[nodiscard]] std::uint64_t wall_ms(const Timestamp &timestamp) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.wall.time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// Owner stack: executor + MiraRuntime + FileEventStore + SqliteMemoryStore +
// WorkflowRuntime, with the documented shutdown order in close().
// ---------------------------------------------------------------------------

struct PersistenceHarness final {
    std::filesystem::path root;
    executor::Executor executor;
    std::unique_ptr<MiraRuntime> runtime;
    std::shared_ptr<SimulatorEnvironment> environment;
    std::shared_ptr<BuiltinToolRegistry> registry;
    std::shared_ptr<FileEventStore> events;
    SessionId session{};
    std::unique_ptr<WorkflowRuntime> workflow;
    std::shared_ptr<SqliteMemoryStore> store;

    explicit PersistenceHarness(std::filesystem::path root_dir) : root(std::move(root_dir)) {
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 4;
        executor_config.max_threads = 4;
        executor_config.queue_capacity = 64;
        if (!executor.initialize(executor_config)) {
            std::cerr << "executor initialize failed\n";
            std::abort();
        }
        runtime = std::make_unique<MiraRuntime>(RuntimeConfig{2, 16, 64});
        if (!runtime->initialize().has_value()) {
            std::cerr << "runtime initialize failed\n";
            std::abort();
        }
        environment = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
        const auto opened = runtime->open_session(environment);
        if (!opened.has_value()) {
            std::cerr << "open_session failed\n";
            std::abort();
        }
        if (!opened.value().command.outcome(std::chrono::seconds(2)).has_value()) {
            std::cerr << "session open outcome failed\n";
            std::abort();
        }
        session = opened.value().id;
        registry = std::make_shared<BuiltinToolRegistry>();
        events = std::make_shared<FileEventStore>(this->root / "events");
        SqliteMemoryStoreOptions options;
        options.path = this->root / "learning.db";
        auto opened_store = SqliteMemoryStore::open(executor, options);
        if (!opened_store.has_value()) {
            std::cerr << "sqlite memory store open failed: "
                      << opened_store.error().safe_message << '\n';
            std::abort();
        }
        store = std::move(opened_store).value();
        workflow = std::make_unique<WorkflowRuntime>(executor, *runtime, session, environment,
                                                     WorkflowRuntimeConfig{});
        workflow->set_event_store(events);
        workflow->set_tool_registry(registry);
        if (!workflow->set_learning_context(store, learning_scope()).has_value()) {
            std::cerr << "set_learning_context failed\n";
            std::abort();
        }
    }

    void close() {
        if (workflow) {
            static_cast<void>(workflow->shutdown());
            workflow.reset();
        }
        if (store) {
            if (!store->close().has_value()) {
                std::cerr << "sqlite memory store close failed\n";
                std::abort();
            }
            store.reset();
        }
        if (runtime) {
            const auto shutdown = runtime->request_shutdown();
            if (shutdown.has_value()) {
                static_cast<void>(shutdown.value().outcome(std::chrono::seconds(5)));
            }
            static_cast<void>(runtime->finish_shutdown());
            runtime.reset();
        }
        if (executor.shutdown(true) != executor::ShutdownResult::Completed) {
            std::cerr << "executor shutdown did not complete\n";
            std::abort();
        }
    }

    PersistenceHarness(const PersistenceHarness &) = delete;
    PersistenceHarness &operator=(const PersistenceHarness &) = delete;
};

// ---------------------------------------------------------------------------
// The driving scenario shared by the persistence and rebuild evidence groups:
// run A fails terminally under Strict; run B escalates under Recoverable, is
// repaired by a skip patch, resumes, completes and records a recovery lesson.
// ---------------------------------------------------------------------------

struct ScenarioEvidence final {
    WorkflowRunId failed_run;
    WorkflowRunId recovered_run;
    MemoryRecord episode_failed;
    MemoryRecord episode_recovered;
    MemoryRecord lesson_recovered;
    std::vector<EventEnvelope> events;
    std::filesystem::path db_path;
};

[[nodiscard]] WorkflowDefinition strict_failure_definition() {
    auto definition = base_definition("mnt25-strict");
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

[[nodiscard]] WorkflowDefinition recoverable_definition() {
    auto definition = base_definition("mnt25-recoverable");
    definition.default_policy = WorkflowPolicy::Recoverable;
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

// Returns nullptr-style sentinel on success; on failure prints and aborts
// (fixture plumbing, not an evidence outcome).
void drive_scenario(PersistenceHarness &harness, ScenarioEvidence &evidence) {
    ScriptedTool tool{std::vector<int>{1, 2}};
    if (!register_registration(*harness.registry, tool.registration("scripted")).has_value()) {
        std::cerr << "scripted tool registration failed\n";
        std::abort();
    }
    OperationContext context;
    context.session = harness.session;
    context.started_at = Timestamp::now();

    const auto strict_definition = strict_failure_definition();
    const auto run_a = harness.workflow->create_run(strict_definition, JsonValue{}, std::nullopt);
    if (!run_a.has_value()) {
        std::cerr << "run A create failed\n";
        std::abort();
    }
    evidence.failed_run = run_a.value().run_id;
    const auto settled_a =
        harness.workflow->execute_run(run_a.value().run_id, context);
    if (!settled_a.has_value() || settled_a.value().state != WorkflowRunState::Failed) {
        std::cerr << "run A did not settle Failed\n";
        std::abort();
    }

    const auto recoverable = recoverable_definition();
    const auto run_b = harness.workflow->create_run(recoverable, JsonValue{}, std::nullopt);
    if (!run_b.has_value()) {
        std::cerr << "run B create failed\n";
        std::abort();
    }
    evidence.recovered_run = run_b.value().run_id;
    const auto escalated = harness.workflow->execute_run(run_b.value().run_id, context);
    if (!escalated.has_value() || escalated.value().state != WorkflowRunState::WaitingAgent) {
        std::cerr << "run B did not escalate to WaitingAgent\n";
        std::abort();
    }
    const auto continuation = harness.workflow->agent_continuation(run_b.value().run_id);
    if (!continuation.has_value() || !continuation.value().current_step.has_value()) {
        std::cerr << "run B continuation missing the current step\n";
        std::abort();
    }
    WorkflowPatchEntry skip;
    skip.target = WorkflowPatchTarget::StepArguments;
    skip.op = WorkflowPatchOp::Skip;
    skip.path = continuation.value().current_step->to_string();
    const auto patched = harness.workflow->patch_run(run_b.value().run_id,
                                                     WorkflowPatchId::generate(), {skip});
    if (!patched.has_value() || !patched.value().applied) {
        std::cerr << "run B skip patch was not applied\n";
        std::abort();
    }
    if (!harness.workflow->resume_run(run_b.value().run_id).has_value()) {
        std::cerr << "run B resume failed\n";
        std::abort();
    }
    const auto settled_b =
        harness.workflow->wait_run(run_b.value().run_id, std::chrono::seconds(10));
    if (!settled_b.has_value() || settled_b.value().state != WorkflowRunState::Completed) {
        std::cerr << "run B did not recover to Completed\n";
        std::abort();
    }
    const auto lesson = harness.workflow->record_recovery_lesson(run_b.value().run_id);
    if (!lesson.has_value()) {
        std::cerr << "run B recovery lesson failed: " << lesson.error().safe_message << '\n';
        std::abort();
    }

    // Snapshot the direct-path records straight from the store before any
    // teardown: the persistence and rebuild groups compare against these.
    const auto fetch = [&](const MemoryId &id) -> MemoryRecord {
        auto record = harness.store->get(id);
        if (!record.has_value() || !record.value().has_value()) {
            std::cerr << "learning record missing from sqlite store\n";
            std::abort();
        }
        return record.value().value();
    };
    evidence.episode_failed = fetch(workflow_episode_memory_id(run_a.value().run_id.to_string()));
    evidence.episode_recovered =
        fetch(workflow_episode_memory_id(run_b.value().run_id.to_string()));
    evidence.lesson_recovered =
        fetch(recovery_lesson_memory_id(run_b.value().run_id.to_string()));
    evidence.db_path = harness.root / "learning.db";

    EventQuery query;
    query.session_id = harness.session;
    while (true) {
        const auto page = harness.events->read(query);
        if (!page.has_value()) {
            std::cerr << "event read failed\n";
            std::abort();
        }
        for (const auto &envelope : page.value().events) {
            evidence.events.push_back(envelope);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
}

[[nodiscard]] std::optional<std::string> episode_outcome_of(WorkflowRunState state) {
    switch (state) {
    case WorkflowRunState::Completed:
        return std::string{"completed"};
    case WorkflowRunState::Failed:
        return std::string{"failed"};
    case WorkflowRunState::Cancelled:
        return std::string{"cancelled"};
    default:
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// The DEC-030 §5 rebuild recipe, implemented strictly: only RunStarted,
// StepSettled, RunSettled, PatchApplied and the two audit events participate.
// Every field the recipe payloads cannot carry stays at its default; the
// field evidence below decides per field whether that is faithful.
// ---------------------------------------------------------------------------

struct RebuiltEpisode final {
    WorkflowEpisodeRecord record;
    EventId settled_event;
    std::uint64_t settled_at_ms = 0;
    bool audit_recorded = false;
    Sha256Digest audit_digest{};
};

struct RebuiltLesson final {
    WorkflowRecoveryLesson record;
    EventId settled_event;
    std::uint64_t settled_at_ms = 0;
    bool audit_recorded = false;
    Sha256Digest audit_digest{};
    std::vector<std::string> applied_patch_ids;
};

[[nodiscard]] std::optional<RebuiltEpisode>
rebuild_episode(const std::vector<EventEnvelope> &events, const std::string &run_id) {
    std::optional<WorkflowRunStartedEvent> started;
    std::optional<std::string> failed_step;
    std::optional<WorkflowRunSettledEvent> settled;
    RebuiltEpisode rebuilt;
    for (const auto &envelope : events) {
        const auto &payload = envelope.payload;
        if (payload.type == "WorkflowRunStarted") {
            auto parsed = parse_workflow_run_started(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id) {
                started = parsed.value();
            }
        } else if (payload.type == "WorkflowStepSettled") {
            auto parsed = parse_workflow_step_settled(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id &&
                parsed.value().disposition == WorkflowStepDisposition::Failed) {
                failed_step = parsed.value().step_id.to_string();
            }
        } else if (payload.type == "WorkflowRunSettled") {
            auto parsed = parse_workflow_run_settled(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id) {
                settled = parsed.value();
                rebuilt.settled_event = envelope.event_id;
                rebuilt.settled_at_ms = wall_ms(envelope.timestamp);
            }
        } else if (payload.type == "WorkflowEpisodeRecorded") {
            auto parsed = parse_workflow_episode_recorded(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id &&
                parsed.value().outcome == "recorded") {
                rebuilt.audit_recorded = true;
                rebuilt.audit_digest = parsed.value().episode_digest;
            }
        }
    }
    if (!started.has_value() || !settled.has_value()) {
        return std::nullopt;
    }
    const auto outcome = episode_outcome_of(settled.value().terminal_state);
    if (!outcome.has_value()) {
        return std::nullopt;
    }
    WorkflowEpisodeRecord episode;
    episode.run_id = started.value().run_id.to_string();
    episode.workflow_id = started.value().workflow_id.to_string();
    episode.ir_digest = started.value().ir_digest.to_string();
    episode.policy = workflow_policy_name(started.value().policy);
    episode.outcome = outcome.value();
    episode.failed_step_id = failed_step;
    // failure_reason_code, escalations, checkpoint_handoffs: no recipe payload
    // carries them. recorded_at_ms is proxied by the settled event timestamp.
    episode.recorded_at_ms = rebuilt.settled_at_ms;
    rebuilt.record = episode;
    return rebuilt;
}

[[nodiscard]] std::optional<RebuiltLesson>
rebuild_lesson(const std::vector<EventEnvelope> &events, const std::string &run_id) {
    std::optional<WorkflowRunStartedEvent> started;
    std::optional<std::string> failed_step;
    std::optional<WorkflowRunSettledEvent> settled;
    RebuiltLesson rebuilt;
    for (const auto &envelope : events) {
        const auto &payload = envelope.payload;
        if (payload.type == "WorkflowRunStarted") {
            auto parsed = parse_workflow_run_started(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id) {
                started = parsed.value();
            }
        } else if (payload.type == "WorkflowStepSettled") {
            auto parsed = parse_workflow_step_settled(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id &&
                parsed.value().disposition == WorkflowStepDisposition::Failed) {
                failed_step = parsed.value().step_id.to_string();
            }
        } else if (payload.type == "WorkflowRunSettled") {
            auto parsed = parse_workflow_run_settled(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id) {
                settled = parsed.value();
                rebuilt.settled_event = envelope.event_id;
                rebuilt.settled_at_ms = wall_ms(envelope.timestamp);
            }
        } else if (payload.type == "WorkflowPatchApplied") {
            auto parsed = parse_workflow_patch_applied(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id) {
                rebuilt.applied_patch_ids.push_back(parsed.value().patch_id.to_string());
            }
        } else if (payload.type == "WorkflowLessonRecorded") {
            auto parsed = parse_workflow_lesson_recorded(payload);
            if (parsed.has_value() && parsed.value().run_id.to_string() == run_id &&
                parsed.value().outcome == "recorded") {
                rebuilt.audit_recorded = true;
                rebuilt.audit_digest = parsed.value().lesson_digest;
            }
        }
    }
    if (!started.has_value() || !settled.has_value()) {
        return std::nullopt;
    }
    WorkflowRecoveryLesson lesson;
    lesson.lesson_id = started.value().run_id.to_string();
    lesson.workflow_id = started.value().workflow_id.to_string();
    lesson.ir_digest = started.value().ir_digest.to_string();
    lesson.recovered_run_id = started.value().run_id.to_string();
    lesson.failure.workflow_id = started.value().workflow_id.to_string();
    lesson.failure.step_id = failed_step;
    // failure.step_kind and failure.reason_code: not in the recipe payloads.
    for (const auto &patch_id : rebuilt.applied_patch_ids) {
        WorkflowRecoveryAction action;
        action.patch_id = patch_id;
        // patch_digest and targets: WorkflowPatchApplied carries neither.
        lesson.recovery.push_back(std::move(action));
    }
    lesson.resumed_without_patch = rebuilt.applied_patch_ids.empty();
    lesson.outcome = "recovered";
    lesson.recorded_at_ms = rebuilt.settled_at_ms;
    rebuilt.record = lesson;
    return rebuilt;
}

[[nodiscard]] WorkflowEpisodeRecord parse_episode_statement(const MemoryRecord &record) {
    const auto parsed = workflow_episode_from_json(parse_json(record.statement).value());
    if (!parsed.has_value()) {
        std::cerr << "episode statement failed to parse\n";
        std::abort();
    }
    return parsed.value();
}

// ---------------------------------------------------------------------------
// Evidence group 1: SQLite persistence across a full owner rebuild.
// ---------------------------------------------------------------------------

int sqlite_records_survive_owner_rebuild() {
    ScenarioEvidence evidence;
    {
        PersistenceHarness harness(temp_dir("sqlite"));
        drive_scenario(harness, evidence);
        // The audit trail must show three recorded writes before teardown.
        std::size_t episodes = 0;
        std::size_t lessons = 0;
        for (const auto &envelope : evidence.events) {
            if (envelope.payload.type != "WorkflowEpisodeRecorded" &&
                envelope.payload.type != "WorkflowLessonRecorded") {
                continue;
            }
            const auto payload = parse_json(envelope.payload.data).value();
            const auto &outcome = *payload.find("outcome")->as_string();
            MIRA_CHECK(outcome == "recorded");
            if (envelope.payload.type == "WorkflowEpisodeRecorded") {
                ++episodes;
            } else {
                ++lessons;
            }
        }
        MIRA_CHECK(episodes == 2);
        MIRA_CHECK(lessons == 1);
        harness.close();
    }

    // Rebuild the whole owner stack against the same database file.
    {
        executor::Executor executor;
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 4;
        executor_config.max_threads = 4;
        executor_config.queue_capacity = 64;
        MIRA_CHECK(executor.initialize(executor_config));
        {
            SqliteMemoryStoreOptions options;
            options.path = evidence.db_path;
            auto opened = SqliteMemoryStore::open(executor, options);
            MIRA_CHECK(opened.has_value());
            auto &store = *opened.value();

            // Same-id reads return every persisted field unchanged.
            const auto episode = store.get(workflow_episode_memory_id(
                evidence.failed_run.to_string()));
            MIRA_CHECK(episode.has_value() && episode.value().has_value());
            const auto &reloaded = episode.value().value();
            MIRA_CHECK(reloaded.statement == evidence.episode_failed.statement);
            MIRA_CHECK(reloaded.kind == MemoryKind::Episode);
            MIRA_CHECK(reloaded.scope == evidence.episode_failed.scope);
            MIRA_CHECK(reloaded.scope == learning_scope());
            MIRA_CHECK(reloaded.verification == MemoryVerification::Verified);
            MIRA_CHECK(reloaded.confidence == 1.0F);
            MIRA_CHECK(reloaded.status == MemoryStatus::Active);
            MIRA_CHECK(reloaded.version == 1);
            MIRA_CHECK(reloaded.provenance == evidence.episode_failed.provenance);
            MIRA_CHECK(reloaded.provenance.size() == 1);

            const auto lesson =
                store.get(recovery_lesson_memory_id(evidence.recovered_run.to_string()));
            MIRA_CHECK(lesson.has_value() && lesson.value().has_value());
            MIRA_CHECK(lesson.value().value().statement == evidence.lesson_recovered.statement);
            MIRA_CHECK(lesson.value().value().kind == MemoryKind::RecoveryLesson);

            // Same-scope failure retrieval finds the failed episode verbatim.
            const auto original = parse_episode_statement(evidence.episode_failed);
            WorkflowFailureSignature signature;
            signature.workflow_id = original.workflow_id;
            signature.step_id = original.failed_step_id;
            signature.reason_code = original.failure_reason_code.value_or("unsettled");
            const auto query =
                failure_retrieval_query(signature, learning_scope(), WorkflowLearningLimits{});
            MIRA_CHECK(query.has_value());
            const auto found = store.query(query.value());
            MIRA_CHECK(found.has_value());
            bool matched = false;
            for (const auto &record : found.value().records) {
                if (record.id == evidence.episode_failed.id) {
                    matched = record.statement == evidence.episode_failed.statement;
                }
            }
            MIRA_CHECK(matched);

            // Foreign scopes read nothing (ACL first gate, DEC-029 §1).
            auto other_subject = learning_scope();
            other_subject.subject_id = "another.agent";
            auto other_kind = learning_scope();
            other_kind.kind = MemoryScopeKind::Application;
            for (const auto &foreign : {other_subject, other_kind}) {
                MemoryQuery outside;
                outside.scopes = {foreign};
                outside.kinds =
                    std::vector<MemoryKind>{MemoryKind::Episode, MemoryKind::RecoveryLesson};
                const auto leaked = store.query(outside);
                MIRA_CHECK(leaked.has_value());
                MIRA_CHECK(leaked.value().records.empty());
            }

            // Replaying the deterministic mutation id is an idempotent NoOp:
            // a projection rebuilt from events lands on the same record
            // instead of duplicating it (DEC-030 §2).
            const auto recovered_episode =
                parse_episode_statement(evidence.episode_recovered);
            const auto rebuilt_record = episode_to_memory_record(
                recovered_episode, learning_scope(), evidence.episode_recovered.provenance,
                evidence.episode_recovered.recorded_at);
            MemoryMutation replay;
            replay.id = workflow_episode_mutation_id(recovered_episode.run_id);
            replay.type = MemoryMutationType::Add;
            replay.scope = learning_scope();
            replay.proposed = rebuilt_record;
            replay.evidence = evidence.episode_recovered.provenance;
            replay.reason = MutationReasonCode::VerifiedEvent;
            const auto replayed = store.apply(replay);
            MIRA_CHECK(replayed.has_value());
            MIRA_CHECK(replayed.value().idempotent_replay);
            MIRA_CHECK(replayed.value().applied == MemoryMutationType::Add);
            MIRA_CHECK(replayed.value().new_version == 1);

            MIRA_CHECK(store.close().has_value());
        }
        MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Evidence group 2: the DEC-030 §5 recipe, field by field.
// ---------------------------------------------------------------------------

int event_recipe_rebuild_field_evidence() {
    ScenarioEvidence evidence;
    {
        PersistenceHarness harness(temp_dir("rebuild"));
        drive_scenario(harness, evidence);
        harness.close();
    }

    const auto episode_failed = parse_episode_statement(evidence.episode_failed);
    const auto episode_recovered = parse_episode_statement(evidence.episode_recovered);
    const auto lesson =
        recovery_lesson_from_record(evidence.lesson_recovered).value();

    // --- Episode of the terminally failed run ------------------------------
    const auto rebuilt_a =
        rebuild_episode(evidence.events, evidence.failed_run.to_string());
    MIRA_CHECK(rebuilt_a.has_value());
    MIRA_CHECK(rebuilt_a.value().audit_recorded);
    // Direct-path self-consistency: the audit digest is the digest of the
    // stored record's own episode content.
    MIRA_CHECK(workflow_episode_digest(episode_failed) == rebuilt_a.value().audit_digest);

    const auto &recipe_a = rebuilt_a.value().record;
    MIRA_CHECK(recipe_a.run_id == episode_failed.run_id);
    MIRA_CHECK(recipe_a.workflow_id == episode_failed.workflow_id);
    MIRA_CHECK(recipe_a.ir_digest == episode_failed.ir_digest);
    MIRA_CHECK(recipe_a.policy == episode_failed.policy);
    MIRA_CHECK(recipe_a.outcome == episode_failed.outcome);
    MIRA_CHECK(recipe_a.failed_step_id == episode_failed.failed_step_id);
    MIRA_CHECK(recipe_a.failed_step_id.has_value());

    // Frozen gap list (recipe payloads carry none of these):
    MIRA_CHECK(episode_failed.failure_reason_code.has_value());
    MIRA_CHECK(!recipe_a.failure_reason_code.has_value());
    // The recovered run escalated exactly once; the recipe rebuild cannot
    // know that (no event carries the escalation count).
    MIRA_CHECK(episode_recovered.escalations == 1);
    const auto rebuilt_b =
        rebuild_episode(evidence.events, evidence.recovered_run.to_string());
    MIRA_CHECK(rebuilt_b.has_value());
    MIRA_CHECK(rebuilt_b.value().record.escalations == 0);
    // The proxied timestamp is a second clock reading of the same
    // settlement: bounded drift, no bit-for-bit guarantee.
    const auto drift_a = recipe_a.recorded_at_ms > episode_failed.recorded_at_ms
                             ? recipe_a.recorded_at_ms - episode_failed.recorded_at_ms
                             : episode_failed.recorded_at_ms - recipe_a.recorded_at_ms;
    MIRA_CHECK(drift_a <= 60'000);
    // Because of the gaps above, the rebuilt episode digest cannot match the
    // audited digest of the stored record.
    MIRA_CHECK(!(workflow_episode_digest(recipe_a) == rebuilt_a.value().audit_digest));

    // Identity and provenance are fully recoverable: deterministic ids and
    // the settled-event anchor.
    MIRA_CHECK(workflow_episode_memory_id(episode_failed.run_id) ==
               evidence.episode_failed.id);
    MIRA_CHECK(evidence.episode_failed.provenance.size() == 1);
    MIRA_CHECK(evidence.episode_failed.provenance[0] == rebuilt_a.value().settled_event);

    // --- Recovery lesson of the recovered run ------------------------------
    const auto rebuilt_lesson =
        rebuild_lesson(evidence.events, evidence.recovered_run.to_string());
    MIRA_CHECK(rebuilt_lesson.has_value());
    MIRA_CHECK(rebuilt_lesson.value().audit_recorded);
    MIRA_CHECK(recovery_lesson_digest(lesson) == rebuilt_lesson.value().audit_digest);

    const auto &recipe_lesson = rebuilt_lesson.value().record;
    MIRA_CHECK(recipe_lesson.lesson_id == lesson.lesson_id);
    MIRA_CHECK(recipe_lesson.workflow_id == lesson.workflow_id);
    MIRA_CHECK(recipe_lesson.ir_digest == lesson.ir_digest);
    MIRA_CHECK(recipe_lesson.recovered_run_id == lesson.recovered_run_id);
    MIRA_CHECK(recipe_lesson.failure.workflow_id == lesson.failure.workflow_id);
    MIRA_CHECK(recipe_lesson.failure.step_id == lesson.failure.step_id);
    MIRA_CHECK(recipe_lesson.resumed_without_patch == lesson.resumed_without_patch);
    MIRA_CHECK(!recipe_lesson.resumed_without_patch);
    MIRA_CHECK(recipe_lesson.outcome == lesson.outcome);
    // Patch identity survives via WorkflowPatchApplied; the patch digest and
    // per-entry targets do not (frozen gap list).
    MIRA_CHECK(rebuilt_lesson.value().applied_patch_ids.size() == 1);
    MIRA_CHECK(lesson.recovery.size() == 1);
    MIRA_CHECK(recipe_lesson.recovery.size() == 1);
    MIRA_CHECK(recipe_lesson.recovery[0].patch_id == lesson.recovery[0].patch_id);
    MIRA_CHECK(!lesson.recovery[0].patch_digest.empty());
    MIRA_CHECK(recipe_lesson.recovery[0].patch_digest.empty());
    MIRA_CHECK(lesson.recovery[0].targets.size() == 1);
    MIRA_CHECK(recipe_lesson.recovery[0].targets.empty());
    // The failing step's kind and sanitized reason code are not in the
    // recipe payloads either.
    MIRA_CHECK(lesson.failure.step_kind.has_value());
    MIRA_CHECK(!recipe_lesson.failure.step_kind.has_value());
    MIRA_CHECK(!lesson.failure.reason_code.empty());
    MIRA_CHECK(recipe_lesson.failure.reason_code.empty());
    const auto drift_lesson =
        recipe_lesson.recorded_at_ms > lesson.recorded_at_ms
            ? recipe_lesson.recorded_at_ms - lesson.recorded_at_ms
            : lesson.recorded_at_ms - recipe_lesson.recorded_at_ms;
    MIRA_CHECK(drift_lesson <= 60'000);
    MIRA_CHECK(!(recovery_lesson_digest(recipe_lesson) ==
                 rebuilt_lesson.value().audit_digest));

    MIRA_CHECK(recovery_lesson_memory_id(lesson.recovered_run_id) ==
               evidence.lesson_recovered.id);
    MIRA_CHECK(evidence.lesson_recovered.provenance.size() == 1);
    MIRA_CHECK(evidence.lesson_recovered.provenance[0] ==
               rebuilt_lesson.value().settled_event);
    return 0;
}

// ---------------------------------------------------------------------------
// Evidence group 3: slow and failing stores vs cancel and shutdown.
// ---------------------------------------------------------------------------

// Wraps an IMemory and stalls query/apply on the calling thread, mirroring a
// store worker that is merely slow (not broken).
class SlowMemory final : public IMemory {
  public:
    SlowMemory(std::shared_ptr<IMemory> inner, std::chrono::milliseconds delay)
        : inner_(std::move(inner)), delay_(delay) {}

    Result<MemoryQueryResult> query(const MemoryQuery &query) const override {
        std::this_thread::sleep_for(delay_);
        return inner_->query(query);
    }
    Result<std::optional<MemoryRecord>> get(MemoryId record) const override {
        return inner_->get(record);
    }
    Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override {
        std::this_thread::sleep_for(delay_);
        return inner_->apply(mutation);
    }
    Result<MemoryCompactionResult> compact(const MemoryScope &scope) override {
        return inner_->compact(scope);
    }
    Result<ErasureResult> erase(const ErasureRequest &request) override {
        return inner_->erase(request);
    }

  private:
    std::shared_ptr<IMemory> inner_;
    std::chrono::milliseconds delay_;
};

[[nodiscard]] WorkflowDefinition mnt25_recoverable_definition() {
    auto definition = base_definition("mnt25-slow");
    definition.default_policy = WorkflowPolicy::Recoverable;
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

int slow_store_keeps_cancel_closed() {
    WorkflowFixture fixture;
    auto inner = std::make_shared<FakeLearningMemory>();
    auto memory = std::make_shared<SlowMemory>(inner, std::chrono::milliseconds{200});
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    const auto run = workflow->create_run(mnt25_recoverable_definition(), JsonValue{},
                                          std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    // The escalation drive waits out the slow retrieval query (200 ms) and
    // still reaches WaitingAgent; degradation never blocks the escalation.
    const auto escalated = workflow->execute_run(run.value().run_id, context);
    MIRA_CHECK(escalated.has_value() &&
               escalated.value().state == WorkflowRunState::WaitingAgent);
    const auto continuation = workflow->agent_continuation(run.value().run_id);
    MIRA_CHECK(continuation.has_value());
    MIRA_CHECK(continuation.value().relevant_lessons.empty());

    // Cancelling from the wait state settles through the slow store (the
    // cancelled episode write stalls 200 ms) and still reaches the terminal
    // state idempotently.
    const auto cancelled = workflow->cancel_run(run.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(cancelled.value().state == WorkflowRunState::Cancelled);
    const auto again = workflow->cancel_run(run.value().run_id);
    MIRA_CHECK(again.has_value() && again.value().state == WorkflowRunState::Cancelled);
    const auto report = workflow->shutdown();
    MIRA_CHECK(report.clean);
    MIRA_CHECK(inner->stored_records() == 1); // the cancelled episode
    return 0;
}

int slow_store_keeps_shutdown_drain_clean() {
    WorkflowFixture fixture;
    auto inner = std::make_shared<FakeLearningMemory>();
    auto memory = std::make_shared<SlowMemory>(inner, std::chrono::milliseconds{200});
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    // An asynchronous drive enters its failing settlement (with the slow
    // episode write) while shutdown runs concurrently: the drain budget must
    // absorb the stall and still report clean.
    const auto run = workflow->create_run(strict_failure_definition(), JsonValue{},
                                          std::nullopt);
    MIRA_CHECK(run.has_value());
    MIRA_CHECK(workflow->start_run(run.value().run_id).has_value());
    const auto report = workflow->shutdown();
    MIRA_CHECK(report.clean);
    MIRA_CHECK(report.cancelled_runs <= 1);
    return 0;
}

int failing_store_keeps_cancel_and_shutdown_closed() {
    WorkflowFixture fixture;
    auto memory = std::make_shared<FakeLearningMemory>();
    memory->fail_applies = true;
    ScriptedTool tool{std::vector<int>{1}};
    MIRA_CHECK(register_registration(*fixture.registry_, tool.registration("scripted")).has_value());
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_learning_context(memory, learning_scope()).has_value());

    // A failing store settles the run, discloses the miss through the audit
    // event and never un-settles the terminal state.
    const auto run = workflow->create_run(strict_failure_definition(), JsonValue{},
                                          std::nullopt);
    MIRA_CHECK(run.has_value());
    OperationContext context;
    context.session = fixture.session_id_;
    context.started_at = Timestamp::now();
    const auto settled = workflow->execute_run(run.value().run_id, context);
    MIRA_CHECK(settled.has_value() && settled.value().state == WorkflowRunState::Failed);

    // The fake's failure knob is one-shot (it self-clears on the first
    // attempt); re-arm it so the cancelled run's episode write fails too.
    memory->fail_applies = true;

    // Cancelling an unrelated created run and shutting down stay closed with
    // the store still failing.
    const auto idle = workflow->create_run(strict_failure_definition(), JsonValue{},
                                           std::nullopt);
    MIRA_CHECK(idle.has_value());
    const auto cancelled = workflow->cancel_run(idle.value().run_id);
    MIRA_CHECK(cancelled.has_value() && cancelled.value().state == WorkflowRunState::Cancelled);
    const auto report = workflow->shutdown();
    MIRA_CHECK(report.clean);
    const auto outcomes = learning_event_outcomes(*fixture.events_, fixture.session_id_,
                                                  "WorkflowEpisodeRecorded");
    std::size_t failed = 0;
    for (const auto &outcome : outcomes) {
        if (outcome == "failed") {
            ++failed;
        }
    }
    MIRA_CHECK(failed == 2); // both settled runs disclosed the store failure
    MIRA_CHECK(memory->stored_records() == 0);
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {sqlite_records_survive_owner_rebuild,
         event_recipe_rebuild_field_evidence,
         slow_store_keeps_cancel_closed,
         slow_store_keeps_shutdown_drain_clean,
         failing_store_keeps_cancel_and_shutdown_closed});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    std::cout << "m13 learning persistence tests passed\n";
    return 0;
}
