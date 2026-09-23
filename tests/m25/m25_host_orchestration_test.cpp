// M25 (DEC-045) host-integration round — zero automation and explicit host
// orchestration over the Working Context lifecycle, contract gates HI-G3 and
// HI-G4 over the frozen plan §4.3/§4.4 semantics (fixtures:
// tests/m25/m25_support.hpp):
//
//   HI-G3 zero automation and host orchestration:
//     - symbol level: the AgentLoop header and implementation never reference
//       Supervisor / AutoCurator / promotion / fork / merge / commit entry
//       points, while carrying the frozen seam contract (positive control);
//     - behavior level: with supervisor, auto curator, store, memory and
//       consolidator all live, a seam-wired loop run leaves every
//       orchestration counter at zero — only host calls mutate the plane;
//     - host operation order: flush future bounded-await precedes the
//       terminal flag (the terminal-first order discards the forced
//       refresh); promotion routes through the generic
//       submit<WorkingContextPromotionReport> Deferrable and its future is
//       consumed; fork/merge commits only on a strictly advancing parent
//       watermark, a same-watermark different-digest commit fails closed as
//       "conflicting-watermark" with the store unchanged.
//
//   HI-G4 lifecycle and the two shutdown faces:
//     - run() is hosted by executor.submit_auto and its future is consumed;
//     - Supervisor face: after begin_shutdown() the in-flight Deferrable
//       resolves Cancelled and later orchestration submissions are rejected;
//     - Executor face: after shutdown(wait_for_tasks) a new run() submission
//       is rejected as an explicit result (AGENTS.md rule 8), never silently
//       dropped or executed;
//     - Loop face: an in-flight run() cancels through the OperationContext
//       probe without hanging; the seam supply has no blocking point on the
//       cancel path;
//     - snapshot store concurrent reads coexist with curation writes
//       (store-serialized; clean under ASAN/TSAN).

#include "m25_support.hpp"

#include "../m23/m23_promotion_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/memory_consolidation.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// HI-G3: symbol-level zero-automation assertion over the Loop contract files.
// The positive control pins the seam API so the scan cannot pass vacuously.
// ---------------------------------------------------------------------------

[[nodiscard]] bool file_contains(const std::string &path, const std::string &needle) {
    std::ifstream stream(path);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str().find(needle) != std::string::npos;
}

