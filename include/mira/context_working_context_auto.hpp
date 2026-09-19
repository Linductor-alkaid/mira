#pragma once

#include <mira/context_consolidation.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_working_context.hpp>
#include <mira/core_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Curator Stage W3: supervisor-driven automatic refresh of the
// Working Context (DEC-035, design §8/§9/§13; M22)
//
// Stage W2 shipped the model-mediated curator behind the supervisor's
// Deferrable route, but the host had to decide *when* to curate. This header
// freezes the trigger policy and the coordinator that applies it: the host
// reports observable signals (a committed checkpoint with its recent events,
// plus how much execution-event activity happened since the last signal) and
// the coordinator decides whether the snapshot chain should refresh now, wait,
// or absorb the signal into the refresh already in flight.
//
// Everything here is deterministic control-plane logic. The coordinator owns
// no threads and schedules nothing outside `ContextMemorySupervisor::
// schedule_working_context_curate` — the same Deferrable route, shutdown
// order and cancellation semantics as every other Context/Memory operation
// (AGENTS.md Executor discipline: no hidden background loops, every issued
// future is either handed to the caller or owned and drained by the
// coordinator).
//
// Two staleness axes drive the policy (M22 §4.1):
// - watermark: the conversation-sequence distance between the last refresh
//   attempt and the input checkpoint's `through_event_sequence` — the
//   freshest conversation state curation can legally advance to;
// - event count: execution-event activity reported by the host since the
//   last attempt, orthogonal to the conversation sequence.
// A fire additionally requires the input checkpoint to be *unsettled* (its
// watermark past the last committed/no-op watermark of the session chain):
// re-curating a checkpoint the store already covers could only produce an
// idempotent no-op or a same-watermark conflict, so the coordinator never
// schedules one. Request *size* budgets stay in `ContextCurationOptions`
// (M21); frequency budgeting is thresholds + coalescing.
// ---------------------------------------------------------------------------

// Trigger thresholds and tracking bounds. Defaults are documented values, not
// a frozen contract (mirrors `ConsolidationOptions`).
struct WorkingContextTriggerPolicy final {
    // Conversation-sequence distance between the last attempt and the input
    // checkpoint that qualifies as a watermark fire.
    std::uint64_t watermark_interval = 8;
    // Reported execution-event delta since the last attempt that qualifies as
    // an event-count fire.
    std::uint64_t event_count_interval = 16;
    // Bounded per-coordinator session tracking (RULE-08); signals beyond it
    // resolve with ResourceExhausted instead of growing the map.
    std::size_t max_tracked_sessions = 64;

    [[nodiscard]] Result<void> validate() const;
};

enum class WorkingContextTriggerKind : std::uint8_t {
    None,       // thresholds unmet (or no unsettled checkpoint): no refresh
    Watermark,  // conversation-sequence distance reached the interval
    EventCount, // reported execution-event delta reached the interval
};

[[nodiscard]] std::string working_context_trigger_kind_name(WorkingContextTriggerKind kind);

struct WorkingContextTriggerDecision final {
    bool refresh = false;
    WorkingContextTriggerKind kind = WorkingContextTriggerKind::None;
};

// Pure policy: watermark distance takes precedence over the event-count axis
// (both thresholds met is a watermark fire). Fires are evaluated against the
// watermark of the input checkpoint, not the live session tail — see the
// header comment above.
[[nodiscard]] WorkingContextTriggerDecision evaluate_working_context_trigger(
    const WorkingContextTriggerPolicy &policy, std::uint64_t last_attempt_watermark,
    std::uint64_t events_since_attempt, std::uint64_t current_watermark) noexcept;

// One host-reported signal: the freshest committed checkpoint with its
// bounded recent-events tail (M21 curation semantics), the task-side identity
// and live control-plane state for the commit pipeline, and the execution-
// event delta since the previous signal. `checkpoint.session_id` must equal
// the signaled session; recent events must not run past the checkpoint
// watermark (the curator's existing pre-checks resolve the future with
// InvalidArgument otherwise).
struct WorkingContextRefreshInput final {
    ConversationCheckpoint checkpoint;
    std::vector<ConversationSegmentEntry> recent_events;
    WorkingContextIdentity identity;
    WorkingContextCommitState live;
    std::uint64_t reported_events = 0;
};

