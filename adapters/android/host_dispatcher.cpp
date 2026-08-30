#include <mira/adapters/android/host_dispatcher.hpp>

#include <executor/executor.hpp>

#include <deque>
#include <utility>

namespace mira::adapters::android {
namespace {

Error bridge_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.android.bridge";
    error.safe_message = std::move(message);
    return error;
}

Error host_status_error(MiraHostStatus status) {
    Error error;
    error.domain = "mira.android.host";
    error.domain_code = static_cast<std::int32_t>(status);
    switch (status) {
    case MIRA_HOST_ERR_PERMISSION_DENIED:
        error.code = ErrorCode::PermissionDenied;
        error.safe_message = "host reported permission denied";
        break;
    case MIRA_HOST_ERR_UNAVAILABLE:
        error.code = ErrorCode::Unavailable;
        error.safe_message = "host reported temporarily unavailable";
        error.retryable = true;
        break;
    case MIRA_HOST_ERR_UNSUPPORTED_VERSION:
        error.code = ErrorCode::UnsupportedVersion;
        error.safe_message = "host reported unsupported ABI version";
        break;
    case MIRA_HOST_ERR_CANCELLED:
        error.code = ErrorCode::Cancelled;
        error.safe_message = "host operation was cancelled";
        break;
    case MIRA_HOST_ERR_DEADLINE_EXCEEDED:
        error.code = ErrorCode::DeadlineExceeded;
        error.safe_message = "host operation exceeded its deadline";
        break;
    case MIRA_HOST_ERR_INVALID_BUFFER:
        error.code = ErrorCode::InvalidObservation;
        error.safe_message = "host reported an invalid buffer";
        break;
    case MIRA_HOST_ERR_INVALID_TOPOLOGY:
        error.code = ErrorCode::InvalidObservation;
        error.safe_message = "host reported an invalid topology";
        break;
    case MIRA_HOST_ERR_EXECUTION_UNCERTAIN:
        error.code = ErrorCode::ExecutionUncertain;
        error.safe_message = "host could not confirm the execution outcome";
        break;
    case MIRA_HOST_ERR_CAPACITY:
        error.code = ErrorCode::ResourceExhausted;
        error.safe_message = "host reported capacity exhaustion";
        error.retryable = true;
        break;
    case MIRA_HOST_ERR_PLATFORM_ERROR:
        error.code = ErrorCode::PlatformError;
        error.safe_message = "host reported a platform error";
        break;
    case MIRA_HOST_ERR_INVALID_ARGUMENT:
    case MIRA_HOST_ERR_INVALID_STATE:
    case MIRA_HOST_OK:
        error.code = ErrorCode::Internal;
        error.safe_message = "host reported an unexpected status";
        break;
    }
    return error;
}

// struct_size + correlation: the minimum every versioned result carries.
constexpr std::uint64_t kResultPrefixSize = 16;

} // namespace

HostLeaseGuard::HostLeaseGuard(MiraHostBufferLeaseV1 lease) : lease_(lease), valid_(true) {
    const bool plausible = lease_.data != nullptr && lease_.size > 0 && lease_.release != nullptr &&
                           lease_.plane_count > 0 && lease_.plane_count <= MIRA_MAX_PLANES;
    if (!plausible) {
        valid_ = false;
        lease_ = MiraHostBufferLeaseV1{};
    }
}

HostLeaseGuard::~HostLeaseGuard() { release(); }

HostLeaseGuard::HostLeaseGuard(HostLeaseGuard &&other) noexcept
    : lease_(other.lease_), released_(other.released_.load()), valid_(other.valid_) {
    other.lease_ = MiraHostBufferLeaseV1{};
    other.released_.store(false);
    other.valid_ = false;
}

HostLeaseGuard &HostLeaseGuard::operator=(HostLeaseGuard &&other) noexcept {
    if (this != &other) {
        release();
        lease_ = other.lease_;
        released_.store(other.released_.load());
        valid_ = other.valid_;
        other.lease_ = MiraHostBufferLeaseV1{};
        other.released_.store(false);
        other.valid_ = false;
    }
    return *this;
}

void HostLeaseGuard::release() noexcept {
    if (!valid_ || released_.exchange(true)) {
        return;
    }
    if (lease_.release != nullptr) {
        lease_.release(lease_.lease_id, lease_.user_data);
    }
}

