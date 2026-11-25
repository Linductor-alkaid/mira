#include "m28/m28_support.hpp"

#include <mira/goal_dispatch_poller.hpp>
#include <mira/sqlite_goal_store.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>

namespace {
using namespace mira;
using namespace std::chrono_literals;

GoalTime now_ms() {
    return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

struct FulfillmentFixture final {
    FulfillmentFixture() {
        std::filesystem::create_directories(root);
        events = std::make_shared<FileEventStore>(root / "events");
        host.coordinator_ = std::make_unique<InteractionCoordinator>(
            CoordinatorConfig{}, *host.runtime_, host.executor_);
        host.coordinator_->set_event_store(events, runtime_id);
        host.coordinator_->set_environment_provider(
            [this]() -> Result<std::shared_ptr<IEnvironment>> {
                return std::shared_ptr<IEnvironment>{host.environment_};
            });
        if (!host.coordinator_->initialize() || !reopen_goals())
            std::abort();
    }

    ~FulfillmentFixture() {
        host.coordinator_.reset();
        goals.reset();
        events.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    bool reopen_goals(GoalStoreAuthorizer authorize = {}) {
        if (goals && !goals->close())
            return false;
        goals.reset();
        const SqliteGoalStoreOptions options{.path = root / "goals.db"};
        auto store = authorize ? SqliteGoalStore::open_authorized(host.executor_, options,
                                                                  std::move(authorize))
                               : SqliteGoalStore::open(host.executor_, options);
        if (!store)
            return false;
        goals = std::move(store).value();
        return true;
    }

    Result<GoalDispatchKey> record_dispatch(const GoalKey &goal, GoalDispatchSubmission submission,
                                            std::optional<GoalTime> expires = {}) {
        const auto created = goals->create(goal, {GoalKind::Commitment, "Review the archive",
                                                  EventId::generate(), submission.submitted_at - 2h,
                                                  submission.submitted_at - 1min, expires});
        if (!created)
            return created.error();
        const auto prepared = goals->prepare_dispatch(goal, 1, submission.submitted_at);
        if (!prepared)
            return prepared.error();
        const auto recorded = goals->record_dispatch_submission(prepared.value().key, submission);
        if (!recorded)
            return recorded.error();
        return recorded.value().key;
    }

    Result<InputDisposition> submit(const char *id) {
        auto input = host.envelope(id, "Review the archive", principal);
        input.requested_intent = RouteIntent::NewTask;
        return host.submit(input);
    }

    bool complete(TaskId task, TaskState state = TaskState::Completed) {
        const auto command = host.runtime_->complete_task(task, {state, {}});
        if (!command)
            return false;
        const auto result = command.value().outcome(5s);
        return result && result.value().status == SettlementStatus::Applied;
    }

    template <typename Predicate> bool wait_for(Predicate ready) {
        const auto until = std::chrono::steady_clock::now() + 4s;
        while (!ready() && std::chrono::steady_clock::now() < until) {
            auto pause = host.executor_.submit_delayed(5, [] {});
            pause.future.get();
        }
        return ready();
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("mira-fulfillment-" + Id128::generate().to_string());
    const RuntimeId runtime_id = RuntimeId::generate();
    const PrincipalContext principal = m28::make_principal();
    const GoalOwner owner{principal.tenant_id, principal.user_id};
    m28::CoordinatorFixture host;
    std::shared_ptr<FileEventStore> events;
    std::shared_ptr<SqliteGoalStore> goals;
};

int only_runtime_completion_can_fulfill_after_restart() {
    FulfillmentFixture fixture;
    const GoalKey goal{fixture.owner, Id128::generate()};
    const auto work = fixture.submit("verified-completion");
    MIRA_CHECK(work && work.value().task_id && work.value().session_id);
    const GoalDispatchSubmission submitted{*work.value().session_id, *work.value().task_id,
                                           now_ms()};
    const auto dispatch = fixture.record_dispatch(goal, submitted);
    MIRA_CHECK(dispatch);
    auto &coordinator = *fixture.host.coordinator_;
    MIRA_CHECK(coordinator.record_settlement("verified-completion", TaskState::Completed));
    MIRA_CHECK(!coordinator.record_verified_task_completion(fixture.principal, submitted.task));
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events));
    MIRA_CHECK(fixture.goals->read(goal).value()->state == GoalState::Pending);

    MIRA_CHECK(fixture.complete(submitted.task));
    {
        auto buffered = std::make_shared<MemoryEventStore>();
        InteractionCoordinator transient(CoordinatorConfig{}, *fixture.host.runtime_,
                                         fixture.host.executor_);
        transient.set_event_store(buffered, fixture.runtime_id);
        MIRA_CHECK(transient.initialize());
        MIRA_CHECK(!transient.record_verified_task_completion(fixture.principal, submitted.task));
        const auto non_durable = fixture.goals->fulfill_dispatch(dispatch.value(), *buffered);
        MIRA_CHECK(!non_durable && non_durable.error().code == ErrorCode::UnsupportedCapability);
        MIRA_CHECK(transient.close());
    }
    auto ungranted = fixture.principal;
    ungranted.grants.clear();
    const auto denied = coordinator.record_verified_task_completion(ungranted, submitted.task);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
    auto foreign = fixture.principal;
    foreign.user_id = UserId::generate();
    const auto wrong_owner = coordinator.record_verified_task_completion(foreign, submitted.task);
    MIRA_CHECK(!wrong_owner && wrong_owner.error().code == ErrorCode::PermissionDenied);
    const auto completed =
        coordinator.record_verified_task_completion(fixture.principal, submitted.task);
    MIRA_CHECK(completed && completed.value().achieved >= Durability::ProcessCrash);
    const auto replay =
        coordinator.record_verified_task_completion(fixture.principal, submitted.task);
    MIRA_CHECK(replay && replay.value().event_id == completed.value().event_id &&
               replay.value().session_sequence == completed.value().session_sequence);
    const auto snapshot = fixture.host.runtime_->task_snapshot(submitted.task);
    MIRA_CHECK(snapshot);

    MIRA_CHECK(coordinator.close());
    fixture.host.coordinator_.reset();
    fixture.events.reset();
    auto shutdown = fixture.host.runtime_->request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(5s));
    MIRA_CHECK(fixture.host.runtime_->finish_shutdown().clean);
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    MIRA_CHECK(fixture.reopen_goals());
    const auto fulfilled = fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events);
    MIRA_CHECK(fulfilled && fulfilled.value().revision == 2 &&
               fulfilled.value().state == GoalState::Fulfilled && fulfilled.value().resolution);
    MIRA_CHECK(fulfilled.value().resolution->evidence == completed.value().event_id &&
               fulfilled.value().resolution->at ==
                   std::chrono::floor<std::chrono::milliseconds>(snapshot.value().updated_at.wall));
    MIRA_CHECK(fixture.reopen_goals());
    MIRA_CHECK(
        fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events).value().resolution ==
        fulfilled.value().resolution);
    MIRA_CHECK(fixture.goals->erase(goal));
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events));
    return 0;
}

