// M22 (DEC-035 Stage W3) Working Context auto-trigger contract/integration
// suite. Covers the milestone §7 matrix against the frozen §4 semantics: the
// trigger-policy pure function (watermark distance precedence, event-count
// axis, regression never fires on the watermark axis) and validate();
// construction validation; on_signal fire/no-fire/absorb/catch-up with the
// unsettled-checkpoint gate and epoch-chain settled reset; outcome accounting
// (Committed/IdempotentNoOp advance the settled watermark — the no-op at the
// fire-point watermark — discards only count, errors grow the failure streak);
// flush three branches (forced below threshold, auto-refresh-current no-op
// short-circuit, session-mismatch rejection) plus monotonic coexistence with
// an in-flight policy refresh and terminal lateness; the five curator failure
// classes with re-arm (no tight retry) and retry-on-next-crossing; previous
// selection (fresh and epoch-bumped chains pass nullptr, the same chain passes
// the store latest) observed through a capturing scripted curator; shared
// future ownership; rejections (session mismatch, tracking capacity);
// shutdown (in-flight cancellation, post-close rejection with zero curator
// calls); statistics and session-view consistency; destructor bounded drain.
//
// Determinism: gate synchronization through mutex/condition_variable and
// coordinator-state spin drains only — no sleep-based sequencing (the two
// timed failure/destruction budgets assert bounds, never ordering).
//
// Note: the double-share() defect called out in early review rounds (a caller
// future without associated state) was fixed within the milestone — issue()
// now shares exactly once. consume_caller_future() keeps its defect-recording
// shape as regression armor: a future_error surfaces as a recorded defect and
// fails the case instead of aborting the run.

#include "../support/m3_support.hpp"
#include "../support/test.hpp"

#include <mira/context_consolidation.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/model_provider.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mira;

using AutoFuture = std::shared_future<Result<WorkingContextCommitOutcome>>;

// Consumes a caller-side shared future. The frozen contract (M22 §4.2)
// requires every fire and forced flush to hand the caller a consumable
// future; one without associated state is an implementation defect, recorded
// in `defects` instead of terminating the run so the remaining semantics
// still verify. Tests keep a standing MIRA_CHECK on the defect count, so the
// suite stays red while the deviation exists.
[[nodiscard]] Result<WorkingContextCommitOutcome> consume_caller_future(const AutoFuture &future,
                                                                        std::size_t &defects) {
    try {
        return future.get();
    } catch (const std::future_error &) {
        ++defects;
        Error broken;
        broken.code = ErrorCode::Internal;
        broken.domain = "test.m22";
        broken.safe_message = "caller shared_future has no associated state";
        return Result<WorkingContextCommitOutcome>(broken);
    }
}

// Deterministic sequencing: drains and yields until the chain no longer owns
// an in-flight refresh. The supervised work always settles, so the loop is
// bounded by real progress, never by wall-clock assumptions.
[[nodiscard]] bool await_settlement(WorkingContextAutoCurator &curator, const SessionId &session) {
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

// ---------------------------------------------------------------------------
// Deterministic builders (M21 style)
// ---------------------------------------------------------------------------

[[nodiscard]] Id128 id_from_seed(std::uint64_t seed) {
    // All eight seed bytes participate, so distinct seeds never collide on
    // low-byte congruence.
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

[[nodiscard]] SessionId session_from_seed(std::uint64_t seed) {
    return SessionId{id_from_seed(seed)};
}

[[nodiscard]] TaskId task_from_seed(std::uint64_t seed) { return TaskId{id_from_seed(seed)}; }

[[nodiscard]] EventId event_from_seed(std::uint64_t seed) { return EventId{id_from_seed(seed)}; }

[[nodiscard]] ConversationStatement make_statement(std::string content, std::uint64_t event_seed,
                                                   std::uint64_t sequence) {
    ConversationStatement statement;
    statement.content = std::move(content);
    statement.source_events = {event_from_seed(event_seed)};
    statement.source_sequence = sequence;
    statement.confidence = 0.9;
    return statement;
}

// Deterministic checkpoint builder standing in for the M19 commit pipeline.
[[nodiscard]] ConversationCheckpoint make_checkpoint(const SessionId &session, const TaskId &task,
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
    checkpoint.created_at = Timestamp::now();
    checkpoint.constraints.push_back(
        make_statement("constraint r" + std::to_string(revision) + " confirm before sending",
                       watermark + 1, watermark - 1));
    checkpoint.decisions.push_back(
        make_statement("decision r" + std::to_string(revision) + " use batch provider",
                       watermark + 2, watermark - 1));
    checkpoint.unresolved_threads.push_back(
        make_statement("thread r" + std::to_string(revision) + " waiting for quota reply",
                       watermark + 3, watermark - 1));
    checkpoint.summary = "revision " + std::to_string(revision);
    checkpoint.source_events = {event_from_seed(watermark + 1), event_from_seed(watermark + 2),
                                event_from_seed(watermark + 3)};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] WorkingContextIdentity make_identity(const TaskId &task) {
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = 3;
    identity.environment_epoch = 7;
    return identity;
}

[[nodiscard]] WorkingContextCommitState make_live(const SessionId &session, const TaskId &task) {
    WorkingContextCommitState live;
    live.session = session;
    live.task = task;
    live.task_epoch = 3;
    live.environment_epoch = 7;
    return live;
}

[[nodiscard]] WorkingContextRefreshInput make_input(const ConversationCheckpoint &checkpoint,
                                                    const WorkingContextIdentity &identity,
                                                    const WorkingContextCommitState &live,
                                                    std::uint64_t reported_events = 0) {
    WorkingContextRefreshInput input;
    input.checkpoint = checkpoint;
    input.identity = identity;
    input.live = live;
    input.reported_events = reported_events;
    return input;
}

// Cross-run comparable payload: created_at and generated_by are wall-clock /
// per-process values and never participate in byte comparisons.
[[nodiscard]] std::string normalized_json(const WorkingContextSnapshot &snapshot) {
    WorkingContextSnapshot copy = snapshot;
    copy.created_at = Timestamp{};
    copy.generated_by = ModelProfileId{};
    return to_json_string(working_context_to_json(copy));
}

[[nodiscard]] std::string store_latest_json(IWorkingContextStore &store, const SessionId &session) {
    const auto latest = store.latest(session);
    if (!latest.has_value() || !latest.value().has_value()) {
        return "<none>";
    }
    return normalized_json(latest.value().value());
}

[[nodiscard]] std::uint64_t store_latest_watermark(IWorkingContextStore &store,
                                                   const SessionId &session) {
    const auto latest = store.latest(session);
    if (!latest.has_value() || !latest.value().has_value()) {
        return 0;
    }
    return latest.value()->through_event_sequence;
}

// ---------------------------------------------------------------------------
// Deterministic gate: the scripted curator blocks inside curate() until the
// test releases it. Entry is observable, so sequencing never depends on
// sleeps; both sides block on the condition variable.
// ---------------------------------------------------------------------------

struct CuratorGate final {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered_count = 0;
    bool released = false;

    void wait_at_gate() {
        std::unique_lock lock(mutex);
        ++entered_count;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    // Blocks until `count` curate calls have entered the gate (the ones the
    // scenario holds back); the delayed schedule of a second refresh is
    // awaited deterministically instead of being raced.
    void wait_entered(std::size_t count) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this, count] { return entered_count >= count; });
    }

    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};

// Releases the gate on every exit path so a failing MIRA_CHECK between arming
// and the deliberate release can never strand an Executor worker on a
// destroyed condition variable.
struct GateReleaser final {
    CuratorGate &gate;
    ~GateReleaser() { gate.release(); }
};

// ---------------------------------------------------------------------------
// Scripted IContextCurator: projects the checkpoint deterministically, can
// mutate the candidate (conflict / identity mismatch), fail outright, or block
// at a gate; captures every call's previous snapshot for selection asserts.
// ---------------------------------------------------------------------------