HostDispatcherBridge::HostDispatcherBridge(executor::Executor &executor) : executor_(executor) {
    callbacks_table_.struct_size = sizeof(MiraHostCallbacksV1);
    callbacks_table_.user_data = this;
    callbacks_table_.on_operation_complete = [](void *user_data,
                                                const MiraHostOperationResultV1 *result) {
        if (user_data == nullptr || result == nullptr) {
            return;
        }
        static_cast<HostDispatcherBridge *>(user_data)->handle_operation_complete(*result);
    };
    callbacks_table_.on_capabilities_changed = [](void *user_data,
                                                  const MiraHostCapabilitiesV1 *capabilities) {
        if (user_data == nullptr || capabilities == nullptr) {
            return;
        }
        static_cast<HostDispatcherBridge *>(user_data)->handle_capabilities_changed(*capabilities);
    };
}

HostDispatcherBridge::~HostDispatcherBridge() {
    std::unordered_map<std::uint64_t, RegistryEntry> pending;
    {
        std::lock_guard lock(mutex_);
        detached_ = true;
        pending = std::move(registry_);
        registry_.clear();
    }
    // The bridge is going away; settle waiters with a cancellation so no
    // future is left hanging. Real callbacks arriving later find an empty
    // registry and are counted as late violations.
    for (auto &[correlation, entry] : pending) {
        MiraHostOperationResultV1 cancelled{};
        cancelled.struct_size = sizeof(MiraHostOperationResultV1);
        cancelled.correlation = correlation;
        cancelled.kind = entry.kind;
        cancelled.status = MIRA_HOST_ERR_CANCELLED;
        try {
            entry.fulfill(cancelled);
        } catch (...) {
        }
    }
    std::deque<std::future<void>> completions;
    {
        std::lock_guard lock(mutex_);
        for (auto &future : completion_futures_) {
            completions.push_back(std::move(future));
        }
        completion_futures_.clear();
        stats_.operations_settled += pending.size();
    }
    for (auto &future : completions) {
        try {
            future.get();
        } catch (...) {
        }
    }
}

const MiraHostCallbacksV1 *HostDispatcherBridge::host_callbacks() { return &callbacks_table_; }

HostBridgeStats HostDispatcherBridge::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::optional<MiraHostCapabilitiesV1> HostDispatcherBridge::latest_capabilities() const {
    std::lock_guard lock(mutex_);
    return latest_capabilities_;
}

void HostDispatcherBridge::note_capabilities(const MiraHostCapabilitiesV1 &capabilities) {
    std::lock_guard lock(mutex_);
    if (latest_capabilities_.has_value() &&
        capabilities.host_sequence <= latest_capabilities_->host_sequence) {
        // Host sequences are monotonic; stale snapshots are a violation.
        ++stats_.contract_violations;
        return;
    }
    if (!latest_capabilities_.has_value() ||
        capabilities.host_generation != latest_capabilities_->host_generation) {
        expected_generation_ = capabilities.host_generation;
    }
    latest_capabilities_ = capabilities;
}

void HostDispatcherBridge::record_violation() {
    std::lock_guard lock(mutex_);
    ++stats_.contract_violations;
}

void HostDispatcherBridge::handle_capabilities_changed(const MiraHostCapabilitiesV1 &capabilities) {
    if (capabilities.struct_size < sizeof(MiraHostCapabilitiesV1)) {
        record_violation();
        return;
    }
    note_capabilities(capabilities);
}

void HostDispatcherBridge::handle_operation_complete(const MiraHostOperationResultV1 &result) {
    if (result.struct_size < kResultPrefixSize || result.correlation == 0) {
        record_violation();
        return;
    }
    RegistryEntry entry;
    {
        std::lock_guard lock(mutex_);
        if (detached_) {
            ++stats_.late_callbacks_after_detach;
            ++stats_.contract_violations;
            return;
        }
        const auto found = registry_.find(result.correlation);
        if (found == registry_.end()) {
            // Exactly-once terminal: a callback for a settled or unknown
            // operation is dropped and counted, never applied twice. The
            // lease, if any, stays owned by the first settlement.
            ++stats_.duplicate_terminal_callbacks;
            ++stats_.contract_violations;
            return;
        }
        if (expected_generation_.has_value() && result.host_generation != *expected_generation_) {
            ++stats_.wrong_generation_results;
            ++stats_.contract_violations;
        }
        entry = std::move(found->second);
        registry_.erase(found);
        ++stats_.operations_settled;
    }

    // The callback ends here: the result struct and, when present, the
    // lease record are copied to the heap inside the callback (the host
    // only guarantees their validity during the call). Conversion and
    // promise settlement run later on the Executor.
    auto copied = std::make_shared<MiraHostOperationResultV1>(result);
    std::shared_ptr<MiraHostBufferLeaseV1> lease_copy;
    if (result.lease != nullptr) {
        lease_copy = std::make_shared<MiraHostBufferLeaseV1>(*result.lease);
        copied->lease = lease_copy.get();
    }
    try {
        auto completion = executor_.submit_auto([entry, copied, lease_copy]() mutable {
            try {
                entry.fulfill(*copied);
            } catch (...) {
            }
        });
        std::lock_guard lock(mutex_);
        completion_futures_.push_back(std::move(completion));
        while (!completion_futures_.empty() &&
               completion_futures_.front().wait_for(std::chrono::seconds(0)) ==
                   std::future_status::ready) {
            try {
                completion_futures_.front().get();
            } catch (...) {
            }
            completion_futures_.pop_front();
        }
    } catch (...) {
        // Executor rejection (e.g. during shutdown): settle inline so the
        // waiting future never hangs. This stays bounded by contract.
        std::lock_guard lock(mutex_);
        ++stats_.executor_submission_rejections;
        try {
            entry.fulfill(*copied);
        } catch (...) {
        }
    }
}

