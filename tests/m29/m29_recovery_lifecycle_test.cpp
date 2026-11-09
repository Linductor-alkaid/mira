#include "m29_recovery_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

namespace {
class ProbeEnvironment final : public IEnvironment {
  public:
    EnvironmentCapabilities capabilities() const override { return inner.capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        ++observations;
        if (on_observe)
            on_observe(context);
        return inner.observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        return inner.execute(input, context);
    }
    Result<void> interrupt(const OperationContext &context) override {
        return inner.interrupt(context);
    }
    test::FakeEnvironment inner;
    std::function<void(const OperationContext &)> on_observe;
    int observations = 0;
};

int lifecycle_and_stale_permits() {
    RecoveryRoot root;
    const auto principal = make_principal();
    TaskId task;
    SessionId session;
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto initial = host.create("goal", "recover with bounded lifecycle", principal);
        MIRA_CHECK(initial);
        task = *initial.value().task_id;
        session = *initial.value().session_id;
        MIRA_CHECK(host.coordinator().close());
    }
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        auto environment = std::make_shared<ProbeEnvironment>();
        auto cancelled = recovery_context();
        cancelled.cancellation_requested = [] { return true; };
        const auto stopped =
            host.coordinator().recover_work(principal, task, environment, cancelled);
        MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled &&
                   environment->observations == 0);
        environment->on_observe = [](const OperationContext &) {
            throw std::runtime_error("private observation exception");
        };
        const auto threw =
            host.coordinator().recover_work(principal, task, environment, recovery_context());
        MIRA_CHECK(!threw && threw.error().safe_message.find("private") == std::string::npos);
        auto timed = recovery_context();
        timed.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        environment->on_observe = [](const OperationContext &) {
            std::promise<void> never;
            (void)never.get_future().wait_for(std::chrono::milliseconds(20));
        };
        const auto timeout = host.coordinator().recover_work(principal, task, environment, timed);
        MIRA_CHECK(!timeout && timeout.error().code == ErrorCode::DeadlineExceeded);

        // A real takeover is reflected through the host's cancellation probe.
        const auto control = host.core.runtime_->open_session(environment);
        MIRA_CHECK(control && control.value().command.outcome(std::chrono::seconds(2)));
        bool took_over = false;
        environment->on_observe = [&](const OperationContext &) {
            const auto command = host.core.runtime_->request_human_takeover(control.value().id);
            if (command) {
                const auto result = command.value().outcome(std::chrono::seconds(2));
                took_over = result && result.value().status == SettlementStatus::Applied;
            }
        };
        auto takeover = recovery_context();
        takeover.cancellation_requested = [&] {
            const auto state = host.core.runtime_->session_snapshot(control.value().id);
            return state && state.value().state == SessionState::HumanControlled;
        };
        const auto taken = host.coordinator().recover_work(principal, task, environment, takeover);
        MIRA_CHECK(took_over && !taken && taken.error().code == ErrorCode::Cancelled);
        environment->on_observe = {};
        const auto ready =
            host.coordinator().recover_work(principal, task, environment, recovery_context());
        MIRA_CHECK(ready && ready.value().ready_for_new_task);
        MIRA_CHECK(seed_uncertain(host, task, session));
        const auto stale =
            host.coordinator().submit_input(host.input("stale", "continue", principal, task));
        MIRA_CHECK(stale && stale.value().stage == InputStage::Rejected);
        MIRA_CHECK(stale.value().error->domain_code ==
                   static_cast<std::int32_t>(InteractionDomainCode::RecoveryStale));
        const auto rejected_verifier = host.coordinator().recover_work(
            principal, task, environment, recovery_context(),
            [](const WorkRecoveryReport &, const Observation &,
               const OperationContext &) -> Result<bool> { return false; });
        MIRA_CHECK(rejected_verifier && !rejected_verifier.value().ready_for_new_task);
        const auto bad_verifier = host.coordinator().recover_work(
            principal, task, environment, recovery_context(),
            [](const WorkRecoveryReport &, const Observation &, const OperationContext &)
                -> Result<bool> { throw std::runtime_error("private verifier exception"); });
        MIRA_CHECK(!bad_verifier &&
                   bad_verifier.error().safe_message.find("private") == std::string::npos);
        const auto raced =
            host.coordinator().recover_work(principal, task, environment, recovery_context(),
                                            [&](const WorkRecoveryReport &, const Observation &,
                                                const OperationContext &) -> Result<bool> {
                                                const auto saved =
                                                    seed_uncertain(host, task, session);
                                                return saved.has_value();
                                            });
        MIRA_CHECK(!raced && raced.error().domain_code ==
                                 static_cast<std::int32_t>(InteractionDomainCode::RecoveryStale));
        const auto pinned =
            RecoveryPlanner(*host.events, *host.checkpoints).plan(task, session, Timestamp::now());
        MIRA_CHECK(pinned && pinned.value().pending_side_effects.size() == 2);
        MIRA_CHECK(environment->inner.executed_event_count() == 0);
    }
    {
        CoordinatorConfig config;
        config.recovery_permit_ttl = std::chrono::seconds(0);
        RecoveryHost host(root.path, config);
        MIRA_CHECK(host.start());
        const auto ready = host.coordinator().recover_work(
            principal, task, host.core.environment_, recovery_context(),
            [](const WorkRecoveryReport &, const Observation &,
               const OperationContext &) -> Result<bool> { return true; });
        MIRA_CHECK(ready && ready.value().ready_for_new_task);
        const auto expired = host.coordinator().submit_input(
            host.input("expired-permit", "continue", principal, task));
        MIRA_CHECK(expired && expired.value().stage == InputStage::Rejected);
        MIRA_CHECK(expired.value().error->domain_code ==
                   static_cast<std::int32_t>(InteractionDomainCode::RecoveryStale));
    }
    return 0;
}

