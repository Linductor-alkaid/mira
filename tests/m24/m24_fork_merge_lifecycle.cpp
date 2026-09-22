// M24 fork/merge G4-G6 integration & lifecycle cases, moved verbatim from
// m24_fork_merge_test.cpp (architecture max-file-lines split): commit/race
// integration, supervisor Deferrable routing, erasure, W3/W4 co-existence
// and recovery rebuild. Case names, assertions and semantics are unchanged;
// the case table in m24_fork_merge_test.cpp drives these via the
// declarations below its anonymous namespace.

#include "m24_fork_merge_support.hpp"

#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "m24_fork_merge_cases.hpp"

using namespace mira;
using mira::testing::GatedWorkingContextStore;
using mira::testing::m24_event_from_seed;
using mira::testing::m24_fill_all_sections;
using mira::testing::m24_fixed_now;
using mira::testing::m24_identity_from_seed;
using mira::testing::m24_item;
using mira::testing::m24_profile_from_seed;
using mira::testing::m24_section_names;
using mira::testing::m24_session_from_seed;
using mira::testing::m24_snapshot;
using mira::testing::m24_task_from_seed;
using mira::testing::StoreGateReleaser;

using namespace mira::m24_cases;

namespace {

// ---------------------------------------------------------------------------
// W5-G5: W3 co-existence — parent and child AutoCurator chains keep
// independent re-arm anchors and in-flight slots
// ---------------------------------------------------------------------------

// Scripted curator: deterministic previous-plus-appendix curation with an
// armable gate so an in-flight refresh can be held mid-flight (m22 pattern).
class ScriptedCurator final : public IContextCurator {
  public:
    Result<WorkingContextSnapshot> curate(const WorkingContextSnapshot *previous,
                                          const ConversationCheckpoint &checkpoint,
                                          std::span<const ConversationSegmentEntry>,
                                          const ContextCurationOptions &) override {
        {
            std::unique_lock lock(mutex_);
            // Session-scoped gate: only the target session's curation parks,
            // so a parallel session chain refreshes straight through (the
            // per-session in-flight property this case asserts).
            if (gated_ && checkpoint.session_id == gated_session_) {
                ++entered_;
                entered_cv_.notify_all();
                while (!released_) {
                    gate_cv_.wait_for(lock, std::chrono::milliseconds(1));
                }
            }
        }
        WorkingContextSnapshot snapshot;
        if (previous != nullptr) {
            snapshot = *previous;
        } else {
            snapshot.id = working_context_snapshot_id_from_seed("m24-auto-fresh-" +
                                                                checkpoint.session_id.to_string());
            snapshot.session_id = checkpoint.session_id;
            snapshot.task_id = checkpoint.task_id;
            snapshot.task_epoch = checkpoint.task_epoch;
            snapshot.environment_epoch = checkpoint.environment_epoch;
            snapshot.created_at = checkpoint.created_at;
        }
        snapshot.through_event_sequence = checkpoint.through_event_sequence;
        snapshot.source_checkpoints.push_back(checkpoint.id);
        WorkingContextItem appendix;
        appendix.content = "task: curated at " + std::to_string(checkpoint.through_event_sequence);
        appendix.source_events = checkpoint.source_events;
        appendix.source_sequence = checkpoint.through_event_sequence;
        appendix.confidence = 0.9;
        snapshot.active_tasks.push_back(appendix);
        return snapshot;
    }

    void arm_gate(const SessionId &session) {
        std::lock_guard lock(mutex_);
        gated_session_ = session;
        gated_ = true;
    }

    void release_gate() {
        std::lock_guard lock(mutex_);
        released_ = true;
        gate_cv_.notify_all();
    }

    void wait_curate_entered(std::size_t count) const {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this, count] { return entered_ >= count; });
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable entered_cv_;
    std::condition_variable gate_cv_;
    bool gated_ = false;
    SessionId gated_session_;
    bool released_ = false;
    std::size_t entered_ = 0;
};

