#pragma once

#include <mira/adapters/android/host_abi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mira::test {

class FakeAndroidHost;

// Owning wrapper behind the opaque MiraAndroidHostV1 handle; defined here
// so from_abi_host() can recover the instance tests control. The instance
// is shared so tests can inspect counters after the handle is destroyed.
struct FakeAndroidHostHandle final {
    std::shared_ptr<FakeAndroidHost> owner;
};

// In-memory implementation of the frozen Android Host ABI used by the M2
// contract tests. The free C functions forward to the instance that owns
// the opaque host handle, so the adapter under test exercises the real ABI
// surface while tests keep direct control over delivery and faults.
class FakeAndroidHost final {
  public:
    struct Behaviour final {
        // Hold terminal callbacks until release_pending() is called.
        bool defer_callbacks = false;
        // Deliver a second terminal callback for the next operation.
        bool duplicate_next_callback = false;
        // Stamp the next operation result with a foreign host generation.
        bool wrong_generation_next_callback = false;
        // Announce a plane layout that escapes the leased buffer.
        bool oversize_next_lease = false;
        // Complete the next input operation with an uncertain receipt.
        bool uncertain_next_input = false;
    };

    FakeAndroidHost();
    ~FakeAndroidHost();

    FakeAndroidHost(const FakeAndroidHost &) = delete;
    FakeAndroidHost &operator=(const FakeAndroidHost &) = delete;

    // Recovers the instance behind an ABI handle created by the free C
    // functions; returns nullptr for unknown handles.
    [[nodiscard]] static FakeAndroidHost *from_abi_host(MiraAndroidHostV1 *host);

    // Shared ownership for tests that outlive the ABI handle.
    [[nodiscard]] static std::shared_ptr<FakeAndroidHost>
    shared_from_abi_host(MiraAndroidHostV1 *host);

    // ABI entry points; the free C functions forward here.
    [[nodiscard]] MiraHostStatus create(const MiraAndroidHostConfigV1 &config,
                                        const MiraHostCallbacksV1 &callbacks);
    [[nodiscard]] MiraHostStatus start();
    [[nodiscard]] MiraHostStatus stop();
    [[nodiscard]] MiraHostStatus destroy();
    [[nodiscard]] MiraHostStatus get_capabilities(MiraHostCapabilitiesV1 *out) const;
    [[nodiscard]] MiraHostStatus get_topology(MiraHostTopologyV1 *out) const;
    [[nodiscard]] MiraHostStatus capture_frame(const MiraHostFrameRequestV1 &request,
                                               std::uint64_t *out_operation);
    [[nodiscard]] MiraHostStatus get_ui_tree(const MiraHostTreeRequestV1 &request,
                                             std::uint64_t *out_operation);
    [[nodiscard]] MiraHostStatus dispatch_input(const MiraHostInputRequestV1 &request,
                                                std::uint64_t *out_operation);
    [[nodiscard]] MiraHostStatus cancel_operation(std::uint64_t operation);

    // --- test controls ------------------------------------------------------
    void set_behaviour(Behaviour behaviour);
    void rotate();                 // bumps epoch, topology version, capability event
    void revoke_projection();      // capability change: no screenshot formats
    void release_pending();        // delivers deferred terminal callbacks
    void force_capability_event(); // delivers the current snapshot again
    // Delivers a fabricated late callback through the stored table; used to
    // prove the bridge isolates callbacks for unknown operations.
    void deliver_raw_result(const MiraHostOperationResultV1 &result);

    [[nodiscard]] std::size_t outstanding_leases() const noexcept;
    [[nodiscard]] std::uint64_t environment_epoch() const noexcept;
    [[nodiscard]] std::uint64_t host_generation() const noexcept;
    [[nodiscard]] std::size_t stopped_count() const noexcept;
    [[nodiscard]] std::size_t destroyed_count() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] std::vector<std::vector<double>> dispatched_inputs() const;

  private:
    struct Operation final {
        std::uint64_t correlation = 0;
        std::uint32_t kind = 0;
        MiraHostOperationResultV1 result{};
        // Lease storage for frame/tree operations; result.lease points here
        // only while the callback is being delivered.
        MiraHostBufferLeaseV1 lease_storage{};
        bool duplicate_planned = false;
    };

    static void lease_released(std::uint64_t lease_id, void *user_data);
    [[nodiscard]] MiraHostBufferLeaseV1 make_lease(const std::vector<std::uint8_t> &payload,
                                                   bool oversize);
    void deliver_locked(const Operation &operation);
    void publish_capabilities_locked();
    [[nodiscard]] MiraHostCapabilitiesV1 capabilities_locked() const;

    mutable std::mutex mutex_;
    Behaviour behaviour_{};
    MiraHostCallbacksV1 callbacks_{};
    bool created_ = false;
    bool started_ = false;
    bool stopped_ = false;
    bool destroyed_ = false;
    bool revoked_ = false;
    std::size_t stopped_count_ = 0;
    std::size_t destroyed_count_ = 0;
    std::uint64_t environment_epoch_ = 1;
    std::uint64_t topology_version_ = 1;
    std::uint64_t host_sequence_ = 1;
    std::uint64_t host_generation_ = 1;
    std::uint64_t next_lease_id_ = 1;
    std::atomic<std::uint64_t> live_leases_{0};
    std::vector<Operation> pending_operations_;
    std::vector<std::vector<double>> dispatched_inputs_;
    std::vector<std::uint8_t> tree_payload_{'t', 'r', 'e', 'e', '-', 'v', '1'};
};

} // namespace mira::test
