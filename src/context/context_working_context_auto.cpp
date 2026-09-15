#include <mira/context_working_context_auto.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace mira {

namespace {

constexpr const char *kDomain = "mira.context_working_context_auto";

[[nodiscard]] Error auto_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = kDomain;
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] bool same_chain(const WorkingContextIdentity &identity, const TaskId &task,
                              std::uint64_t task_epoch, std::uint64_t environment_epoch) {
    return identity.task == task && identity.task_epoch == task_epoch &&
           identity.environment_epoch == environment_epoch;
}

} // namespace

Result<void> WorkingContextTriggerPolicy::validate() const {
    if (watermark_interval == 0) {
        return auto_error(ErrorCode::InvalidArgument, "watermark_interval must be positive");
    }
    if (event_count_interval == 0) {
        return auto_error(ErrorCode::InvalidArgument, "event_count_interval must be positive");
    }
    if (max_tracked_sessions == 0) {
        return auto_error(ErrorCode::InvalidArgument, "max_tracked_sessions must be positive");
    }
    return Result<void>();
}

std::string working_context_trigger_kind_name(WorkingContextTriggerKind kind) {
    switch (kind) {
    case WorkingContextTriggerKind::Watermark:
        return "watermark";
    case WorkingContextTriggerKind::EventCount:
        return "event_count";
    case WorkingContextTriggerKind::None:
        break;
    }
    return "none";
}

WorkingContextTriggerDecision evaluate_working_context_trigger(
    const WorkingContextTriggerPolicy &policy, std::uint64_t last_attempt_watermark,
    std::uint64_t events_since_attempt, std::uint64_t current_watermark) noexcept {
    // Watermark distance takes precedence: both thresholds met is a watermark
    // fire. A regressed observation (current behind the last attempt) is not
    // a distance and never fires.
    if (current_watermark >= last_attempt_watermark &&
        current_watermark - last_attempt_watermark >= policy.watermark_interval) {
        return WorkingContextTriggerDecision{true, WorkingContextTriggerKind::Watermark};
    }
    if (events_since_attempt >= policy.event_count_interval) {
        return WorkingContextTriggerDecision{true, WorkingContextTriggerKind::EventCount};
    }
    return WorkingContextTriggerDecision{false, WorkingContextTriggerKind::None};
}

class WorkingContextAutoCurator::Impl final {
  public:
    Impl(ContextMemorySupervisor &supervisor, IContextCurator &curator, IWorkingContextStore &store,
         WorkingContextTriggerPolicy policy, ContextCurationOptions options)
        : supervisor_(supervisor), curator_(curator), store_(store),
          // WorkingContextTriggerPolicy is trivially copyable: a plain copy,
          // no move (performance-move-const-arg).
          policy_(policy), options_(std::move(options)) {}

    using SharedOutcome = std::shared_future<Result<WorkingContextCommitOutcome>>;

    struct ChainState final {
        // One shared future per outstanding refresh: the caller consumes its
        // copy, the coordinator drains its own to keep the accounting true.
        std::optional<SharedOutcome> in_flight;
        std::uint64_t in_flight_watermark = 0;
        TaskId in_flight_task;
        std::uint64_t in_flight_task_epoch = 0;
        std::uint64_t in_flight_environment_epoch = 0;
        // Threshold re-arm anchor (failures re-arm too — one attempt per
        // fresh threshold crossing, never a tight loop).
        std::uint64_t last_attempt_watermark = 0;
        // Watermark of the last settled Committed/IdempotentNoOp snapshot;
        // failures and discards do not advance it.
        std::uint64_t settled_watermark = 0;
        bool has_settled = false;
        TaskId settled_task;
        std::uint64_t settled_task_epoch = 0;
        std::uint64_t settled_environment_epoch = 0;
        std::uint64_t events_since_attempt = 0;
        std::uint64_t consecutive_failures = 0;
    };

    [[nodiscard]] Result<void> validate_construction() const {
        if (auto valid = policy_.validate(); !valid) {
            return valid;
        }
        if (auto valid = options_.validate(); !valid) {
            return valid;
        }
        return Result<void>();
    }

