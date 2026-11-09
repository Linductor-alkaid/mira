// Offline M29 host composition: durable input/working projections, a frozen
// clarification, file reopen, read-only journal recovery, and a linked new
// task. The native file is a controlled external-result fixture. No input is
// ever dispatched to the simulator and no model or network is used.
#include <kairo/executor.hpp>
#include <mira/action_journal.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/interaction_coordinator.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace mira;
using namespace std::chrono_literals;

class PreviewSink final : public IUserInputSink {
  public:
    Result<void> deliver(const InputEnvelope &input, TaskId) override {
        received.push_back(input);
        return {};
    }
    std::vector<InputEnvelope> received;
};

class Host final {
  public:
    Host(kairo::Executor &executor, const std::filesystem::path &root,
         std::shared_ptr<IEnvironment> environment)
        : executor_(executor), events(std::make_shared<FileEventStore>(root / "events")),
          artifacts(std::make_shared<FileArtifactStore>(root / "artifacts")),
          checkpoints(std::make_shared<MemoryCheckpointStore>()),
          environment_(std::move(environment)) {}
    ~Host() {
        if (coordinator)
            (void)coordinator->close();
        if (initialized_) {
            const auto stopped = runtime.request_shutdown();
            if (stopped)
                (void)stopped.value().outcome(5s);
            (void)runtime.finish_shutdown();
        }
    }
    bool start() {
        const auto recovered = events->recover({});
        if (!recovered || recovered.value().status != RecoveryStatus::Clean ||
            !runtime.initialize())
            return false;
        initialized_ = true;
        coordinator =
            std::make_unique<InteractionCoordinator>(CoordinatorConfig{}, runtime, executor_);
        coordinator->set_event_store(events, runtime_id);
        coordinator->set_recovery_stores(artifacts, checkpoints);
        coordinator->set_environment_provider(
            [this]() -> Result<std::shared_ptr<IEnvironment>> { return environment_; });
        return coordinator->initialize().has_value();
    }
    kairo::Executor &executor_;
    MiraRuntime runtime;
    std::shared_ptr<FileEventStore> events;
    std::shared_ptr<FileArtifactStore> artifacts;
    std::shared_ptr<MemoryCheckpointStore> checkpoints;
    std::unique_ptr<InteractionCoordinator> coordinator;
    RuntimeId runtime_id = RuntimeId::generate();

  private:
    std::shared_ptr<IEnvironment> environment_;
    bool initialized_ = false;
};

InputEnvelope input(const PrincipalContext &principal, std::string id, std::string text,
                    std::optional<TaskId> target = {}) {
    InputEnvelope envelope;
    envelope.principal = principal;
    envelope.input_id = std::move(id);
    envelope.content = std::move(text);
    envelope.target_task = target;
    return envelope;
}

bool run(kairo::Executor &executor, const std::filesystem::path &root) {
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>();
    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.host_id = HostInstanceId::generate();
    principal.identity_version = 1;
    CapabilityGrant grant;
    grant.capability = "mira.interaction.input";
    grant.source = GrantSource::Host;
    grant.version = 1;
    principal.grants.push_back(grant);
    TaskId source;
    {
        Host host(executor, root, environment);
        if (!host.start())
            return false;
        auto goal = input(principal, "report", "prepare the saved report");
        goal.requested_intent = RouteIntent::NewTask;
        const auto first = host.coordinator->submit_input(goal);
        goal.input_id = "comparison";
        goal.content = "compare the report figures";
        if (!first || !first.value().task_id || !host.coordinator->submit_input(goal))
            return false;
        source = *first.value().task_id;
        auto loop = std::make_shared<PreviewSink>();
        if (!host.coordinator->register_sink(source, loop))
            return false;
        const auto question = host.coordinator->submit_input(
            input(principal, "appendix", "include the original appendix"));
        if (!question || !question.value().clarification)
            return false;
        auto answer = input(principal, "answer", "the saved report", source);
        answer.clarification_for = "appendix";
        const auto clarified = host.coordinator->submit_input(answer);
        if (!clarified || loop->received.size() != 2 ||
            loop->received.back().content.find("include the original appendix") ==
                std::string::npos)
            return false;
        // Fault fixture: the durable dispatch prefix survived but its external
        // result was lost. No simulator execute() occurs in this composition.
        ActionIntent intent{ActionId::generate(),
                            host.runtime_id,
                            *first.value().session_id,
                            source,
                            0,
                            environment->environment_epoch(),
                            digest_string("save report"),
                            {"fixture", "report", "save"}};
        ActionJournal journal(host.runtime_id, *host.events);
        if (!journal.prepare(intent) || !journal.dispatch_started(intent) ||
            !journal.execution_uncertain(intent, "fixture lost receipt"))
            return false;
        std::ofstream result(root / "saved-report.txt");
        result << "saved and verified\n";
        result.close();
        if (!result || !host.coordinator->close())
            return false;
    }
    {
        Host host(executor, root, environment);
        if (!host.start() || host.runtime.task_snapshot(source))
            return false;
        const auto page = host.coordinator->replay_inputs(principal, 0, 8);
        if (!page || page.value().entries.size() != 4)
            return false;
        auto context = make_control_context();
        context.deadline = std::chrono::steady_clock::now() + 5s;
        auto observe = executor.submit_auto([&] {
            return host.coordinator->recover_work(principal, source, environment, context);
        });
        auto observed = observe.get();
        if (!observed || observed.value().ready_for_new_task ||
            observed.value().recovery.pending_side_effects.empty())
            return false;
        auto verify = executor.submit_auto([&] {
            return host.coordinator->recover_work(
                principal, source, environment, context,
                [&](const WorkRecoveryReport &, const Observation &,
                    const OperationContext &) -> Result<bool> {
                    std::ifstream report(root / "saved-report.txt");
                    std::string line;
                    return std::getline(report, line) && line == "saved and verified";
                });
        });
        const auto verified = verify.get();
        if (!verified || !verified.value().ready_for_new_task)
            return false;
        const auto next = host.coordinator->submit_input(input(
            principal, "resume", "inspect the existing report; do not create a duplicate", source));
        if (!next || !next.value().task_id || next.value().task_id == source)
            return false;
        auto loop = std::make_shared<PreviewSink>();
        const auto candidates =
            host.coordinator->recovered_context_items(principal, *next.value().task_id);
        if (!candidates || candidates.value().size() != 1 || candidates.value().front().pinned ||
            candidates.value().front().authority != ContextAuthority::UntrustedExternalData ||
            !host.coordinator->register_sink(*next.value().task_id, loop) ||
            loop->received.size() != 1 || !environment->executed_inputs().empty() ||
            !host.coordinator->close())
            return false;
        std::cout << "interaction recovery: clarification preserved, read-only replay, "
                     "Observe/Verify consumed, linked new task; native input calls: 0\n";
    }
    return true;
}
} // namespace

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    if (!executor.initialize(config))
        return 1;
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-interaction-recovery-" + Id128::generate().to_string());
    std::filesystem::create_directories(root);
    bool ok = false;
    try {
        ok = run(executor, root);
    } catch (...) {
        std::cerr << "interaction recovery fixture failed\n";
    }
    const auto stopped = executor.shutdown(true);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return ok && stopped == kairo::ShutdownResult::Completed ? 0 : 1;
}
