#pragma once
#include "m29_support.hpp"
#include <filesystem>
#include <mira/action_journal.hpp>

namespace mira::m29 {
struct RecoveryRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("mira-m29-recovery-" + Id128::generate().to_string());
    ~RecoveryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

class RecoveryHost {
  public:
    explicit RecoveryHost(const std::filesystem::path &root, CoordinatorConfig config = {})
        : core(config), events(std::make_shared<FileEventStore>(root / "events")),
          artifacts(std::make_shared<FileArtifactStore>(root / "artifacts")),
          checkpoints(std::make_shared<MemoryCheckpointStore>()) {}
    Result<void> start(std::shared_ptr<IEventStore> adapter = {}) {
        const auto opened = events->recover({});
        if (!opened || opened.value().status != RecoveryStatus::Clean)
            return interaction_error(ErrorCode::DataLoss,
                                     InteractionDomainCode::RecoveryUnavailable,
                                     "fixture store failed recovery");
        core.coordinator_ =
            std::make_unique<InteractionCoordinator>(core.config_, *core.runtime_, core.executor_);
        core.coordinator_->set_event_store(adapter ? std::move(adapter) : events, runtime);
        core.coordinator_->set_recovery_stores(artifacts, checkpoints);
        core.coordinator_->set_environment_provider(
            [this]() -> Result<std::shared_ptr<IEnvironment>> {
                return std::shared_ptr<IEnvironment>{core.environment_};
            });
        return core.coordinator_->initialize();
    }
    InteractionCoordinator &coordinator() { return *core.coordinator_; }
    InputEnvelope input(std::string id, std::string content, const PrincipalContext &principal,
                        std::optional<TaskId> task = {}) {
        auto value = core.envelope(std::move(id), std::move(content), principal);
        value.target_task = task;
        return value;
    }
    Result<InputDisposition> create(std::string id, std::string content,
                                    const PrincipalContext &principal) {
        auto envelope = input(std::move(id), std::move(content), principal);
        envelope.requested_intent = RouteIntent::NewTask;
        return coordinator().submit_input(envelope);
    }
    m28::CoordinatorFixture core;
    std::shared_ptr<FileEventStore> events;
    std::shared_ptr<FileArtifactStore> artifacts;
    std::shared_ptr<MemoryCheckpointStore> checkpoints;
    RuntimeId runtime = RuntimeId::generate();
};

class FaultEventStore final : public IEventStore {
  public:
    explicit FaultEventStore(std::shared_ptr<IEventStore> inner) : inner_(std::move(inner)) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (before_append)
            before_append(request);
        if (remaining > 0 && fail_when && fail_when(request)) {
            --remaining;
            if (after_write) {
                const auto written = inner_->append(request);
                if (!written)
                    return written.error();
            }
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::LedgerUnavailable,
                                     "injected ambiguous append response");
        }
        return inner_->append(request);
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return inner_->append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override {
        ++reads;
        if (hide_tasks && query.session_id != interaction_ledger_partition())
            return EventPage{};
        auto page = inner_->read(query);
        if (on_read)
            on_read();
        return page;
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return inner_->recover(options);
    }
    Result<void> flush(Durability durability) override { return inner_->flush(durability); }
    std::function<bool(const AppendRequest &)> fail_when;
    std::function<void(const AppendRequest &)> before_append;
    std::function<void()> on_read;
    bool after_write = false;
    bool hide_tasks = false;
    int remaining = 1;
    mutable std::size_t reads = 0;

  private:
    std::shared_ptr<IEventStore> inner_;
};

inline bool routed_with(const AppendRequest &request, std::string_view delivery,
                        std::string_view input = {}) {
    if (request.payload.type != "InputRouted")
        return false;
    const auto json = parse_json(request.payload.data);
    if (!json)
        return false;
    const auto *state = json.value().find("delivery");
    const auto *id = json.value().find("input_id");
    return state && state->is_string() && *state->as_string() == delivery &&
           (input.empty() || (id && id->is_string() && *id->as_string() == input));
}

inline OperationContext recovery_context() {
    auto context = make_control_context();
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    return context;
}

inline Result<void> seed_uncertain(RecoveryHost &host, TaskId task, SessionId session,
                                   bool completed = false) {
    ActionIntent intent{ActionId::generate(),
                        host.runtime,
                        session,
                        task,
                        0,
                        1,
                        digest_string("fixture-input"),
                        {"fixture", "primary", "test"}};
    ActionJournal journal(host.runtime, *host.events);
    if (auto prepared = journal.prepare(intent); !prepared)
        return prepared.error();
    if (auto started = journal.dispatch_started(intent); !started)
        return started.error();
    if (completed) {
        ExecutionReceipt receipt;
        receipt.status = ExecutionStatus::Completed;
        receipt.environment_epoch = 1;
        const auto saved = journal.receipt(intent, receipt);
        if (!saved)
            return saved.error();
    } else {
        const auto saved = journal.execution_uncertain(intent, "fixture uncertainty");
        if (!saved)
            return saved.error();
    }
    return {};
}
} // namespace mira::m29
