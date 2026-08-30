#include "support/fake_android_host.hpp"
#include "support/test.hpp"

#include <mira/adapters/android/android_host_adapter.hpp>
#include <mira/adapters/android/host_dispatcher.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

using mira::ErrorCode;
using mira::ObservationRequest;
using mira::OperationContext;
using mira::adapters::android::AndroidHostAdapter;
using mira::test::FakeAndroidHost;

class ExecutorFixture final {
  public:
    ExecutorFixture() {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        if (!executor.initialize(config)) {
            std::abort();
        }
    }
    ~ExecutorFixture() { static_cast<void>(executor.shutdown(true)); }
    executor::Executor executor;
};

OperationContext context(std::chrono::milliseconds budget) {
    OperationContext value;
    value.operation = mira::OperationId::generate();
    value.started_at = mira::Timestamp::now();
    value.deadline = std::chrono::steady_clock::now() + budget;
    return value;
}

int check_abi_validation_and_lifecycle() {
    // struct_size and abi_version are validated fail-closed at create.
    MiraAndroidHostConfigV1 config{};
    config.struct_size = sizeof(MiraAndroidHostConfigV1);
    config.abi_version = MIRA_ANDROID_ABI_VERSION;
    MiraHostCallbacksV1 callbacks{};
    callbacks.struct_size = sizeof(MiraHostCallbacksV1);
    callbacks.on_operation_complete = [](void *, const MiraHostOperationResultV1 *) {};

    MiraAndroidHostV1 *host = nullptr;
    MiraAndroidHostConfigV1 bad_size = config;
    bad_size.struct_size = 8;
    MIRA_CHECK(mira_android_host_create_v1(&bad_size, &callbacks, &host) ==
               MIRA_HOST_ERR_INVALID_ARGUMENT);
    MiraAndroidHostConfigV1 bad_version = config;
    bad_version.abi_version = 99;
    MIRA_CHECK(mira_android_host_create_v1(&bad_version, &callbacks, &host) ==
               MIRA_HOST_ERR_UNSUPPORTED_VERSION);
    MiraHostCallbacksV1 no_terminal = callbacks;
    no_terminal.on_operation_complete = nullptr;
    MIRA_CHECK(mira_android_host_create_v1(&config, &no_terminal, &host) ==
               MIRA_HOST_ERR_INVALID_ARGUMENT);

    // Requests before start fail with an explicit state error.
    MIRA_CHECK(mira_android_host_create_v1(&config, &callbacks, &host) == MIRA_HOST_OK);
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(host);
    MIRA_CHECK(fake != nullptr);
    MiraHostFrameRequestV1 request{};
    request.struct_size = sizeof(MiraHostFrameRequestV1);
    request.correlation = 1;
    MIRA_CHECK(mira_android_host_capture_frame_v1(host, &request, nullptr) ==
               MIRA_HOST_ERR_INVALID_STATE);

    MIRA_CHECK(mira_android_host_start_v1(host) == MIRA_HOST_OK);
    MIRA_CHECK(fake->started());
    // stop and destroy are idempotent.
    MIRA_CHECK(mira_android_host_stop_v1(host) == MIRA_HOST_OK);
    MIRA_CHECK(mira_android_host_stop_v1(host) == MIRA_HOST_OK);
    MIRA_CHECK(fake->stopped_count() == 2);
    // Destroy frees the handle; a second ABI call would use freed memory,
    // so idempotency is checked on the still-alive instance.
    const auto kept = FakeAndroidHost::shared_from_abi_host(host);
    MIRA_CHECK(mira_android_host_destroy_v1(host) == MIRA_HOST_OK);
    MIRA_CHECK(fake->destroyed_count() == 1);
    MIRA_CHECK(kept->destroy() == MIRA_HOST_OK);
    MIRA_CHECK(kept->destroyed_count() == 1);
    return 0;
}