class ScriptedAutoCurator final : public IContextCurator {
  public:
    enum class Mode { Project, Conflict, IdentityMismatch, Error };

    struct CallRecord final {
        bool had_previous = false;
        WorkingContextSnapshot previous;
        ConversationCheckpoint checkpoint;
    };

    [[nodiscard]] Result<WorkingContextSnapshot>
    curate(const WorkingContextSnapshot *previous, const ConversationCheckpoint &checkpoint,
           std::span<const ConversationSegmentEntry> /*recent_events*/,
           const ContextCurationOptions &options) override {
        {
            std::lock_guard lock(record_mutex_);
            CallRecord record;
            record.had_previous = previous != nullptr;
            if (previous != nullptr) {
                record.previous = *previous;
            }
            record.checkpoint = checkpoint;
            records_.push_back(std::move(record));
        }
        calls_.fetch_add(1, std::memory_order_release);
        if (gate_ != nullptr) {
            gate_->wait_at_gate();
        }
        if (park_) {
            {
                std::lock_guard lock(park_mutex_);
                parked_ = true;
            }
            park_cv_.notify_all();
            std::unique_lock lock(park_mutex_);
            // The stop flag is owned by the supervisor, which cannot wake
            // this condition variable: probe the cancellation cooperatively
            // (the same pattern the scripted model provider uses).
            while (!(go_ || options.cancelled())) {
                park_cv_.wait_for(lock, std::chrono::milliseconds(1));
            }
            parked_ = false;
        }
        // The supervisor's stop flag travels inside the options: a released
        // gate after begin_shutdown deterministically yields a Cancelled
        // error, mirroring a provider observing cancellation.
        if (options.cancelled()) {
            Error cancelled;
            cancelled.code = ErrorCode::Cancelled;
            cancelled.domain = "test";
            cancelled.safe_message = "scripted curator observed cancellation";
            return cancelled;
        }
        if (mode_ == Mode::Error) {
            Error unavailable;
            unavailable.code = ErrorCode::Unavailable;
            unavailable.domain = "test";
            unavailable.safe_message = "scripted curator down";
            return unavailable;
        }
        WorkingContextIdentity identity;
        identity.task = checkpoint.task_id;
        identity.task_epoch = checkpoint.task_epoch;
        identity.environment_epoch = checkpoint.environment_epoch;
        auto candidate = working_context_from_checkpoint(checkpoint, identity);
        if (!candidate.has_value()) {
            return candidate;
        }
        switch (mode_) {
        case Mode::Project:
            break;
        case Mode::Conflict:
            candidate.value().constraints.front().content += " (conflicting revision)";
            break;
        case Mode::IdentityMismatch:
            candidate.value().task_epoch += 1;
            break;
        case Mode::Error:
            break;
        }
        return candidate;
    }

    void set_mode(Mode mode) { mode_ = mode; }
    void arm_gate(CuratorGate &gate) { gate_ = &gate; }

    // After the gate, the call parks until the supervisor's stop flag fires
    // (cooperative cancellation, the only path begin_shutdown can rely on) or
    // the test lets it go. Entry and parking are both observable.
    void park_after_gate() {
        std::lock_guard lock(park_mutex_);
        park_ = true;
    }

    void wait_parked() const {
        std::unique_lock lock(park_mutex_);
        park_cv_.wait(lock, [this] { return parked_; });
    }

    [[nodiscard]] std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }

    [[nodiscard]] std::optional<CallRecord> record(std::size_t index) const {
        std::lock_guard lock(record_mutex_);
        if (index >= records_.size()) {
            return std::nullopt;
        }
        return records_[index];
    }

  private:
    Mode mode_ = Mode::Project;
    CuratorGate *gate_ = nullptr;
    mutable std::mutex park_mutex_;
    mutable std::condition_variable park_cv_;
    bool park_ = false;
    bool parked_ = false;
    bool go_ = false;
    std::atomic<std::uint64_t> calls_{0};
    mutable std::mutex record_mutex_;
    std::vector<CallRecord> records_;
};

// ---------------------------------------------------------------------------
// Stub provider driving ProviderContextCurator through the five frozen failure
// classes (provider error / malformed JSON / refusal / deadline / cancel).
// ---------------------------------------------------------------------------

class FailureStubProvider final : public IModelProvider {
  public:
    enum class Behavior {
        Ok,
        ProviderError,
        MalformedJson,
        Refusal,
        SleepPastDeadline,
        ReturnCancelled,
    };

    FailureStubProvider()
        : profile_(std::make_shared<ModelProfile>(mira::testing::make_profile(
              ProtocolDialect::OpenAIResponsesV1, "https://m22-auto.test"))) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext & /*context*/,
                                              const ProviderInferOptions &) override {
        calls_.fetch_add(1, std::memory_order_release);
        if (behavior_ == Behavior::SleepPastDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (behavior_ == Behavior::ProviderError) {
            Error unavailable;
            unavailable.code = ErrorCode::Unavailable;
            unavailable.domain = "test";
            unavailable.safe_message = "provider down";
            return unavailable;
        }
        if (behavior_ == Behavior::ReturnCancelled) {
            Error cancelled;
            cancelled.code = ErrorCode::Cancelled;
            cancelled.domain = "test";
            cancelled.safe_message = "provider observed cancellation";
            return cancelled;
        }
        ModelResponse response;
        response.contract_version = SchemaVersion{1, 0};
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_->id;
        response.requested_model = profile_->model_selector;
        response.status = ModelCompletionStatus::Completed;
        MessageOutput message;
        if (behavior_ == Behavior::Refusal) {
            OutputRefusalPart refusal;
            refusal.safe_summary = "cannot help with that";
            message.content.emplace_back(std::move(refusal));
        } else {
            OutputTextPart text;
            text.text =
                behavior_ == Behavior::MalformedJson ? "this is not json" : valid_output_json_;
            message.content.emplace_back(std::move(text));
        }
        response.output.emplace_back(std::move(message));
        return response;
    }

    void set_behavior(Behavior behavior) { behavior_ = behavior; }
    void set_output(std::string json) { valid_output_json_ = std::move(json); }
    [[nodiscard]] std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }

  private:
    std::shared_ptr<ModelProfile> profile_;
    Behavior behavior_ = Behavior::Ok;
    std::string valid_output_json_ = "{}";
    std::atomic<std::uint64_t> calls_{0};
};

[[nodiscard]] std::string item_json(const std::string &content, const std::vector<int> &sources) {
    std::string sources_json;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (index > 0) {
            sources_json += ",";
        }
        sources_json += std::to_string(sources[index]);
    }
    return "{\"content\":\"" + content + "\",\"sources\":[" + sources_json +
           "],\"confidence\":0.9}";
}

// Valid curation output that works on a fresh chain (citation 0 is the first
// checkpoint entry) and on an incremental one (citation 0 is the first
// previous entry, satisfying the degenerate-merge guard).
[[nodiscard]] std::string valid_output_json() {
    return "{\"confidence\":0.9,\"constraints\":[" + item_json("carried constraint", {0}) +
           "],\"decisions\":[],\"open_issues\":[],\"active_tasks\":[],\"verified_facts\":[],"
           "\"failed_attempts\":[],\"important_refs\":[],\"next_actions\":[]}";
}

// ---------------------------------------------------------------------------
// 1. Trigger policy: validate() boundaries and the frozen pure-function table
// ---------------------------------------------------------------------------