int close_interrupts_observation() {
    RecoveryRoot root;
    const auto principal = make_principal();
    TaskId task;
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto initial = host.create("goal", "shutdown recovery safely", principal);
        MIRA_CHECK(initial);
        task = *initial.value().task_id;
        MIRA_CHECK(host.coordinator().close());
    }
    RecoveryHost host(root.path);
    MIRA_CHECK(host.start());
    auto environment = std::make_shared<ProbeEnvironment>();
    std::promise<void> entered;
    auto ready = entered.get_future();
    environment->on_observe = [&](const OperationContext &context) {
        entered.set_value();
        std::promise<void> never;
        auto wait = never.get_future();
        while (!context.cancelled_or_expired(Timestamp::now()))
            (void)wait.wait_for(std::chrono::milliseconds(1));
    };
    auto work = host.core.executor_.submit_auto([&] {
        return host.coordinator().recover_work(principal, task, environment, recovery_context());
    });
    const bool observing = ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const auto closed = host.coordinator().close();
    const auto result = work.get();
    MIRA_CHECK(observing && closed && !result && result.error().code == ErrorCode::Cancelled);
    MIRA_CHECK(environment->inner.executed_event_count() == 0);
    return 0;
}

int planner_checks_cancellation_and_missing_history() {
    auto memory = std::make_shared<MemoryEventStore>();
    auto proxy = std::make_shared<FaultEventStore>(memory);
    const auto task = TaskId::generate();
    const auto session = SessionId::generate();
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = RuntimeId::generate();
    request.task_id = task;
    request.session_id = session;
    request.payload = {"TaskEpochAdvanced", R"({"epoch":0})", EventClass::State};
    MIRA_CHECK(memory->append(request));
    bool cancelled = false;
    proxy->on_read = [&] { cancelled = true; };
    auto context = recovery_context();
    context.cancellation_requested = [&] { return cancelled; };
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(*proxy, checkpoints);
    const auto stopped = planner.plan(task, session, Timestamp::now(), context);
    MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled && proxy->reads == 1);
    MIRA_CHECK(planner.plan(task, session, Timestamp::now())); // Legacy overload unchanged.

    RecoveryRoot root;
    const auto principal = make_principal();
    TaskId source;
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto input = host.create("goal", "missing task history", principal);
        MIRA_CHECK(input);
        source = *input.value().task_id;
        MIRA_CHECK(host.coordinator().close());
    }
    RecoveryHost host(root.path);
    auto hidden = std::make_shared<FaultEventStore>(host.events);
    hidden->hide_tasks = true;
    MIRA_CHECK(host.start(hidden));
    auto environment = std::make_shared<ProbeEnvironment>();
    const auto missing =
        host.coordinator().recover_work(principal, source, environment, recovery_context());
    MIRA_CHECK(!missing && missing.error().code == ErrorCode::DataLoss &&
               environment->observations == 0);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(lifecycle_and_stale_permits() == 0);
    MIRA_CHECK(close_interrupts_observation() == 0);
    MIRA_CHECK(planner_checks_cancellation_and_missing_history() == 0);
    return 0;
}