int fulfillment_respects_dispatch_owner_version_and_current_access() {
    FulfillmentFixture fixture;
    const auto work = fixture.submit("completed-review");
    MIRA_CHECK(work && work.value().task_id && work.value().session_id);
    const GoalDispatchSubmission admission{*work.value().session_id, *work.value().task_id,
                                           now_ms()};
    const GoalKey goal{fixture.owner, Id128::generate()};
    const GoalKey corrected{fixture.owner, Id128::generate()};
    const GoalKey foreign{{TenantId::generate(), fixture.owner.user}, goal.id};
    const GoalKey late{fixture.owner, Id128::generate()};
    const auto dispatch = fixture.record_dispatch(goal, admission);
    const auto stale = fixture.record_dispatch(corrected, admission);
    const auto other = fixture.record_dispatch(foreign, admission);
    auto old_admission = admission;
    old_admission.submitted_at -= 2h;
    const auto expired = fixture.record_dispatch(late, old_admission, admission.submitted_at - 1h);
    MIRA_CHECK(dispatch && stale && other && expired);
    auto version = *fixture.goals->read(corrected).value()->definition;
    version.source_event = EventId::generate();
    version.title = "Review the revised archive";
    MIRA_CHECK(fixture.goals->correct(corrected, 1, version));
    const auto page = fixture.goals->pending_fulfillments({fixture.owner, 1, {}});
    MIRA_CHECK(page && page.value().records.size() == 1 && page.value().next);
    const auto following =
        fixture.goals->pending_fulfillments({fixture.owner, 1, page.value().next});
    MIRA_CHECK(following && following.value().records.size() == 1 && !following.value().next);
    const bool goal_first = goal.id < late.id;
    MIRA_CHECK(page.value().records.front() == (goal_first ? dispatch.value() : expired.value()));
    MIRA_CHECK(following.value().records.front() ==
               (goal_first ? expired.value() : dispatch.value()));
    const auto other_page = fixture.goals->pending_fulfillments({foreign.owner});
    MIRA_CHECK(other_page && other_page.value().records == std::vector{other.value()});
    MIRA_CHECK(!fixture.goals->pending_fulfillments({fixture.owner, 0, {}}));
    MIRA_CHECK(fixture.complete(admission.task));
    MIRA_CHECK(fixture.host.coordinator_->record_verified_task_completion(fixture.principal,
                                                                          admission.task));
    auto context = make_control_context();
    context.cancellation_requested = [] { return true; };
    const auto cancelled =
        fixture.goals->reconcile_dispatch_completion(dispatch.value(), *fixture.events, context);
    MIRA_CHECK(!cancelled && cancelled.error().code == ErrorCode::Cancelled);
    context.cancellation_requested = {};
    context.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto deadline =
        fixture.goals->reconcile_dispatch_completion(dispatch.value(), *fixture.events, context);
    MIRA_CHECK(!deadline && deadline.error().code == ErrorCode::DeadlineExceeded);
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(stale.value(), *fixture.events));
    const auto wrong_owner = fixture.goals->fulfill_dispatch(other.value(), *fixture.events);
    MIRA_CHECK(!wrong_owner && wrong_owner.error().code == ErrorCode::InvalidState);
    const auto too_late = fixture.goals->fulfill_dispatch(expired.value(), *fixture.events);
    MIRA_CHECK(!too_late && too_late.error().code == ErrorCode::InvalidArgument);
    auto wrong_operation = dispatch.value();
    wrong_operation.operation = OperationId::generate();
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(wrong_operation, *fixture.events));
    std::atomic<bool> read_authorized{false};
    MIRA_CHECK(fixture.reopen_goals([&](const GoalStoreAccess &access) {
        if (access.owner != fixture.owner || access.id != goal.id ||
            access.operation != GoalStoreOperation::ReadDispatch)
            return false;
        read_authorized.store(true);
        return true;
    }));
    const auto list_denied = fixture.goals->pending_fulfillments({fixture.owner});
    MIRA_CHECK(!list_denied && list_denied.error().code == ErrorCode::PermissionDenied);
    context.deadline.reset();
    context.cancellation_requested = [&] { return read_authorized.load(); };
    const auto interrupted =
        fixture.goals->reconcile_dispatch_completion(dispatch.value(), *fixture.events, context);
    MIRA_CHECK(!interrupted && interrupted.error().code == ErrorCode::Cancelled &&
               read_authorized.load());
    const auto denied = fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(fixture.reopen_goals());
    MIRA_CHECK(fixture.goals->read(goal).value()->state == GoalState::Pending);
    MIRA_CHECK(fixture.goals->read(corrected).value()->definition == version);
    MIRA_CHECK(fixture.goals->read(foreign).value()->state == GoalState::Pending);

    const auto failed = fixture.submit("failed-review");
    MIRA_CHECK(failed && failed.value().task_id && failed.value().session_id);
    const GoalKey failed_goal{fixture.owner, Id128::generate()};
    const auto failed_dispatch = fixture.record_dispatch(
        failed_goal, {*failed.value().session_id, *failed.value().task_id, now_ms()});
    MIRA_CHECK(failed_dispatch && fixture.complete(*failed.value().task_id, TaskState::Failed));
    MIRA_CHECK(!fixture.host.coordinator_->record_verified_task_completion(
        fixture.principal, *failed.value().task_id));
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(failed_dispatch.value(), *fixture.events));

    AppendRequest malformed{
        EventId::generate(),
        fixture.runtime_id,
        admission.session,
        admission.task,
        {std::string(kVerifiedTaskCompletionEvent), "{}", EventClass::Critical}};
    MIRA_CHECK(fixture.events->append(malformed));
    const auto ambiguous = fixture.goals->fulfill_dispatch(dispatch.value(), *fixture.events);
    MIRA_CHECK(!ambiguous && ambiguous.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(fixture.goals->read(goal).value()->state == GoalState::Pending);
    return 0;
}