std::future<Result<HostFrameOutcome>>
HostDispatcherBridge::capture_frame(MiraAndroidHostV1 *host,
                                    const MiraHostFrameRequestV1 &request) {
    MiraHostFrameRequestV1 prepared = request;
    prepared.correlation = next_correlation_.fetch_add(1) + 1;
    auto promise = std::make_shared<std::promise<Result<HostFrameOutcome>>>();
    auto future = promise->get_future();
    {
        std::lock_guard lock(mutex_);
        registry_.emplace(
            prepared.correlation,
            RegistryEntry{
                static_cast<std::uint32_t>(MIRA_HOST_OP_CAPTURE_FRAME),
                [this, promise](const MiraHostOperationResultV1 &result) {
                    HostFrameOutcome outcome;
                    outcome.status = result.status;
                    if (result.status != MIRA_HOST_OK) {
                        // Cancelled, rejected or failed captures may
                        // still carry a lease; release it exactly
                        // once before settling the error.
                        if (result.lease != nullptr && result.lease->release != nullptr) {
                            result.lease->release(result.lease->lease_id, result.lease->user_data);
                            std::lock_guard stats_lock(mutex_);
                            ++stats_.leases_released;
                        }
                        promise->set_value(host_status_error(result.status));
                        return;
                    }
                    outcome.frame_id = result.frame_id;
                    outcome.display_id = result.display_id;
                    outcome.width = result.width;
                    outcome.height = result.height;
                    outcome.pixel_format = result.pixel_format;
                    outcome.rotation = result.rotation;
                    outcome.capture_begin_ns = result.capture_begin_ns;
                    outcome.capture_end_ns = result.capture_end_ns;
                    outcome.environment_epoch = result.environment_epoch;
                    if (result.lease != nullptr) {
                        outcome.lease = HostLeaseGuard(*result.lease);
                    }
                    if (!outcome.lease.valid()) {
                        std::lock_guard stats_lock(mutex_);
                        ++stats_.contract_violations;
                        promise->set_value(bridge_error(ErrorCode::InvalidObservation,
                                                        "frame result carried no usable lease"));
                        return;
                    }
                    promise->set_value(std::move(outcome));
                }});
        ++stats_.operations_submitted;
    }
    const MiraHostStatus submitted = mira_android_host_capture_frame_v1(host, &prepared, nullptr);
    if (submitted != MIRA_HOST_OK) {
        settle_rejected(prepared.correlation,
                        static_cast<std::uint32_t>(MIRA_HOST_OP_CAPTURE_FRAME), submitted);
    }
    return future;
}

