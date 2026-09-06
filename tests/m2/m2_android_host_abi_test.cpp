#include "support/fake_android_host.hpp"
#include "support/test.hpp"

#include <mira/adapters/android/android_host_adapter.hpp>
#include <mira/adapters/android/host_dispatcher.hpp>
#include <mira/artifact_store.hpp>
#include <mira/event_store.hpp>

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
    // The fake host declares partial accessibility; the adapter mirrors it.
    MIRA_CHECK(capabilities.ui_tree);

    ObservationRequest request;
    request.required.screen = true;
    const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    MIRA_CHECK(observation.value().screen.has_value());
    MIRA_CHECK(!observation.value().screen->value.payload_artifact.is_nil());
    MIRA_CHECK(observation.value().environment_epoch == fake->environment_epoch());
    // The lease was released exactly once after the artifact commit; the
    // observe-tail release path must be visible in the bridge statistics.
    MIRA_CHECK(fake->outstanding_leases() == 0);
    MIRA_CHECK(adapter->bridge_stats().leases_released == 1);
    MIRA_CHECK(adapter->bridge_stats().operations_settled >= 1);
    MIRA_CHECK(adapter->bridge_stats().contract_violations == 0);

    // Required components the host cannot provide fail closed. With
    // accessibility disabled the adapter must not even submit a tree
    // operation.
    fake->set_accessibility_completeness(0);
    MIRA_CHECK(!adapter->capabilities().ui_tree);
    ObservationRequest unsupported = request;
    unsupported.required.structure = true;
    const auto refused = adapter->observe(unsupported, context(std::chrono::seconds(1)));
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::UnsupportedCapability);
    MIRA_CHECK(adapter->bridge_stats().operations_settled == 1);
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
    // The stale frame's lease was released through the guard; the bridge
    // must count that path too.
    MIRA_CHECK(adapter->bridge_stats().leases_released == 1);
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
        MIRA_CHECK(adapter->bridge_stats().leases_released == 1);
    }
    // The adapter destructor stopped and destroyed the host exactly once,
    // after every lease was already released.
    MIRA_CHECK(fake->stopped_count() == 1);
    MIRA_CHECK(fake->destroyed_count() == 1);
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_leases_released_counts_every_release_path() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    ObservationRequest screen_only;
    screen_only.required.screen = true;
    MIRA_CHECK(adapter->observe(screen_only, context(std::chrono::seconds(2))).has_value());
    std::uint64_t released = adapter->bridge_stats().leases_released;
    MIRA_CHECK(released == 1);

    // Screen + structure in one observation: the frame lease releases at
    // the adapter tail, the tree lease inside the bridge; both count.
    ObservationRequest both;
    both.required.screen = true;
    both.required.structure = true;
    MIRA_CHECK(adapter->observe(both, context(std::chrono::seconds(2))).has_value());
    released = adapter->bridge_stats().leases_released;
    MIRA_CHECK(released == 3);
    MIRA_CHECK(fake->outstanding_leases() == 0);

    // An error-settled capture (oversize lease) still releases and counts.
    FakeAndroidHost::Behaviour behaviour;
    behaviour.oversize_next_lease = true;
    fake->set_behaviour(behaviour);
    MIRA_CHECK(!adapter->observe(screen_only, context(std::chrono::seconds(2))).has_value());
    MIRA_CHECK(adapter->bridge_stats().leases_released == released + 1);
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_structure_observation_aggregation() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    // Structure-only observation is legal and delivers a parsed snapshot.
    ObservationRequest structure_only;
    structure_only.required.structure = true;
    const auto tree = adapter->observe(structure_only, context(std::chrono::seconds(2)));
    MIRA_CHECK(tree.has_value());
    MIRA_CHECK(tree.value().structure.has_value());
    MIRA_CHECK(!tree.value().screen.has_value());
    const auto &snapshot = tree.value().structure->value;
    MIRA_CHECK(snapshot.nodes.size() == 3);
    MIRA_CHECK(snapshot.complete);
    MIRA_CHECK(!snapshot.truncated);
    MIRA_CHECK(snapshot.nodes[0].role == mira::UiRole::Root);
    MIRA_CHECK(snapshot.nodes[1].role == mira::UiRole::Button);
    MIRA_CHECK(snapshot.nodes[1].text == "OK");
    MIRA_CHECK(snapshot.nodes[1].stable_hint.has_value());
    MIRA_CHECK(snapshot.nodes[1].stable_hint->hint == "com.example/ok");
    MIRA_CHECK(mira::supports_action(snapshot.nodes[1].supported_actions,
                                     mira::UiNodeAction::Click));
    MIRA_CHECK(mira::has_state(snapshot.nodes[0].state, mira::UiNodeState::Visible));
    MIRA_CHECK(snapshot.nodes[1].bounds.left == 0.1);
    MIRA_CHECK(tree.value().structure->quality == mira::ComponentQuality::Good);
    MIRA_CHECK(tree.value().structure->environment_epoch == fake->environment_epoch());
    MIRA_CHECK(fake->outstanding_leases() == 0);

    // Screen + structure aggregate into one observation with a bounded-skew
    // span covering both components.
    ObservationRequest aggregate;
    aggregate.required.screen = true;
    aggregate.required.structure = true;
    const auto combined = adapter->observe(aggregate, context(std::chrono::seconds(2)));
    MIRA_CHECK(combined.has_value());
    MIRA_CHECK(combined.value().screen.has_value());
    MIRA_CHECK(combined.value().structure.has_value());
    MIRA_CHECK(combined.value().atomicity == mira::ObservationAtomicity::BoundedSkew);
    MIRA_CHECK(combined.value().structure->provenance.source == "android.host.tree.v1");
    MIRA_CHECK(fake->outstanding_leases() == 0);

    // A tree delivered with a non-JSON payload fails closed when required
    // and never leaks its lease.
    FakeAndroidHost::Behaviour behaviour;
    behaviour.invalid_next_tree = true;
    fake->set_behaviour(behaviour);
    const auto poisoned = adapter->observe(structure_only, context(std::chrono::seconds(2)));
    MIRA_CHECK(!poisoned.has_value());
    MIRA_CHECK(poisoned.error().code == ErrorCode::InvalidObservation);
    MIRA_CHECK(fake->outstanding_leases() == 0);
    return 0;
}