[[nodiscard]] ConversationCheckpoint m24_checkpoint(const SessionId &session, const TaskId &task,
                                                    std::uint64_t watermark,
                                                    std::uint64_t revision) {
    ConversationCheckpoint checkpoint;
    checkpoint.id = conversation_checkpoint_id_from_seed(
        session.to_string() + "|" + std::to_string(watermark) + "|" + std::to_string(revision));
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = 3;
    checkpoint.environment_epoch = 7;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = m24_fixed_now();
    ConversationStatement statement;
    statement.content = "constraint r" + std::to_string(revision) + " keep the volume bounded";
    statement.source_events = {m24_event_from_seed(watermark + 1)};
    statement.source_sequence = watermark - 1;
    statement.confidence = 0.9;
    checkpoint.constraints.push_back(statement);
    checkpoint.summary = "revision " + std::to_string(revision);
    checkpoint.source_events = {m24_event_from_seed(watermark + 1)};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] WorkingContextRefreshInput m24_refresh_input(const ConversationCheckpoint &checkpoint,
                                                           const TaskId &task) {
    WorkingContextRefreshInput input;
    input.checkpoint = checkpoint;
    input.identity.task = task;
    input.identity.task_epoch = 3;
    input.identity.environment_epoch = 7;
    input.live.session = checkpoint.session_id;
    input.live.task = task;
    input.live.task_epoch = 3;
    input.live.environment_epoch = 7;
    input.reported_events = 0;
    return input;
}

[[nodiscard]] bool await_auto_settlement(WorkingContextAutoCurator &curator,
                                         const SessionId &session) {
    for (int guard = 0; guard < 100'000'000; ++guard) {
        curator.drain(session);
        const auto view = curator.session_view(session);
        if (!view.has_value() || !view->in_flight) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}
} // namespace

