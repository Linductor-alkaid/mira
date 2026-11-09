#pragma once

#include <mira/subagent.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace mira {

namespace subagent_support {
inline Error error(ErrorCode code, std::string message) {
    Error result;
    result.code = code;
    result.domain = "mira.subagent";
    result.safe_message = std::move(message);
    return result;
}

class Worker final : public kairo::IBlockingIoWorker {
  public:
    Worker(std::function<void(kairo::StopToken)> run, std::function<void()> wake)
        : run_(std::move(run)), wake_(std::move(wake)) {}
    void run(kairo::StopToken stop) override { run_(stop); }
    void wakeup() noexcept override { wake_(); }

  private:
    std::function<void(kairo::StopToken)> run_;
    std::function<void()> wake_;
};
} // namespace subagent_support

struct SubagentCoordinator::State final : std::enable_shared_from_this<State> {
    struct Child final {
        SubagentSnapshot snapshot;
        std::shared_ptr<const SubagentProfile> profile;
        AgentLoopSpec spec;
        OperationContext context;
        std::string prompt;
        std::atomic<bool> cancelled{false};
        bool done = false;
        std::size_t verified_effects = 0;
        bool effects_uncertain = false;
        std::thread::id worker_id;
        std::optional<kairo::WorkerHandle> worker;
    };

    State(kairo::Executor &ex, AgentLoopSpec spec, std::shared_ptr<IEventStore> log,
          std::shared_ptr<IArtifactStore> store, RuntimeId id, SubagentConfig bounds)
        : executor(ex), parent(std::move(spec)), events(std::move(log)),
          artifacts(std::move(store)), runtime(id), config(bounds) {}

    kairo::Executor &executor;
    const AgentLoopSpec parent;
    std::shared_ptr<IEventStore> events;
    std::shared_ptr<IArtifactStore> artifacts;
    RuntimeId runtime;
    const SubagentConfig config;
    mutable std::mutex mutex;
    std::mutex close_mutex;
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<const SubagentProfile>> profiles;
    std::vector<std::shared_ptr<Child>> children;
    std::optional<kairo::WorkerHandle> parent_worker;
    OperationId parent_execution = OperationId::generate();
    std::thread::id parent_worker_id;
    bool started = false;
    bool profiles_sealed = false;
    bool accepting = false;
    bool parent_done = false;
    bool closed = false;
    std::atomic<bool> cancelled{false};
    std::uint64_t reserved_requests = 0;
    std::uint64_t reserved_inputs = 0;
    std::uint64_t reserved_outputs = 0;
    std::shared_future<Result<ConversationLoopResult>> parent_result;
    std::optional<Error> audit_error;

    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<void> emit(const char *type, const SubagentSnapshot *child = nullptr,
                                    const char *reason = nullptr);
    [[nodiscard]] Result<SubagentHandle> spawn(const std::string &profile, std::string prompt,
                                               const OperationContext &caller);
    [[nodiscard]] Result<SubagentSnapshot> get(const SubagentHandle &handle) const;
    [[nodiscard]] Result<void> cancel(const SubagentHandle &handle);
    void stop(const char *reason);
    void run_child(const std::shared_ptr<Child> &child, kairo::StopToken stop);
    void settle(const std::shared_ptr<Child> &child, SubagentStatus status, std::string answer,
                std::string summary);
    [[nodiscard]] Result<std::vector<std::string>>
    collect(const AgentLoopSpec &spec, const OperationContext &context, bool wait);
};
} // namespace mira