GoalKey ordered_goal(const GoalOwner &owner, std::uint8_t value) {
    Id128::Bytes bytes{};
    bytes.back() = value;
    return {owner, Id128{bytes}};
}

int poller_reconciles_existing_receipts_after_restart() {
    FulfillmentFixture fixture;
    const GoalKey waiting_for_evidence = ordered_goal(fixture.owner, 1);
    const GoalKey completed = ordered_goal(fixture.owner, 2);
    const GoalKey due_for_dispatch = ordered_goal(fixture.owner, 3);
    const GoalKey prepared_only = ordered_goal(fixture.owner, 4);
    const auto live = fixture.submit("persistent-completion");
    MIRA_CHECK(live && live.value().session_id && live.value().task_id);
    const GoalDispatchSubmission submission{*live.value().session_id, *live.value().task_id,
                                            now_ms()};
    MIRA_CHECK(fixture.complete(submission.task));
    const auto snapshot = fixture.host.runtime_->task_snapshot(submission.task);
    MIRA_CHECK(snapshot);
    const auto evidence = fixture.host.coordinator_->record_verified_task_completion(
        fixture.principal, submission.task);
    MIRA_CHECK(evidence);
    const auto completed_at =
        std::chrono::floor<std::chrono::milliseconds>(snapshot.value().updated_at.wall);
    const auto expires = completed_at + 1ms;
    MIRA_CHECK(fixture.record_dispatch(completed, submission, expires));
    MIRA_CHECK(fixture.record_dispatch(waiting_for_evidence,
                                       {SessionId::generate(), TaskId::generate(), now_ms()}));
    const auto now = now_ms();
    MIRA_CHECK(fixture.goals->create(
        due_for_dispatch,
        {GoalKind::Commitment, "Review once due", EventId::generate(), now - 1h, now - 1min, {}}));
    MIRA_CHECK(fixture.goals->create(prepared_only,
                                     {GoalKind::Commitment, "An unconfirmed dispatch",
                                      EventId::generate(), now - 1h, now - 1min, expires + 1h}));
    MIRA_CHECK(fixture.goals->prepare_dispatch(prepared_only, 1, now));
    const auto pending = fixture.goals->pending_fulfillments({fixture.owner});
    MIRA_CHECK(pending && pending.value().records.size() == 2);
    MIRA_CHECK(pending.value().records.front().goal == waiting_for_evidence &&
               pending.value().records.back().goal == completed);
    // Close this prepared goal before the next scan, so the automatic callback only handles the new
    // goal.
    MIRA_CHECK(
        fixture.goals->settle(prepared_only, 1, {GoalState::Cancelled, EventId::generate(), now}));

    MIRA_CHECK(fixture.host.coordinator_->close());
    fixture.host.coordinator_.reset();
    fixture.events.reset();
    const auto shutdown = fixture.host.runtime_->request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(5s));
    MIRA_CHECK(fixture.host.runtime_->finish_shutdown().clean);
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    std::atomic<bool> allow_settle{false};
    MIRA_CHECK(fixture.reopen_goals([&](const GoalStoreAccess &access) {
        return access.owner == fixture.owner &&
               (access.operation != GoalStoreOperation::Settle || allow_settle.load());
    }));
    MIRA_CHECK(fixture.wait_for([&] { return now_ms() >= expires; }));
    GoalDispatchPollerOptions options{10ms, 2s, 1, fixture.events};
    std::atomic<std::size_t> unnecessary_publications{0};
    options.publish_completion = [&](const GoalDispatchKey &key, const GoalDispatchSubmission &,
                                     const OperationContext &) -> Result<bool> {
        if (key.goal == completed)
            ++unnecessary_publications;
        return false;
    };
    auto started = GoalDispatchPoller::start(
        fixture.host.executor_, fixture.goals, fixture.owner,
        [&](const GoalRecord &goal, const GoalDispatchKey &,
            const OperationContext &) -> Result<GoalDispatchSubmission> {
            if (goal.key == due_for_dispatch)
                return GoalDispatchSubmission{SessionId::generate(), TaskId::generate(), now_ms()};
            return interaction_error(ErrorCode::InvalidState, InteractionDomainCode::RouteRejected,
                                     "unexpected goal dispatch");
        },
        options);
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] {
        const auto status = poller->status();
        return status.submitted == 1 && status.failures > 0;
    }));
    const auto denied = poller->status();
    MIRA_CHECK(denied.fulfilled == 0 && denied.last_error &&
               denied.last_error->code == ErrorCode::PermissionDenied);
    MIRA_CHECK(fixture.goals->read(completed).value()->state == GoalState::Pending);
    allow_settle.store(true);
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().fulfilled == 1; }));
    MIRA_CHECK(poller->close());
    const auto fulfilled = fixture.goals->read(completed);
    MIRA_CHECK(fulfilled && fulfilled.value() && fulfilled.value()->resolution &&
               fulfilled.value()->resolution->evidence == evidence.value().event_id &&
               fulfilled.value()->resolution->at == completed_at);
    MIRA_CHECK(fixture.goals->read(waiting_for_evidence).value()->state == GoalState::Pending);
    MIRA_CHECK(fixture.goals->read_dispatch(due_for_dispatch).value()->submission);
    poller.reset();
    MIRA_CHECK(fixture.reopen_goals());

    auto fulfillment_only = GoalDispatchPoller::start(fixture.host.executor_, fixture.goals,
                                                      fixture.owner, {}, options);
    MIRA_CHECK(fulfillment_only);
    poller = std::move(fulfillment_only).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().scans >= 3; }));
    MIRA_CHECK(poller->close());
    const auto refreshed = poller->status();
    MIRA_CHECK(!refreshed.running && refreshed.fulfilled == 0 && refreshed.submitted == 0 &&
               refreshed.failures == 0);
    MIRA_CHECK(unnecessary_publications.load() == 0);
    return 0;
}