int trigger_policy_pure_function() {
    WorkingContextTriggerPolicy policy;
    MIRA_CHECK(policy.validate().has_value());
    MIRA_CHECK(policy.watermark_interval == 8);
    MIRA_CHECK(policy.event_count_interval == 16);
    MIRA_CHECK(policy.max_tracked_sessions == 64);

    policy.watermark_interval = 0;
    MIRA_CHECK(!policy.validate().has_value());
    policy.watermark_interval = 8;
    policy.event_count_interval = 0;
    MIRA_CHECK(!policy.validate().has_value());
    policy.event_count_interval = 16;
    policy.max_tracked_sessions = 0;
    MIRA_CHECK(!policy.validate().has_value());
    policy.max_tracked_sessions = 64;
    MIRA_CHECK(policy.validate().has_value());

    // Frozen boundary table: watermark distance takes precedence, both axes
    // must be exactly at-threshold to fire, a regressed observation never
    // fires on the watermark axis.
    struct Case final {
        std::uint64_t last_attempt;
        std::uint64_t events;
        std::uint64_t current;
        bool refresh;
        WorkingContextTriggerKind kind;
    };
    const Case cases[] = {
        // Distance axis: below, exactly at, above the interval.
        {0, 0, 7, false, WorkingContextTriggerKind::None},
        {0, 0, 8, true, WorkingContextTriggerKind::Watermark},
        {0, 0, 9, true, WorkingContextTriggerKind::Watermark},
        {8, 0, 15, false, WorkingContextTriggerKind::None},
        {8, 0, 16, true, WorkingContextTriggerKind::Watermark},
        {8, 0, 24, true, WorkingContextTriggerKind::Watermark},
        // Event-count axis: below, exactly at, above the interval.
        {0, 15, 0, false, WorkingContextTriggerKind::None},
        {0, 16, 0, true, WorkingContextTriggerKind::EventCount},
        {0, 17, 0, true, WorkingContextTriggerKind::EventCount},
        // Precedence: both thresholds met is a watermark fire.
        {0, 32, 8, true, WorkingContextTriggerKind::Watermark},
        {8, 16, 16, true, WorkingContextTriggerKind::Watermark},
        // Distance regression never fires on the watermark axis; the
        // event-count axis stays independent of the observation direction.
        {24, 0, 16, false, WorkingContextTriggerKind::None},
        {24, 16, 0, true, WorkingContextTriggerKind::EventCount},
        {24, 3, 8, false, WorkingContextTriggerKind::None},
        // Non-zero anchors.
        {40, 15, 47, false, WorkingContextTriggerKind::None},
        {40, 15, 48, true, WorkingContextTriggerKind::Watermark},
        {40, 16, 44, true, WorkingContextTriggerKind::EventCount},
    };
    for (const Case &item : cases) {
        const WorkingContextTriggerPolicy default_policy;
        const auto decision = evaluate_working_context_trigger(default_policy, item.last_attempt,
                                                               item.events, item.current);
        MIRA_CHECK(decision.refresh == item.refresh);
        MIRA_CHECK(decision.kind == item.kind);
    }

    // A non-default policy scales the thresholds, not the semantics.
    WorkingContextTriggerPolicy scaled;
    scaled.watermark_interval = 3;
    scaled.event_count_interval = 5;
    MIRA_CHECK(!evaluate_working_context_trigger(scaled, 10, 4, 12).refresh);
    const auto scaled_watermark = evaluate_working_context_trigger(scaled, 10, 4, 13);
    MIRA_CHECK(scaled_watermark.refresh);
    MIRA_CHECK(scaled_watermark.kind == WorkingContextTriggerKind::Watermark);
    const auto scaled_events = evaluate_working_context_trigger(scaled, 10, 5, 12);
    MIRA_CHECK(scaled_events.refresh);
    MIRA_CHECK(scaled_events.kind == WorkingContextTriggerKind::EventCount);

    MIRA_CHECK(working_context_trigger_kind_name(WorkingContextTriggerKind::None) == "none");
    MIRA_CHECK(working_context_trigger_kind_name(WorkingContextTriggerKind::Watermark) ==
               "watermark");
    MIRA_CHECK(working_context_trigger_kind_name(WorkingContextTriggerKind::EventCount) ==
               "event_count");
    return 0;
}

// ---------------------------------------------------------------------------
// 2. Construction validation: invalid policy or curation options throw
// ---------------------------------------------------------------------------

