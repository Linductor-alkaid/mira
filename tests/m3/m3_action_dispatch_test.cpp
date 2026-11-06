#include "support/test.hpp"

#include <mira/action_dispatcher.hpp>
#include <mira/task_recovery.hpp>

#include <functional>
#include <memory>
#include <stdexcept>

namespace {
using namespace mira;

Error failure(ErrorCode code) {
    Error error;
    error.code = code;
    error.domain = "mira.test";
    return error;
}

class Store final : public IEventStore {
  public:
    MemoryEventStore backing;
    std::string reject;
    std::string weak;
    std::function<void(const AppendRequest &)> after;
    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (request.payload.type == reject) {
            return failure(ErrorCode::ResourceExhausted);
        }
        auto result = backing.append(request);
        // Explicit protocol acknowledgement simulation, not storage durability.
        if (result)
            result.value().achieved = request.required;
        if (result && request.payload.type == weak) {
            result.value().achieved = Durability::Buffered;
        }
        if (after) {
            after(request);
        }
        return result;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return backing.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return backing.read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return backing.recover(options);
    }
    Result<void> flush(Durability durability) override { return backing.flush(durability); }
};

class Environment final : public IEnvironment {
  public:
    int calls = 0;
    int releases = 0;
    int held = 0;
    bool called_with_lease = false;
    bool raise = false;
    bool error = false;
    bool release_error = false;
    bool released_with_lease = false;
    ExecutionReceipt receipt{ExecutionStatus::Completed, false, 7, "sensitive-platform-message"};
    std::function<void()> on_execute;
    EnvironmentCapabilities capabilities() const override { return {}; }
    Result<Observation> observe(const ObservationRequest &, const OperationContext &) override {
        return failure(ErrorCode::UnsupportedCapability);
    }
    Result<ExecutionReceipt> execute(const InputSequence &, const OperationContext &) override {
        ++calls;
        called_with_lease = held == 1;
        if (on_execute) {
            on_execute();
        }
        if (raise) {
            throw std::runtime_error("secret exception");
        }
        if (error) {
            return failure(ErrorCode::Internal);
        }
        return receipt;
    }
    Result<void> interrupt(const OperationContext &) override {
        ++releases;
        released_with_lease = held == 1;
        if (release_error) {
            return failure(ErrorCode::Internal);
        }
        return {};
    }
};

struct Fixture final {
    RuntimeId runtime = RuntimeId::generate();
    SessionId session = SessionId::generate();
    std::shared_ptr<Store> store = std::make_shared<Store>();
    Environment environment;
    OperationContext context;
    InputSequence input;
    bool cancelled = false;
    bool admitted = true;
    bool null_lease = false;
    bool throw_acquire = false;
    bool throw_validate = false;
    std::uint64_t epoch = 7;
    int validations = 0;

    struct Lease final : IActionDispatchLease {
        Fixture &owner;
        explicit Lease(Fixture &value) : owner(value) { ++owner.environment.held; }
        ~Lease() override { --owner.environment.held; }
        Result<void> validate(const ActionIntent &intent,
                              const OperationContext &context) override {
            ++owner.validations;
            if (owner.throw_validate) {
                throw std::runtime_error("secret validation exception");
            }
            if (!owner.admitted || owner.epoch != intent.environment_epoch ||
                context.task_epoch != intent.task_epoch) {
                return failure(ErrorCode::InvalidState);
            }
            return {};
        }
    };

    Fixture() {
        context.session = session;
        context.task = TaskId::generate();
        context.operation = OperationId::generate();
        context.task_epoch = 3;
        context.started_at = Timestamp::now();
        context.cancellation_requested = [this] { return cancelled; };
        input.events.push_back({"type", "sensitive-typed-text", 0});
    }
    std::unique_ptr<JournaledActionDispatcher> dispatcher() {
        return std::make_unique<JournaledActionDispatcher>(
            runtime, session, store,
            [this](const ActionIntent &,
                   const OperationContext &) -> Result<std::unique_ptr<IActionDispatchLease>> {
                if (throw_acquire) {
                    throw std::runtime_error("secret acquisition exception");
                }
                if (!admitted) {
                    return failure(ErrorCode::InvalidState);
                }
                if (null_lease) {
                    return std::unique_ptr<IActionDispatchLease>{};
                }
                return std::unique_ptr<IActionDispatchLease>(std::make_unique<Lease>(*this));
            });
    }
    Result<RecoveryOutcome> recovery() {
        MemoryCheckpointStore checkpoints;
        RecoveryPlanner planner(*store, checkpoints);
        return planner.plan(context.task, session, Timestamp::now());
    }
};