    // Records a settled refresh outcome against its chain (M22 §4.2):
    // committed/no-op advance the settled watermark and reset the failure
    // streak, discards change only their counters, errors grow the streak.
    void record_outcome(ChainState &chain, const Result<WorkingContextCommitOutcome> &outcome) {
        if (!outcome.has_value()) {
            ++stats_.errors;
            ++chain.consecutive_failures;
            return;
        }
        const WorkingContextCommitOutcome &value = outcome.value();
        switch (value.disposition) {
        case WorkingContextCommitDisposition::Committed:
            ++stats_.committed;
            chain.consecutive_failures = 0;
            if (value.committed) {
                settle(chain, value.committed->through_event_sequence, value.committed->task_id,
                       value.committed->task_epoch, value.committed->environment_epoch);
            }
            break;
        case WorkingContextCommitDisposition::IdempotentNoOp:
            ++stats_.idempotent_noops;
            chain.consecutive_failures = 0;
            settle(chain, chain.in_flight_watermark, chain.in_flight_task,
                   chain.in_flight_task_epoch, chain.in_flight_environment_epoch);
            break;
        case WorkingContextCommitDisposition::DiscardedStale:
            ++stats_.discarded_stale;
            break;
        case WorkingContextCommitDisposition::DiscardedTerminal:
            ++stats_.discarded_terminal;
            break;
        }
    }