int merge_commit_watermark_discipline() {
    WorkingContextSnapshot parent = m24_snapshot(230);
    parent.constraints.push_back(m24_item("constraint: volume=30", 1301, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(231, 232, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: from child";
    addition.source_events = {m24_event_from_seed(1302)};
    addition.source_sequence = 1302;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    // 1. Strictly advancing watermark: the merge commits and the store
    // reflects the merged chain tip.
    {
        InMemoryWorkingContextStore store;
        const auto base = commit_working_context(store, parent, m24_live(parent));
        MIRA_CHECK(base.disposition == WorkingContextCommitDisposition::Committed);
        const auto advanced =
            merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                        parent.through_event_sequence + 1);
        MIRA_CHECK(advanced.has_value());
        const auto committed =
            commit_working_context(store, advanced.value().merged, m24_live(parent));
        MIRA_CHECK(committed.disposition == WorkingContextCommitDisposition::Committed);
        MIRA_CHECK(committed.committed.has_value());
        const auto latest = store.latest(parent.session_id);
        MIRA_CHECK(latest.has_value() && latest.value().has_value());
        MIRA_CHECK(latest.value()->through_event_sequence == parent.through_event_sequence + 1);
        MIRA_CHECK(snapshot_json(latest.value().value()) == snapshot_json(advanced.value().merged));
    }

    // 2. Same watermark, different digest: fail closed as a conflict, store
    // untouched (the merge does not waive §5.2).
    {
        InMemoryWorkingContextStore store;
        const auto base = commit_working_context(store, parent, m24_live(parent));
        MIRA_CHECK(base.disposition == WorkingContextCommitDisposition::Committed);
        const auto stored_before = store.latest(parent.session_id);
        MIRA_CHECK(stored_before.has_value() && stored_before.value().has_value());
        const std::string bytes_before = snapshot_json(stored_before.value().value());
        const auto replayed =
            merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                        parent.through_event_sequence); // same watermark
        MIRA_CHECK(replayed.has_value());
        const auto conflict =
            commit_working_context(store, replayed.value().merged, m24_live(parent));
        MIRA_CHECK(conflict.disposition == WorkingContextCommitDisposition::DiscardedStale);
        MIRA_CHECK(conflict.reason_code == "conflicting-watermark");
        const auto stored_after = store.latest(parent.session_id);
        MIRA_CHECK(stored_after.has_value() && stored_after.value().has_value());
        MIRA_CHECK(snapshot_json(stored_after.value().value()) == bytes_before);
    }

    // 3. Zero-effect delta at the same watermark: IdempotentNoOp (§4.4 item
    // 7), asserted on its own parent pair so the stored chain tip IS the
    // zero-effect target.
    {
        InMemoryWorkingContextStore store;
        WorkingContextSnapshot quiet_parent = parent;
        const auto base = commit_working_context(store, quiet_parent, m24_live(quiet_parent));
        MIRA_CHECK(base.disposition == WorkingContextCommitDisposition::Committed);
        WorkingContextSnapshot quiet_child = forked.value();
        quiet_child.id = working_context_snapshot_id_from_seed("m24-child-quiet-230");
        quiet_child.generated_by = m24_profile_from_seed(233);
        const auto quiet_delta = working_context_delta_from_fork(forked.value(), quiet_child);
        MIRA_CHECK(quiet_delta.has_value() && quiet_delta.value().entries.empty());
        const auto quiet_merge = merge_working_context_delta(
            forked.value(), quiet_parent, quiet_delta.value(), m24_parent_identity(quiet_parent),
            quiet_parent.through_event_sequence);
        MIRA_CHECK(quiet_merge.has_value());
        MIRA_CHECK(quiet_merge.value().merged.state_digest() == quiet_parent.state_digest());
        const auto noop =
            commit_working_context(store, quiet_merge.value().merged, m24_live(quiet_parent));
        MIRA_CHECK(noop.disposition == WorkingContextCommitDisposition::IdempotentNoOp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G4: stale watermarks, tuple mismatches and terminal lateness discard
// merge candidates exactly like every other snapshot
// ---------------------------------------------------------------------------

int merge_commit_stale_and_terminal_discards() {
    WorkingContextSnapshot parent = m24_snapshot(240);
    parent.constraints.push_back(m24_item("constraint: A", 1401, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(241, 242, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: from child";
    addition.source_events = {m24_event_from_seed(1402)};
    addition.source_sequence = 1402;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    InMemoryWorkingContextStore store;
    const WorkingContextCommitState live = m24_live(parent);
    MIRA_CHECK(commit_working_context(store, parent, live).disposition ==
               WorkingContextCommitDisposition::Committed);

    // The parent already committed its own watermark-49 tip; a merge
    // candidate at the fork-point watermark 48 is stale and changes nothing.
    WorkingContextSnapshot parent_tip = parent;
    parent_tip.id = working_context_snapshot_id_from_seed("m24-parent-tip-240");
    parent_tip.through_event_sequence = parent.through_event_sequence + 1;
    MIRA_CHECK(commit_working_context(store, parent_tip, live).disposition ==
               WorkingContextCommitDisposition::Committed);

    const auto stale_merge = merge_working_context_delta(
        forked.value(), parent, delta, m24_parent_identity(parent), parent.through_event_sequence);
    MIRA_CHECK(stale_merge.has_value());
    const auto stale = commit_working_context(store, stale_merge.value().merged, live);
    MIRA_CHECK(stale.disposition == WorkingContextCommitDisposition::DiscardedStale);

    // Tuple mismatch: the live chain moved to a new task epoch.
    WorkingContextCommitState moved_live = live;
    moved_live.task_epoch = live.task_epoch + 1;
    const auto advanced =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 2);
    MIRA_CHECK(advanced.has_value());
    const auto tuple_mismatch = commit_working_context(store, advanced.value().merged, moved_live);
    MIRA_CHECK(tuple_mismatch.disposition == WorkingContextCommitDisposition::DiscardedStale);

    // Terminal lateness: the session went terminal, the merge is dropped.
    WorkingContextCommitState terminal_live = live;
    terminal_live.session_terminal = true;
    const auto terminal = commit_working_context(store, advanced.value().merged, terminal_live);
    MIRA_CHECK(terminal.disposition == WorkingContextCommitDisposition::DiscardedTerminal);

    // None of the rejected candidates touched the stored chain.
    const auto latest = store.latest(parent.session_id);
    MIRA_CHECK(latest.has_value() && latest.value().has_value());
    MIRA_CHECK(latest.value()->state_digest() == parent_tip.state_digest());
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G4: parent and child chains are isolated — interleaved commits never
// interfere, and no child operation ever writes the parent store side
// ---------------------------------------------------------------------------

int parent_child_chains_isolated() {
    WorkingContextSnapshot parent = m24_snapshot(250);
    parent.constraints.push_back(m24_item("constraint: parent rule", 1501, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(251, 252, 4);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    InMemoryWorkingContextStore store;
    const WorkingContextCommitState parent_live = m24_live(parent);

    // The fork is a pure projection: the parent store side is untouched.
    MIRA_CHECK(store.count(parent.session_id).value() == 0);

    // The child baseline commits into the child chain only.
    WorkingContextCommitState child_live;
    child_live.session = seed.child_session;
    child_live.task = seed.child_identity.task;
    child_live.task_epoch = seed.child_identity.task_epoch;
    child_live.environment_epoch = seed.child_identity.environment_epoch;
    const auto baseline_commit = commit_working_context(store, forked.value(), child_live);
    MIRA_CHECK(baseline_commit.disposition == WorkingContextCommitDisposition::Committed);
    MIRA_CHECK(store.count(parent.session_id).value() == 0);
    MIRA_CHECK(store.count(seed.child_session).value() == 1);

    // Interleaved progress: each chain advances on its own watermark; a
    // child-watermark number inside the parent chain is a regression there.
    WorkingContextSnapshot child_advance = forked.value();
    child_advance.id = working_context_snapshot_id_from_seed("m24-child-curated-250");
    child_advance.through_event_sequence = 8;
    child_advance.active_tasks.push_back(m24_item("task: child subtask", 1502, 1.0));
    const auto child_commit = commit_working_context(store, child_advance, child_live);
    MIRA_CHECK(child_commit.disposition == WorkingContextCommitDisposition::Committed);

    WorkingContextSnapshot parent_advance = parent;
    parent_advance.id = working_context_snapshot_id_from_seed("m24-parent-advanced-250");
    parent_advance.through_event_sequence = 52;
    parent_advance.constraints.push_back(m24_item("constraint: parent rule v2", 1503, 0.9));
    const auto parent_commit = commit_working_context(store, parent_advance, parent_live);
    MIRA_CHECK(parent_commit.disposition == WorkingContextCommitDisposition::Committed);

    WorkingContextSnapshot parent_regressed = parent;
    parent_regressed.id = working_context_snapshot_id_from_seed("m24-parent-regressed-250");
    parent_regressed.through_event_sequence = 8; // below the parent chain watermark 52
    const auto regressed = commit_working_context(store, parent_regressed, parent_live);
    MIRA_CHECK(regressed.disposition == WorkingContextCommitDisposition::DiscardedStale);

    const auto parent_latest = store.latest(parent.session_id);
    MIRA_CHECK(parent_latest.has_value() && parent_latest.value().has_value());
    MIRA_CHECK(parent_latest.value()->through_event_sequence == 52);
    const auto child_latest = store.latest(seed.child_session);
    MIRA_CHECK(child_latest.has_value() && child_latest.value().has_value());
    MIRA_CHECK(child_latest.value()->through_event_sequence == 8);
    MIRA_CHECK(child_latest.value()->fork.has_value()); // the baseline anchors the child chain
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G4: after the parent opens a new epoch chain, a merge candidate stamped
// with the old identity is discarded — the merge never resurrects an old
// chain; merging onto the new chain commits
// ---------------------------------------------------------------------------

int fork_then_parent_epoch_change_rejects_stale_merge() {
    WorkingContextSnapshot parent = m24_snapshot(260);
    parent.constraints.push_back(m24_item("constraint: A", 1601, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(261, 262, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: from child";
    addition.source_events = {m24_event_from_seed(1602)};
    addition.source_sequence = 1602;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    InMemoryWorkingContextStore store;
    const WorkingContextCommitState live = m24_live(parent);
    MIRA_CHECK(commit_working_context(store, parent, live).disposition ==
               WorkingContextCommitDisposition::Committed);

    // The parent environment moved on: the live chain is now epoch 8.
    WorkingContextSnapshot parent_next = parent;
    parent_next.id = working_context_snapshot_id_from_seed("m24-parent-epoch8-260");
    parent_next.environment_epoch = 8;
    parent_next.through_event_sequence = 60;
    WorkingContextCommitState next_live = live;
    next_live.environment_epoch = 8;
    const auto new_chain = commit_working_context(store, parent_next, next_live);
    MIRA_CHECK(new_chain.disposition == WorkingContextCommitDisposition::Committed);

    // The merge candidate still carries the old-epoch identity: discarded.
    const auto stale_merge =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(stale_merge.has_value());
    const auto stale = commit_working_context(store, stale_merge.value().merged, next_live);
    MIRA_CHECK(stale.disposition == WorkingContextCommitDisposition::DiscardedStale);

    // Merging onto the new parent chain (identity matching the live epoch)
    // commits — the host re-anchors the merge against the current chain.
    const auto rebased_merge = merge_working_context_delta(forked.value(), parent_next, delta,
                                                           m24_parent_identity(parent_next),
                                                           parent_next.through_event_sequence + 1);
    MIRA_CHECK(rebased_merge.has_value());
    const auto rebased_commit =
        commit_working_context(store, rebased_merge.value().merged, next_live);
    MIRA_CHECK(rebased_commit.disposition == WorkingContextCommitDisposition::Committed);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G5: supervisor Deferrable routing — consumed future on the happy path,
// in-flight cancellation with zero partial writes, post-close rejection
// ---------------------------------------------------------------------------

int supervisor_routes_merge_commit_and_shuts_down() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        ContextMemorySupervisor supervisor(exec);
        WorkingContextSnapshot parent = m24_snapshot(270);
        parent.constraints.push_back(m24_item("constraint: A", 1701, 0.9));
        const WorkingContextForkSeed seed = m24_fork_seed(271, 272, 2);
        const auto forked = fork_working_context(parent, seed);
        MIRA_CHECK(forked.has_value());

        WorkingContextDelta delta;
        delta.schema_version = SchemaVersion{1, 0};
        delta.fork_base_snapshot_id = forked.value().id;
        delta.child_session_id = seed.child_session;
        WorkingContextDeltaEntry addition;
        addition.kind = WorkingContextDeltaEntryKind::Addition;
        addition.section = "constraints";
        addition.content = "constraint: from child";
        addition.source_events = {m24_event_from_seed(1702)};
        addition.source_sequence = 1702;
        addition.confidence = 0.8;
        delta.entries.push_back(addition);

        const auto candidate =
            merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                        parent.through_event_sequence + 1);
        MIRA_CHECK(candidate.has_value());

        WorkingContextCommitState live = m24_live(parent);
        GatedWorkingContextStore gated_store;

        // 1. Normal completion through the generic Deferrable route; the
        // future is consumed and the store holds the merged candidate.
        {
            auto future = supervisor.submit<WorkingContextCommitOutcome>(
                "working-context-merge-commit", SupervisedOpClass::Deferrable,
                [&](SupervisorToken) {
                    return commit_working_context(gated_store, candidate.value().merged, live);
                });
            const auto outcome = future.get();
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
            MIRA_CHECK(gated_store.puts_completed() == 1);
        }

        // 2. In-flight cancellation: the gated store parks the merge write;
        // shutdown flips the supervisor stop flag, the gated wait observes
        // it and refuses, the host op resolves the future with Cancelled —
        // and the store holds zero partial writes.
        WorkingContextSnapshot parent_two = m24_snapshot(273);
        parent_two.constraints.push_back(m24_item("constraint: A2", 1703, 0.9));
        const auto candidate_two = merge_working_context_delta(
            forked.value(), parent_two, delta, m24_parent_identity(parent_two),
            parent_two.through_event_sequence + 1);
        MIRA_CHECK(candidate_two.has_value());
        const WorkingContextCommitState live_two = m24_live(parent_two);
        gated_store.arm_gate();
        StoreGateReleaser releaser{gated_store};
        {
            auto future = supervisor.submit<WorkingContextCommitOutcome>(
                "working-context-merge-commit-cancelled", SupervisedOpClass::Deferrable,
                [&](SupervisorToken token) {
                    gated_store.set_cancel_probe(token);
                    const auto attempt =
                        commit_working_context(gated_store, candidate_two.value().merged, live_two);
                    if (attempt.disposition != WorkingContextCommitDisposition::Committed &&
                        token.stop_requested()) {
                        Error cancelled;
                        cancelled.code = ErrorCode::Cancelled;
                        cancelled.domain = "mira.test.m24";
                        cancelled.safe_message = "merge commit cancelled at shutdown";
                        return Result<WorkingContextCommitOutcome>(cancelled);
                    }
                    return Result<WorkingContextCommitOutcome>(attempt);
                });
            gated_store.wait_put_entered(1);
            const auto shutdown = supervisor.begin_shutdown();
            MIRA_CHECK(shutdown.critical_drain_complete);
            const auto outcome = future.get();
            MIRA_CHECK(!outcome.has_value());
            MIRA_CHECK(outcome.error().code == ErrorCode::Cancelled);
            MIRA_CHECK(gated_store.puts_completed() == 1); // only the first write landed
            const auto latest = gated_store.latest(parent.session_id);
            MIRA_CHECK(latest.has_value() && latest.value().has_value());
            MIRA_CHECK(latest.value()->id == candidate.value().merged.id);
            MIRA_CHECK(!gated_store.latest(parent_two.session_id).value().has_value());
        }

        // 3. Post-close rejection: submissions after begin_shutdown resolve
        // immediately with an error and never reach the store.
        {
            auto future = supervisor.submit<WorkingContextCommitOutcome>(
                "working-context-merge-commit-after-close", SupervisedOpClass::Deferrable,
                [&](SupervisorToken) {
                    return commit_working_context(gated_store, candidate_two.value().merged,
                                                  live_two);
                });
            const auto outcome = future.get();
            MIRA_CHECK(!outcome.has_value());
            MIRA_CHECK(gated_store.puts_completed() == 1);
        }

        // 4. Stats line up with the observed outcomes: two admitted, one
        // completed, one failed (the cancelled in-flight op resolves its own
        // Cancelled error), one rejected.
        const auto stats = supervisor.stats();
        MIRA_CHECK(stats.admitted == 2);
        MIRA_CHECK(stats.completed == 1);
        MIRA_CHECK(stats.failed == 1);
        MIRA_CHECK(stats.rejected_closed == 1);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G5: erasing the child session leaves the parent snapshots and the
// merged content untouched; only the child projection clears
// ---------------------------------------------------------------------------

int erase_child_session_preserves_parent_and_merge() {
    WorkingContextSnapshot parent = m24_snapshot(280);
    parent.constraints.push_back(m24_item("constraint: parent rule", 1801, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(281, 282, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: merged from child";
    addition.source_events = {m24_event_from_seed(1802)};
    addition.source_sequence = 1802;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    const auto merged =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(merged.has_value());

    InMemoryWorkingContextStore store;
    const WorkingContextCommitState parent_live = m24_live(parent);
    MIRA_CHECK(commit_working_context(store, parent, parent_live).disposition ==
               WorkingContextCommitDisposition::Committed);
    MIRA_CHECK(commit_working_context(store, merged.value().merged, parent_live).disposition ==
               WorkingContextCommitDisposition::Committed);
    WorkingContextCommitState child_live;
    child_live.session = seed.child_session;
    child_live.task = seed.child_identity.task;
    child_live.task_epoch = seed.child_identity.task_epoch;
    child_live.environment_epoch = seed.child_identity.environment_epoch;
    MIRA_CHECK(commit_working_context(store, forked.value(), child_live).disposition ==
               WorkingContextCommitDisposition::Committed);

    const std::string parent_bytes_before = snapshot_json(merged.value().merged);
    const auto erased = store.erase_session(seed.child_session, "m24 erase child branch");
    MIRA_CHECK(erased.has_value() && erased.value() == 1);
    MIRA_CHECK(store.count(seed.child_session).value() == 0);

    // The parent chain — including the merged child contribution — is
    // untouched by the child erasure.
    const auto parent_latest = store.latest(parent.session_id);
    MIRA_CHECK(parent_latest.has_value() && parent_latest.value().has_value());
    MIRA_CHECK(snapshot_json(parent_latest.value().value()) == parent_bytes_before);
    MIRA_CHECK(store.count(parent.session_id).value() == 2);
    bool merged_content_present = false;
    for (const WorkingContextItem &item : parent_latest.value()->constraints) {
        merged_content_present =
            merged_content_present || item.content == "constraint: merged from child";
    }
    MIRA_CHECK(merged_content_present);
    return 0;
}

int w3_auto_curator_chains_independent() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        ContextMemorySupervisor supervisor(exec);
        InMemoryWorkingContextStore store;
        ScriptedCurator curator;
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);

        const SessionId parent_session = m24_session_from_seed(290);
        const TaskId parent_task = m24_task_from_seed(291);
        const SessionId child_session = m24_session_from_seed(292);
        const TaskId child_task = m24_task_from_seed(293);

        // The child refreshes; the parent chain was never signaled and the
        // parent store side stays empty.
        const auto child_input =
            m24_refresh_input(m24_checkpoint(child_session, child_task, 8, 1), child_task);
        {
            auto future = auto_curator.flush(child_session, child_input);
            const auto outcome = future.get();
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
        }
        MIRA_CHECK(await_auto_settlement(auto_curator, child_session));
        const auto child_view = auto_curator.session_view(child_session);
        MIRA_CHECK(child_view.has_value());
        MIRA_CHECK(child_view->settled_watermark == 8);
        MIRA_CHECK(child_view->in_flight == false);
        MIRA_CHECK(!auto_curator.session_view(parent_session).has_value());
        MIRA_CHECK(!store.latest(parent_session).value().has_value());

        // The parent refreshes on its own anchor; the child view is neither
        // re-armed nor advanced by the parent's fire.
        const auto parent_input =
            m24_refresh_input(m24_checkpoint(parent_session, parent_task, 48, 1), parent_task);
        {
            auto future = auto_curator.flush(parent_session, parent_input);
            const auto outcome = future.get();
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
        }
        MIRA_CHECK(await_auto_settlement(auto_curator, parent_session));
        const auto parent_view = auto_curator.session_view(parent_session);
        MIRA_CHECK(parent_view.has_value());
        MIRA_CHECK(parent_view->settled_watermark == 48);
        const auto child_view_after = auto_curator.session_view(child_session);
        MIRA_CHECK(child_view_after.has_value());
        MIRA_CHECK(child_view_after->settled_watermark == 8);
        MIRA_CHECK(child_view_after->last_attempt_watermark == child_view->last_attempt_watermark);
        MIRA_CHECK(child_view_after->consecutive_failures == 0);

        // In-flight slots are per session chain: the child parks inside the
        // scripted curator while the parent completes its own refresh.
        curator.arm_gate(child_session);
        const auto child_parked_input =
            m24_refresh_input(m24_checkpoint(child_session, child_task, 16, 2), child_task);
        auto child_future = auto_curator.flush(child_session, child_parked_input);
        curator.wait_curate_entered(1);
        const auto parked_view = auto_curator.session_view(child_session);
        MIRA_CHECK(parked_view.has_value() && parked_view->in_flight);

        const auto parent_parallel_input =
            m24_refresh_input(m24_checkpoint(parent_session, parent_task, 56, 2), parent_task);
        auto parent_future = auto_curator.flush(parent_session, parent_parallel_input);
        const auto parent_outcome = parent_future.get();
        MIRA_CHECK(parent_outcome.has_value());
        MIRA_CHECK(parent_outcome.value().disposition ==
                   WorkingContextCommitDisposition::Committed);

        curator.release_gate();
        const auto child_outcome = child_future.get();
        MIRA_CHECK(child_outcome.has_value());
        MIRA_CHECK(child_outcome.value().disposition == WorkingContextCommitDisposition::Committed);
        MIRA_CHECK(await_auto_settlement(auto_curator, child_session));
        const auto child_settled = auto_curator.session_view(child_session);
        MIRA_CHECK(child_settled.has_value() && child_settled->settled_watermark == 16);
        MIRA_CHECK(await_auto_settlement(auto_curator, parent_session));
        const auto parent_settled = auto_curator.session_view(parent_session);
        MIRA_CHECK(parent_settled.has_value() && parent_settled->settled_watermark == 56);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G5: W4 co-existence — the merged parent promotes through the frozen
// mapping; the unmerged child branch content has no promotion path
// ---------------------------------------------------------------------------

int w4_promotion_maps_merged_parent_only() {
    WorkingContextSnapshot parent = m24_snapshot(300);
    parent.constraints.push_back(m24_item("constraint: deploy only on tuesdays", 1901, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(301, 302, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    // Child branch content that never merges back.
    WorkingContextSnapshot child = forked.value();
    child.id = working_context_snapshot_id_from_seed("m24-child-unmerged-300");
    child.verified_facts.push_back(m24_item("fact: only the child ever saw this", 1902, 0.9));

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "decisions";
    addition.content = "decision: batch provider merged from the child";
    addition.source_events = {m24_event_from_seed(1903)};
    addition.source_sequence = 1903;
    addition.confidence = 0.85;
    delta.entries.push_back(addition);

    const auto merged =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(merged.has_value());

    // The frozen M23 section->kind mapping is provenance-blind: the merged
    // decision projects like any parent-side decision.
    const auto projection =
        memory_candidates_from_working_context(merged.value().merged, testing_scope());
    MIRA_CHECK(projection.has_value());
    bool merged_decision_promoted = false;
    bool unmerged_branch_promoted = false;
    for (const auto &candidate : projection.value().candidates) {
        merged_decision_promoted =
            merged_decision_promoted ||
            candidate.proposed.statement == "decision: batch provider merged from the child";
        unmerged_branch_promoted =
            unmerged_branch_promoted ||
            candidate.proposed.statement == "fact: only the child ever saw this";
    }
    MIRA_CHECK(merged_decision_promoted);
    MIRA_CHECK(!unmerged_branch_promoted);

    // The child chain itself still projects its own content — but that is
    // the child session's own promotion path, never the parent's.
    const auto child_projection = memory_candidates_from_working_context(child, testing_scope());
    MIRA_CHECK(child_projection.has_value());
    bool child_sees_own_fact = false;
    for (const auto &candidate : child_projection.value().candidates) {
        child_sees_own_fact = child_sees_own_fact ||
                              candidate.proposed.statement == "fact: only the child ever saw this";
    }
    MIRA_CHECK(child_sees_own_fact);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G6 (test half): from an empty store the rebuilt parent baseline re-forks
// to the same child baseline id and digest
// ---------------------------------------------------------------------------

int recovery_rebuilds_fork_baseline_idempotently() {
    const SessionId session = m24_session_from_seed(310);
    const TaskId task = m24_task_from_seed(311);
    const ConversationCheckpoint checkpoint = m24_checkpoint(session, task, 24, 1);
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = 3;
    identity.environment_epoch = 7;

    // Original chain: deterministic projection, then fork.
    const auto parent_original = working_context_from_checkpoint(checkpoint, identity);
    MIRA_CHECK(parent_original.has_value());
    const WorkingContextForkSeed seed = m24_fork_seed(312, 313, 3);
    const auto child_original = fork_working_context(parent_original.value(), seed);
    MIRA_CHECK(child_original.has_value());

    // Recovery: the store is gone; the parent baseline re-projects from the
    // retained checkpoint (RULE-07) and the child baseline re-forks from it.
    const auto parent_rebuilt = working_context_from_checkpoint(checkpoint, identity);
    MIRA_CHECK(parent_rebuilt.has_value());
    MIRA_CHECK(parent_rebuilt.value().id == parent_original.value().id);
    MIRA_CHECK(parent_rebuilt.value().state_digest() == parent_original.value().state_digest());
    const auto child_rebuilt = fork_working_context(parent_rebuilt.value(), seed);
    MIRA_CHECK(child_rebuilt.has_value());
    MIRA_CHECK(child_rebuilt.value().id == child_original.value().id);
    MIRA_CHECK(child_rebuilt.value().state_digest() == child_original.value().state_digest());
    MIRA_CHECK(snapshot_json(child_rebuilt.value()) == snapshot_json(child_original.value()));
    return 0;
}
