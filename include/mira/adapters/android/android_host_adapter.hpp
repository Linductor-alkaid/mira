#pragma once

#include <mira/adapters/android/host_dispatcher.hpp>
#include <mira/artifact_store.hpp>
#include <mira/environment.hpp>

#include <cstdint>
#include <memory>

namespace executor {
class Executor;
} // namespace executor

namespace mira::adapters::android {

// Native Android Adapter skeleton for M2. It demonstrates and contract-tests
// the frozen host boundary: Executor-managed submissions, callback
// settlement through HostDispatcherBridge, exactly-once lease release and
// epoch invalidation. The capability snapshot honestly declares only what
// the skeleton implements (screen capture and discrete input); UI trees,
// foreground and device state arrive with later milestones.
class AndroidHostAdapter final : public IEnvironment {
  public:
    // Creates the host through the frozen ABI with the bridge callbacks,
    // starts it and records the initial capability snapshot.
    [[nodiscard]] static Result<std::unique_ptr<AndroidHostAdapter>>
    create(executor::Executor &executor);
    ~AndroidHostAdapter() override;

    AndroidHostAdapter(const AndroidHostAdapter &) = delete;
    AndroidHostAdapter &operator=(const AndroidHostAdapter &) = delete;

    EnvironmentCapabilities capabilities() const override;
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override;
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override;
    Result<void> interrupt(const OperationContext &context) override;

    [[nodiscard]] HostBridgeStats bridge_stats() const;
    [[nodiscard]] EnvironmentEpoch environment_epoch() const;
    [[nodiscard]] HostDispatcherBridge &bridge();
    [[nodiscard]] MiraAndroidHostV1 *host() const noexcept { return host_; }

  private:
    AndroidHostAdapter(executor::Executor &executor, MiraAndroidHostV1 *host);

    executor::Executor &executor_;
    HostDispatcherBridge bridge_;
    MiraAndroidHostV1 *host_ = nullptr;
    MemoryArtifactStore artifacts_{8ULL * 1024ULL * 1024ULL};
};

} // namespace mira::adapters::android