    // Drains the chain's in-flight refresh when it has settled; leaves it
    // untouched while it still runs.
    void drain_chain(ChainState &chain) {
        if (!chain.in_flight ||
            chain.in_flight->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        record_outcome(chain, chain.in_flight->get());
        chain.in_flight.reset();
    }

    void drain_session(const SessionId &session) {
        auto found = chains_.find(session);
        if (found != chains_.end()) {
            drain_chain(found->second);
        }
    }

    // Chain-scoped settled state: a signal whose identity opens a new chain
    // (epoch bump) must not inherit the old chain's settled watermark.
    void reset_settled_on_chain_change(ChainState &chain, const WorkingContextIdentity &identity) {
        if (chain.has_settled && !same_chain(identity, chain.settled_task, chain.settled_task_epoch,
                                             chain.settled_environment_epoch)) {
            chain.settled_watermark = 0;
            chain.has_settled = false;
        }
    }

    void settle(ChainState &chain, std::uint64_t watermark, const TaskId &task,
                std::uint64_t task_epoch, std::uint64_t environment_epoch) {
        if (chain.has_settled && watermark <= chain.settled_watermark) {
            return;
        }
        chain.settled_watermark = watermark;
        chain.settled_task = task;
        chain.settled_task_epoch = task_epoch;
        chain.settled_environment_epoch = environment_epoch;
        chain.has_settled = true;
    }

    // Selects the previous snapshot for the curator: the store's latest iff
    // its chain matches the input identity (epoch bump -> nullopt, M21 §4.1).
    // A store read error surfaces instead of silently curating without a
    // baseline the host thinks exists.
    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    select_previous(const SessionId &session, const WorkingContextIdentity &identity) {
        auto latest = store_.latest(session);
        if (!latest) {
            return Result<std::optional<WorkingContextSnapshot>>(latest.error());
        }
        if (latest.value() && same_chain(identity, latest.value()->task_id,
                                         latest.value()->task_epoch,
                                         latest.value()->environment_epoch)) {
            return Result<std::optional<WorkingContextSnapshot>>(latest.value());
        }
        return Result<std::optional<WorkingContextSnapshot>>(std::optional<WorkingContextSnapshot>());
    }

    // Schedules one curation through the supervisor's Deferrable route and
    // installs it as the chain's outstanding refresh. Thresholds re-arm at
    // fire time before the outcome is known (M22 §4.2).
    [[nodiscard]] SharedOutcome issue(const SessionId &session, ChainState &chain,
                                      WorkingContextRefreshInput &input, bool forced,
                                      WorkingContextTriggerKind kind) {
        auto previous = select_previous(session, input.identity);
        if (!previous) {
            return rejected(previous.error());
        }
        auto future = supervisor_.schedule_working_context_curate(
            curator_, store_, previous.value(), input.checkpoint, std::move(input.recent_events),
            input.live, options_);
        if (forced) {
            ++stats_.forced_flushes;
        } else if (kind == WorkingContextTriggerKind::Watermark) {
            ++stats_.fires_watermark;
        } else {
            ++stats_.fires_event_count;
        }
        chain.last_attempt_watermark = input.checkpoint.through_event_sequence;
        chain.events_since_attempt = 0;
        chain.in_flight_watermark = input.checkpoint.through_event_sequence;
        chain.in_flight_task = input.identity.task;
        chain.in_flight_task_epoch = input.identity.task_epoch;
        chain.in_flight_environment_epoch = input.identity.environment_epoch;
        // One share() only: share() moves the state, a second call on the
        // moved-from future would hand the caller an empty handle.
        auto shared = future.share();
        chain.in_flight = shared;
        return shared;
    }

    [[nodiscard]] SharedOutcome rejected(Error error) {
        ++stats_.errors;
        std::promise<Result<WorkingContextCommitOutcome>> promise;
        promise.set_value(Result<WorkingContextCommitOutcome>(std::move(error)));
        return promise.get_future().share();
    }

    [[nodiscard]] SharedOutcome rejected(ErrorCode code, std::string message) {
        return rejected(auto_error(code, std::move(message)));
    }

    // Creates the chain unless tracking is full (nullptr).
    [[nodiscard]] ChainState *tracked_chain(const SessionId &session) {
        auto found = chains_.find(session);
        if (found == chains_.end()) {
            if (chains_.size() >= policy_.max_tracked_sessions) {
                return nullptr;
            }
            found = chains_.emplace(session, ChainState{}).first;
        }
        return &found->second;
    }

    [[nodiscard]] std::optional<SharedOutcome> on_signal(const SessionId &session,
                                                         WorkingContextRefreshInput input) {
        ++stats_.signals;
        if (session != input.checkpoint.session_id) {
            return rejected(ErrorCode::InvalidArgument,
                            "signal session does not match the checkpoint's session");
        }
        ChainState *chain = tracked_chain(session);
        if (chain == nullptr) {
            return rejected(ErrorCode::ResourceExhausted,
                            "working context auto curator session tracking is full");
        }
        // Absorbed and evaluated signals alike accrue their execution-event
        // delta; only fires reset the accumulator.
        chain->events_since_attempt += input.reported_events;
        reset_settled_on_chain_change(*chain, input.identity);
        drain_chain(*chain);
        if (chain->in_flight) {
            // Coalescing (M22 §4.2): one policy refresh per session chain;
            // signals arriving meanwhile never queue a duplicate.
            ++stats_.absorbed;
            return std::nullopt;
        }
        const WorkingContextTriggerDecision decision = evaluate_working_context_trigger(
            policy_, chain->last_attempt_watermark, chain->events_since_attempt,
            input.checkpoint.through_event_sequence);
        // The unsettled-checkpoint gate: re-curating a checkpoint the store
        // already covers could only be a no-op or a same-watermark conflict,
        // so it never fires (and never re-arms — distance keeps accruing so
        // the next checkpoint catches up immediately).
        if (!decision.refresh ||
            input.checkpoint.through_event_sequence <= chain->settled_watermark) {
            return std::nullopt;
        }
        return issue(session, *chain, input, false, decision.kind);
    }

    [[nodiscard]] SharedOutcome flush(const SessionId &session, WorkingContextRefreshInput input) {
        if (session != input.checkpoint.session_id) {
            return rejected(ErrorCode::InvalidArgument,
                            "flush session does not match the checkpoint's session");
        }
        ChainState *chain = tracked_chain(session);
        if (chain == nullptr) {
            return rejected(ErrorCode::ResourceExhausted,
                            "working context auto curator session tracking is full");
        }
        reset_settled_on_chain_change(*chain, input.identity);
        // Barrier: wait once, bounded, for the in-flight policy refresh so
        // the forced refresh is the last outstanding work at this boundary.
        // A timeout leaves it in flight beside the forced refresh — the
        // monotonic commit discipline orders them either way.
        if (chain->in_flight) {
            (void)chain->in_flight->wait_for(options_.deadline);
            drain_chain(*chain);
        }
        if (input.checkpoint.through_event_sequence > chain->settled_watermark) {
            return issue(session, *chain, input, true, WorkingContextTriggerKind::None);
        }
        // The boundary is already covered: resolve immediately instead of
        // re-curating a settled watermark (that path could only no-op or
        // fail closed as a same-watermark conflict).
        auto latest = store_.latest(session);
        if (!latest) {
            return rejected(latest.error());
        }
        ++stats_.flush_noops;
        WorkingContextCommitOutcome outcome;
        outcome.disposition = WorkingContextCommitDisposition::IdempotentNoOp;
        outcome.reason_code = "auto-refresh-current";
        outcome.committed = latest.value();
        std::promise<Result<WorkingContextCommitOutcome>> promise;
        promise.set_value(Result<WorkingContextCommitOutcome>(std::move(outcome)));
        return promise.get_future().share();
    }

    [[nodiscard]] std::optional<WorkingContextSessionView>
    session_view(const SessionId &session) const {
        auto found = chains_.find(session);
        if (found == chains_.end()) {
            return std::nullopt;
        }
        const ChainState &chain = found->second;
        WorkingContextSessionView view;
        view.in_flight = chain.in_flight.has_value();
        view.last_attempt_watermark = chain.last_attempt_watermark;
        view.settled_watermark = chain.settled_watermark;
        view.events_since_attempt = chain.events_since_attempt;
        view.consecutive_failures = chain.consecutive_failures;
        return view;
    }

    [[nodiscard]] WorkingContextAutoStats stats() const {
        WorkingContextAutoStats stats = stats_;
        for (const auto &entry : chains_) {
            stats.consecutive_failures =
                std::max(stats.consecutive_failures, entry.second.consecutive_failures);
        }
        return stats;
    }

    // Bounded safety net: hosts should flush/drain explicitly. Waits at most
    // twice the curation deadline in total across sessions, then releases
    // whatever is still running (the supervised work settles on the Executor
    // regardless; nothing hangs on a broken promise).
    ~Impl() {
        const auto budget = 2 * options_.deadline;
        const auto started = std::chrono::steady_clock::now();
        for (auto &entry : chains_) {
            ChainState &chain = entry.second;
            if (!chain.in_flight) {
                continue;
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed >= budget) {
                return;
            }
            (void)chain.in_flight->wait_for(budget - elapsed);
            if (chain.in_flight->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                record_outcome(chain, chain.in_flight->get());
                chain.in_flight.reset();
            }
        }
    }

  private:
    ContextMemorySupervisor &supervisor_;
    IContextCurator &curator_;
    IWorkingContextStore &store_;
    WorkingContextTriggerPolicy policy_;
    ContextCurationOptions options_;
    std::map<SessionId, ChainState> chains_;
    WorkingContextAutoStats stats_;
};

WorkingContextAutoCurator::WorkingContextAutoCurator(ContextMemorySupervisor &supervisor,
                                                     IContextCurator &curator,
                                                     IWorkingContextStore &store,
                                                     WorkingContextTriggerPolicy policy,
                                                     ContextCurationOptions options)
    : impl_(std::make_unique<Impl>(supervisor, curator, store, policy, std::move(options))) {
    // Fail fast before any signal is accepted, mirroring the supervisor's
    // eager config validation.
    const auto valid = impl_->validate_construction();
    if (!valid) {
        impl_.reset();
        throw std::invalid_argument(valid.error().safe_message);
    }
}

WorkingContextAutoCurator::~WorkingContextAutoCurator() = default;

std::optional<std::shared_future<Result<WorkingContextCommitOutcome>>>
WorkingContextAutoCurator::on_signal(const SessionId &session, WorkingContextRefreshInput input) {
    return impl_->on_signal(session, std::move(input));
}

std::shared_future<Result<WorkingContextCommitOutcome>>
WorkingContextAutoCurator::flush(const SessionId &session, WorkingContextRefreshInput input) {
    return impl_->flush(session, std::move(input));
}

void WorkingContextAutoCurator::drain(const SessionId &session) {
    impl_->drain_session(session);
}

std::optional<WorkingContextSessionView> WorkingContextAutoCurator::session_view(
    const SessionId &session) const {
    return impl_->session_view(session);
}

WorkingContextAutoStats WorkingContextAutoCurator::stats() const {
    return impl_->stats();
}

} // namespace mira
