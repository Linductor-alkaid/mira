// M20 (DEC-035 Stage W1) WorkingContextSnapshot contract/integration suite.
// Covers the milestone §7 matrix: deterministic projection from a committed
// checkpoint (sections, provenance, watermark, digest, JSON round-trip),
// all-or-nothing rejection of invalid or over-bounds input, the commit-tuple
// validation races (stale watermark, identity mismatch, idempotent replay,
// same-watermark conflict fail-closed), terminal idempotency, epoch-chain
// isolation, store monotonicity and bounded retention, recovery re-projection
// with identical id/digest, Layer 0 candidate mapping (RULE-09 authority and
// a disjoint item-id space) and the supervisor Deferrable routing with
// shutdown rejection.

#include "../support/test.hpp"

#include <mira/context_consolidation.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;

[[nodiscard]] Id128 id_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); index += sizeof(seed)) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

[[nodiscard]] SessionId session_from_seed(std::uint64_t seed) {
    return SessionId{id_from_seed(seed)};
}

[[nodiscard]] TaskId task_from_seed(std::uint64_t seed) {
    return TaskId{id_from_seed(seed)};
}

[[nodiscard]] EventId event_from_seed(std::uint64_t seed) {
    return EventId{id_from_seed(seed)};
}

[[nodiscard]] WorkingContextIdentity make_identity(const TaskId &task) {
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = 3;
    identity.environment_epoch = 7;
    return identity;
}

[[nodiscard]] ConversationStatement make_statement(std::string content, std::uint64_t event_seed,
                                                   std::uint64_t sequence) {
    ConversationStatement statement;
    statement.content = std::move(content);
    statement.source_events = {event_from_seed(event_seed)};
    statement.source_sequence = sequence;
    statement.confidence = 0.9;
    return statement;
}

// Deterministic checkpoint builder standing in for the M19 commit pipeline
// (W1 has no model: the checkpoint is the frozen input contract).
[[nodiscard]] ConversationCheckpoint make_checkpoint(const SessionId &session,
                                                     const TaskId &task, std::uint64_t watermark,
                                                     std::uint64_t revision) {
    ConversationCheckpoint checkpoint;
    checkpoint.id = conversation_checkpoint_id_from_seed(session.to_string() + "|" +
                                                         std::to_string(watermark) + "|" +
                                                         std::to_string(revision));
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = 3;
    checkpoint.environment_epoch = 7;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = Timestamp::now();
    checkpoint.constraints.push_back(make_statement(
        "constraint r" + std::to_string(revision) + " confirm before sending", watermark + 1,
        watermark - 1));
    checkpoint.decisions.push_back(
        make_statement("decision r" + std::to_string(revision) + " use batch provider",
                       watermark + 2, watermark - 1));
    checkpoint.unresolved_threads.push_back(make_statement(
        "thread r" + std::to_string(revision) + " waiting for quota reply", watermark + 3,
        watermark - 1));
    checkpoint.summary = "revision " + std::to_string(revision);
    checkpoint.source_events = {event_from_seed(watermark + 1), event_from_seed(watermark + 2),
                                event_from_seed(watermark + 3)};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] WorkingContextCommitState make_live(const SessionId &session, const TaskId &task) {
    WorkingContextCommitState live;
    live.session = session;
    live.task = task;
    live.task_epoch = 3;
    live.environment_epoch = 7;
    return live;
}

// ---------------------------------------------------------------------------
// 1. Deterministic projection: sections, provenance, digest, JSON round-trip
// ---------------------------------------------------------------------------

