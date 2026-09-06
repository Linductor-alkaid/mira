#pragma once

#include <mira/adapters/android/host_dispatcher.hpp>
#include <mira/artifact_store.hpp>
#include <mira/environment.hpp>
#include <mira/observation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace executor {
class Executor;
} // namespace executor

namespace mira::adapters::android {

// Tunables for AndroidHostAdapter::create. The defaults keep the previous
// create(executor) behaviour except for the artifact capacity, which was
// raised from 8 MiB because real-device frames are several MiB each and the
// old ceiling exhausted on the second capture (see DEC-012 and GitHub #10).
struct AndroidHostAdapterOptions final {
    // Observation payload store. When null the adapter owns a
    // MemoryArtifactStore sized by memory_artifact_capacity_bytes. Injected
    // stores (e.g. FileArtifactStore) must outlive the adapter.
    std::shared_ptr<IArtifactStore> artifact_store;
    std::size_t memory_artifact_capacity_bytes = 64ULL * 1024ULL * 1024ULL;
};

// Native Android Adapter for the frozen host boundary: Executor-managed
// submissions, callback settlement through HostDispatcherBridge,
// exactly-once lease release and epoch invalidation. The capability
// snapshot honestly mirrors the host declaration: screen capture and
// discrete input from the input/format masks, UI trees when the host
// declares accessibility_completeness >= 1. Foreground and device state
// arrive with later milestones.
class AndroidHostAdapter final : public IEnvironment {
  public:
    // Creates the host through the frozen ABI with the bridge callbacks,
    // starts it and records the initial capability snapshot.
    [[nodiscard]] static Result<std::unique_ptr<AndroidHostAdapter>>
    create(executor::Executor &executor, const AndroidHostAdapterOptions &options = {});
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
    // Read/write access to the observation payload store. Hosts using the
    // adapter-owned default store read frame payloads (e.g. to transcode
    // RGBA captures into image/* wire formats) through this handle; hosts
    // that injected their own store already hold it (DEC-013).
    [[nodiscard]] IArtifactStore &artifact_store() const { return *artifacts_; }
    [[nodiscard]] MiraAndroidHostV1 *host() const noexcept { return host_; }

  private:
    AndroidHostAdapter(executor::Executor &executor, MiraAndroidHostV1 *host,
                       std::shared_ptr<IArtifactStore> artifacts);

    Result<ObservationComponent<ScreenFrameDescriptor>>
    capture_screen_component(const MiraHostTopologyV1 &topology,
                             const MiraHostCapabilitiesV1 &snapshot,
                             const OperationContext &context);
    Result<ObservationComponent<UiTreeSnapshot>>
    capture_structure_component(const MiraHostTopologyV1 &topology,
                                const MiraHostCapabilitiesV1 &snapshot,
                                const OperationContext &context);

    HostDispatcherBridge bridge_;
    MiraAndroidHostV1 *host_ = nullptr;
    std::shared_ptr<IArtifactStore> artifacts_;
};

} // namespace mira::adapters::android
