#pragma once

#include <mira/adapters/android/host_abi.h>
#include <mira/core_contracts.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira::adapters::android {

struct HostBridgeStats final {
    std::uint64_t operations_submitted = 0;
    std::uint64_t operations_settled = 0;
    std::uint64_t duplicate_terminal_callbacks = 0;
    std::uint64_t unknown_operation_callbacks = 0;
    std::uint64_t late_callbacks_after_detach = 0;
    std::uint64_t wrong_generation_results = 0;
    std::uint64_t executor_submission_rejections = 0;
    // Counts every lease release executed against the host, whether the
    // bridge performs it internally (error settlement, tree payload copy)
    // or a consumer releases a successfully delivered HostLeaseGuard later
    // (e.g. the adapter's observe tail). Abandoned guards releasing after
    // bridge destruction keep counting through the shared counter, so this
    // number never under-reports host-side releases.
    std::uint64_t leases_released = 0;
    std::uint64_t contract_violations = 0;
};

// Owns the exactly-once release of one host buffer lease. Release is
// idempotent and runs from the destructor if it was not requested earlier.
class HostLeaseGuard final {
  public:
    // Invoked exactly once, right after the real host release call; used to
    // keep bridge lease accounting accurate for consumer-side releases. Must
    // not throw; exceptions are swallowed to preserve release() noexcept.
    using ReleaseObserver = std::function<void()>;

    HostLeaseGuard() = default;
    explicit HostLeaseGuard(MiraHostBufferLeaseV1 lease);
    ~HostLeaseGuard();

    HostLeaseGuard(const HostLeaseGuard &) = delete;
    HostLeaseGuard &operator=(const HostLeaseGuard &) = delete;
    HostLeaseGuard(HostLeaseGuard &&other) noexcept;
    HostLeaseGuard &operator=(HostLeaseGuard &&other) noexcept;

    void release() noexcept;
    void set_release_observer(ReleaseObserver observer);
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const MiraHostBufferLeaseV1 &lease() const noexcept { return lease_; }

  private:
    MiraHostBufferLeaseV1 lease_{};
    std::atomic<bool> released_{false};
    bool valid_ = false;
    ReleaseObserver release_observer_;
};

struct HostFrameOutcome final {
    MiraHostStatus status = MIRA_HOST_OK;
    std::uint64_t frame_id = 0;
    std::uint64_t display_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_format = 0;
    std::uint32_t rotation = 0;
    std::uint64_t capture_begin_ns = 0;
    std::uint64_t capture_end_ns = 0;
    std::uint64_t environment_epoch = 0;
    HostLeaseGuard lease;
};

struct HostTreeOutcome final {
    MiraHostStatus status = MIRA_HOST_OK;
    std::uint64_t environment_epoch = 0;
    std::vector<std::byte> bytes;
};

struct HostInputOutcome final {
    MiraHostStatus status = MIRA_HOST_OK;
    std::uint32_t receipt = 0; // MiraHostInputReceipt
    std::uint32_t side_effect_may_have_occurred = 0;
    std::uint64_t environment_epoch = 0;
};

// Bridges the frozen host ABI onto Executor-managed futures. Host callbacks
// only validate, copy the bounded result struct and submit a completion
// task; conversion and lease handling happen on the Executor. The bridge
// does not own the host or the Executor; both outlive it or are shut down
// after detach by the embedder.
class HostDispatcherBridge final {
  public:
    explicit HostDispatcherBridge(executor::Executor &executor);
    ~HostDispatcherBridge();

    HostDispatcherBridge(const HostDispatcherBridge &) = delete;
    HostDispatcherBridge &operator=(const HostDispatcherBridge &) = delete;

    // Callback table to pass to mira_android_host_create_v1. The bridge is
    // the user_data; keep it alive until after the host is destroyed.
    [[nodiscard]] const MiraHostCallbacksV1 *host_callbacks();

    [[nodiscard]] HostBridgeStats stats() const;
    [[nodiscard]] std::optional<MiraHostCapabilitiesV1> latest_capabilities() const;
    void note_capabilities(const MiraHostCapabilitiesV1 &capabilities);

    std::future<Result<HostFrameOutcome>> capture_frame(MiraAndroidHostV1 *host,
                                                        const MiraHostFrameRequestV1 &request);
    std::future<Result<HostTreeOutcome>> get_ui_tree(MiraAndroidHostV1 *host,
                                                     const MiraHostTreeRequestV1 &request);
    std::future<Result<HostInputOutcome>> dispatch_input(MiraAndroidHostV1 *host,
                                                         const MiraHostInputRequestV1 &request);

    // Best-effort cooperative cancellation of all outstanding operations.
    // Settled operations are a successful no-op on the host side.
    Result<std::size_t> cancel_outstanding(MiraAndroidHostV1 *host);

    [[nodiscard]] std::size_t outstanding_operations() const;

  private:
    struct RegistryEntry final {
        std::uint32_t kind = 0;
        std::function<void(const MiraHostOperationResultV1 &)> fulfill;
    };

    void handle_operation_complete(const MiraHostOperationResultV1 &result);
    void handle_capabilities_changed(const MiraHostCapabilitiesV1 &capabilities);
    void record_violation();
    void settle_rejected(std::uint64_t correlation, std::uint32_t kind, MiraHostStatus status);
    // Notifier handed to delivered lease guards; shares ownership of the
    // release counter so it stays valid even after the bridge is gone.
    [[nodiscard]] HostLeaseGuard::ReleaseObserver lease_release_observer();

    executor::Executor &executor_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, RegistryEntry> registry_;
    std::atomic<std::uint64_t> next_correlation_{0};
    std::deque<std::future<void>> completion_futures_;
    MiraHostCallbacksV1 callbacks_table_{};
    HostBridgeStats stats_;
    // Shared with delivered guards: a guard released after bridge
    // destruction (abandoned future shared state) must not touch the bridge.
    std::shared_ptr<std::atomic<std::uint64_t>> lease_releases_{
        std::make_shared<std::atomic<std::uint64_t>>(0)};
    std::optional<MiraHostCapabilitiesV1> latest_capabilities_;
    std::optional<std::uint64_t> expected_generation_;
    bool detached_ = false;
};

} // namespace mira::adapters::android