int check_adapter_observe_and_lease_release() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());
    MIRA_CHECK(fake != nullptr);

    const auto capabilities = adapter->capabilities();
    MIRA_CHECK(capabilities.screen_capture);
    MIRA_CHECK(capabilities.discrete_input);
    MIRA_CHECK(capabilities.epoch_invalidation);
    // The skeleton is honest: no UI tree support is declared.
    MIRA_CHECK(!capabilities.ui_tree);

    ObservationRequest request;
    request.required.screen = true;
    const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    MIRA_CHECK(observation.value().screen.has_value());
    MIRA_CHECK(!observation.value().screen->value.payload_artifact.is_nil());
    MIRA_CHECK(observation.value().environment_epoch == fake->environment_epoch());
    // The lease was released exactly once after the artifact commit.
    MIRA_CHECK(fake->outstanding_leases() == 0);
    MIRA_CHECK(adapter->bridge_stats().operations_settled >= 1);
    MIRA_CHECK(adapter->bridge_stats().contract_violations == 0);

    // Required components the skeleton cannot provide fail closed.
    ObservationRequest unsupported = request;
    unsupported.required.structure = true;
    const auto refused = adapter->observe(unsupported, context(std::chrono::seconds(1)));
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::UnsupportedCapability);
    return 0;
}