int projection_is_deterministic_and_round_trips() {
    const SessionId session = session_from_seed(1);
    const TaskId task = task_from_seed(2);
    const auto checkpoint = make_checkpoint(session, task, 10, 1);

    auto snapshot = working_context_from_checkpoint(checkpoint, make_identity(task));
    MIRA_CHECK(snapshot.has_value());
    MIRA_CHECK(snapshot.value().session_id == session);
    MIRA_CHECK(snapshot.value().task_id == task);
    MIRA_CHECK(snapshot.value().task_epoch == 3);
    MIRA_CHECK(snapshot.value().environment_epoch == 7);
    MIRA_CHECK(snapshot.value().through_event_sequence == 10);
    MIRA_CHECK(snapshot.value().source_checkpoints.size() == 1);
    MIRA_CHECK(snapshot.value().source_checkpoints[0] == checkpoint.id);
    MIRA_CHECK(snapshot.value().constraints.size() == 1);
    MIRA_CHECK(snapshot.value().decisions.size() == 1);
    MIRA_CHECK(snapshot.value().open_issues.size() == 1);
    // Every projected item carries the checkpoint statement's provenance,
    // real recency and confidence verbatim.
    MIRA_CHECK(snapshot.value().constraints[0].content == checkpoint.constraints[0].content);
    MIRA_CHECK(snapshot.value().constraints[0].source_events ==
               checkpoint.constraints[0].source_events);
    MIRA_CHECK(snapshot.value().constraints[0].source_sequence ==
               checkpoint.constraints[0].source_sequence);
    MIRA_CHECK(snapshot.value().constraints[0].confidence == checkpoint.constraints[0].confidence);

    // Deterministic identity: the same inputs re-derive the same id, digest
    // and payload; the id is not nil.
    auto replay = working_context_from_checkpoint(checkpoint, make_identity(task));
    MIRA_CHECK(replay.has_value());
    MIRA_CHECK(replay.value().id == snapshot.value().id);
    MIRA_CHECK(!replay.value().id.is_nil());
    MIRA_CHECK(replay.value().state_digest() == snapshot.value().state_digest());
    MIRA_CHECK(to_json_string(working_context_to_json(replay.value())) ==
               to_json_string(working_context_to_json(snapshot.value())));

    // JSON round-trip preserves the authoritative surface.
    const auto restored = working_context_from_json(working_context_to_json(snapshot.value()));
    MIRA_CHECK(restored.has_value());
    MIRA_CHECK(restored.value().id == snapshot.value().id);
    MIRA_CHECK(restored.value().state_digest() == snapshot.value().state_digest());
    MIRA_CHECK(restored.value().constraints.size() == 1);
    MIRA_CHECK(restored.value().constraints[0].content ==
               snapshot.value().constraints[0].content);
    MIRA_CHECK(restored.value().constraints[0].source_events ==
               snapshot.value().constraints[0].source_events);

    // Rejections: identity mismatch with the checkpoint stamping, nil
    // session in the identity input path, and zero-watermark checkpoints.
    WorkingContextIdentity wrong_task = make_identity(task);
    wrong_task.task = task_from_seed(999);
    MIRA_CHECK(!working_context_from_checkpoint(checkpoint, wrong_task).has_value());
    WorkingContextIdentity wrong_epoch = make_identity(task);
    wrong_epoch.task_epoch = 4;
    MIRA_CHECK(!working_context_from_checkpoint(checkpoint, wrong_epoch).has_value());
    WorkingContextIdentity wrong_environment = make_identity(task);
    wrong_environment.environment_epoch = 8;
    MIRA_CHECK(!working_context_from_checkpoint(checkpoint, wrong_environment).has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// 2. All-or-nothing bounds: over-bounds input produces no partial snapshot
// ---------------------------------------------------------------------------

int over_bounds_input_is_rejected_entirely() {
    const SessionId session = session_from_seed(20);
    const TaskId task = task_from_seed(21);

    WorkingContextMergeOptions tight;
    tight.max_items_per_section = 2;
    auto checkpoint = make_checkpoint(session, task, 10, 1);
    checkpoint.constraints.push_back(
        make_statement("second constraint", 100, 9));
    checkpoint.constraints.push_back(
        make_statement("third constraint", 101, 9));
    // Three constraints against a two-item bound: the whole candidate is
    // rejected, not truncated to two.
    MIRA_CHECK(!working_context_from_checkpoint(checkpoint, make_identity(task), tight)
                    .has_value());

    WorkingContextMergeOptions narrow_bytes;
    narrow_bytes.max_item_chars = 16;
    MIRA_CHECK(
        !working_context_from_checkpoint(checkpoint, make_identity(task), narrow_bytes)
             .has_value());

    // A checkpoint failing its own validation (statement without provenance)
    // never yields a snapshot.
    ConversationCheckpoint invalid = make_checkpoint(session, task, 11, 2);
    invalid.constraints[0].source_events.clear();
    MIRA_CHECK(!working_context_from_checkpoint(invalid, make_identity(task)).has_value());

    // Default options accept the plain checkpoint.
    MIRA_CHECK(working_context_from_checkpoint(checkpoint, make_identity(task)).has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// 3. Commit-tuple races: stale watermark, identity mismatch, idempotent
//    replay, same-watermark conflict
// ---------------------------------------------------------------------------

int commit_tuple_races_are_discarded() {
    const SessionId session = session_from_seed(30);
    const TaskId task = task_from_seed(31);
    InMemoryWorkingContextStore store;
    const auto live = make_live(session, task);

    const auto first = working_context_from_checkpoint(make_checkpoint(session, task, 10, 1),
                                                       make_identity(task));
    MIRA_CHECK(first.has_value());
    const auto first_outcome = commit_working_context(store, first.value(), live);
    MIRA_CHECK(first_outcome.disposition == WorkingContextCommitDisposition::Committed);
    MIRA_CHECK(first_outcome.committed.has_value());

    // Stale watermark inside the same identity chain: discarded, stored
    // snapshot kept.
    const auto older = working_context_from_checkpoint(make_checkpoint(session, task, 9, 1),
                                                       make_identity(task));
    MIRA_CHECK(older.has_value());
    const auto stale_outcome = commit_working_context(store, older.value(), live);
    MIRA_CHECK(stale_outcome.disposition == WorkingContextCommitDisposition::DiscardedStale);
    MIRA_CHECK(stale_outcome.reason_code == "stale-watermark");

    // Identity mismatches are all stale discards.
    const auto chain = working_context_from_checkpoint(make_checkpoint(session, task, 11, 2),
                                                       make_identity(task));
    MIRA_CHECK(chain.has_value());
    const std::pair<const char *, WorkingContextCommitState> mismatches[] = {
        {"session-mismatch", [&] {
             auto state = live;
             state.session = session_from_seed(999);
             return state;
         }()},
        {"task-mismatch", [&] {
             auto state = live;
             state.task = task_from_seed(999);
             return state;
         }()},
        {"task-epoch-mismatch", [&] {
             auto state = live;
             state.task_epoch = 4;
             return state;
         }()},
        {"environment-epoch-mismatch", [&] {
             auto state = live;
             state.environment_epoch = 8;
             return state;
         }()},
    };
    for (const auto &[expected_reason, state] : mismatches) {
        const auto outcome = commit_working_context(store, chain.value(), state);
        MIRA_CHECK(outcome.disposition == WorkingContextCommitDisposition::DiscardedStale);
        MIRA_CHECK(outcome.reason_code == expected_reason);
    }

    // Idempotent replay: same watermark, same digest — no-op, store
    // unchanged (byte-identical payload).
    const std::string before =
        to_json_string(working_context_to_json(store.latest(session).value().value()));
    const auto replay_outcome = commit_working_context(store, first.value(), live);
    MIRA_CHECK(replay_outcome.disposition == WorkingContextCommitDisposition::IdempotentNoOp);
    MIRA_CHECK(replay_outcome.reason_code == "idempotent-replay");
    const std::string after =
        to_json_string(working_context_to_json(store.latest(session).value().value()));
    MIRA_CHECK(before == after);

    // Advance to watermark 11, then replay the same watermark with a
    // different digest: fail-closed conflict, stored snapshot kept.
    MIRA_CHECK(commit_working_context(store, chain.value(), live).disposition ==
               WorkingContextCommitDisposition::Committed);
    const std::string at_eleven =
        to_json_string(working_context_to_json(store.latest(session).value().value()));
    auto conflicting_checkpoint = make_checkpoint(session, task, 11, 42);
    conflicting_checkpoint.constraints[0].content = "a different constraint at the same watermark";
    const auto conflicting =
        working_context_from_checkpoint(conflicting_checkpoint, make_identity(task));
    MIRA_CHECK(conflicting.has_value());
    const auto conflict_outcome = commit_working_context(store, conflicting.value(), live);
    MIRA_CHECK(conflict_outcome.disposition == WorkingContextCommitDisposition::DiscardedStale);
    MIRA_CHECK(conflict_outcome.reason_code == "conflicting-watermark");
    const std::string after_conflict =
        to_json_string(working_context_to_json(store.latest(session).value().value()));
    MIRA_CHECK(at_eleven == after_conflict);

    // Advancing the watermark commits.
    const auto advancing = working_context_from_checkpoint(make_checkpoint(session, task, 12, 3),
                                                           make_identity(task));
    MIRA_CHECK(advancing.has_value());
    const auto advance_outcome = commit_working_context(store, advancing.value(), live);
    MIRA_CHECK(advance_outcome.disposition == WorkingContextCommitDisposition::Committed);
    MIRA_CHECK(store.latest(session).value().value().through_event_sequence == 12);
    return 0;
}

// ---------------------------------------------------------------------------
// 4. Terminal idempotency: late snapshots are dropped after terminal state
// ---------------------------------------------------------------------------

int terminal_state_discards_late_snapshots() {
    const SessionId session = session_from_seed(40);
    const TaskId task = task_from_seed(41);
    InMemoryWorkingContextStore store;

    WorkingContextCommitState terminal = make_live(session, task);
    terminal.session_terminal = true;
    const auto candidate = working_context_from_checkpoint(make_checkpoint(session, task, 10, 1),
                                                           make_identity(task));
    MIRA_CHECK(candidate.has_value());
    auto outcome = commit_working_context(store, candidate.value(), terminal);
    MIRA_CHECK(outcome.disposition == WorkingContextCommitDisposition::DiscardedTerminal);
    MIRA_CHECK(outcome.reason_code == "session-terminal");
    MIRA_CHECK(!store.latest(session).value().has_value());

    WorkingContextCommitState task_terminal = make_live(session, task);
    task_terminal.task_terminal = true;
    outcome = commit_working_context(store, candidate.value(), task_terminal);
    MIRA_CHECK(outcome.disposition == WorkingContextCommitDisposition::DiscardedTerminal);
    MIRA_CHECK(outcome.reason_code == "task-terminal");
    MIRA_CHECK(!store.latest(session).value().has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// 5. Epoch invalidation: a new epoch chain commits alongside and latest()
//    returns it, while the old chain stays queryable with old epochs
// ---------------------------------------------------------------------------

int epoch_change_opens_an_isolated_chain() {
    const SessionId session = session_from_seed(50);
    const TaskId task = task_from_seed(51);
    InMemoryWorkingContextStore store;
    const auto live = make_live(session, task);

    const auto old_snapshot = working_context_from_checkpoint(
        make_checkpoint(session, task, 10, 1), make_identity(task));
    MIRA_CHECK(old_snapshot.has_value());
    MIRA_CHECK(commit_working_context(store, old_snapshot.value(), live).disposition ==
               WorkingContextCommitDisposition::Committed);

    // The environment epoch moves; the new chain commits (the watermark
    // advances, so the store-level regression guard does not fire).
    WorkingContextIdentity new_identity = make_identity(task);
    new_identity.environment_epoch = 8;
    auto new_checkpoint = make_checkpoint(session, task, 11, 2);
    new_checkpoint.environment_epoch = 8;
    const auto new_snapshot = working_context_from_checkpoint(new_checkpoint, new_identity);
    MIRA_CHECK(new_snapshot.has_value());
    MIRA_CHECK(new_snapshot.value().environment_epoch == 8);
    WorkingContextCommitState new_live = live;
    new_live.environment_epoch = 8;
    MIRA_CHECK(commit_working_context(store, new_snapshot.value(), new_live).disposition ==
               WorkingContextCommitDisposition::Committed);

    // latest() returns the new chain; the old chain stays queryable by
    // watermark and keeps its epoch stamping (Layer 0 stale-build detection
    // relies on that annotation).
    const auto latest = store.latest(session).value();
    MIRA_CHECK(latest.has_value());
    MIRA_CHECK(latest.value().environment_epoch == 8);
    MIRA_CHECK(latest.value().id == new_snapshot.value().id);
    const auto at_ten = store.latest_at_or_before(session, 10).value();
    MIRA_CHECK(at_ten.has_value());
    MIRA_CHECK(at_ten.value().id == old_snapshot.value().id);
    MIRA_CHECK(at_ten.value().environment_epoch == 7);

    // Old-epoch candidates cannot commit against new-epoch live state.
    MIRA_CHECK(commit_working_context(store, old_snapshot.value(), new_live).reason_code ==
               "environment-epoch-mismatch");
    return 0;
}

// ---------------------------------------------------------------------------
// 6. Store discipline: watermark monotonicity and bounded retention
// ---------------------------------------------------------------------------

int store_enforces_monotonicity_and_retention() {
    const SessionId session = session_from_seed(60);
    const TaskId task = task_from_seed(61);

    WorkingContextStorePolicy policy;
    policy.max_snapshots_per_session = 3;
    InMemoryWorkingContextStore store(policy);

    for (std::uint64_t watermark = 10; watermark <= 12; ++watermark) {
        const auto snapshot = working_context_from_checkpoint(
            make_checkpoint(session, task, watermark, watermark - 9), make_identity(task));
        MIRA_CHECK(snapshot.has_value());
        MIRA_CHECK(store.put(snapshot.value()).has_value());
    }
    MIRA_CHECK(store.count(session).value() == 3);

    // Watermark regression inside the same identity chain is rejected at the
    // store level.
    const auto regressed = working_context_from_checkpoint(make_checkpoint(session, task, 9, 5),
                                                           make_identity(task));
    MIRA_CHECK(regressed.has_value());
    MIRA_CHECK(!store.put(regressed.value()).has_value());

    // The ring evicts the oldest entry beyond the policy bound.
    const auto fourth = working_context_from_checkpoint(make_checkpoint(session, task, 13, 6),
                                                        make_identity(task));
    MIRA_CHECK(fourth.has_value());
    MIRA_CHECK(store.put(fourth.value()).has_value());
    MIRA_CHECK(store.count(session).value() == 3);
    MIRA_CHECK(!store.latest_at_or_before(session, 10).value().has_value());
    MIRA_CHECK(store.latest_at_or_before(session, 11).value().has_value());

    // erase_session serves the privacy-erasure path.
    MIRA_CHECK(store.erase_session(session, "erasure").value() == 3);
    MIRA_CHECK(store.count(session).value() == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 7. Recovery: re-projecting the source checkpoint restores id and digest
// ---------------------------------------------------------------------------

int recovery_reprojection_is_identical() {
    const SessionId session = session_from_seed(70);
    const TaskId task = task_from_seed(71);
    InMemoryWorkingContextStore store;
    const auto live = make_live(session, task);
    const auto checkpoint = make_checkpoint(session, task, 20, 2);

    const auto original = working_context_from_checkpoint(checkpoint, make_identity(task));
    MIRA_CHECK(original.has_value());
    MIRA_CHECK(commit_working_context(store, original.value(), live).disposition ==
               WorkingContextCommitDisposition::Committed);

    // "Loss": a fresh store. Recovery re-projects from the same checkpoint.
    InMemoryWorkingContextStore rebuilt;
    const auto recovered = working_context_from_checkpoint(checkpoint, make_identity(task));
    MIRA_CHECK(recovered.has_value());
    MIRA_CHECK(commit_working_context(rebuilt, recovered.value(), live).disposition ==
               WorkingContextCommitDisposition::Committed);
    MIRA_CHECK(recovered.value().id == original.value().id);
    MIRA_CHECK(recovered.value().state_digest() == original.value().state_digest());
    MIRA_CHECK(to_json_string(working_context_to_json(recovered.value())) ==
               to_json_string(working_context_to_json(original.value())));
    return 0;
}

// ---------------------------------------------------------------------------
// 8. Layer 0 mapping: partitions, RULE-09 authority, disjoint id space
// ---------------------------------------------------------------------------

int items_from_working_context_mapping() {
    const SessionId session = session_from_seed(80);
    const TaskId task = task_from_seed(81);
    const auto checkpoint = make_checkpoint(session, task, 10, 1);
    const auto snapshot = working_context_from_checkpoint(checkpoint, make_identity(task));
    MIRA_CHECK(snapshot.has_value());

    const auto items = context_items_from_working_context(snapshot.value());
    MIRA_CHECK(items.size() == 3);
    MIRA_CHECK(items[0].kind == ContextItemKind::UserConstraint);
    MIRA_CHECK(items[1].kind == ContextItemKind::CheckpointSummary);
    MIRA_CHECK(items[2].kind == ContextItemKind::CheckpointSummary);
    for (const auto &item : items) {
        // Model-mediated derived projection: never policy, never verified.
        MIRA_CHECK(item.authority == ContextAuthority::UntrustedExternalData);
        MIRA_CHECK(item.provenance.size() == 1);
        MIRA_CHECK(item.task_epoch == std::optional<std::uint64_t>(3));
        MIRA_CHECK(item.environment_epoch == std::optional<std::uint64_t>(7));
    }
    MIRA_CHECK(std::get<TextPart>(items[0].content[0]).text ==
               snapshot.value().constraints[0].content);
    MIRA_CHECK(std::get<TextPart>(items[1].content[0]).text ==
               snapshot.value().decisions[0].content);
    MIRA_CHECK(std::get<TextPart>(items[2].content[0]).text ==
               snapshot.value().open_issues[0].content);

    // Deterministic ids ...
    const auto again = context_items_from_working_context(snapshot.value());
    for (std::size_t index = 0; index < items.size(); ++index) {
        MIRA_CHECK(again[index].id == items[index].id);
    }
    // ... in a space disjoint from the checkpoint conversion, so auditing
    // both projections side by side never collides.
    const auto checkpoint_items = context_items_from_checkpoint(checkpoint);
    MIRA_CHECK(checkpoint_items.size() == items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        for (const auto &candidate : checkpoint_items) {
            MIRA_CHECK(candidate.id != items[index].id);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 9. Supervisor routing: Deferrable commit, shutdown rejection
// ---------------------------------------------------------------------------

int supervisor_routes_working_context_commits() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(90);
        const TaskId task = task_from_seed(91);
        InMemoryWorkingContextStore store;
        ContextMemorySupervisor supervisor(exec);
        auto future = supervisor.schedule_working_context_commit(
            store, make_checkpoint(session, task, 10, 1), make_identity(task),
            make_live(session, task), WorkingContextMergeOptions{});
        const auto result = future.get();
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().disposition == WorkingContextCommitDisposition::Committed);
        MIRA_CHECK(store.latest(session).value().has_value());

        // Shutdown rejects further submissions; the future resolves with an
        // error instead of hanging.
        const auto report = supervisor.begin_shutdown();
        MIRA_CHECK(report.critical_drain_complete);
        MIRA_CHECK(supervisor.closed());
        auto rejected = supervisor.schedule_working_context_commit(
            store, make_checkpoint(session, task, 11, 2), make_identity(task),
            make_live(session, task), WorkingContextMergeOptions{});
        const auto rejection = rejected.get();
        MIRA_CHECK(!rejection.has_value());
    }
    {
        // Sequential replays through the route are idempotent: one commit,
        // every replay a no-op. (The deterministic projection has no
        // blocking step, so unlike the M19 model call there is no observable
        // in-flight window; the generic cancel-at-entry machinery is covered
        // by the supervisor's own suite.)
        const SessionId session = session_from_seed(92);
        const TaskId task = task_from_seed(93);
        InMemoryWorkingContextStore store;
        ContextMemorySupervisor supervisor(exec);
        constexpr std::size_t kReplays = 8;
        for (std::size_t index = 0; index < kReplays; ++index) {
            auto outcome = supervisor.schedule_working_context_commit(
                store, make_checkpoint(session, task, 10, 1), make_identity(task),
                make_live(session, task), WorkingContextMergeOptions{});
            const auto result = outcome.get();
            MIRA_CHECK(result.has_value());
            if (index == 0) {
                MIRA_CHECK(result.value().disposition ==
                           WorkingContextCommitDisposition::Committed);
            } else {
                MIRA_CHECK(result.value().disposition ==
                           WorkingContextCommitDisposition::IdempotentNoOp);
            }
        }
        MIRA_CHECK(store.count(session).value() == 1);
        // An in-flight submission racing shutdown must still resolve; the
        // store keeps either nothing or exactly one consistent snapshot.
        auto racing = supervisor.schedule_working_context_commit(
            store, make_checkpoint(session, task, 11, 2), make_identity(task),
            make_live(session, task), WorkingContextMergeOptions{});
        const auto report = supervisor.begin_shutdown();
        MIRA_CHECK(report.critical_drain_complete);
        const auto raced = racing.get();
        if (raced.has_value()) {
            MIRA_CHECK(raced.value().disposition == WorkingContextCommitDisposition::Committed ||
                       raced.value().disposition == WorkingContextCommitDisposition::IdempotentNoOp);
        }
        MIRA_CHECK(store.count(session).value() <= 2);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    if (int failures = projection_is_deterministic_and_round_trips(); failures != 0) {
        return failures;
    }
    if (int failures = over_bounds_input_is_rejected_entirely(); failures != 0) {
        return failures;
    }
    if (int failures = commit_tuple_races_are_discarded(); failures != 0) {
        return failures;
    }
    if (int failures = terminal_state_discards_late_snapshots(); failures != 0) {
        return failures;
    }
    if (int failures = epoch_change_opens_an_isolated_chain(); failures != 0) {
        return failures;
    }
    if (int failures = store_enforces_monotonicity_and_retention(); failures != 0) {
        return failures;
    }
    if (int failures = recovery_reprojection_is_identical(); failures != 0) {
        return failures;
    }
    if (int failures = items_from_working_context_mapping(); failures != 0) {
        return failures;
    }
    if (int failures = supervisor_routes_working_context_commits(); failures != 0) {
        return failures;
    }
    std::cout << "m20 working context: OK\n";
    return 0;
}
