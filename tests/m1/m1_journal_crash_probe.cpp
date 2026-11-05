// CTest owns the subprocess lifecycle. _Exit deliberately bypasses all C++
// destructors; no Executor/background work is created by this crash probe.
#include <mira/action_dispatcher.hpp>
#include <mira/task_recovery.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using namespace mira;
constexpr RuntimeId runtime{Id128{Id128::Bytes{1}}};
constexpr SessionId session{Id128{Id128::Bytes{2}}};
constexpr TaskId task{Id128{Id128::Bytes{3}}};

class CrashStore final : public IEventStore {
  public:
    FileEventStore backing;
    std::string stage;
    CrashStore(const std::filesystem::path &root, std::string point)
        : backing(root / "events"), stage(std::move(point)) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        const auto &type = request.payload.type;
        if ((stage == "before_prepare" && type == "ActionPrepared") ||
            (stage == "before_start" && type == "ActionDispatchStarted") ||
            (stage == "before_receipt" && type == "ActionReceipt")) {
            std::_Exit(0);
        }
        auto result = backing.append(request);
        if (result && ((stage == "after_prepare" && type == "ActionPrepared") ||
                       (stage == "after_start" && type == "ActionDispatchStarted") ||
                       (stage == "after_receipt" && type == "ActionReceipt") ||
                       (stage == "after_uncertain" && type == "ActionExecutionUncertain"))) {
            std::_Exit(0);
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

int count(const std::filesystem::path &root) {
    int result = 0;
    std::ifstream file(root / "input-count");
    if (file) {
        file >> result;
    }
    return result;
}

class ProbeEnvironment final : public IEnvironment {
  public:
    std::filesystem::path root;
    std::string stage;
    EnvironmentCapabilities capabilities() const override { return {}; }
    Result<Observation> observe(const ObservationRequest &, const OperationContext &) override {
        return Observation{};
    }
    Result<ExecutionReceipt> execute(const InputSequence &, const OperationContext &) override {
        const int next = count(root) + 1;
        std::ofstream file(root / "input-count", std::ios::trunc);
        file << next;
        file.flush();
        if (!file) {
            std::_Exit(96);
        }
        if (stage == "after_execute") {
            std::_Exit(0);
        }
        return ExecutionReceipt{stage == "after_uncertain" ? ExecutionStatus::Unknown
                                                           : ExecutionStatus::Completed,
                                stage == "after_uncertain", 7, "probe receipt"};
    }
    Result<void> interrupt(const OperationContext &) override { return {}; }
};

class ProbeLease final : public IActionDispatchLease {
  public:
    Result<void> validate(const ActionIntent &, const OperationContext &) override { return {}; }
};

int write(const std::filesystem::path &root, const std::string &stage) {
    std::filesystem::create_directories(root);
    auto store = std::make_shared<CrashStore>(root, stage);
    if (!store->recover({})) {
        return 1;
    }
    ProbeEnvironment environment;
    environment.root = root;
    environment.stage = stage;
    JournaledActionDispatcher dispatcher(
        runtime, session, store,
        [](const ActionIntent &,
           const OperationContext &) -> Result<std::unique_ptr<IActionDispatchLease>> {
            return std::unique_ptr<IActionDispatchLease>(std::make_unique<ProbeLease>());
        });
    OperationContext context;
    context.session = session;
    context.task = task;
    context.task_epoch = 2;
    context.operation = OperationId::generate();
    InputSequence input;
    input.events.push_back({"back", "", 0});
    auto result = dispatcher.dispatch(environment, input, context, 7);
    if (result && stage == "terminal") {
        for (const auto &payload :
             {EventPayload{"VerificationResult", "{\"verdict\":\"satisfied\"}"},
              EventPayload{"LoopSettled", "{\"outcome\":\"Completed\"}"}}) {
            if (!store->append({EventId::generate(), runtime, session, task, payload})) {
                return 1;
            }
        }
        std::_Exit(0);
    }
    return 97; // A requested crash boundary was never reached.
}

int verify(const std::filesystem::path &root, const std::string &stage) {
    FileEventStore store(root / "events");
    if (!store.recover({})) {
        return 1;
    }
    const bool executed = stage == "after_execute" || stage == "before_receipt" ||
                          stage == "after_receipt" || stage == "after_uncertain" ||
                          stage == "terminal";
    if (count(root) != (executed ? 1 : 0)) {
        return 2;
    }
    const auto expected = stage == "before_prepare" ? RecoveryAction::NoState
                          : (stage == "after_prepare" || stage == "before_start")
                              ? RecoveryAction::ResumeObserving
                          : stage == "after_uncertain" ? RecoveryAction::ResumeObserving
                          : stage == "terminal"        ? RecoveryAction::AlreadyTerminal
                                                       : RecoveryAction::ResumeVerifying;
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(store, checkpoints);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto recovered = planner.plan(task, session, Timestamp::now());
        if (!recovered || recovered.value().action != expected ||
            count(root) != (executed ? 1 : 0)) {
            return 3;
        }
        if (stage == "after_uncertain" && recovered.value().pending_side_effects.size() != 1) {
            return 4;
        }
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        return 1;
    }
    return std::string(argv[1]) == "write" ? write(argv[2], argv[3]) : verify(argv[2], argv[3]);
}
