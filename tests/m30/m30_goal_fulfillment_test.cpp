#include "m28/m28_support.hpp"

#include <mira/sqlite_goal_store.hpp>

#include <cstdlib>
#include <filesystem>

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

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("mira-fulfillment-" + Id128::generate().to_string());
    const RuntimeId runtime_id = RuntimeId::generate();
    const PrincipalContext principal = m28::make_principal();
    const GoalOwner owner{principal.tenant_id, principal.user_id};
    m28::CoordinatorFixture host;
    std::shared_ptr<FileEventStore> events;
    std::unique_ptr<SqliteGoalStore> goals;
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
    MIRA_CHECK(fixture.complete(admission.task));
    MIRA_CHECK(fixture.host.coordinator_->record_verified_task_completion(fixture.principal,
                                                                          admission.task));
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(stale.value(), *fixture.events));
    const auto wrong_owner = fixture.goals->fulfill_dispatch(other.value(), *fixture.events);
    MIRA_CHECK(!wrong_owner && wrong_owner.error().code == ErrorCode::InvalidState);
    const auto too_late = fixture.goals->fulfill_dispatch(expired.value(), *fixture.events);
    MIRA_CHECK(!too_late && too_late.error().code == ErrorCode::InvalidArgument);
    auto wrong_operation = dispatch.value();
    wrong_operation.operation = OperationId::generate();
    MIRA_CHECK(!fixture.goals->fulfill_dispatch(wrong_operation, *fixture.events));
    MIRA_CHECK(fixture.reopen_goals([&](const GoalStoreAccess &access) {
        return access.owner == fixture.owner && access.id == goal.id &&
               access.operation == GoalStoreOperation::ReadDispatch;
    }));
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
} // namespace

int main() {
    if (const int result = only_runtime_completion_can_fulfill_after_restart(); result != 0)
        return result;
    return fulfillment_respects_dispatch_owner_version_and_current_access();
}
