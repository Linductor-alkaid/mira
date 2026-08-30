#include "support/fake_android_host.hpp"

#include <cstring>
#include <memory>
#include <utility>

namespace mira::test {
namespace {

constexpr std::uint32_t kFakeWidth = 4;
constexpr std::uint32_t kFakeHeight = 4;

std::vector<std::uint8_t> synthetic_frame() {
    std::vector<std::uint8_t> payload;
    payload.resize(static_cast<std::size_t>(kFakeWidth) * kFakeHeight * 4U);
    for (std::uint32_t y = 0; y < kFakeHeight; ++y) {
        for (std::uint32_t x = 0; x < kFakeWidth; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * kFakeWidth + x) * 4U;
            payload[offset + 0] = static_cast<std::uint8_t>(x * 16U);
            payload[offset + 1] = static_cast<std::uint8_t>(y * 16U);
            payload[offset + 2] = static_cast<std::uint8_t>(0x80U);
            payload[offset + 3] = 0xFFU;
        }
    }
    return payload;
}

} // namespace

// Lease payload record; freed by the exactly-once release trampoline.
struct FakeLeaseRecord final {
    std::shared_ptr<std::vector<std::uint8_t>> payload;
    std::atomic<std::uint64_t> *live_counter = nullptr;
};

FakeAndroidHost::FakeAndroidHost() = default;

FakeAndroidHost::~FakeAndroidHost() = default;

FakeAndroidHost *FakeAndroidHost::from_abi_host(MiraAndroidHostV1 *host) {
    if (host == nullptr) {
        return nullptr;
    }
    auto *handle = reinterpret_cast<FakeAndroidHostHandle *>(host);
    return handle->owner.get();
}

std::shared_ptr<FakeAndroidHost> FakeAndroidHost::shared_from_abi_host(MiraAndroidHostV1 *host) {
    if (host == nullptr) {
        return nullptr;
    }
    auto *handle = reinterpret_cast<FakeAndroidHostHandle *>(host);
    return handle->owner;
}

