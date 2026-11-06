#include "support/test.hpp"
#include <mira/action_dispatcher.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using namespace mira;
struct Fixture final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("mira-durability-" + SessionId::generate().to_string());
    AppendRequest request{EventId::generate(),
                          RuntimeId::generate(),
                          SessionId::generate(),
                          TaskId::generate(),
                          {"Fixture", "data"}};
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string bytes() const {
        std::ifstream file(root / "events.log", std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
};

int actual_grades_and_rejection() {
    Fixture f;
    MemoryEventStore memory;
    for (auto grade : {Durability::Buffered, Durability::ProcessCrash, Durability::PowerLoss}) {
        f.request.required = grade;
        auto ack = memory.append(f.request);
        MIRA_CHECK(ack && ack.value().achieved == Durability::Buffered);
    }
    MIRA_CHECK(memory.size() == 1 && memory.flush(Durability::Buffered));
    MIRA_CHECK(!memory.flush(Durability::ProcessCrash));
    MIRA_CHECK(!memory.flush(Durability::PowerLoss));
    auto invalid = f.request;
    invalid.required = static_cast<Durability>(255);
    MIRA_CHECK(!memory.append(invalid) && memory.size() == 1);
    MIRA_CHECK(!memory.flush(invalid.required));

    FileEventStore file(f.root);
    auto reject = file.append(f.request);
    MIRA_CHECK(!reject && reject.error().code == ErrorCode::UnsupportedCapability);
    MIRA_CHECK(!file.flush(Durability::PowerLoss));
    MIRA_CHECK(!file.append(invalid) && !file.flush(invalid.required));
    MIRA_CHECK(!std::filesystem::exists(f.root / "events.log"));
    f.request.required = Durability::Buffered;
    auto first = file.append(f.request);
    MIRA_CHECK(first && first.value().achieved == Durability::ProcessCrash);
    const auto before = f.bytes();
    f.request.required = Durability::ProcessCrash;
    auto duplicate = file.append(f.request);
    MIRA_CHECK(duplicate && duplicate.value().achieved == Durability::ProcessCrash);
    f.request.required = Durability::PowerLoss;
    MIRA_CHECK(!file.append(f.request));
    MIRA_CHECK(file.flush(Durability::Buffered) && file.flush(Durability::ProcessCrash));
    MIRA_CHECK(f.bytes() == before);
    FileEventStore reopened(f.root);
    f.request.required = Durability::Buffered;
    auto recovered = reopened.append(f.request);
    MIRA_CHECK(recovered && recovered.value().achieved == Durability::ProcessCrash);
    MIRA_CHECK(f.bytes() == before);
    return 0;
}

int read_only_flush_preserves_file() {
    Fixture f;
    std::filesystem::create_directories(f.root);
    {
        std::ofstream output(f.root / "events.log");
        output << "MIRA_EVENT_LOG 999\nMIRA_EVENT_RECORDS\n";
    }
    const auto before = f.bytes();
    FileEventStore file(f.root);
    MIRA_CHECK(!file.flush(Durability::ProcessCrash));
    MIRA_CHECK(file.read_only() && f.bytes() == before);
    // Even a missing backing file cannot be recreated by a read-only instance.
    std::filesystem::remove(f.root / "events.log");
    MIRA_CHECK(!file.flush(Durability::Buffered));
    MIRA_CHECK(!std::filesystem::exists(f.root / "events.log"));
    return 0;
}

class Environment final : public IEnvironment {
  public:
    int inputs = 0;
    EnvironmentCapabilities capabilities() const override { return {}; }
    Result<Observation> observe(const ObservationRequest &, const OperationContext &) override {
        return Observation{};
    }
    Result<ExecutionReceipt> execute(const InputSequence &, const OperationContext &) override {
        ++inputs;
        return ExecutionReceipt{};
    }
    Result<void> interrupt(const OperationContext &) override { return {}; }
};
class Lease final : public IActionDispatchLease {
  public:
    Result<void> validate(const ActionIntent &, const OperationContext &) override { return {}; }
};
int memory_store_cannot_authorize_input() {
    Fixture f;
    auto memory = std::make_shared<MemoryEventStore>();
    Environment environment;
    JournaledActionDispatcher dispatcher(
        f.request.runtime_id, f.request.session_id, memory,
        [](const ActionIntent &,
           const OperationContext &) -> Result<std::unique_ptr<IActionDispatchLease>> {
            return std::unique_ptr<IActionDispatchLease>(std::make_unique<Lease>());
        });
    OperationContext context;
    context.session = f.request.session_id;
    context.task = *f.request.task_id;
    context.task_epoch = 1;
    context.operation = OperationId::generate();
    InputSequence input;
    input.events.push_back({"back", "", 0});
    auto result = dispatcher.dispatch(environment, input, context, 1);
    MIRA_CHECK(!result && result.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(environment.inputs == 0);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(actual_grades_and_rejection() == 0);
    MIRA_CHECK(read_only_flush_preserves_file() == 0);
    MIRA_CHECK(memory_store_cannot_authorize_input() == 0);
    return 0;
}