std::future<Result<HostTreeOutcome>>
HostDispatcherBridge::get_ui_tree(MiraAndroidHostV1 *host, const MiraHostTreeRequestV1 &request) {
    MiraHostTreeRequestV1 prepared = request;
    prepared.correlation = next_correlation_.fetch_add(1) + 1;
    auto promise = std::make_shared<std::promise<Result<HostTreeOutcome>>>();
    auto future = promise->get_future();
    {
        std::lock_guard lock(mutex_);
        registry_.emplace(
            prepared.correlation,
            RegistryEntry{
                static_cast<std::uint32_t>(MIRA_HOST_OP_GET_UI_TREE),
                [this, promise](const MiraHostOperationResultV1 &result) {
                    HostTreeOutcome outcome;
                    outcome.status = result.status;
                    if (result.status != MIRA_HOST_OK) {
                        if (result.lease != nullptr && result.lease->release != nullptr) {
                            result.lease->release(result.lease->lease_id, result.lease->user_data);
                            std::lock_guard stats_lock(mutex_);
                            ++stats_.leases_released;
                        }
                        promise->set_value(host_status_error(result.status));
                        return;
                    }
                    if (result.lease == nullptr || result.lease->data == nullptr ||
                        result.lease->size == 0) {
                        std::lock_guard stats_lock(mutex_);
                        ++stats_.contract_violations;
                        promise->set_value(bridge_error(ErrorCode::InvalidObservation,
                                                        "tree result carried no usable lease"));
                        return;
                    }
                    HostLeaseGuard guard(*result.lease);
                    if (!guard.valid()) {
                        std::lock_guard stats_lock(mutex_);
                        ++stats_.contract_violations;
                        promise->set_value(bridge_error(ErrorCode::InvalidObservation,
                                                        "tree lease was not plausible"));
                        return;
                    }
                    const auto *payload = reinterpret_cast<const std::byte *>(guard.lease().data);
                    outcome.bytes.assign(payload, payload + guard.lease().size);
                    outcome.environment_epoch = result.environment_epoch;
                    {
                        std::lock_guard stats_lock(mutex_);
                        ++stats_.leases_released;
                    }
                    guard.release();
                    promise->set_value(std::move(outcome));
                }});
        ++stats_.operations_submitted;
    }
    const MiraHostStatus submitted = mira_android_host_get_ui_tree_v1(host, &prepared, nullptr);
    if (submitted != MIRA_HOST_OK) {
        settle_rejected(prepared.correlation, static_cast<std::uint32_t>(MIRA_HOST_OP_GET_UI_TREE),
                        submitted);
    }
    return future;
}

std::future<Result<HostInputOutcome>>
HostDispatcherBridge::dispatch_input(MiraAndroidHostV1 *host,
                                     const MiraHostInputRequestV1 &request) {
    MiraHostInputRequestV1 prepared = request;
    prepared.correlation = next_correlation_.fetch_add(1) + 1;
    auto promise = std::make_shared<std::promise<Result<HostInputOutcome>>>();
    auto future = promise->get_future();
    {
        std::lock_guard lock(mutex_);
        registry_.emplace(prepared.correlation,
                          RegistryEntry{static_cast<std::uint32_t>(MIRA_HOST_OP_DISPATCH_INPUT),
                                        [promise](const MiraHostOperationResultV1 &result) {
                                            if (result.status != MIRA_HOST_OK) {
                                                promise->set_value(
                                                    host_status_error(result.status));
                                                return;
                                            }
                                            HostInputOutcome outcome;
                                            outcome.status = result.status;
                                            outcome.receipt = result.input_receipt;
                                            outcome.side_effect_may_have_occurred =
                                                result.side_effect_may_have_occurred;
                                            outcome.environment_epoch = result.environment_epoch;
                                            promise->set_value(std::move(outcome));
                                        }});
        ++stats_.operations_submitted;
    }
    const MiraHostStatus submitted = mira_android_host_dispatch_input_v1(host, &prepared, nullptr);
    if (submitted != MIRA_HOST_OK) {
        settle_rejected(prepared.correlation,
                        static_cast<std::uint32_t>(MIRA_HOST_OP_DISPATCH_INPUT), submitted);
    }
    return future;
}

void HostDispatcherBridge::settle_rejected(std::uint64_t correlation, std::uint32_t kind,
                                           MiraHostStatus status) {
    RegistryEntry entry;
    {
        std::lock_guard lock(mutex_);
        const auto found = registry_.find(correlation);
        if (found == registry_.end()) {
            return;
        }
        entry = std::move(found->second);
        registry_.erase(found);
        ++stats_.operations_settled;
    }
    MiraHostOperationResultV1 rejected{};
    rejected.struct_size = sizeof(MiraHostOperationResultV1);
    rejected.correlation = correlation;
    rejected.kind = kind;
    rejected.status = status;
    try {
        entry.fulfill(rejected);
    } catch (...) {
    }
}

Result<std::size_t> HostDispatcherBridge::cancel_outstanding(MiraAndroidHostV1 *host) {
    std::vector<std::uint64_t> correlations;
    {
        std::lock_guard lock(mutex_);
        correlations.reserve(registry_.size());
        for (const auto &item : registry_) {
            correlations.push_back(item.first);
        }
    }
    for (const std::uint64_t correlation : correlations) {
        static_cast<void>(mira_android_host_cancel_operation_v1(host, correlation));
    }
    return correlations.size();
}

std::size_t HostDispatcherBridge::outstanding_operations() const {
    std::lock_guard lock(mutex_);
    return registry_.size();
}

} // namespace mira::adapters::android