int poller_publishes_live_completion_and_settles_under_current_authorization() {
    FulfillmentFixture fixture;
    const auto live = fixture.submit("publish-when-complete");
    const auto failed = fixture.submit("leave-failed-pending");
    MIRA_CHECK(live && live.value().task_id && live.value().session_id && failed &&
               failed.value().task_id && failed.value().session_id);
    const auto now = now_ms();
    const GoalKey completed = ordered_goal(fixture.owner, 2);
    const GoalKey failed_goal = ordered_goal(fixture.owner, 1);
    MIRA_CHECK(
        fixture.record_dispatch(completed, {*live.value().session_id, *live.value().task_id, now}));
    MIRA_CHECK(fixture.record_dispatch(failed_goal,
                                       {*failed.value().session_id, *failed.value().task_id, now}));
    MIRA_CHECK(fixture.complete(*failed.value().task_id, TaskState::Failed));

    auto &coordinator = *fixture.host.coordinator_;
    auto context = make_control_context();
    const auto mismatched_session = coordinator.publish_verified_task_completion_if_ready(
        fixture.principal, SessionId::generate(), *live.value().task_id, context);
    MIRA_CHECK(!mismatched_session &&
               mismatched_session.error().code == ErrorCode::PermissionDenied);
    const auto missing = coordinator.publish_verified_task_completion_if_ready(
        fixture.principal, *live.value().session_id, TaskId::generate(), context);
    MIRA_CHECK(missing && !missing.value());
    context.cancellation_requested = [] { return true; };
    const auto cancelled = coordinator.publish_verified_task_completion_if_ready(
        fixture.principal, *live.value().session_id, *live.value().task_id, context);
    MIRA_CHECK(!cancelled && cancelled.error().code == ErrorCode::Cancelled);

    {
        GoalDispatchPollerOptions false_ack{10ms, 2s, 1};
        false_ack.publish_completion = [](const GoalDispatchKey &, const GoalDispatchSubmission &,
                                          const OperationContext &) -> Result<bool> {
            return true;
        };
        MIRA_CHECK(!GoalDispatchPoller::start(fixture.host.executor_, fixture.goals, fixture.owner,
                                              {}, false_ack));
        false_ack.completion_events = fixture.events;
        auto started = GoalDispatchPoller::start(fixture.host.executor_, fixture.goals,
                                                 fixture.owner, {}, false_ack);
        MIRA_CHECK(started);
        auto poller = std::move(started).value();
        MIRA_CHECK(fixture.wait_for([&] { return poller->status().failures > 0; }));
        MIRA_CHECK(poller->close());
        const auto status = poller->status();
        MIRA_CHECK(status.fulfilled == 0 && status.last_error &&
                   status.last_error->code == ErrorCode::Unavailable);
    }

    std::atomic<bool> allow_publication{false};
    std::atomic<std::size_t> checked_live{0};
    GoalDispatchPollerOptions options{10ms, 2s, 1, fixture.events};
    options.publish_completion = [&](const GoalDispatchKey &key,
                                     const GoalDispatchSubmission &receipt,
                                     const OperationContext &attempt) -> Result<bool> {
        if (attempt.operation != key.operation || attempt.session != receipt.session ||
            attempt.task != receipt.task)
            return interaction_error(ErrorCode::InvalidState,
                                     InteractionDomainCode::RuntimeCommandFailed,
                                     "unexpected publication context");
        auto principal = fixture.principal;
        if (!allow_publication.load())
            principal.grants.clear();
        auto result = coordinator.publish_verified_task_completion_if_ready(
            principal, receipt.session, receipt.task, attempt);
        if (result && key.goal == completed)
            ++checked_live;
        return result;
    };
    auto started = GoalDispatchPoller::start(fixture.host.executor_, fixture.goals, fixture.owner,
                                             {}, options);
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().failures > 0; }));
    const auto denied = poller->status();
    MIRA_CHECK(denied.last_error && denied.last_error->code == ErrorCode::PermissionDenied);
    allow_publication.store(true);
    MIRA_CHECK(fixture.wait_for([&] { return checked_live.load() >= 1; }));
    MIRA_CHECK(fixture.goals->read(completed).value()->state == GoalState::Pending);
    MIRA_CHECK(fixture.complete(*live.value().task_id));
    MIRA_CHECK(fixture.wait_for([&] { return poller->status().fulfilled == 1; }));
    MIRA_CHECK(poller->close());
    const auto completed_record = fixture.goals->read(completed);
    MIRA_CHECK(completed_record && completed_record.value()->state == GoalState::Fulfilled &&
               completed_record.value()->resolution);
    MIRA_CHECK(fixture.goals->read(failed_goal).value()->state == GoalState::Pending);

    EventSliceQuery query;
    query.session_id = *live.value().session_id;
    query.task_id = *live.value().task_id;
    query.event_type = kVerifiedTaskCompletionEvent;
    const auto events = fixture.events->read_slice(query);
    MIRA_CHECK(events && events.value().events.size() == 1 &&
               events.value().events.front().event_id ==
                   completed_record.value()->resolution->evidence);
    const auto replay = coordinator.publish_verified_task_completion_if_ready(
        fixture.principal, *live.value().session_id, *live.value().task_id, make_control_context());
    MIRA_CHECK(replay && replay.value());
    MIRA_CHECK(fixture.events->read_slice(query).value().events.size() == 1);
    return 0;
}