int construction_validation() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);

        WorkingContextTriggerPolicy bad_policy;
        bad_policy.watermark_interval = 0;
        bool threw = false;
        try {
            WorkingContextAutoCurator invalid(supervisor, curator, store, bad_policy,
                                              ContextCurationOptions{});
            (void)invalid;
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        MIRA_CHECK(threw);

        ContextCurationOptions bad_options;
        bad_options.deadline = std::chrono::milliseconds::zero();
        threw = false;
        try {
            WorkingContextAutoCurator invalid(supervisor, curator, store,
                                              WorkingContextTriggerPolicy{}, bad_options);
            (void)invalid;
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        MIRA_CHECK(threw);

        WorkingContextAutoCurator valid(supervisor, curator, store);
        MIRA_CHECK(!valid.session_view(session_from_seed(1)).has_value());
        const WorkingContextAutoStats stats = valid.stats();
        MIRA_CHECK(stats.signals == 0 && stats.absorbed == 0 && stats.fires_watermark == 0 &&
                   stats.fires_event_count == 0 && stats.forced_flushes == 0 &&
                   stats.flush_noops == 0 && stats.committed == 0 && stats.idempotent_noops == 0 &&
                   stats.discarded_stale == 0 && stats.discarded_terminal == 0 &&
                   stats.errors == 0 && stats.consecutive_failures == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 3. on_signal: no-fire below thresholds (delta accrual, zero schedules)
// ---------------------------------------------------------------------------

int on_signal_no_fire_accrues_events() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(100);
        const TaskId task = task_from_seed(101);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);

        // Watermark 2 with no history: distance 2 < 8, events 0 < 16.
        const auto checkpoint = make_checkpoint(session, task, 2, 1);
        auto result = auto_curator.on_signal(session, make_input(checkpoint, identity, live, 5));
        MIRA_CHECK(!result.has_value());
        MIRA_CHECK(curator.calls() == 0);
        const auto view = auto_curator.session_view(session);
        MIRA_CHECK(view.has_value());
        MIRA_CHECK(!view->in_flight);
        MIRA_CHECK(view->last_attempt_watermark == 0);
        MIRA_CHECK(view->settled_watermark == 0);
        MIRA_CHECK(view->events_since_attempt == 5);
        MIRA_CHECK(view->consecutive_failures == 0);
        MIRA_CHECK(auto_curator.stats().signals == 1);

        // Second no-fire accrues on top; still no schedule and no re-arm.
        const auto checkpoint2 = make_checkpoint(session, task, 4, 2);
        result = auto_curator.on_signal(session, make_input(checkpoint2, identity, live, 6));
        MIRA_CHECK(!result.has_value());
        MIRA_CHECK(curator.calls() == 0);
        MIRA_CHECK(auto_curator.session_view(session)->events_since_attempt == 11);
        MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 0);
        MIRA_CHECK(store.latest(session).has_value() && !store.latest(session).value().has_value());
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 4. on_signal: watermark fire, event-count fire, watermark precedence
// ---------------------------------------------------------------------------

int on_signal_fires_both_kinds() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(110);
        const TaskId task = task_from_seed(111);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        // Watermark fire: distance 8 from the zero anchor; the coordinator
        // re-arms at the checkpoint watermark and hands the caller a future.
        const auto first = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(first, identity, live, 3));
        MIRA_CHECK(result.has_value());
        const auto outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        const WorkingContextAutoStats after_first = auto_curator.stats();
        MIRA_CHECK(after_first.fires_watermark == 1);
        MIRA_CHECK(after_first.fires_event_count == 0);
        MIRA_CHECK(after_first.committed == 1);
        const auto first_view = auto_curator.session_view(session);
        MIRA_CHECK(first_view->last_attempt_watermark == 8);
        MIRA_CHECK(first_view->settled_watermark == 8);
        MIRA_CHECK(first_view->events_since_attempt == 0);
        MIRA_CHECK(!first_view->in_flight);
        MIRA_CHECK(curator.calls() == 1);
        const auto first_record = curator.record(0);
        MIRA_CHECK(first_record.has_value() && !first_record->had_previous);
        const auto latest_after_first = store.latest(session);
        MIRA_CHECK(latest_after_first.has_value() && latest_after_first.value().has_value());
        // The caller future must have been consumable and carried the commit.
        if (outcome.has_value()) {
            MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
            MIRA_CHECK(outcome.value().committed.has_value());
        }

        // Event-count fire with (almost) zero conversation advance: the same
        // checkpoint watermark cannot refire (unsettled gate), so advance one
        // step and drive the event axis past its interval.
        const auto second = make_checkpoint(session, task, 9, 2);
        result = auto_curator.on_signal(session, make_input(second, identity, live, 16));
        MIRA_CHECK(result.has_value());
        const auto second_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        const WorkingContextAutoStats after_second = auto_curator.stats();
        MIRA_CHECK(after_second.fires_watermark == 1);
        MIRA_CHECK(after_second.fires_event_count == 1);
        MIRA_CHECK(after_second.committed == 2);
        MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 9);
        MIRA_CHECK(curator.calls() == 2);
        const auto second_record = curator.record(1);
        MIRA_CHECK(second_record.has_value() && second_record->had_previous);
        MIRA_CHECK(second_record->previous.id == latest_after_first.value()->id);
        if (second_outcome.has_value()) {
            MIRA_CHECK(second_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
        }

        // Precedence: distance 8 and events 16 both met is a watermark fire.
        const auto third = make_checkpoint(session, task, 17, 3);
        result = auto_curator.on_signal(session, make_input(third, identity, live, 16));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        const WorkingContextAutoStats after_third = auto_curator.stats();
        MIRA_CHECK(after_third.fires_watermark == 2);
        MIRA_CHECK(after_third.fires_event_count == 1);
        MIRA_CHECK(curator.calls() == 3);
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 5. Coalescing: absorbed signals accrue, exactly one schedule, the release
//    refires once with the latest input
// ---------------------------------------------------------------------------

int on_signal_absorbs_and_refires_latest() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(120);
        const TaskId task = task_from_seed(121);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        CuratorGate gate;
        curator.arm_gate(gate);
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        const auto first = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(first, identity, live, 0));
        MIRA_CHECK(result.has_value());
        GateReleaser releaser{gate};
        gate.wait_entered(1);
        MIRA_CHECK(auto_curator.session_view(session)->in_flight);

        // Four signals while the refresh is in flight: all absorbed, none
        // queues a duplicate, events accrue across the absorbed inputs.
        for (std::uint64_t index = 0; index < 4; ++index) {
            const auto checkpoint = make_checkpoint(session, task, 8 + index, 2 + index);
            auto absorbed =
                auto_curator.on_signal(session, make_input(checkpoint, identity, live, 4));
            MIRA_CHECK(!absorbed.has_value());
        }
        const WorkingContextAutoStats stats_while_blocked = auto_curator.stats();
        MIRA_CHECK(stats_while_blocked.absorbed == 4);
        MIRA_CHECK(stats_while_blocked.signals == 5);
        MIRA_CHECK(curator.calls() == 1);
        const auto blocked_view = auto_curator.session_view(session);
        MIRA_CHECK(blocked_view->in_flight);
        MIRA_CHECK(blocked_view->events_since_attempt == 16);
        MIRA_CHECK(blocked_view->last_attempt_watermark == 8);

        gate.release();
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
        MIRA_CHECK(auto_curator.stats().committed == 1);

        // The next threshold-crossing signal refires exactly once and carries
        // the latest checkpoint (16), not any absorbed intermediate input.
        const auto latest = make_checkpoint(session, task, 16, 9);
        result = auto_curator.on_signal(session, make_input(latest, identity, live, 0));
        MIRA_CHECK(result.has_value());
        const auto refire_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(curator.calls() == 2);
        MIRA_CHECK(auto_curator.stats().absorbed == 4);
        MIRA_CHECK(auto_curator.session_view(session)->events_since_attempt == 0);
        MIRA_CHECK(store_latest_watermark(store, session) == 16);
        const auto refire_record = curator.record(1);
        MIRA_CHECK(refire_record.has_value());
        MIRA_CHECK(refire_record->checkpoint.through_event_sequence == 16);
        if (refire_outcome.has_value()) {
            MIRA_CHECK(refire_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
            MIRA_CHECK(refire_outcome.value().committed.has_value());
            MIRA_CHECK(refire_outcome.value().committed->through_event_sequence == 16);
        }
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 6. Unsettled gate and catch-up: a true decision at a covered checkpoint
//    neither fires nor re-arms; the next fresh checkpoint catches up
// ---------------------------------------------------------------------------

int settled_gate_and_catch_up() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(130);
        const TaskId task = task_from_seed(131);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        const auto first = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(first, identity, live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);

        // Checkpoint gap: the conversation stalls at the covered checkpoint
        // while execution events pile up. Every event-count decision is true,
        // but the covered checkpoint blocks the fire and the re-arm: the
        // distance keeps accruing for the catch-up.
        for (std::uint64_t index = 0; index < 3; ++index) {
            result = auto_curator.on_signal(session, make_input(first, identity, live, 10));
            MIRA_CHECK(!result.has_value());
        }
        MIRA_CHECK(curator.calls() == 1);
        const auto gap_view = auto_curator.session_view(session);
        MIRA_CHECK(gap_view->last_attempt_watermark == 8);
        MIRA_CHECK(gap_view->events_since_attempt == 30);
        MIRA_CHECK(gap_view->settled_watermark == 8);
        MIRA_CHECK(!gap_view->in_flight);
        const WorkingContextAutoStats gap_stats = auto_curator.stats();
        MIRA_CHECK(gap_stats.fires_event_count == 0 && gap_stats.fires_watermark == 1);

        // Catch-up via the event axis: a fresh (unsettled) checkpoint with a
        // sub-threshold watermark distance still fires immediately.
        const auto catch_up = make_checkpoint(session, task, 10, 2);
        result = auto_curator.on_signal(session, make_input(catch_up, identity, live, 2));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        WorkingContextAutoStats stats = auto_curator.stats();
        MIRA_CHECK(stats.fires_event_count == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 10);
        MIRA_CHECK(auto_curator.session_view(session)->events_since_attempt == 0);

        // Catch-up via the distance axis: a covered stall (no events) followed
        // by a checkpoint that jumps the full interval fires on arrival.
        const auto stall = make_checkpoint(session, task, 10, 3);
        result = auto_curator.on_signal(session, make_input(stall, identity, live, 0));
        MIRA_CHECK(!result.has_value());
        const auto jump = make_checkpoint(session, task, 18, 4);
        result = auto_curator.on_signal(session, make_input(jump, identity, live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        stats = auto_curator.stats();
        MIRA_CHECK(stats.fires_watermark == 2);
        MIRA_CHECK(stats.fires_event_count == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 18);
        MIRA_CHECK(curator.calls() == 3);
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 7. Outcome accounting: Committed/NoOp settle, discards only count, errors
//    grow the streak; the no-op settles at the fire-point watermark
// ---------------------------------------------------------------------------

int outcome_accounting_committed_noop_discard_error() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(140);
        const TaskId task = task_from_seed(141);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        // Baseline: a committed fire settles the chain at watermark 8.
        const auto baseline = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(baseline, identity, live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);

        // Conflicting candidate at a covered watermark: the host commits
        // watermark 20 out of band, so a fire carrying checkpoint 20 produces
        // a candidate at the store's own watermark with a different digest —
        // the commit pipeline fails closed. The discard counts without
        // touching the settled watermark or the failure streak.
        const auto replayed = make_checkpoint(session, task, 20, 2);
        const auto projection = working_context_from_checkpoint(replayed, make_identity(task));
        MIRA_CHECK(projection.has_value());
        const auto direct = commit_working_context(store, projection.value(), live);
        MIRA_CHECK(direct.disposition == WorkingContextCommitDisposition::Committed);
        const std::string covered = store_latest_json(store, session);
        curator.set_mode(ScriptedAutoCurator::Mode::Conflict);
        result = auto_curator.on_signal(session, make_input(replayed, identity, live, 0));
        MIRA_CHECK(result.has_value());
        const auto conflict_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        WorkingContextAutoStats after_conflict = auto_curator.stats();
        MIRA_CHECK(after_conflict.discarded_stale == 1);
        MIRA_CHECK(after_conflict.errors == 0);
        MIRA_CHECK(store_latest_json(store, session) == covered);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
        if (conflict_outcome.has_value()) {
            MIRA_CHECK(conflict_outcome.value().disposition ==
                       WorkingContextCommitDisposition::DiscardedStale);
            MIRA_CHECK(conflict_outcome.value().reason_code == "conflicting-watermark");
        }
        // The conflicting fire re-armed at watermark 20; a same-watermark
        // replay with only the event axis re-crossed now curates the covered
        // checkpoint identically and settles as an idempotent no-op AT THE
        // FIRE-POINT watermark.
        curator.set_mode(ScriptedAutoCurator::Mode::Project);
        result = auto_curator.on_signal(session, make_input(replayed, identity, live, 16));
        MIRA_CHECK(result.has_value());
        const auto noop_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        const WorkingContextAutoStats after_noop = auto_curator.stats();
        MIRA_CHECK(after_noop.idempotent_noops == 1);
        MIRA_CHECK(after_noop.committed == 1);
        MIRA_CHECK(after_noop.discarded_stale == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 20);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
        if (noop_outcome.has_value()) {
            MIRA_CHECK(noop_outcome.value().disposition ==
                       WorkingContextCommitDisposition::IdempotentNoOp);
            MIRA_CHECK(noop_outcome.value().reason_code == "idempotent-replay");
        }

        // Identity-mismatched candidate: discard with the tuple reason.
        curator.set_mode(ScriptedAutoCurator::Mode::IdentityMismatch);
        const auto mismatch = make_checkpoint(session, task, 28, 3);
        result = auto_curator.on_signal(session, make_input(mismatch, identity, live, 0));
        MIRA_CHECK(result.has_value());
        const auto mismatch_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        after_conflict = auto_curator.stats();
        MIRA_CHECK(after_conflict.discarded_stale == 2);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 20);
        if (mismatch_outcome.has_value()) {
            MIRA_CHECK(mismatch_outcome.value().disposition ==
                       WorkingContextCommitDisposition::DiscardedStale);
            MIRA_CHECK(mismatch_outcome.value().reason_code == "task-epoch-mismatch");
        }

        // Terminal lateness: a fire whose live state went terminal discards.
        curator.set_mode(ScriptedAutoCurator::Mode::Project);
        WorkingContextCommitState terminal_live = live;
        terminal_live.session_terminal = true;
        const auto terminal = make_checkpoint(session, task, 36, 4);
        result = auto_curator.on_signal(session, make_input(terminal, identity, terminal_live, 0));
        MIRA_CHECK(result.has_value());
        const auto terminal_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        WorkingContextAutoStats after_terminal = auto_curator.stats();
        MIRA_CHECK(after_terminal.discarded_terminal == 1);
        MIRA_CHECK(after_terminal.discarded_stale == 2);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 20);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
        if (terminal_outcome.has_value()) {
            MIRA_CHECK(terminal_outcome.value().disposition ==
                       WorkingContextCommitDisposition::DiscardedTerminal);
            MIRA_CHECK(terminal_outcome.value().reason_code == "session-terminal");
        }
        // A discarding fire still re-arms (the next signal needs a fresh
        // threshold crossing before it retries the chain).
        MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 36);

        // Error: the streak grows and the settled watermark stays.
        curator.set_mode(ScriptedAutoCurator::Mode::Error);
        const auto failing = make_checkpoint(session, task, 44, 5);
        result = auto_curator.on_signal(session, make_input(failing, identity, live, 0));
        MIRA_CHECK(result.has_value());
        const auto error_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        WorkingContextAutoStats after_error = auto_curator.stats();
        MIRA_CHECK(after_error.errors == 1);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 20);
        if (error_outcome.has_value()) {
            MIRA_CHECK(!error_outcome.has_value());
        }
        MIRA_CHECK(store_latest_json(store, session) != std::string("<none>"));
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 8. flush: forced fire below threshold, no-op short-circuit, mismatch
//    rejection, and monotonic coexistence with an in-flight refresh
// ---------------------------------------------------------------------------