int check_structure_epoch_and_capability_degradation() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    // A rotation landing between submission and settlement marks the tree
    // stale, exactly like frames.
    FakeAndroidHost::Behaviour behaviour;
    behaviour.defer_callbacks = true;
    fake->set_behaviour(behaviour);
    ObservationRequest structure_only;
    structure_only.required.structure = true;
    auto outcome = fixture.executor.submit_auto([&adapter, structure_only]() {
        return adapter->observe(structure_only, context(std::chrono::seconds(2)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fake->rotate();
    fake->release_pending();
    const auto stale = outcome.get();
    MIRA_CHECK(!stale.has_value());
    MIRA_CHECK(stale.error().code == ErrorCode::StaleObservation);
    MIRA_CHECK(fake->outstanding_leases() == 0);

    // Losing accessibility support degrades optional structure captures but
    // never fails a screen-only observation.
    fake->set_behaviour(FakeAndroidHost::Behaviour{});
    fake->set_accessibility_completeness(0);
    ObservationRequest optional_structure;
    optional_structure.required.screen = true;
    optional_structure.optional.structure = true;
    const auto degraded = adapter->observe(optional_structure, context(std::chrono::seconds(2)));
    MIRA_CHECK(degraded.has_value());
    MIRA_CHECK(degraded.value().screen.has_value());
    MIRA_CHECK(!degraded.value().structure.has_value());
    MIRA_CHECK(!degraded.value().quality.degradations.empty());
    MIRA_CHECK(adapter->bridge_stats().contract_violations == 0);
    return 0;
}

int check_artifact_store_capacity_and_injection() {
    ExecutorFixture fixture;
    // Each synthetic frame is 64 bytes; a 128-byte ceiling fits exactly two
    // captures and exhausts on the third, mirroring the real-device
    // capacity exhaustion from GitHub #10.
    mira::adapters::android::AndroidHostAdapterOptions options;
    options.memory_artifact_capacity_bytes = 128;
    auto created = AndroidHostAdapter::create(fixture.executor, options);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();

    ObservationRequest request;
    request.required.screen = true;
    MIRA_CHECK(adapter->observe(request, context(std::chrono::seconds(2))).has_value());
    MIRA_CHECK(adapter->observe(request, context(std::chrono::seconds(2))).has_value());
    const auto exhausted = adapter->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(!exhausted.has_value());
    MIRA_CHECK(exhausted.error().code == ErrorCode::ResourceExhausted);

    // An injected store replaces the adapter-owned one; captures land in
    // the caller's store and its capacity governs.
    auto injected = std::make_shared<mira::MemoryArtifactStore>(256);
    mira::adapters::android::AndroidHostAdapterOptions inject_options;
    inject_options.artifact_store = injected;
    auto injected_adapter = AndroidHostAdapter::create(fixture.executor, inject_options);
    MIRA_CHECK(injected_adapter.has_value());
    auto inject_result = std::move(injected_adapter).value();
    for (int step = 0; step < 4; ++step) {
        MIRA_CHECK(inject_result->observe(request, context(std::chrono::seconds(2))).has_value());
    }
    const auto injected_exhausted =
        inject_result->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(!injected_exhausted.has_value());
    MIRA_CHECK(injected_exhausted.error().code == ErrorCode::ResourceExhausted);
    return 0;
}

int check_frame_payload_metadata_and_store_access() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();

    ObservationRequest request;
    request.required.screen = true;
    const auto observation = adapter->observe(request, context(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    const auto &screen = observation.value().screen->value;

    // The frame descriptor publishes the store record's media type, size
    // and digest; the raw host frame is honestly labeled as such (DEC-013).
    MIRA_CHECK(screen.payload_media_type == "image/x-host-frame");
    MIRA_CHECK(screen.payload_byte_size ==
               4ULL * 4ULL * 4ULL); // 4x4 RGBA frame from the fake host
    MIRA_CHECK(screen.payload_digest != mira::Sha256Digest{});

    // Hosts without an injected store reopen the payload through the
    // adapter's store handle using only published frame metadata.
    mira::ArtifactDescriptor descriptor;
    descriptor.id = screen.payload_artifact;
    descriptor.media_type = screen.payload_media_type;
    descriptor.byte_size = screen.payload_byte_size;
    descriptor.digest = screen.payload_digest;
    auto reader = adapter->artifact_store().open(descriptor);
    MIRA_CHECK(reader.has_value());
    MIRA_CHECK(reader.value().size() == screen.payload_byte_size);
    MIRA_CHECK(mira::digest_bytes(reader.value().bytes()) == screen.payload_digest);
    return 0;
}

int check_input_duration_semantics() {
    ExecutorFixture fixture;
    auto created = AndroidHostAdapter::create(fixture.executor);
    MIRA_CHECK(created.has_value());
    auto adapter = std::move(created).value();
    FakeAndroidHost *fake = FakeAndroidHost::from_abi_host(adapter->host());

    // Explicit durations travel to the host ABI; zero keeps the default.
    mira::InputSequence sequence;
    mira::InputEvent slow_press{"long_press", "0.5,0.5"};
    slow_press.duration_ms = 3000;
    mira::InputEvent slow_swipe{"swipe", "0.1,0.2,0.3,0.4"};
    slow_swipe.duration_ms = 1500;
    sequence.events.push_back(slow_press);
    sequence.events.push_back(slow_swipe);
    sequence.events.push_back(mira::InputEvent{"long_press", "0.2,0.2"});
    const auto receipt = adapter->execute(sequence, context(std::chrono::seconds(2)));
    MIRA_CHECK(receipt.has_value());
    MIRA_CHECK(fake->dispatched_inputs().size() == 3);
    MIRA_CHECK(fake->dispatched_inputs()[0].duration_ms == 3000);
    MIRA_CHECK(fake->dispatched_inputs()[1].duration_ms == 1500);
    MIRA_CHECK(fake->dispatched_inputs()[2].duration_ms == 0);

    // Durations beyond the host's declared maximum are rejected before the
    // host sees them (the fake host declares 60'000 ms).
    mira::InputSequence too_long;
    mira::InputEvent over_limit{"long_press", "0.5,0.5"};
    over_limit.duration_ms = 60'001;
    too_long.events.push_back(over_limit);
    const auto refused = adapter->execute(too_long, context(std::chrono::seconds(1)));
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(fake->dispatched_inputs().size() == 3);
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
    if (const int code = check_input_duration_semantics(); code != 0)
        return code;
    if (const int code = check_cancellation_and_interrupt(); code != 0)
        return code;
    if (const int code = check_adapter_shutdown_releases_everything(); code != 0)
        return code;
    if (const int code = check_leases_released_counts_every_release_path(); code != 0)
        return code;
    if (const int code = check_structure_observation_aggregation(); code != 0)
        return code;
    if (const int code = check_structure_epoch_and_capability_degradation(); code != 0)
        return code;
    if (const int code = check_artifact_store_capacity_and_injection(); code != 0)
        return code;
    if (const int code = check_frame_payload_metadata_and_store_access(); code != 0)
        return code;
    return 0;
}