int publication_yields_when_the_coordinator_is_busy() {
    FulfillmentFixture fixture;
    const auto work = fixture.submit("completed-while-coordinator-busy");
    MIRA_CHECK(work && work.value().task_id && work.value().session_id);
    MIRA_CHECK(fixture.complete(*work.value().task_id));

    std::promise<void> entered;
    auto waiting_for_entry = entered.get_future();
    std::promise<void> release;
    auto waiting_for_release = release.get_future();
    auto &coordinator = *fixture.host.coordinator_;
    coordinator.set_route_resolver(
        [&](const InputEnvelope &, const PrincipalState &) -> std::optional<RouteSuggestion> {
            entered.set_value();
            (void)waiting_for_release.wait_for(3s);
            return std::nullopt;
        });
    auto submit = fixture.host.executor_.submit_auto([&] {
        return fixture.host.submit(
            fixture.host.envelope("contending-input", "Review another file", fixture.principal));
    });
    const auto entered_status = waiting_for_entry.wait_for(2s);
    if (entered_status != std::future_status::ready) {
        release.set_value();
        (void)submit.get();
        MIRA_CHECK(entered_status == std::future_status::ready);
    }
    auto publish = fixture.host.executor_.submit_auto([&] {
        return coordinator.publish_verified_task_completion_if_ready(
            fixture.principal, *work.value().session_id, *work.value().task_id,
            make_control_context());
    });
    const auto publish_status = publish.wait_for(500ms);
    release.set_value();
    const auto observed = publish.get();
    const auto input = submit.get();
    MIRA_CHECK(input && publish_status == std::future_status::ready);
    MIRA_CHECK(!observed && observed.error().code == ErrorCode::ResourceExhausted &&
               observed.error().retryable);
    const auto published = coordinator.publish_verified_task_completion_if_ready(
        fixture.principal, *work.value().session_id, *work.value().task_id, make_control_context());
    MIRA_CHECK(published && published.value());
    return 0;
}