int normal_dispatch_order_and_redaction() {
    Fixture fixture;
    std::vector<std::string> timeline;
    fixture.store->after = [&](const AppendRequest &request) {
        timeline.push_back(request.payload.type);
    };
    fixture.environment.on_execute = [&] { timeline.emplace_back("execute"); };
    auto dispatcher = fixture.dispatcher();
    auto result = dispatcher->dispatch(fixture.environment, fixture.input, fixture.context, 7);
    MIRA_CHECK(result && result.value().status == ExecutionStatus::Completed);
    MIRA_CHECK((timeline == std::vector<std::string>{"ActionPrepared", "ActionDispatchStarted",
                                                     "execute", "ActionReceipt"}));
    MIRA_CHECK(fixture.environment.calls == 1 && fixture.environment.called_with_lease);
    MIRA_CHECK(fixture.environment.held == 0 && fixture.validations == 1);
    auto page = fixture.store->read({fixture.session, std::nullopt, 100});
    MIRA_CHECK(page);
    for (const auto &event : page.value().events) {
        MIRA_CHECK(event.task_id == fixture.context.task);
        MIRA_CHECK(event.payload.data.find("sensitive") == std::string::npos);
    }
    return 0;
}

int pre_dispatch_failures_never_input() {
    for (int scenario = 0; scenario < 11; ++scenario) {
        Fixture fixture;
        switch (scenario) {
        case 0:
            fixture.store->reject = "ActionPrepared";
            break;
        case 1:
            fixture.store->weak = "ActionPrepared";
            break;
        case 2:
            fixture.store->reject = "ActionDispatchStarted";
            break;
        case 3:
            fixture.store->weak = "ActionDispatchStarted";
            break;
        case 4:
            fixture.admitted = false;
            break;
        case 5:
            fixture.null_lease = true;
            break;
        case 6:
            fixture.throw_acquire = true;
            break;
        case 7:
            fixture.epoch = 8;
            break;
        case 8:
            fixture.context.deadline = Timestamp::now().monotonic;
            break;
        case 9:
            fixture.cancelled = true;
            break;
        case 10:
            fixture.context.session = SessionId::generate();
            break;
        }
        auto dispatcher = fixture.dispatcher();
        auto result = dispatcher->dispatch(fixture.environment, fixture.input, fixture.context, 7);
        MIRA_CHECK(!result && fixture.environment.calls == 0 && fixture.environment.held == 0);
    }
    return 0;
}

int withdrawal_during_start_revalidates() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Fixture fixture;
        fixture.store->after = [&](const AppendRequest &request) {
            if (request.payload.type != "ActionDispatchStarted") {
                return;
            }
            if (scenario == 0) {
                fixture.admitted = false;
                fixture.cancelled = true;
                (void)fixture.environment.interrupt(fixture.context);
            } else if (scenario == 1) {
                ++fixture.epoch;
            } else if (scenario == 2) {
                fixture.context.deadline = Timestamp::now().monotonic;
            } else {
                fixture.throw_validate = true;
            }
        };
        auto dispatcher = fixture.dispatcher();
        auto result = dispatcher->dispatch(fixture.environment, fixture.input, fixture.context, 7);
        MIRA_CHECK(!result && fixture.environment.calls == 0 && fixture.environment.held == 0);
        auto recovery = fixture.recovery();
        MIRA_CHECK(recovery && recovery.value().checkpoint->verification_pending);
        MIRA_CHECK(recovery.value().action == RecoveryAction::ResumeVerifying ||
                   recovery.value().action == RecoveryAction::ResumeObserving);
    }
    return 0;
}

int execution_and_result_failures_keep_uncertainty() {
    for (int scenario = 0; scenario < 8; ++scenario) {
        Fixture fixture;
        switch (scenario) {
        case 0:
            fixture.environment.error = true;
            break;
        case 1:
            fixture.environment.raise = true;
            break;
        case 2:
            fixture.store->reject = "ActionReceipt";
            break;
        case 3:
            fixture.store->weak = "ActionReceipt";
            break;
        case 4:
            fixture.environment.receipt.status = ExecutionStatus::Unknown;
            break;
        case 5:
            fixture.environment.receipt.status = ExecutionStatus::Dispatched;
            break;
        case 6:
            fixture.environment.receipt.status = ExecutionStatus::Rejected;
            fixture.environment.receipt.side_effect_may_have_occurred = true;
            break;
        case 7:
            fixture.environment.receipt.status = ExecutionStatus::Dispatched;
            fixture.environment.release_error = true;
            break;
        }
        auto dispatcher = fixture.dispatcher();
        auto result = dispatcher->dispatch(fixture.environment, fixture.input, fixture.context, 7);
        MIRA_CHECK(result.has_value() == (scenario >= 4 && scenario != 7));
        MIRA_CHECK(fixture.environment.calls == 1 && fixture.environment.held == 0);
        if (scenario == 0 || scenario == 1 || scenario >= 4) {
            MIRA_CHECK(fixture.environment.releases == 1 &&
                       fixture.environment.released_with_lease);
        }
        auto recovery = fixture.recovery();
        MIRA_CHECK(recovery && recovery.value().action == RecoveryAction::ResumeObserving);
        MIRA_CHECK(recovery.value().pending_side_effects.size() == 1);
        MIRA_CHECK(fixture.environment.calls == 1);
    }
    return 0;
}
} // namespace

int main() {
    for (auto test :
         {normal_dispatch_order_and_redaction, pre_dispatch_failures_never_input,
          withdrawal_during_start_revalidates, execution_and_result_failures_keep_uncertainty}) {
        if (const int status = test(); status != 0) {
            return status;
        }
    }
    return 0;
}