// Control-plane view of one tracked session chain (observability for hosts
// and tests; all scalars, no handles).
struct WorkingContextSessionView final {
    bool in_flight = false;
    // Threshold re-arm anchor: the watermark observed at the last fire.
    // Failures re-arm too — the next fire waits for a fresh threshold
    // crossing, so a failing curator degrades instead of tight-looping.
    std::uint64_t last_attempt_watermark = 0;
    // Watermark of the last settled Committed/IdempotentNoOp snapshot;
    // failures and discards do not advance it. Reset to zero when the input
    // identity opens a new chain (epoch bump), so a new chain is never
    // misread as "already covered" by the old chain's watermark.
    std::uint64_t settled_watermark = 0;
    std::uint64_t events_since_attempt = 0;
    std::uint64_t consecutive_failures = 0;
};

struct WorkingContextAutoStats final {
    std::uint64_t signals = 0;
    std::uint64_t absorbed = 0; // coalesced into an in-flight refresh
    std::uint64_t fires_watermark = 0;
    std::uint64_t fires_event_count = 0;
    std::uint64_t forced_flushes = 0;
    std::uint64_t flush_noops = 0; // "auto-refresh-current" short-circuits
    std::uint64_t committed = 0;
    std::uint64_t idempotent_noops = 0;
    std::uint64_t discarded_stale = 0;
    std::uint64_t discarded_terminal = 0;
    std::uint64_t errors = 0;               // error-resolved futures (op failures
                                            // and rejected signals)
    std::uint64_t consecutive_failures = 0; // current worst session streak
};

// Applies the frozen trigger policy to host-reported signals and refreshes
// the Working Context through the supervisor's Deferrable curation route.
//
// Ownership and concurrency (design §8, M22 §4.2): all methods must be called
// from the owning control plane (no internal locking; supervised operations
// only touch their own captures). Per session chain at most one policy
// refresh is in flight; signals arriving meanwhile are absorbed (counted,
// event deltas accrue, nothing queues). `flush` is the task-boundary barrier:
// it bounded-drains the in-flight refresh, then force-fires any unsettled
// checkpoint regardless of thresholds. The monotonic commit discipline
// (design §5.2) backstops any forced/policy concurrency and drops post-
// terminal latecomers — hosts must await `flush`'s future before flipping
// session/task terminal flags.
class WorkingContextAutoCurator final {
  public:
    // The supervisor, curator and store must outlive the coordinator.
    // Validates policy and curation options (InvalidArgument on failure).
    WorkingContextAutoCurator(ContextMemorySupervisor &supervisor, IContextCurator &curator,
                              IWorkingContextStore &store, WorkingContextTriggerPolicy policy = {},
                              ContextCurationOptions options = {});
    ~WorkingContextAutoCurator();

    WorkingContextAutoCurator(const WorkingContextAutoCurator &) = delete;
    WorkingContextAutoCurator &operator=(const WorkingContextAutoCurator &) = delete;

    // Applies §4.2: drains a settled in-flight refresh, absorbs the signal if
    // one is still running, otherwise evaluates the policy and — thresholds
    // met and the checkpoint unsettled — schedules one refresh. The returned
    // shared future is the caller's handle to consume; the coordinator keeps
    // its own copy to record the outcome on a later drain (futures on one
    // promise are shared, never duplicated work). Returns nullopt when the
    // signal produced no refresh. Rejected signals (session mismatch,
    // tracking capacity) return an already-resolved error shared future; the
    // caller must consume it either way.
    [[nodiscard]] std::optional<std::shared_future<Result<WorkingContextCommitOutcome>>>
    on_signal(const SessionId &session, WorkingContextRefreshInput input);

    // Task-boundary forced flush (M22 §4.2): bounded-drains the in-flight
    // refresh (budget = the curation deadline; a timeout leaves it in flight
    // beside the forced refresh — the commit discipline orders them), then
    // force-fires the input checkpoint when it is unsettled. An already
    // covered boundary resolves immediately with IdempotentNoOp /
    // "auto-refresh-current" and zero curator calls. The returned shared
    // future must be consumed; await it before declaring the task or session
    // terminal.
    [[nodiscard]] std::shared_future<Result<WorkingContextCommitOutcome>>
    flush(const SessionId &session, WorkingContextRefreshInput input);

    // Records the outcome of a settled in-flight refresh, if any (no-op when
    // nothing is owned or it has not settled). Results surface through
    // session_view()/stats().
    void drain(const SessionId &session);

    [[nodiscard]] std::optional<WorkingContextSessionView>
    session_view(const SessionId &session) const;
    [[nodiscard]] WorkingContextAutoStats stats() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