int durable_task_admission_survives_runtime_restart() {
    FulfillmentFixture fixture;
    const GoalKey goal{fixture.owner, Id128::generate()};
    const auto now = now_ms();
    MIRA_CHECK(fixture.goals->create(goal, {GoalKind::Commitment,
                                            "Review the archive",
                                            EventId::generate(),
                                            now - 2h,
                                            now - 1min,
                                            {}}));
    const auto prepared = fixture.goals->prepare_dispatch_attempt(goal, 1, now);
    MIRA_CHECK(prepared && prepared.value().newly_prepared);
    const auto dispatch = prepared.value().receipt.key;
    auto &runtime = *fixture.host.runtime_;
    auto &coordinator = *fixture.host.coordinator_;
    const auto session = runtime.open_session(fixture.host.environment_, {fixture.principal});
    MIRA_CHECK(session && session.value().command.outcome(5s));
    MIRA_CHECK(!runtime.submit_task(session.value().id,
                                    {"Invalid admission operation", {}, OperationId{}}));
    const auto submitted =
        runtime.submit_task(session.value().id, {"Review the archive", {}, dispatch.operation});
    MIRA_CHECK(submitted);
    const auto outcome = submitted.value().command.outcome(5s);
    MIRA_CHECK(outcome && outcome.value().status == SettlementStatus::Applied &&
               outcome.value().task &&
               outcome.value().task->admission_operation == dispatch.operation);
    const auto context = make_control_context();
    const auto vacant =
        coordinator.lookup_task_admission(fixture.principal, dispatch.operation, context);
    MIRA_CHECK(vacant && !vacant.value());
    const auto unrelated = coordinator.record_task_admission(
        fixture.principal, OperationId::generate(), submitted.value(), context);
    MIRA_CHECK(!unrelated && unrelated.error().code == ErrorCode::InvalidState);
    auto wrong_task = submitted.value();
    wrong_task.id = TaskId::generate();
    const auto mismatched = coordinator.record_task_admission(fixture.principal, dispatch.operation,
                                                              wrong_task, context);
    MIRA_CHECK(!mismatched && mismatched.error().code == ErrorCode::InvalidState);
    auto foreign = fixture.principal;
    foreign.user_id = UserId::generate();
    const auto unowned =
        coordinator.record_task_admission(foreign, dispatch.operation, submitted.value(), context);
    MIRA_CHECK(!unowned && unowned.error().code == ErrorCode::PermissionDenied);

    // The buffered adapter may expose an event without confirming process-crash durability.
    {
        const auto buffered = std::make_shared<MemoryEventStore>();
        InteractionCoordinator transient(CoordinatorConfig{}, runtime, fixture.host.executor_);
        transient.set_event_store(buffered, RuntimeId::generate());
        MIRA_CHECK(transient.initialize());
        MIRA_CHECK(!transient.record_task_admission(fixture.principal, dispatch.operation,
                                                    submitted.value(), context));
        const auto unsafe =
            transient.lookup_task_admission(fixture.principal, dispatch.operation, context);
        MIRA_CHECK(!unsafe && unsafe.error().code == ErrorCode::UnsupportedCapability);
        const auto guarded_operation = OperationId::generate();
        const auto recorder = transient.make_task_admission_recorder(
            fixture.principal, session.value().id, guarded_operation, context);
        MIRA_CHECK(recorder);
        const auto rejected = runtime.submit_task(
            session.value().id, {"Require durable admission", {}, guarded_operation},
            recorder.value());
        MIRA_CHECK(rejected);
        const auto terminal = rejected.value().command.outcome(5s);
        MIRA_CHECK(terminal && terminal.value().status == SettlementStatus::Failed &&
                   !runtime.task_snapshot(rejected.value().id));
        MIRA_CHECK(transient.close());
    }

    MIRA_CHECK(fixture.complete(submitted.value().id));
    const auto recorded = coordinator.record_task_admission(fixture.principal, dispatch.operation,
                                                            submitted.value(), context);
    MIRA_CHECK(recorded && recorded.value().session == session.value().id &&
               recorded.value().task == submitted.value().id &&
               recorded.value().admitted_at == outcome.value().task->updated_at.wall);
    const auto replay = coordinator.record_task_admission(fixture.principal, dispatch.operation,
                                                          submitted.value(), context);
    MIRA_CHECK(replay && replay.value() == recorded.value());
    const auto private_events = fixture.events->read({session.value().id, {}, 2});
    MIRA_CHECK(private_events && private_events.value().events.empty());
    const auto duplicate = runtime.submit_task(
        session.value().id, {"Different task under the same operation", {}, dispatch.operation});
    MIRA_CHECK(duplicate && duplicate.value().command.outcome(5s));
    const auto conflict = coordinator.record_task_admission(fixture.principal, dispatch.operation,
                                                            duplicate.value(), context);
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::AlreadyExists);
    const auto isolated = coordinator.lookup_task_admission(foreign, dispatch.operation, context);
    MIRA_CHECK(isolated && !isolated.value());
    auto revoked = fixture.principal;
    revoked.grants.clear();
    const auto denied = coordinator.lookup_task_admission(revoked, dispatch.operation, context);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(!fixture.goals->read_dispatch(goal).value()->submission);

    const GoalKey guarded_goal{fixture.owner, Id128::generate()};
    MIRA_CHECK(fixture.goals->create(guarded_goal, {GoalKind::Commitment,
                                                    "Review with durable admission",
                                                    EventId::generate(),
                                                    now - 2h,
                                                    now - 2min,
                                                    {}}));
    const auto guarded_preparation = fixture.goals->prepare_dispatch_attempt(guarded_goal, 1, now);
    MIRA_CHECK(guarded_preparation && guarded_preparation.value().newly_prepared);
    const auto guarded_key = guarded_preparation.value().receipt.key;
    const auto recorder = coordinator.make_task_admission_recorder(
        fixture.principal, session.value().id, guarded_key.operation, context);
    MIRA_CHECK(recorder);
    const auto completion = coordinator.make_task_completion_recorder(
        fixture.principal, session.value().id, guarded_key.operation);
    MIRA_CHECK(completion);
    const auto denied_completion = coordinator.make_task_completion_recorder(
        revoked, session.value().id, guarded_key.operation);
    MIRA_CHECK(!denied_completion && denied_completion.error().code == ErrorCode::PermissionDenied);
    const auto other_session = runtime.open_session(fixture.host.environment_, {foreign});
    MIRA_CHECK(other_session && other_session.value().command.outcome(5s));
    const auto wrong_session = runtime.submit_task(
        other_session.value().id, {"Wrong session", {}, guarded_key.operation}, recorder.value());
    MIRA_CHECK(wrong_session);
    const auto rejected = wrong_session.value().command.outcome(5s);
    MIRA_CHECK(rejected && rejected.value().status == SettlementStatus::Failed &&
               rejected.value().error &&
               rejected.value().error->code == ErrorCode::PermissionDenied &&
               !runtime.task_snapshot(wrong_session.value().id));
    const auto untouched =
        coordinator.lookup_task_admission(fixture.principal, guarded_key.operation, context);
    MIRA_CHECK(untouched && !untouched.value());

    auto short_context = context;
    short_context.deadline = std::chrono::steady_clock::now() + 100ms;
    const auto expired_operation = OperationId::generate();
    const auto expiring_recorder = coordinator.make_task_admission_recorder(
        fixture.principal, session.value().id, expired_operation, short_context);
    MIRA_CHECK(expiring_recorder);
    MIRA_CHECK(fixture.wait_for([&] { return short_context.expired(Timestamp::now()); }));
    const auto late_task = runtime.submit_task(session.value().id,
                                               {"Expired durable admission", {}, expired_operation},
                                               expiring_recorder.value());
    MIRA_CHECK(late_task);
    const auto late_result = late_task.value().command.outcome(5s);
    MIRA_CHECK(late_result && late_result.value().status == SettlementStatus::Failed &&
               late_result.value().error &&
               late_result.value().error->code == ErrorCode::DeadlineExceeded &&
               !runtime.task_snapshot(late_task.value().id));
    const auto expired_record =
        coordinator.lookup_task_admission(fixture.principal, expired_operation, context);
    MIRA_CHECK(expired_record && !expired_record.value());

    std::promise<void> persisted;
    auto durable = persisted.get_future();
    std::promise<void> publish;
    auto publish_ready = publish.get_future();
    const auto guarded = runtime.submit_task(
        session.value().id, {"Review with durable admission", {}, guarded_key.operation},
        [&](const TaskSnapshot &snapshot, TenantId tenant, UserId user) -> Result<void> {
            const auto written = recorder.value()(snapshot, tenant, user);
            if (!written)
                return written.error();
            persisted.set_value();
            if (publish_ready.wait_for(3s) != std::future_status::ready)
                return interaction_error(ErrorCode::DeadlineExceeded,
                                         InteractionDomainCode::RuntimeCommandTimeout,
                                         "fixture publication timed out");
            return {};
        },
        [write = completion.value()](const TaskSnapshot &candidate, TenantId tenant,
                                     UserId user) -> Result<void> {
            const auto written = write(candidate, tenant, user);
            if (!written)
                return written.error();
            return interaction_error(ErrorCode::ExecutionUncertain,
                                     InteractionDomainCode::RuntimeCommandFailed,
                                     "fixture lost the completion acknowledgement");
        });
    MIRA_CHECK(guarded);
    const auto durable_status = durable.wait_for(2s);
    if (durable_status != std::future_status::ready) {
        publish.set_value();
        (void)guarded.value().command.outcome(5s);
        MIRA_CHECK(durable_status == std::future_status::ready);
    }
    const auto invisible = runtime.task_snapshot(guarded.value().id);
    const auto before_publication =
        coordinator.lookup_task_admission(fixture.principal, guarded_key.operation, context);
    publish.set_value();
    const auto guarded_outcome = guarded.value().command.outcome(5s);
    MIRA_CHECK(!invisible && invisible.error().code == ErrorCode::NotFound);
    MIRA_CHECK(before_publication && before_publication.value() &&
               before_publication.value()->task == guarded.value().id);
    MIRA_CHECK(guarded_outcome && guarded_outcome.value().status == SettlementStatus::Applied &&
               guarded_outcome.value().task);
    const auto duplicate_guarded = runtime.submit_task(
        session.value().id, {"Review with durable admission", {}, guarded_key.operation},
        recorder.value());
    MIRA_CHECK(duplicate_guarded);
    const auto conflict_guarded = duplicate_guarded.value().command.outcome(5s);
    MIRA_CHECK(conflict_guarded && conflict_guarded.value().status == SettlementStatus::Failed &&
               conflict_guarded.value().error &&
               conflict_guarded.value().error->code == ErrorCode::AlreadyExists &&
               !runtime.task_snapshot(duplicate_guarded.value().id));
    // No post-submission call to record_task_admission is made for this task.
    MIRA_CHECK(!fixture.goals->read_dispatch(guarded_goal).value()->submission);
    const auto complete = runtime.complete_task(guarded.value().id, {TaskState::Completed, {}});
    MIRA_CHECK(complete);
    const auto ambiguous = complete.value().outcome(5s);
    MIRA_CHECK(ambiguous && ambiguous.value().status == SettlementStatus::Failed &&
               ambiguous.value().error &&
               ambiguous.value().error->code == ErrorCode::ExecutionUncertain);
    MIRA_CHECK(runtime.task_snapshot(guarded.value().id).value().state == TaskState::Idle);
    EventSliceQuery query;
    query.session_id = session.value().id;
    query.task_id = guarded.value().id;
    query.event_type = kVerifiedTaskCompletionEvent;
    const auto evidence = fixture.events->read_slice(query);
    MIRA_CHECK(evidence && evidence.value().events.size() == 1);
    const auto completion_event = evidence.value().events.front().event_id;

    // Lose both live records before the goal store can record dispatch or completion.
    MIRA_CHECK(coordinator.close());
    fixture.host.coordinator_.reset();
    const auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(5s));
    MIRA_CHECK(runtime.finish_shutdown().clean);
    fixture.host.runtime_ = std::make_unique<MiraRuntime>();
    MIRA_CHECK(fixture.host.runtime_->initialize());
    MIRA_CHECK(!fixture.host.runtime_->task_snapshot(recorded.value().task));
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    fixture.host.coordinator_ = std::make_unique<InteractionCoordinator>(
        CoordinatorConfig{}, *fixture.host.runtime_, fixture.host.executor_);
    fixture.host.coordinator_->set_event_store(fixture.events, RuntimeId::generate());
    MIRA_CHECK(fixture.host.coordinator_->initialize() && fixture.reopen_goals());
    const auto recovered = fixture.host.coordinator_->lookup_task_admission(
        fixture.principal, dispatch.operation, context);
    MIRA_CHECK(recovered && recovered.value() == recorded.value());
    const auto guarded_recovered = fixture.host.coordinator_->lookup_task_admission(
        fixture.principal, guarded_key.operation, context);
    MIRA_CHECK(guarded_recovered && guarded_recovered.value() &&
               guarded_recovered.value()->task == guarded.value().id &&
               guarded_recovered.value()->session == session.value().id &&
               guarded_recovered.value()->admitted_at ==
                   guarded_outcome.value().task->updated_at.wall);

    GoalDispatchPollerOptions options{10ms, 2s, 1, fixture.events};
    options.reconcile_submission =
        [&](const GoalDispatchKey &key,
            const OperationContext &attempt) -> Result<std::optional<GoalDispatchSubmission>> {
        const auto known = fixture.host.coordinator_->lookup_task_admission(fixture.principal,
                                                                            key.operation, attempt);
        if (!known)
            return known.error();
        if (!known.value())
            return std::optional<GoalDispatchSubmission>{};
        const auto &value = *known.value();
        return std::optional{GoalDispatchSubmission{
            value.session, value.task,
            std::chrono::floor<std::chrono::milliseconds>(value.admitted_at)}};
    };
    std::atomic<bool> attempted_new_task{false};
    auto started = GoalDispatchPoller::start(
        fixture.host.executor_, fixture.goals, fixture.owner,
        [&](const GoalRecord &, const GoalDispatchKey &,
            const OperationContext &) -> Result<GoalDispatchSubmission> {
            attempted_new_task.store(true);
            return interaction_error(ErrorCode::InvalidState, InteractionDomainCode::RouteRejected,
                                     "reconciliation attempted a new task");
        },
        options);
    MIRA_CHECK(started);
    auto poller = std::move(started).value();
    MIRA_CHECK(fixture.wait_for([&] {
        const auto status = poller->status();
        return status.submitted == 2 && status.fulfilled == 1;
    }));
    MIRA_CHECK(poller->close() && !attempted_new_task.load());
    const auto result = fixture.goals->read_dispatch(goal);
    MIRA_CHECK(result && result.value()->key == dispatch && result.value()->submission &&
               result.value()->submission->task == recorded.value().task &&
               result.value()->submission->session == recorded.value().session);
    const auto guarded_result = fixture.goals->read_dispatch(guarded_goal);
    MIRA_CHECK(guarded_result && guarded_result.value() &&
               guarded_result.value()->key == guarded_key && guarded_result.value()->submission &&
               guarded_result.value()->submission->task == guarded.value().id);
    const auto fulfilled = fixture.goals->read(guarded_goal);
    MIRA_CHECK(fulfilled && fulfilled.value()->state == GoalState::Fulfilled &&
               fulfilled.value()->resolution &&
               fulfilled.value()->resolution->evidence == completion_event);
    MIRA_CHECK(fixture.goals->read(goal).value()->state == GoalState::Pending);
    return 0;
}
} // namespace

int main() {
    if (const int result = only_runtime_completion_can_fulfill_after_restart(); result != 0)
        return result;
    if (const int result = fulfillment_respects_dispatch_owner_version_and_current_access();
        result != 0)
        return result;
    if (const int result = poller_reconciles_existing_receipts_after_restart(); result != 0)
        return result;
    if (const int result =
            poller_publishes_live_completion_and_settles_under_current_authorization();
        result != 0)
        return result;
    if (const int result = publication_yields_when_the_coordinator_is_busy(); result != 0)
        return result;
    return durable_task_admission_survives_runtime_restart();
}