int loop_sources_carry_zero_orchestration_symbols() {
    const std::string source_dir = M25_SOURCE_DIR;
    const std::string header = source_dir + "/include/mira/agent_loop.hpp";
    const std::string implementation = source_dir + "/src/model/agent_loop.cpp";

    // Positive control: the frozen seam contract must be present in the
    // public header, or the scan below proves nothing.
    const char *seam_contract[] = {"WorkingContextSeamOptions", "WorkingContextSupplier",
                                   "set_working_context_supplier"};
    for (const auto *token : seam_contract) {
        MIRA_CHECK(file_contains(header, token));
    }

    // Zero automation: no orchestration entry point may be referenced by the
    // Loop header or implementation (plan §4.3; DEC-045 decision 3).
    const char *forbidden[] = {"ContextMemorySupervisor",
                               "WorkingContextAutoCurator",
                               "IContextCurator",
                               "promote_working_context",
                               "fork_working_context",
                               "working_context_delta_from_fork",
                               "merge_working_context_delta",
                               "commit_working_context",
                               "schedule_working_context",
                               "on_signal"};
    for (const auto *token : forbidden) {
        MIRA_CHECK(!file_contains(header, token));
        MIRA_CHECK(!file_contains(implementation, token));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G3: behavior-level zero automation — a seam-wired loop run issues no
// signal, no flush, no promotion and no fork/merge; only host calls do.
// ---------------------------------------------------------------------------

int loop_run_issues_zero_orchestration_calls() {
    const auto loop_session = m25_session_from_seed(600);
    const auto loop_task = m25_task_from_seed(601);
    const auto curate_session = m25_session_from_seed(602);
    const auto curate_task = m25_task_from_seed(603);

    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    MIRA_CHECK(executor.initialize(config));
    {
        ContextMemorySupervisor supervisor(executor);
        InMemoryWorkingContextStore store;
        M25ScriptedCurator curator;
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        FaithfulMemory memory;
        MemoryConsolidator consolidator;

        // The seam reads one aligned committed snapshot for the loop frame.
        auto snapshot = m25_snapshot(loop_session, loop_task, 1, 604);
        snapshot.constraints.push_back(
            m25_item("m25 loop constraint: confirm before sending", 1001, 0.9));
        MIRA_CHECK(store.put(snapshot).has_value());

        SeamLoopHarness harness(loop_session, loop_task);
        WorkingContextSupplierHarness supplier_harness(store, loop_session);
        auto loop = harness.make_loop();
        loop->set_working_context_supplier(supplier_harness.supplier());
        ModelDoneVerifier verifier;
        harness.script(m25_tap_then_done_script());
        const auto context = harness.loop_context();
        const auto outcome = loop->run(harness.spec(), context, verifier);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);
        MIRA_CHECK(supplier_harness.calls() == 2);

        // Zero automation: the loop run moved no orchestration counter, and
        // the curated chain and memory were never touched.
        const auto stats = auto_curator.stats();
        MIRA_CHECK(stats.signals == 0 && stats.committed == 0 && stats.forced_flushes == 0 &&
                   stats.flush_noops == 0 && stats.errors == 0);
        MIRA_CHECK(m25_count_or(store, curate_session, 1) == 0);
        MIRA_CHECK(m25_count_or(store, loop_session, 1) == 1);
        MIRA_CHECK(memory.stored_records() == 0);

        // Host-triggered curation is the only path that advances the chain.
        const auto checkpoint = m25_checkpoint(curate_session, curate_task, 8, 1);
        const auto scheduled = auto_curator.on_signal(
            curate_session, m25_refresh_input(checkpoint, m25_identity(curate_task),
                                              m25_live(curate_session, curate_task)));
        MIRA_CHECK(scheduled.has_value());
        const auto committed = scheduled->get();
        MIRA_CHECK(committed.has_value());
        MIRA_CHECK(committed.value().disposition == WorkingContextCommitDisposition::Committed);
        // The coordinator records the settled outcome on its own drain (the
        // host-orchestrated W3 surface); consume it before reading stats.
        auto_curator.drain(curate_session);
        const auto after = auto_curator.stats();
        MIRA_CHECK(after.signals == 1 && after.committed == 1);
        MIRA_CHECK(m25_count_or(store, curate_session, 0) == 1);
        MIRA_CHECK(memory.stored_records() == 0);
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G3: the flush future is bounded-awaited before the terminal flag; the
// terminal-first order lets the commit discipline discard the refresh.
// ---------------------------------------------------------------------------

int flush_future_precedes_terminal_flag() {
    const auto session = m25_session_from_seed(610);
    const auto task = m25_task_from_seed(611);

    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    MIRA_CHECK(executor.initialize(config));
    {
        ContextMemorySupervisor supervisor(executor);
        InMemoryWorkingContextStore store;
        M25ScriptedCurator curator;
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);

        // Wrong order, fresh chain: the host flips the session terminal
        // before the boundary flush; the forced refresh runs but the commit
        // validation discards it (DiscardedTerminal) and the store stays
        // empty.
        const auto checkpoint = m25_checkpoint(session, task, 8, 1);
        auto live = m25_live(session, task);
        live.session_terminal = true;
        const auto flushed =
            auto_curator.flush(session, m25_refresh_input(checkpoint, m25_identity(task), live));
        MIRA_CHECK(flushed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto outcome = flushed.get();
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().disposition ==
                   WorkingContextCommitDisposition::DiscardedTerminal);
        MIRA_CHECK(m25_count_or(store, session, 1) == 0);

        // Right order, second chain: the boundary refresh commits because
        // the session was still live; only after the flush future resolves
        // does the host flip the terminal flag.
        const auto live_session = m25_session_from_seed(612);
        const auto live_task = m25_task_from_seed(613);
        const auto live_checkpoint = m25_checkpoint(live_session, live_task, 8, 2);
        const auto scheduled = auto_curator.on_signal(
            live_session, m25_refresh_input(live_checkpoint, m25_identity(live_task),
                                            m25_live(live_session, live_task)));
        MIRA_CHECK(scheduled.has_value());
        MIRA_CHECK(scheduled->get().has_value());

        const auto boundary = auto_curator.flush(
            live_session, m25_refresh_input(live_checkpoint, m25_identity(live_task),
                                            m25_live(live_session, live_task)));
        MIRA_CHECK(boundary.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        MIRA_CHECK(boundary.get().has_value());
        // Terminal flag after the awaited flush: the committed boundary
        // snapshot survives, and a late arrival is discarded by the existing
        // commit discipline.
        auto late_live = m25_live(live_session, live_task);
        late_live.session_terminal = true;
        auto late_candidate = m25_snapshot(live_session, live_task, 3, 614);
        late_candidate.through_event_sequence = 9;
        const auto late = commit_working_context(store, late_candidate, late_live);
        MIRA_CHECK(late.disposition == WorkingContextCommitDisposition::DiscardedTerminal);
        const auto kept = store.latest(live_session);
        MIRA_CHECK(kept.has_value() && kept.value().has_value());
        MIRA_CHECK(kept.value()->through_event_sequence == 8);
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G3: promotion routes through the generic Deferrable submit and its
// future is consumed (W4 host face).
// ---------------------------------------------------------------------------

int promotion_routes_through_generic_deferrable() {
    const auto session = m25_session_from_seed(620);
    const auto task = m25_task_from_seed(621);

    executor::Executor executor;
    MIRA_CHECK(executor.initialize(executor::ExecutorConfig{}));
    {
        ContextMemorySupervisor supervisor(executor);
        FaithfulMemory memory;
        MemoryConsolidator consolidator;
        auto snapshot = m25_snapshot(session, task, 3, 622);
        snapshot.decisions.push_back(
            m25_item("m25 promoted: batch provider after the rate limit", 1010, 0.85));
        auto future = supervisor.submit<WorkingContextPromotionReport>(
            "working-context-promotion", SupervisedOpClass::Deferrable, [&](SupervisorToken) {
                return promote_working_context_to_memory(consolidator, memory, snapshot,
                                                         m23_env_scope("env-42"), m25_fixed_now());
            });
        const auto report = future.get();
        MIRA_CHECK(report.has_value());
        MIRA_CHECK(report.value().consolidation.entries.size() == 1);
        MIRA_CHECK(memory.stored_records() == 1);
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G3: the fork/merge host operation order — merge commits only on a
// strictly advancing parent watermark; a same-watermark different-digest
// commit fails closed as "conflicting-watermark" with the store unchanged.
// ---------------------------------------------------------------------------

int merge_commits_only_on_strict_parent_advance() {
    const auto parent_session = m25_session_from_seed(630);
    const auto parent_task = m25_task_from_seed(631);
    const auto child_session = m25_session_from_seed(632);
    const auto child_task = m25_task_from_seed(633);

    InMemoryWorkingContextStore store;
    const auto parent_identity = m25_identity(parent_task);

    // Parent chain: committed snapshot at watermark 10.
    const auto parent_checkpoint = m25_checkpoint(parent_session, parent_task, 10, 1);
    auto parent = working_context_from_checkpoint(parent_checkpoint, parent_identity);
    MIRA_CHECK(parent.has_value());
    const auto parent_commit =
        commit_working_context(store, parent.value(), m25_live(parent_session, parent_task));
    MIRA_CHECK(parent_commit.disposition == WorkingContextCommitDisposition::Committed);

    // Fork: the child baseline derives from the committed parent snapshot.
    WorkingContextForkSeed seed;
    seed.child_session = child_session;
    seed.child_identity = m25_identity(child_task);
    seed.child_watermark = 1;
    auto baseline = fork_working_context(parent.value(), seed);
    MIRA_CHECK(baseline.has_value());

    // The child chain advances on its own: one local addition.
    auto child_tip = baseline.value();
    child_tip.constraints.push_back(
        m25_item("m25 child finding: the second login path works", 1020, 0.8));
    child_tip.through_event_sequence = 2;
    MIRA_CHECK(child_tip.validate().has_value());
    const auto delta = working_context_delta_from_fork(baseline.value(), child_tip);
    MIRA_CHECK(delta.has_value());
    MIRA_CHECK(delta.value().entries.size() == 1);

    // Negative first: a different-digest candidate at the stored parent
    // watermark fails closed without touching the store.
    auto conflicting = parent.value();
    conflicting.next_actions.push_back(m25_item("m25 conflicting: deploy immediately", 1021, 0.7));
    MIRA_CHECK(conflicting.validate().has_value());
    const auto rejected =
        commit_working_context(store, conflicting, m25_live(parent_session, parent_task));
    MIRA_CHECK(rejected.disposition == WorkingContextCommitDisposition::DiscardedStale);
    MIRA_CHECK(rejected.reason_code == "conflicting-watermark");
    auto stored = store.latest(parent_session);
    MIRA_CHECK(stored.has_value() && stored.value().has_value());
    const auto stored_digest = stored.value()->state_digest();
    MIRA_CHECK(stored.value()->through_event_sequence == 10);

    // Positive: the mechanical merge product commits at the strictly
    // advanced parent watermark.
    auto merged = merge_working_context_delta(baseline.value(), parent.value(), delta.value(),
                                              parent_identity, 11);
    MIRA_CHECK(merged.has_value());
    MIRA_CHECK(merged.value().additions_appended == 1);
    const auto merged_commit =
        commit_working_context(store, merged.value().merged, m25_live(parent_session, parent_task));
    MIRA_CHECK(merged_commit.disposition == WorkingContextCommitDisposition::Committed);
    stored = store.latest(parent_session);
    MIRA_CHECK(stored.has_value() && stored.value().has_value());
    MIRA_CHECK(stored.value()->through_event_sequence == 11);
    MIRA_CHECK(stored.value()->state_digest() == merged.value().merged.state_digest());
    MIRA_CHECK(stored.value()->state_digest() != stored_digest);

    // The merged entry is host-consumable through the Layer 0 conversion:
    // the child finding rides in the parent's next request candidates.
    const auto items = context_items_from_working_context(stored.value().value());
    bool child_finding_present = false;
    for (const auto &item : items) {
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                if (text->text.find("the second login path works") != std::string::npos) {
                    child_finding_present = true;
                }
            }
        }
    }
    MIRA_CHECK(child_finding_present);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G4: run() hosted by executor.submit_auto, future consumed.
// ---------------------------------------------------------------------------

int run_future_managed_by_executor() {
    const auto session = m25_session_from_seed(640);
    const auto task = m25_task_from_seed(641);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 642);
    snapshot.constraints.push_back(m25_item("m25 constraint: confirm before sending", 1030, 0.9));
    MIRA_CHECK(store.put(snapshot).has_value());
    WorkingContextSupplierHarness supplier_harness(store, session);
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier verifier;
    harness.script(m25_tap_then_done_script());
    const auto context = harness.loop_context();
    auto future = harness.executor().submit_auto([&loop, &harness, &context, &verifier] {
        return loop->run(harness.spec(), context, verifier);
    });
    const auto outcome = future.get();
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(supplier_harness.calls() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G4: Supervisor face — begin_shutdown() cancels the in-flight Deferrable
// and rejects later orchestration submissions.
// ---------------------------------------------------------------------------

int supervisor_begin_shutdown_rejects_and_cancels() {
    const auto session = m25_session_from_seed(650);
    const auto task = m25_task_from_seed(651);

    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    MIRA_CHECK(executor.initialize(config));
    {
        ContextMemorySupervisor supervisor(executor);
        InMemoryWorkingContextStore store;
        M25ScriptedCurator curator;
        curator.park();
        M25CuratorReleaser releaser{curator};
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        FaithfulMemory memory;
        MemoryConsolidator consolidator;

        // In-flight Deferrable: the host signal schedules a refresh that
        // parks inside the scripted curator (deterministic entry evidence,
        // no sleeps for sequencing).
        const auto checkpoint = m25_checkpoint(session, task, 8, 1);
        const auto scheduled = auto_curator.on_signal(
            session, m25_refresh_input(checkpoint, m25_identity(task), m25_live(session, task)));
        MIRA_CHECK(scheduled.has_value());
        curator.wait_entered(1);

        // Supervisor face: the §17.2 order rejects producers and cancels the
        // parked in-flight op through its cooperative probe.
        const auto shutdown = supervisor.begin_shutdown();
        MIRA_CHECK(shutdown.critical_drain_complete);
        const auto cancelled = scheduled->get();
        MIRA_CHECK(!cancelled.has_value());
        MIRA_CHECK(cancelled.error().code == ErrorCode::Cancelled);
        // The coordinator keeps the chain in flight until its outcome is
        // recorded by a host drain; only then can a later signal be issued
        // (and be rejected by the closed supervisor) instead of absorbed.
        auto_curator.drain(session);

        // Later orchestration submissions are rejected: another host signal
        // (fresh threshold crossing — the failed fire re-armed the anchor at
        // watermark 8) and a promotion submit both resolve with errors.
        const auto rejected_checkpoint = m25_checkpoint(session, task, 16, 2);
        const auto rejected_signal = auto_curator.on_signal(
            session,
            m25_refresh_input(rejected_checkpoint, m25_identity(task), m25_live(session, task)));
        MIRA_CHECK(rejected_signal.has_value());
        const auto rejected_outcome = rejected_signal->get();
        MIRA_CHECK(!rejected_outcome.has_value());
        auto snapshot = m25_snapshot(session, task, 3, 652);
        snapshot.decisions.push_back(m25_item("m25 never promoted", 1040, 0.9));
        auto promotion = supervisor.submit<WorkingContextPromotionReport>(
            "working-context-promotion-after-close", SupervisedOpClass::Deferrable,
            [&](SupervisorToken) {
                return promote_working_context_to_memory(consolidator, memory, snapshot,
                                                         m23_env_scope("env-42"), m25_fixed_now());
            });
        const auto rejected_promotion = promotion.get();
        MIRA_CHECK(!rejected_promotion.has_value());
        MIRA_CHECK(memory.stored_records() == 0);
        MIRA_CHECK(supervisor.stats().rejected_closed >= 1);
        // Both failed schedules settle before their stats surface through
        // the coordinator's own drain.
        auto_curator.drain(session);
        MIRA_CHECK(auto_curator.stats().errors >= 1);
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G4: Executor face — after shutdown(wait_for_tasks) a new run()
// submission converts rejection into an explicit result and never executes.
// ---------------------------------------------------------------------------

int executor_shutdown_rejects_new_run_submission() {
    const auto session = m25_session_from_seed(660);
    const auto task = m25_task_from_seed(661);

    SeamLoopHarness harness(session, task);
    ModelDoneVerifier verifier;
    harness.script(m25_tap_then_done_script());
    {
        auto loop = harness.make_loop();
        const auto context = harness.loop_context();
        auto first = harness.executor().submit_auto([&loop, &harness, &context, &verifier] {
            return loop->run(harness.spec(), context, verifier);
        });
        const auto outcome = first.get();
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);
    }
    MIRA_CHECK(harness.executor().shutdown(true) == executor::ShutdownResult::Completed);
    const auto executed_before = harness.provider().requests().size();

    // A new run() submission after the executor closed must become an
    // explicit rejection — an exception from the future, or an error result
    // — never a silently dropped or silently executed task.
    auto loop = harness.make_loop();
    const auto context = harness.loop_context();
    bool explicit_rejection = false;
    try {
        auto rejected = harness.executor().submit_auto([&loop, &harness, &context, &verifier] {
            return loop->run(harness.spec(), context, verifier);
        });
        const auto outcome = rejected.get();
        if (!outcome.has_value()) {
            explicit_rejection = true;
        }
    } catch (const std::exception &) {
        explicit_rejection = true;
    } catch (...) {
        explicit_rejection = true;
    }
    MIRA_CHECK(explicit_rejection);
    MIRA_CHECK(harness.provider().requests().size() == executed_before);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G4: Loop face — an in-flight run() cancels through the OperationContext
// probe without hanging; the seam supply is not a blocking point (one supply
// call for the one assembled request, store untouched).
// ---------------------------------------------------------------------------

int loop_cancels_through_operation_context() {
    const auto session = m25_session_from_seed(670);
    const auto task = m25_task_from_seed(671);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 672);
    snapshot.constraints.push_back(m25_item("m25 constraint: confirm before sending", 1050, 0.9));
    MIRA_CHECK(store.put(snapshot).has_value());
    WorkingContextSupplierHarness supplier_harness(store, session);
    supplier_harness.set_script({WorkingContextSupplierHarness::Mode::Store});

    std::atomic<bool> cancel_requested{false};
    harness.script(m25_tap_then_done_script());
    harness.provider().on_after_response(
        0, [&cancel_requested] { cancel_requested.store(true, std::memory_order_release); });

    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    class NeverSatisfied final : public ILoopVerifier {
      public:
        Verdict verify(const Observation &, const DecisionCandidate &) override {
            return Verdict::NotSatisfied;
        }
    };
    NeverSatisfied verifier;
    auto context = harness.loop_context();
    context.cancellation_requested = [&cancel_requested] {
        return cancel_requested.load(std::memory_order_acquire);
    };
    const auto outcome = loop->run(harness.spec(), context, verifier);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Cancelled);
    // One assembled request, one supply call: the cancel path carried no
    // extra seam blocking point, and the store is untouched by the loop.
    MIRA_CHECK(supplier_harness.calls() == 1);
    MIRA_CHECK(m25_count_or(store, session, 0) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G4: snapshot store concurrent reads coexist with curation writes.
// ---------------------------------------------------------------------------

int store_reads_coexist_with_curation_writes() {
    const auto session = m25_session_from_seed(680);
    const auto task = m25_task_from_seed(681);
    const auto identity = m25_identity(task);
    const auto live = m25_live(session, task);

    InMemoryWorkingContextStore store;
    auto base = working_context_from_checkpoint(m25_checkpoint(session, task, 10, 1), identity);
    MIRA_CHECK(base.has_value());
    MIRA_CHECK(commit_working_context(store, base.value(), live).disposition ==
               WorkingContextCommitDisposition::Committed);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> read_errors{0};
    std::atomic<std::size_t> reads{0};
    executor::Executor executor;
    executor::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 32;
    MIRA_CHECK(executor.initialize(config));
    std::vector<std::future<void>> readers;
    for (std::size_t reader = 0; reader < 4; ++reader) {
        readers.push_back(executor.submit_auto([&store, &session, &stop, &read_errors, &reads] {
            while (!stop.load(std::memory_order_acquire)) {
                auto latest = store.latest(session);
                if (!latest.has_value()) {
                    read_errors.fetch_add(1, std::memory_order_acq_rel);
                }
                reads.fetch_add(1, std::memory_order_acq_rel);
            }
        }));
    }
    // Deterministic overlap: the write phase begins only after at least one
    // reader is observably inside its read loop, so reads genuinely race the
    // commits. The wait is bounded — the readers only ever wait on `stop`,
    // never on the writer, so no ordering deadlock is possible.
    const auto overlap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reads.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() > overlap_deadline) {
            std::cerr << "no reader made progress before the write phase\n";
            stop.store(true, std::memory_order_release);
            for (auto &reader : readers) {
                reader.get();
            }
            (void)executor.shutdown(true);
            return 1;
        }
        std::this_thread::yield();
    }

    // The curation path commits strictly advancing snapshots while readers
    // hammer latest().
    auto candidate = base.value();
    for (std::uint64_t watermark = 20; watermark <= 90; watermark += 10) {
        candidate.through_event_sequence = watermark;
        candidate.constraints.push_back(
            m25_item("m25 revision " + std::to_string(watermark), 1060 + watermark, 0.9));
        MIRA_CHECK(candidate.validate().has_value());
        const auto outcome = commit_working_context(store, candidate, live);
        MIRA_CHECK(outcome.disposition == WorkingContextCommitDisposition::Committed);
    }

    stop.store(true, std::memory_order_release);
    for (auto &reader : readers) {
        reader.get();
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    MIRA_CHECK(read_errors.load() == 0);
    MIRA_CHECK(reads.load() > 0);
    const auto latest = store.latest(session);
    MIRA_CHECK(latest.has_value() && latest.value().has_value());
    MIRA_CHECK(latest.value()->through_event_sequence == 90);
    // The ring policy bounds retention; the committed chain stays readable.
    MIRA_CHECK(m25_count_or(store, session, 99) <= 8);
    return 0;
}

} // namespace

int main() {
    const struct {
        const char *name;
        int (*fn)();
    } cases[] = {
        {"loop_sources_carry_zero_orchestration_symbols",
         loop_sources_carry_zero_orchestration_symbols},
        {"loop_run_issues_zero_orchestration_calls", loop_run_issues_zero_orchestration_calls},
        {"flush_future_precedes_terminal_flag", flush_future_precedes_terminal_flag},
        {"promotion_routes_through_generic_deferrable",
         promotion_routes_through_generic_deferrable},
        {"merge_commits_only_on_strict_parent_advance",
         merge_commits_only_on_strict_parent_advance},
        {"run_future_managed_by_executor", run_future_managed_by_executor},
        {"supervisor_begin_shutdown_rejects_and_cancels",
         supervisor_begin_shutdown_rejects_and_cancels},
        {"executor_shutdown_rejects_new_run_submission",
         executor_shutdown_rejects_new_run_submission},
        {"loop_cancels_through_operation_context", loop_cancels_through_operation_context},
        {"store_reads_coexist_with_curation_writes", store_reads_coexist_with_curation_writes},
    };
    for (const auto &entry : cases) {
        if (const int code = entry.fn(); code != 0) {
            std::cerr << "m25_host_orchestration_test: case failed: " << entry.name << '\n';
            return code;
        }
    }
    return 0;
}