MiraHostStatus FakeAndroidHost::create(const MiraAndroidHostConfigV1 &config,
                                       const MiraHostCallbacksV1 &callbacks) {
    if (config.struct_size < sizeof(MiraAndroidHostConfigV1) ||
        callbacks.struct_size < sizeof(MiraHostCallbacksV1)) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    if (config.abi_version != MIRA_ANDROID_ABI_VERSION) {
        return MIRA_HOST_ERR_UNSUPPORTED_VERSION;
    }
    if (callbacks.on_operation_complete == nullptr) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (created_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    callbacks_ = callbacks;
    created_ = true;
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::start() {
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_ || stopped_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    started_ = true;
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::stop() {
    std::vector<Operation> cancel_now;
    MiraHostStatus status = MIRA_HOST_OK;
    {
        std::lock_guard lock(mutex_);
        if (!created_ || destroyed_) {
            return MIRA_HOST_ERR_INVALID_STATE;
        }
        stopped_ = true;
        ++stopped_count_;
        for (Operation &operation : pending_operations_) {
            operation.result.status = MIRA_HOST_ERR_CANCELLED;
            cancel_now.push_back(operation);
        }
        pending_operations_.clear();
        if (live_leases_.load() != 0) {
            // Memory native may still touch is not released; stop reports
            // the degraded state instead of forcing it.
            status = MIRA_HOST_ERR_EXECUTION_UNCERTAIN;
        }
    }
    for (const Operation &operation : cancel_now) {
        deliver_locked(operation);
    }
    return status;
}

MiraHostStatus FakeAndroidHost::destroy() {
    std::lock_guard lock(mutex_);
    if (destroyed_) {
        return MIRA_HOST_OK;
    }
    if (!created_ || !stopped_ || live_leases_.load() != 0 || !pending_operations_.empty()) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    destroyed_ = true;
    ++destroyed_count_;
    return MIRA_HOST_OK;
}

MiraHostCapabilitiesV1 FakeAndroidHost::capabilities_locked() const {
    MiraHostCapabilitiesV1 capabilities{};
    capabilities.struct_size = sizeof(MiraHostCapabilitiesV1);
    capabilities.abi_version = MIRA_ANDROID_ABI_VERSION;
    capabilities.screenshot_pixel_formats_mask = revoked_ ? 0U : (1U << MIRA_HOST_PIXEL_RGBA8888);
    capabilities.screenshot_backends = revoked_ ? 0U : 1U;
    capabilities.max_frame_width = 4096;
    capabilities.max_frame_height = 4096;
    capabilities.accessibility_completeness = 1;
    capabilities.supported_node_actions_mask = 0;
    capabilities.input_capabilities_mask =
        (1U << MIRA_HOST_INPUT_TAP) | (1U << MIRA_HOST_INPUT_LONG_PRESS) |
        (1U << MIRA_HOST_INPUT_SWIPE) | (1U << MIRA_HOST_INPUT_TYPE) |
        (1U << MIRA_HOST_INPUT_BACK) | (1U << MIRA_HOST_INPUT_HOME) |
        (1U << MIRA_HOST_INPUT_RELEASE_ALL);
    capabilities.max_gesture_duration_ms = 60'000;
    capabilities.max_pointers = 5;
    capabilities.callback_thread_model = 1; // caller thread
    capabilities.lifecycle_state = stopped_ ? 2U : (started_ ? 1U : 0U);
    capabilities.permission_state = revoked_ ? 1U : 0U;
    capabilities.secure_surface_policy = 0;
    capabilities.topology_version = topology_version_;
    capabilities.environment_epoch = environment_epoch_;
    capabilities.host_sequence = host_sequence_;
    capabilities.host_generation = host_generation_;
    return capabilities;
}

MiraHostStatus FakeAndroidHost::get_capabilities(MiraHostCapabilitiesV1 *out) const {
    if (out == nullptr) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    *out = capabilities_locked();
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::get_topology(MiraHostTopologyV1 *out) const {
    if (out == nullptr) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    std::memset(out, 0, sizeof(MiraHostTopologyV1));
    out->struct_size = sizeof(MiraHostTopologyV1);
    out->environment_epoch = environment_epoch_;
    out->topology_version = topology_version_;
    out->display_count = 1;
    out->displays[0].display_id = 1;
    out->displays[0].native_width = kFakeWidth;
    out->displays[0].native_height = kFakeHeight;
    out->displays[0].rotation = MIRA_HOST_ROTATION_0;
    out->displays[0].pixels_per_logical = 1.0;
    out->displays[0].active = 1;
    return MIRA_HOST_OK;
}

void FakeAndroidHost::lease_released(std::uint64_t /*lease_id*/, void *user_data) {
    auto *record = static_cast<FakeLeaseRecord *>(user_data);
    if (record->live_counter != nullptr) {
        record->live_counter->fetch_sub(1);
    }
    delete record;
}

MiraHostBufferLeaseV1 FakeAndroidHost::make_lease(const std::vector<std::uint8_t> &payload,
                                                  bool oversize) {
    auto record = std::make_unique<FakeLeaseRecord>();
    record->payload = std::make_shared<std::vector<std::uint8_t>>(payload);
    MiraHostBufferLeaseV1 lease{};
    lease.struct_size = sizeof(MiraHostBufferLeaseV1);
    lease.lease_id = next_lease_id_++;
    lease.data = record->payload->data();
    lease.size = record->payload->size();
    lease.plane_count = 1;
    lease.planes[0].offset = 0;
    lease.planes[0].row_stride = oversize ? 4096U : kFakeWidth * 4U;
    lease.planes[0].pixel_stride = 4;
    lease.planes[0].width = kFakeWidth;
    lease.planes[0].height = kFakeHeight;
    lease.release = &FakeAndroidHost::lease_released;
    record->live_counter = &live_leases_;
    lease.user_data = record.release();
    live_leases_.fetch_add(1);
    return lease;
}

void FakeAndroidHost::deliver_locked(const Operation &operation) {
    if (callbacks_.on_operation_complete == nullptr || destroyed_) {
        return;
    }
    // The delivered copy keeps result.lease pointing at its own storage so
    // vector reallocation never invalidates it during the callback.
    Operation local = operation;
    if (local.kind == MIRA_HOST_OP_CAPTURE_FRAME || local.kind == MIRA_HOST_OP_GET_UI_TREE) {
        local.result.lease = &local.lease_storage;
    } else {
        local.result.lease = nullptr;
    }
    callbacks_.on_operation_complete(callbacks_.user_data, &local.result);
    if (local.duplicate_planned) {
        callbacks_.on_operation_complete(callbacks_.user_data, &local.result);
    }
}

MiraHostStatus FakeAndroidHost::capture_frame(const MiraHostFrameRequestV1 &request,
                                              std::uint64_t *out_operation) {
    if (request.struct_size < sizeof(MiraHostFrameRequestV1)) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_ || !started_ || stopped_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    Operation operation;
    operation.correlation = request.correlation;
    operation.kind = MIRA_HOST_OP_CAPTURE_FRAME;
    operation.result.struct_size = sizeof(MiraHostOperationResultV1);
    operation.result.correlation = request.correlation;
    operation.result.host_generation =
        behaviour_.wrong_generation_next_callback ? host_generation_ + 999 : host_generation_;
    operation.result.status = MIRA_HOST_OK;
    operation.result.kind = MIRA_HOST_OP_CAPTURE_FRAME;
    operation.lease_storage = make_lease(synthetic_frame(), behaviour_.oversize_next_lease);
    operation.result.frame_id = request.correlation + 100;
    operation.result.display_id = 1;
    operation.result.width = kFakeWidth;
    operation.result.height = kFakeHeight;
    operation.result.pixel_format = MIRA_HOST_PIXEL_RGBA8888;
    operation.result.rotation = MIRA_HOST_ROTATION_0;
    operation.result.capture_begin_ns = 1'000;
    operation.result.capture_end_ns = 2'000;
    operation.result.environment_epoch = environment_epoch_;
    operation.duplicate_planned = behaviour_.duplicate_next_callback;
    behaviour_.duplicate_next_callback = false;
    behaviour_.wrong_generation_next_callback = false;
    behaviour_.oversize_next_lease = false;
    if (out_operation != nullptr) {
        *out_operation = request.correlation;
    }
    if (behaviour_.defer_callbacks) {
        pending_operations_.push_back(operation);
        return MIRA_HOST_OK;
    }
    deliver_locked(operation);
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::get_ui_tree(const MiraHostTreeRequestV1 &request,
                                            std::uint64_t *out_operation) {
    if (request.struct_size < sizeof(MiraHostTreeRequestV1)) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_ || !started_ || stopped_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    Operation operation;
    operation.correlation = request.correlation;
    operation.kind = MIRA_HOST_OP_GET_UI_TREE;
    operation.result.struct_size = sizeof(MiraHostOperationResultV1);
    operation.result.correlation = request.correlation;
    operation.result.host_generation = host_generation_;
    operation.result.status = MIRA_HOST_OK;
    operation.result.kind = MIRA_HOST_OP_GET_UI_TREE;
    operation.lease_storage = make_lease(tree_payload_, false);
    operation.result.environment_epoch = environment_epoch_;
    if (out_operation != nullptr) {
        *out_operation = request.correlation;
    }
    if (behaviour_.defer_callbacks) {
        pending_operations_.push_back(operation);
        return MIRA_HOST_OK;
    }
    deliver_locked(operation);
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::dispatch_input(const MiraHostInputRequestV1 &request,
                                               std::uint64_t *out_operation) {
    if (request.struct_size < sizeof(MiraHostInputRequestV1) ||
        request.event_count > MIRA_MAX_INPUT_EVENTS) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (!created_ || destroyed_ || !started_ || stopped_) {
        return MIRA_HOST_ERR_INVALID_STATE;
    }
    for (std::uint32_t index = 0; index < request.event_count; ++index) {
        dispatched_inputs_.push_back({request.events[index].x, request.events[index].y,
                                      request.events[index].x2, request.events[index].y2});
    }
    Operation operation;
    operation.correlation = request.correlation;
    operation.kind = MIRA_HOST_OP_DISPATCH_INPUT;
    operation.result.struct_size = sizeof(MiraHostOperationResultV1);
    operation.result.correlation = request.correlation;
    operation.result.host_generation = host_generation_;
    operation.result.status = MIRA_HOST_OK;
    operation.result.kind = MIRA_HOST_OP_DISPATCH_INPUT;
    operation.result.input_receipt = behaviour_.uncertain_next_input
                                         ? MIRA_HOST_INPUT_RECEIPT_UNKNOWN
                                         : MIRA_HOST_INPUT_RECEIPT_COMPLETED;
    operation.result.side_effect_may_have_occurred = behaviour_.uncertain_next_input ? 1U : 0U;
    operation.result.environment_epoch = environment_epoch_;
    behaviour_.uncertain_next_input = false;
    if (out_operation != nullptr) {
        *out_operation = request.correlation;
    }
    if (behaviour_.defer_callbacks) {
        pending_operations_.push_back(operation);
        return MIRA_HOST_OK;
    }
    deliver_locked(operation);
    return MIRA_HOST_OK;
}

MiraHostStatus FakeAndroidHost::cancel_operation(std::uint64_t operation_id) {
    std::vector<Operation> cancel_now;
    {
        std::lock_guard lock(mutex_);
        for (auto iter = pending_operations_.begin(); iter != pending_operations_.end();) {
            if (iter->correlation == operation_id) {
                iter->result.status = MIRA_HOST_ERR_CANCELLED;
                cancel_now.push_back(*iter);
                iter = pending_operations_.erase(iter);
            } else {
                ++iter;
            }
        }
    }
    for (const Operation &operation : cancel_now) {
        deliver_locked(operation);
    }
    return MIRA_HOST_OK;
}

void FakeAndroidHost::set_behaviour(Behaviour behaviour) {
    std::lock_guard lock(mutex_);
    behaviour_ = behaviour;
}

void FakeAndroidHost::publish_capabilities_locked() {
    if (callbacks_.on_capabilities_changed == nullptr) {
        return;
    }
    const MiraHostCapabilitiesV1 snapshot = capabilities_locked();
    callbacks_.on_capabilities_changed(callbacks_.user_data, &snapshot);
}

void FakeAndroidHost::rotate() {
    std::lock_guard lock(mutex_);
    ++environment_epoch_;
    ++topology_version_;
    ++host_sequence_;
    publish_capabilities_locked();
}

void FakeAndroidHost::revoke_projection() {
    std::lock_guard lock(mutex_);
    ++host_sequence_;
    revoked_ = true;
    publish_capabilities_locked();
}

void FakeAndroidHost::force_capability_event() {
    std::lock_guard lock(mutex_);
    ++host_sequence_;
    publish_capabilities_locked();
}

void FakeAndroidHost::release_pending() {
    std::vector<Operation> operations;
    {
        std::lock_guard lock(mutex_);
        operations.swap(pending_operations_);
    }
    for (const Operation &operation : operations) {
        deliver_locked(operation);
    }
}

void FakeAndroidHost::deliver_raw_result(const MiraHostOperationResultV1 &result) {
    std::lock_guard lock(mutex_);
    if (callbacks_.on_operation_complete == nullptr || destroyed_) {
        return;
    }
    callbacks_.on_operation_complete(callbacks_.user_data, &result);
}

std::size_t FakeAndroidHost::outstanding_leases() const noexcept {
    return static_cast<std::size_t>(live_leases_.load());
}

std::uint64_t FakeAndroidHost::environment_epoch() const noexcept {
    std::lock_guard lock(mutex_);
    return environment_epoch_;
}

std::uint64_t FakeAndroidHost::host_generation() const noexcept {
    std::lock_guard lock(mutex_);
    return host_generation_;
}

std::size_t FakeAndroidHost::stopped_count() const noexcept {
    std::lock_guard lock(mutex_);
    return stopped_count_;
}

std::size_t FakeAndroidHost::destroyed_count() const noexcept {
    std::lock_guard lock(mutex_);
    return destroyed_count_;
}

bool FakeAndroidHost::started() const noexcept {
    std::lock_guard lock(mutex_);
    return started_;
}

std::vector<std::vector<double>> FakeAndroidHost::dispatched_inputs() const {
    std::lock_guard lock(mutex_);
    return dispatched_inputs_;
}

} // namespace mira::test

// --- frozen C ABI trampolines ------------------------------------------------

extern "C" {

MiraHostStatus mira_android_host_create_v1(const MiraAndroidHostConfigV1 *config,
                                           const MiraHostCallbacksV1 *callbacks,
                                           MiraAndroidHostV1 **out_host) {
    if (config == nullptr || callbacks == nullptr || out_host == nullptr) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    auto handle = std::make_unique<mira::test::FakeAndroidHostHandle>();
    handle->owner = std::make_shared<mira::test::FakeAndroidHost>();
    const MiraHostStatus status = handle->owner->create(*config, *callbacks);
    if (status != MIRA_HOST_OK) {
        return status;
    }
    *out_host = reinterpret_cast<MiraAndroidHostV1 *>(handle.release());
    return MIRA_HOST_OK;
}

MiraHostStatus mira_android_host_start_v1(MiraAndroidHostV1 *host) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->start() : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_stop_v1(MiraAndroidHostV1 *host) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->stop() : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_destroy_v1(MiraAndroidHostV1 *host) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    if (fake == nullptr) {
        return MIRA_HOST_ERR_INVALID_ARGUMENT;
    }
    const MiraHostStatus status = fake->destroy();
    if (status == MIRA_HOST_OK) {
        delete reinterpret_cast<mira::test::FakeAndroidHostHandle *>(host);
    }
    return status;
}

MiraHostStatus mira_android_host_get_capabilities_v1(MiraAndroidHostV1 *host,
                                                     MiraHostCapabilitiesV1 *out) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->get_capabilities(out) : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_get_topology_v1(MiraAndroidHostV1 *host, MiraHostTopologyV1 *out) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->get_topology(out) : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_capture_frame_v1(MiraAndroidHostV1 *host,
                                                  const MiraHostFrameRequestV1 *request,
                                                  uint64_t *out_operation) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->capture_frame(*request, out_operation)
                           : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_get_ui_tree_v1(MiraAndroidHostV1 *host,
                                                const MiraHostTreeRequestV1 *request,
                                                uint64_t *out_operation) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->get_ui_tree(*request, out_operation)
                           : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_dispatch_input_v1(MiraAndroidHostV1 *host,
                                                   const MiraHostInputRequestV1 *request,
                                                   uint64_t *out_operation) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->dispatch_input(*request, out_operation)
                           : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

MiraHostStatus mira_android_host_cancel_operation_v1(MiraAndroidHostV1 *host, uint64_t operation) {
    auto *fake = mira::test::FakeAndroidHost::from_abi_host(host);
    return fake != nullptr ? fake->cancel_operation(operation) : MIRA_HOST_ERR_INVALID_ARGUMENT;
}

} // extern "C"