int check_epoch_invalidation_on_rotation() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    // Defer the frame callback, rotate, then deliver: the frame epoch is
    // older than the capability snapshot, so the observation fails stale.
    FakeAndroidHost::Behaviour behaviour;
    behaviour.defer_callbacks = true;
    fake->set_behaviour(behaviour);

    ObservationRequest request;
    request.required.screen = true;
    auto outcome = fixture.executor.submit_auto([&adapter, request]() {
        return adapter->observe(request, context(std::chrono::seconds(2)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fake->rotate();
    const auto epoch_after_rotation = fake->environment_epoch();
    fake->release_pending();
    const auto observation = outcome.get();
    MIRA_CHECK(!observation.has_value());
    MIRA_CHECK(observation.error().code == ErrorCode::StaleObservation);
    MIRA_CHECK(adapter->environment_epoch() == epoch_after_rotation);
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_duplicate_and_unknown_callbacks_isolated() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    FakeAndroidHost::Behaviour behaviour;
    behaviour.duplicate_next_callback = true;
    fake->set_behaviour(behaviour);

    ObservationRequest request;
    request.required.screen = true;
    const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    const auto stats = adapter->bridge_stats();
    MIRA_CHECK(stats.duplicate_terminal_callbacks == 1);
    MIRA_CHECK(stats.contract_violations == 1);
    // The duplicated delivery did not double-settle the operation.
    MIRA_CHECK(stats.operations_settled == 1);
    MIRA_CHECK(fake->outstanding_leases() == 0);

    // A fabricated late callback for an unknown operation is dropped.
    MiraHostOperationResultV1 bogus{};
    bogus.struct_size = sizeof(MiraHostOperationResultV1);
    bogus.correlation = 0xFFFFFFFFFFFFULL;
    bogus.kind = MIRA_HOST_OP_CAPTURE_FRAME;
    bogus.status = MIRA_HOST_OK;
    fake->deliver_raw_result(bogus);
    MIRA_CHECK(adapter->bridge_stats().duplicate_terminal_callbacks == 2);
    return 0;
}

int check_oversize_lease_rejected_and_released() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    FakeAndroidHost::Behaviour behaviour;
    behaviour.oversize_next_lease = true;
    fake->set_behaviour(behaviour);

    ObservationRequest request;
    request.required.screen = true;
    const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
    // A plane layout escaping the buffer fails closed...
    MIRA_CHECK(!observation.has_value());
    MIRA_CHECK(observation.error().code == ErrorCode::InvalidObservation);
    // ...and the lease is still released exactly once.
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_input_dispatch_and_uncertainty() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    mira::InputSequence sequence;
    sequence.events.push_back(mira::InputEvent{"tap", "0.5,0.5"});
    sequence.events.push_back(mira::InputEvent{"swipe", "0.1,0.2,0.3,0.4"});
    sequence.events.push_back(mira::InputEvent{"type", "fixture"});
    sequence.events.push_back(mira::InputEvent{"home", ""});
    const auto receipt = adapter->execute(sequence, context(std::chrono::seconds(2)));
    MIRA_CHECK(receipt.has_value());
    MIRA_CHECK(receipt.value().status == mira::ExecutionStatus::Completed);
    MIRA_CHECK(!receipt.value().side_effect_may_have_occurred);
    MIRA_CHECK(fake->dispatched_inputs().size() == 4);

    // Malformed canonical input is rejected before reaching the host.
    mira::InputSequence malformed;
    malformed.events.push_back(mira::InputEvent{"tap", "2.5"});
    const auto refused = adapter->execute(malformed, context(std::chrono::seconds(1)));
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(fake->dispatched_inputs().size() == 4);

    // An uncertain host receipt surfaces ExecutionUncertain semantics.
    FakeAndroidHost::Behaviour behaviour;
    behaviour.uncertain_next_input = true;
    fake->set_behaviour(behaviour);
    mira::InputSequence probe;
    probe.events.push_back(mira::InputEvent{"tap", "0.25,0.75"});
    const auto uncertain = adapter->execute(probe, context(std::chrono::seconds(2)));
    MIRA_CHECK(uncertain.has_value());
    MIRA_CHECK(uncertain.value().status == mira::ExecutionStatus::Unknown);
    MIRA_CHECK(uncertain.value().side_effect_may_have_occurred);
    return 0;
}

int check_cancellation_and_interrupt() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    FakeAndroidHost::Behaviour behaviour;
    behaviour.defer_callbacks = true;
    fake->set_behaviour(behaviour);

    ObservationRequest request;
    request.required.screen = true;
    OperationContext cancelled_context = context(std::chrono::seconds(5));
    std::atomic<bool> cancelled{false};
    cancelled_context.cancellation_requested = [&cancelled]() noexcept {
        return cancelled.load(std::memory_order_acquire);
    };
    auto outcome = fixture.executor.submit_auto([&adapter, request, cancelled_context]() {
        return adapter->observe(request, cancelled_context);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancelled.store(true, std::memory_order_release);
    const auto observation = outcome.get();
    MIRA_CHECK(!observation.has_value());
    MIRA_CHECK(observation.error().code == ErrorCode::Cancelled);
    // Interrupt is idempotent and releases pending operations.
    MIRA_CHECK(adapter->interrupt(mira::make_control_context()).has_value());
    MIRA_CHECK(adapter->interrupt(mira::make_control_context()).has_value());
    fake->release_pending();
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_adapter_shutdown_releases_everything() {
    ExecutorFixture fixture;
    std::shared_ptr<FakeAndroidHost> fake;
    {
        auto created = AndroidHostAdapter::create(fixture.executor);
        MIRA_CHECK(created.has_value());
        auto adapter = std::move(created).value();
        fake = FakeAndroidHost::shared_from_abi_host(adapter->host());
        MIRA_CHECK(fake != nullptr);

        ObservationRequest request;
        request.required.screen = true;
        const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
        MIRA_CHECK(observation.has_value());
        MIRA_CHECK(fake->outstanding_leases() == 0);
    }
    // The adapter destructor stopped and destroyed the host exactly once,
    // after every lease was already released.
    MIRA_CHECK(fake->stopped_count() == 1);
    MIRA_CHECK(fake->destroyed_count() == 1);
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

} // namespace

int main() {
    if (const int code = check_abi_validation_and_lifecycle(); code != 0)
        return code;
    if (const int code = check_adapter_observe_and_lease_release(); code != 0)
        return code;
    if (const int code = check_epoch_invalidation_on_rotation(); code != 0)
        return code;
    if (const int code = check_duplicate_and_unknown_callbacks_isolated(); code != 0)
        return code;
    if (const int code = check_oversize_lease_rejected_and_released(); code != 0)
        return code;
    if (const int code = check_input_dispatch_and_uncertainty(); code != 0)
        return code;
    if (const int code = check_cancellation_and_interrupt(); code != 0)
        return code;
    if (const int code = check_adapter_shutdown_releases_everything(); code != 0)
        return code;
    return 0;
}