int flush_branches() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    const SessionId session = session_from_seed(150);
    const TaskId task = task_from_seed(151);
    const auto live = make_live(session, task);
    const auto identity = make_identity(task);
    std::size_t caller_defects = 0;

    // Forced fire below every threshold: the boundary barrier ignores both
    // axes and the returned future must be consumed.
    {
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto boundary = make_checkpoint(session, task, 4, 1);
        auto future = auto_curator.flush(session, make_input(boundary, identity, live, 0));
        const auto outcome = consume_caller_future(future, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(curator.calls() == 1);
        MIRA_CHECK(auto_curator.stats().forced_flushes == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 4);
        MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 4);
        if (outcome.has_value()) {
            MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
            MIRA_CHECK(outcome.value().committed.has_value());
            MIRA_CHECK(outcome.value().committed->through_event_sequence == 4);
        }

        // Already-covered boundary: immediate IdempotentNoOp with the frozen
        // reason, committed == store latest, zero curator calls. This branch
        // mints its own future, so it must stay consumable regardless of the
        // fire-path defect.
        auto noop_future = auto_curator.flush(session, make_input(boundary, identity, live, 0));
        MIRA_CHECK(noop_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        const auto noop = consume_caller_future(noop_future, caller_defects);
        MIRA_CHECK(noop.has_value());
        MIRA_CHECK(noop.value().disposition == WorkingContextCommitDisposition::IdempotentNoOp);
        MIRA_CHECK(noop.value().reason_code == "auto-refresh-current");
        MIRA_CHECK(noop.value().committed.has_value());
        const auto latest = store.latest(session);
        MIRA_CHECK(latest.has_value() && latest.value().has_value());
        MIRA_CHECK(noop.value().committed->id == latest.value()->id);
        MIRA_CHECK(curator.calls() == 1);
        MIRA_CHECK(auto_curator.stats().flush_noops == 1);

        // Session mismatch resolves an InvalidArgument error future (also a
        // coordinator-minted future).
        const auto other_session_checkpoint = make_checkpoint(session_from_seed(999), task, 12, 2);
        auto rejected =
            auto_curator.flush(session, make_input(other_session_checkpoint, identity, live, 0));
        const auto rejection = consume_caller_future(rejected, caller_defects);
        MIRA_CHECK(!rejection.has_value());
        MIRA_CHECK(rejection.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(auto_curator.stats().errors == 1);
        supervisor.begin_shutdown();
    }

    // Coexistence: the barrier bounded-drains the in-flight refresh, the
    // forced refresh fires beside it after the deadline, and the monotonic
    // commit discipline lands the store exactly on the flush watermark with
    // no conflict and no regression. Once the higher forced watermark is
    // stored, no outstanding candidate can move it.
    {
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        CuratorGate gate;
        curator.arm_gate(gate);
        ContextMemorySupervisor supervisor(exec);
        ContextCurationOptions options;
        options.deadline = std::chrono::milliseconds(50);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store,
                                               WorkingContextTriggerPolicy{}, options);

        const auto policy_checkpoint = make_checkpoint(session, task, 8, 1);
        auto policy_result =
            auto_curator.on_signal(session, make_input(policy_checkpoint, identity, live, 0));
        MIRA_CHECK(policy_result.has_value());
        GateReleaser releaser{gate};
        gate.wait_entered(1);

        const auto boundary = make_checkpoint(session, task, 16, 2);
        auto flush_future = auto_curator.flush(session, make_input(boundary, identity, live, 0));
        // The forced refresh is scheduled by flush() and enters the curator
        // asynchronously; the gate counts its entry deterministically.
        gate.wait_entered(2);
        MIRA_CHECK(curator.calls() == 2);
        MIRA_CHECK(auto_curator.stats().forced_flushes == 1);

        gate.release();
        (void)consume_caller_future(policy_result.value(), caller_defects);
        const auto flush_outcome = consume_caller_future(flush_future, caller_defects);
        for (int guard = 0; guard < 100'000'000; ++guard) {
            if (store_latest_watermark(store, session) == 16) {
                break;
            }
            std::this_thread::yield();
        }
        MIRA_CHECK(store_latest_watermark(store, session) == 16);
        if (flush_outcome.has_value()) {
            MIRA_CHECK(flush_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
        }
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    MIRA_CHECK(caller_defects == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 9. Terminal lateness through the boundary: host awaits the flush future,
//    flips terminal, and the late policy refresh is discarded while the
//    forced snapshot stays
// ---------------------------------------------------------------------------

int terminal_lateness_keeps_forced_snapshot() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(160);
        const TaskId task = task_from_seed(161);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        const auto boundary = make_checkpoint(session, task, 8, 1);
        auto future = auto_curator.flush(session, make_input(boundary, identity, live, 0));
        const auto forced = consume_caller_future(future, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
        const std::string before = store_latest_json(store, session);

        // The host flips the session terminal; a threshold-crossing policy
        // refresh still fires (the coordinator is not terminal-aware by
        // design) and the commit pipeline discards it as a latecomer.
        WorkingContextCommitState terminal_live = live;
        terminal_live.session_terminal = true;
        const auto late = make_checkpoint(session, task, 16, 2);
        auto result = auto_curator.on_signal(session, make_input(late, identity, terminal_live, 0));
        MIRA_CHECK(result.has_value());
        const auto late_outcome = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(store_latest_json(store, session) == before);
        MIRA_CHECK(auto_curator.stats().discarded_terminal == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
        if (forced.has_value()) {
            MIRA_CHECK(forced.value().disposition == WorkingContextCommitDisposition::Committed);
        }
        if (late_outcome.has_value()) {
            MIRA_CHECK(late_outcome.value().disposition ==
                       WorkingContextCommitDisposition::DiscardedTerminal);
            MIRA_CHECK(late_outcome.value().reason_code == "session-terminal");
        }
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 10. Failure fallback: five frozen classes — error future, store untouched,
//     no tight retry, retry on the next crossing, streak reset, and a real
//     (non-no-op) flush retry at the failed boundary
// ---------------------------------------------------------------------------

int failure_fallback_five_classes() {
    struct ClassCase final {
        const char *name;
        FailureStubProvider::Behavior behavior;
        ErrorCode expected;
        std::chrono::milliseconds deadline;
    };
    const ClassCase classes[] = {
        {"provider-error", FailureStubProvider::Behavior::ProviderError, ErrorCode::Unavailable,
         std::chrono::milliseconds(10'000)},
        {"malformed-json", FailureStubProvider::Behavior::MalformedJson,
         ErrorCode::InvalidModelOutput, std::chrono::milliseconds(10'000)},
        {"refusal", FailureStubProvider::Behavior::Refusal, ErrorCode::InvalidModelOutput,
         std::chrono::milliseconds(10'000)},
        {"deadline", FailureStubProvider::Behavior::SleepPastDeadline, ErrorCode::DeadlineExceeded,
         std::chrono::milliseconds(20)},
        {"cancel", FailureStubProvider::Behavior::ReturnCancelled, ErrorCode::Cancelled,
         std::chrono::milliseconds(10'000)},
    };

    for (const ClassCase &failure : classes) {
        executor::Executor exec;
        MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
        {
            const SessionId session = session_from_seed(170);
            const TaskId task = task_from_seed(171);
            InMemoryWorkingContextStore store;
            FailureStubProvider provider;
            ProviderContextCurator curator(provider);
            ContextMemorySupervisor supervisor(exec);
            ContextCurationOptions options;
            options.deadline = failure.deadline;
            WorkingContextAutoCurator auto_curator(supervisor, curator, store,
                                                   WorkingContextTriggerPolicy{}, options);
            const auto live = make_live(session, task);
            const auto identity = make_identity(task);
            provider.set_output(valid_output_json());
            std::size_t caller_defects = 0;

            // Baseline commit at watermark 8.
            const auto baseline = make_checkpoint(session, task, 8, 1);
            auto result = auto_curator.on_signal(session, make_input(baseline, identity, live, 0));
            MIRA_CHECK(result.has_value());
            const auto baseline_outcome = consume_caller_future(*result, caller_defects);
            MIRA_CHECK(await_settlement(auto_curator, session));
            MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
            if (baseline_outcome.has_value()) {
                MIRA_CHECK(baseline_outcome.value().disposition ==
                           WorkingContextCommitDisposition::Committed);
            }
            const std::string before = store_latest_json(store, session);

            // Failing fire at watermark 16: the future resolves with the
            // curator's error and the store stays byte-identical.
            provider.set_behavior(failure.behavior);
            const auto failing = make_checkpoint(session, task, 16, 2);
            result = auto_curator.on_signal(session, make_input(failing, identity, live, 0));
            MIRA_CHECK(result.has_value());
            const auto error_outcome = consume_caller_future(*result, caller_defects);
            MIRA_CHECK(await_settlement(auto_curator, session));
            auto_curator.drain(session);
            MIRA_CHECK(store_latest_json(store, session) == before);
            MIRA_CHECK(auto_curator.stats().errors == 1);
            MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 1);
            MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
            // Failures re-arm: no tight retry on the next signal.
            MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 16);
            if (error_outcome.has_value()) {
                MIRA_CHECK(error_outcome.error().code == failure.expected);
            }

            // Below-threshold signals stay silent while the streak stands.
            provider.set_behavior(FailureStubProvider::Behavior::Ok);
            result = auto_curator.on_signal(session, make_input(failing, identity, live, 5));
            MIRA_CHECK(!result.has_value());
            result = auto_curator.on_signal(
                session, make_input(make_checkpoint(session, task, 18, 3), identity, live, 0));
            MIRA_CHECK(!result.has_value());
            MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 1);
            MIRA_CHECK(provider.calls() == 2);

            // The failed boundary flush is a real retry (forced, not a no-op).
            auto flush_future = auto_curator.flush(session, make_input(failing, identity, live, 0));
            const auto flush_outcome = consume_caller_future(flush_future, caller_defects);
            MIRA_CHECK(await_settlement(auto_curator, session));
            MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
            MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 16);
            MIRA_CHECK(auto_curator.stats().forced_flushes == 1);
            if (flush_outcome.has_value()) {
                MIRA_CHECK(flush_outcome.value().disposition ==
                           WorkingContextCommitDisposition::Committed);
            }

            // The next threshold crossing retries through the policy path and
            // succeeds, keeping the streak at zero.
            const auto retry = make_checkpoint(session, task, 24, 4);
            result = auto_curator.on_signal(session, make_input(retry, identity, live, 0));
            MIRA_CHECK(result.has_value());
            const auto retry_outcome = consume_caller_future(*result, caller_defects);
            MIRA_CHECK(await_settlement(auto_curator, session));
            MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
            MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 24);
            if (retry_outcome.has_value()) {
                MIRA_CHECK(retry_outcome.value().disposition ==
                           WorkingContextCommitDisposition::Committed);
            }
            MIRA_CHECK(caller_defects == 0);
            supervisor.begin_shutdown();
        }
        MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 11. Previous selection: fresh chain nullptr, same chain store latest, epoch
//     bump nullptr with the settled reset
// ---------------------------------------------------------------------------

int previous_selection_and_epoch_reset() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(180);
        const TaskId task = task_from_seed(181);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        // Fresh chain: the curator receives no previous snapshot.
        const auto first = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(first, identity, live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        auto record = curator.record(0);
        MIRA_CHECK(record.has_value() && !record->had_previous);
        auto latest = store.latest(session);
        MIRA_CHECK(latest.has_value() && latest.value().has_value());
        const auto first_committed_id = latest.value()->id;

        // Same chain: the curator receives exactly the store's latest.
        const auto second = make_checkpoint(session, task, 16, 2);
        result = auto_curator.on_signal(session, make_input(second, identity, live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        record = curator.record(1);
        MIRA_CHECK(record.has_value() && record->had_previous);
        MIRA_CHECK(record->previous.id == first_committed_id);
        latest = store.latest(session);
        MIRA_CHECK(latest.has_value() && latest.value().has_value());
        MIRA_CHECK(latest.value()->through_event_sequence == 16);

        // Epoch bump on the same watermark with only the event axis crossed:
        // the settled reset must unblock the fire (without it the old chain's
        // settled watermark would swallow the signal) and the new chain gets
        // no previous snapshot.
        WorkingContextIdentity bumped_identity = identity;
        bumped_identity.environment_epoch = 8;
        WorkingContextCommitState bumped_live = live;
        bumped_live.environment_epoch = 8;
        ConversationCheckpoint bumped = make_checkpoint(session, task, 16, 3);
        bumped.environment_epoch = 8;
        result =
            auto_curator.on_signal(session, make_input(bumped, bumped_identity, bumped_live, 16));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        record = curator.record(2);
        MIRA_CHECK(record.has_value() && !record->had_previous);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 16);
        const auto bumped_latest = store.latest(session);
        MIRA_CHECK(bumped_latest.has_value() && bumped_latest.value().has_value());
        MIRA_CHECK(bumped_latest.value()->environment_epoch == 8);
        MIRA_CHECK(curator.calls() == 3);
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 12. Rejections: session mismatch and tracking capacity (coordinator-minted
//     error futures stay consumable)
// ---------------------------------------------------------------------------

int rejected_signals() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(190);
        const TaskId task = task_from_seed(191);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);

        const auto checkpoint = make_checkpoint(session, task, 8, 1);
        auto mismatched = checkpoint;
        mismatched.session_id = session_from_seed(998);
        auto result = auto_curator.on_signal(session, make_input(mismatched, identity, live, 0));
        MIRA_CHECK(result.has_value());
        std::size_t caller_defects = 0;
        const auto rejection = consume_caller_future(*result, caller_defects);
        MIRA_CHECK(!rejection.has_value());
        MIRA_CHECK(rejection.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(auto_curator.stats().errors == 1);
        MIRA_CHECK(!auto_curator.session_view(session).has_value());
        MIRA_CHECK(curator.calls() == 0);

        WorkingContextTriggerPolicy tight_policy;
        tight_policy.max_tracked_sessions = 1;
        WorkingContextAutoCurator tight(supervisor, curator, store, tight_policy,
                                        ContextCurationOptions{});
        const auto first = make_checkpoint(session, task, 8, 1);
        auto ok = tight.on_signal(session, make_input(first, identity, live, 0));
        MIRA_CHECK(ok.has_value());
        const auto ok_outcome = consume_caller_future(*ok, caller_defects);
        MIRA_CHECK(await_settlement(tight, session));
        if (ok_outcome.has_value()) {
            MIRA_CHECK(ok_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
        }

        const SessionId second_session = session_from_seed(192);
        const auto second_checkpoint = make_checkpoint(second_session, task, 8, 1);
        auto over_capacity =
            tight.on_signal(second_session, make_input(second_checkpoint, make_identity(task),
                                                       make_live(second_session, task), 0));
        MIRA_CHECK(over_capacity.has_value());
        const auto capacity_rejection = consume_caller_future(*over_capacity, caller_defects);
        MIRA_CHECK(!capacity_rejection.has_value());
        MIRA_CHECK(capacity_rejection.error().code == ErrorCode::ResourceExhausted);
        MIRA_CHECK(tight.stats().errors == 1);
        MIRA_CHECK(!tight.session_view(second_session).has_value());
        MIRA_CHECK(curator.calls() == 1);

        auto flush_over =
            tight.flush(second_session, make_input(second_checkpoint, make_identity(task),
                                                   make_live(second_session, task), 0));
        const auto flush_rejection = consume_caller_future(flush_over, caller_defects);
        MIRA_CHECK(!flush_rejection.has_value());
        MIRA_CHECK(flush_rejection.error().code == ErrorCode::ResourceExhausted);
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 13. Shared future ownership: consuming the caller's copy never breaks the
//     coordinator's own drain accounting
// ---------------------------------------------------------------------------

int shared_future_ownership() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(200);
        const TaskId task = task_from_seed(201);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        const auto checkpoint = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(checkpoint, identity, live, 0));
        MIRA_CHECK(result.has_value());
        auto caller_future = *result;
        // A second shared copy from the caller stays valid too.
        auto copied_future = caller_future;
        const auto copied_outcome = consume_caller_future(copied_future, caller_defects);
        const auto caller_outcome = consume_caller_future(caller_future, caller_defects);
        // The coordinator's own copy still records the outcome.
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.stats().committed == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 8);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 0);
        MIRA_CHECK(curator.calls() == 1);
        if (copied_outcome.has_value() && caller_outcome.has_value()) {
            MIRA_CHECK(copied_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
            MIRA_CHECK(caller_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
        }

        // A flush future shared the same way: caller consumption first, then
        // the coordinator drain keeps the accounting straight.
        const auto next = make_checkpoint(session, task, 16, 2);
        auto flush_future = auto_curator.flush(session, make_input(next, identity, live, 0));
        const auto flush_outcome = consume_caller_future(flush_future, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, session));
        MIRA_CHECK(auto_curator.stats().committed == 2);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 16);
        if (flush_outcome.has_value()) {
            MIRA_CHECK(flush_outcome.value().disposition ==
                       WorkingContextCommitDisposition::Committed);
        }
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 14. Shutdown: in-flight cancellation resolves Cancelled and is recorded by
//     the drain; post-close signals and flushes resolve rejections with zero
//     curator calls
// ---------------------------------------------------------------------------

int shutdown_cancellation_and_rejection() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(210);
        const TaskId task = task_from_seed(211);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        CuratorGate gate;
        curator.arm_gate(gate);
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto live = make_live(session, task);
        const auto identity = make_identity(task);
        std::size_t caller_defects = 0;

        const auto first = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator.on_signal(session, make_input(first, identity, live, 0));
        MIRA_CHECK(result.has_value());
        GateReleaser releaser{gate};
        curator.park_after_gate();
        gate.wait_entered(1);
        // The curator leaves the gate and parks on a cooperative cancellation
        // wait — the only holding pattern begin_shutdown can legitimately
        // drain. Shutdown flips the supervisor's stop flag; the parked
        // curator observes it and the future resolves Cancelled.
        gate.release();
        curator.wait_parked();
        const auto report = supervisor.begin_shutdown();
        MIRA_CHECK(report.critical_drain_complete);
        const auto cancelled = consume_caller_future(result.value(), caller_defects);
        MIRA_CHECK(store_latest_json(store, session) == "<none>");

        // The explicit drain records the cancellation: error accounting moves,
        // the settled watermark does not.
        auto_curator.drain(session);
        MIRA_CHECK(auto_curator.stats().errors == 1);
        MIRA_CHECK(auto_curator.session_view(session)->consecutive_failures == 1);
        MIRA_CHECK(auto_curator.session_view(session)->settled_watermark == 0);
        MIRA_CHECK(auto_curator.session_view(session)->last_attempt_watermark == 8);
        MIRA_CHECK(auto_curator.session_view(session)->in_flight == false);
        if (cancelled.has_value()) {
            MIRA_CHECK(cancelled.error().code == ErrorCode::Cancelled);
        }

        // Post-close signals resolve the supervisor's rejection with zero new
        // curator calls; the coordinator never re-fires the cancelled work on
        // its own. The rejection lands in the coordinator-owned copy and is
        // visible through the recorded error accounting.
        const auto next = make_checkpoint(session, task, 16, 2);
        auto rejected = auto_curator.on_signal(session, make_input(next, identity, live, 0));
        MIRA_CHECK(rejected.has_value());
        const auto rejection = consume_caller_future(*rejected, caller_defects);
        MIRA_CHECK(curator.calls() == 1);
        auto_curator.drain(session);
        MIRA_CHECK(auto_curator.stats().errors == 2);
        if (rejection.has_value()) {
            MIRA_CHECK(rejection.error().code == ErrorCode::Unavailable);
        }

        auto flush_rejected = auto_curator.flush(session, make_input(next, identity, live, 0));
        const auto flush_rejection = consume_caller_future(flush_rejected, caller_defects);
        MIRA_CHECK(curator.calls() == 1);
        auto_curator.drain(session);
        // The signal's rejected refresh was already drained, so the flush
        // barrier had nothing in flight and force-issued its own rejection.
        MIRA_CHECK(auto_curator.stats().errors == 3);
        if (flush_rejection.has_value()) {
            MIRA_CHECK(flush_rejection.error().code == ErrorCode::Unavailable);
        }
        MIRA_CHECK(caller_defects == 0);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 15. Destructor: bounded drain (total budget 2x deadline) releases the
//     in-flight work instead of abandoning or waiting forever
// ---------------------------------------------------------------------------

int destructor_bounded_drain() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    const SessionId session = session_from_seed(220);
    const TaskId task = task_from_seed(221);
    const auto live = make_live(session, task);
    const auto identity = make_identity(task);
    InMemoryWorkingContextStore store;
    ScriptedAutoCurator curator;
    CuratorGate gate;
    ContextMemorySupervisor supervisor(exec);
    std::size_t caller_defects = 0;

    std::optional<AutoFuture> caller_future;
    {
        curator.arm_gate(gate);
        GateReleaser releaser{gate};
        ContextCurationOptions options;
        options.deadline = std::chrono::milliseconds(100);
        auto auto_curator = std::make_unique<WorkingContextAutoCurator>(
            supervisor, curator, store, WorkingContextTriggerPolicy{}, options);
        const auto checkpoint = make_checkpoint(session, task, 8, 1);
        auto result = auto_curator->on_signal(session, make_input(checkpoint, identity, live, 0));
        MIRA_CHECK(result.has_value());
        caller_future = *result;
        gate.wait_entered(1);

        // The destructor must give up within the bounded budget while the
        // curator is still blocked — it returns, it does not hang.
        const auto started = std::chrono::steady_clock::now();
        auto_curator.reset();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        MIRA_CHECK(elapsed.count() < 5'000);
    }

    // The released supervised work still settles on the Executor and commits;
    // the caller's shared future stays consumable.
    gate.release();
    const auto outcome = consume_caller_future(caller_future.value(), caller_defects);
    for (int guard = 0; guard < 100'000'000; ++guard) {
        if (store_latest_watermark(store, session) == 8) {
            break;
        }
        std::this_thread::yield();
    }
    MIRA_CHECK(store_latest_watermark(store, session) == 8);
    if (outcome.has_value()) {
        MIRA_CHECK(outcome.value().disposition == WorkingContextCommitDisposition::Committed);
    }
    MIRA_CHECK(caller_defects == 0);
    supervisor.begin_shutdown();
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// 16. Statistics and session-view consistency across two chains
// ---------------------------------------------------------------------------

int stats_and_session_view_consistency() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId first_session = session_from_seed(230);
        const SessionId second_session = session_from_seed(231);
        const TaskId task = task_from_seed(232);
        InMemoryWorkingContextStore store;
        ScriptedAutoCurator curator;
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        const auto identity = make_identity(task);
        const auto first_live = make_live(first_session, task);
        const auto second_live = make_live(second_session, task);
        std::size_t caller_defects = 0;

        // Session two fails once; session one commits twice: the aggregate
        // streak is the worst per-session value, not a sum.
        curator.set_mode(ScriptedAutoCurator::Mode::Error);
        auto result = auto_curator.on_signal(
            second_session,
            make_input(make_checkpoint(second_session, task, 8, 1), identity, second_live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, second_session));
        MIRA_CHECK(auto_curator.stats().consecutive_failures == 1);

        curator.set_mode(ScriptedAutoCurator::Mode::Project);
        for (const std::uint64_t watermark : {std::uint64_t{8}, std::uint64_t{16}}) {
            result = auto_curator.on_signal(
                first_session,
                make_input(make_checkpoint(first_session, task, watermark, watermark), identity,
                           first_live, 0));
            MIRA_CHECK(result.has_value());
            (void)consume_caller_future(*result, caller_defects);
            MIRA_CHECK(await_settlement(auto_curator, first_session));
        }
        WorkingContextAutoStats stats = auto_curator.stats();
        MIRA_CHECK(stats.signals == 3);
        // Session two's failing refresh counts as a watermark fire too.
        MIRA_CHECK(stats.fires_watermark == 3);
        MIRA_CHECK(stats.fires_event_count == 0);
        MIRA_CHECK(stats.committed == 2);
        MIRA_CHECK(stats.errors == 1);
        MIRA_CHECK(stats.consecutive_failures == 1);
        MIRA_CHECK(stats.absorbed == 0 && stats.forced_flushes == 0 && stats.flush_noops == 0 &&
                   stats.idempotent_noops == 0 && stats.discarded_stale == 0 &&
                   stats.discarded_terminal == 0);

        // Session two retries successfully on the next crossing: the worst
        // streak clears once every chain has been healed.
        result = auto_curator.on_signal(
            second_session,
            make_input(make_checkpoint(second_session, task, 16, 2), identity, second_live, 0));
        MIRA_CHECK(result.has_value());
        (void)consume_caller_future(*result, caller_defects);
        MIRA_CHECK(await_settlement(auto_curator, second_session));
        stats = auto_curator.stats();
        MIRA_CHECK(stats.committed == 3);
        MIRA_CHECK(stats.consecutive_failures == 0);

        const auto first_view = auto_curator.session_view(first_session);
        const auto second_view = auto_curator.session_view(second_session);
        MIRA_CHECK(first_view.has_value() && second_view.has_value());
        MIRA_CHECK(first_view->settled_watermark == 16 && first_view->consecutive_failures == 0);
        MIRA_CHECK(second_view->settled_watermark == 16 && second_view->consecutive_failures == 0);
        MIRA_CHECK(!first_view->in_flight && !second_view->in_flight);
        MIRA_CHECK(!auto_curator.session_view(session_from_seed(999)).has_value());
        MIRA_CHECK(caller_defects == 0);
        supervisor.begin_shutdown();
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    const struct {
        const char *name;
        int (*run)();
    } cases[] = {
        {"trigger_policy_pure_function", trigger_policy_pure_function},
        {"construction_validation", construction_validation},
        {"on_signal_no_fire_accrues_events", on_signal_no_fire_accrues_events},
        {"on_signal_fires_both_kinds", on_signal_fires_both_kinds},
        {"on_signal_absorbs_and_refires_latest", on_signal_absorbs_and_refires_latest},
        {"settled_gate_and_catch_up", settled_gate_and_catch_up},
        {"outcome_accounting_committed_noop_discard_error",
         outcome_accounting_committed_noop_discard_error},
        {"flush_branches", flush_branches},
        {"terminal_lateness_keeps_forced_snapshot", terminal_lateness_keeps_forced_snapshot},
        {"failure_fallback_five_classes", failure_fallback_five_classes},
        {"previous_selection_and_epoch_reset", previous_selection_and_epoch_reset},
        {"rejected_signals", rejected_signals},
        {"shared_future_ownership", shared_future_ownership},
        {"shutdown_cancellation_and_rejection", shutdown_cancellation_and_rejection},
        {"destructor_bounded_drain", destructor_bounded_drain},
        {"stats_and_session_view_consistency", stats_and_session_view_consistency},
    };
    constexpr std::size_t case_count = sizeof(cases) / sizeof(cases[0]);
    std::size_t failed = 0;
    for (const auto &item : cases) {
        const int result = item.run();
        if (result != 0) {
            ++failed;
            std::cerr << "FAIL " << item.name << '\n';
        } else {
            std::cout << "PASS " << item.name << '\n';
        }
    }
    if (failed == 0) {
        std::cout << "m22 auto trigger: OK\n";
        return 0;
    }
    std::cerr << "m22 auto trigger: " << failed << "/" << case_count << " cases failed\n";
    return 1;
}
