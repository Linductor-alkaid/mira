#include "support/test.hpp"

#include <mira/observation_pipeline.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using mira::AppContext;
using mira::ComponentQuality;
using mira::DeviceState;
using mira::DisplayInfo;
using mira::DisplayTopology;
using mira::EnvironmentEpoch;
using mira::ErrorCode;
using mira::Observation;
using mira::ObservationAtomicity;
using mira::ObservationComponent;
using mira::ObservationPipeline;
using mira::ObservationPipelineConfig;
using mira::ObservationRequest;
using mira::OperationContext;
using mira::ScreenFrameDescriptor;
using mira::Timestamp;
using mira::UiTreeSnapshot;

[[nodiscard]] mira::Timestamp at_ms(std::int64_t ms) {
    Timestamp stamp;
    stamp.monotonic = std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
    stamp.wall = std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
    return stamp;
}

[[nodiscard]] mira::CaptureSpan span_at(std::int64_t begin_ms, std::int64_t end_ms) {
    mira::CaptureSpan span;
    span.normalized_begin = at_ms(begin_ms);
    span.normalized_end = at_ms(end_ms);
    return span;
}

OperationContext context_with_deadline(std::chrono::milliseconds budget) {
    OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = std::chrono::steady_clock::now() + budget;
    return context;
}

[[nodiscard]] mira::Result<DisplayTopology> static_topology(EnvironmentEpoch epoch) {
    DisplayInfo info;
    info.id = mira::DisplayId::generate();
    info.native_width_pixels = 100;
    info.native_height_pixels = 100;
    info.logical_width = 100.0;
    info.logical_height = 100.0;
    return mira::make_display_topology(epoch, {info});
}

[[nodiscard]] ObservationComponent<DeviceState> device_component(EnvironmentEpoch epoch,
                                                                 std::int64_t skew_ms = 0) {
    ObservationComponent<DeviceState> component;
    component.value.battery_percent = 50;
    component.capture = span_at(skew_ms, skew_ms + 5);
    component.environment_epoch = epoch;
    component.provenance.source = "pipeline.test.device";
    return component;
}

[[nodiscard]] ObservationComponent<AppContext> foreground_component(EnvironmentEpoch epoch) {
    ObservationComponent<AppContext> component;
    component.value.package_name = "test.app";
    component.capture = span_at(0, 5);
    component.environment_epoch = epoch;
    component.provenance.source = "pipeline.test.foreground";
    return component;
}

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

int check_settlement_and_quality() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    const EnvironmentEpoch epoch = 7;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &) {
        return device_component(epoch);
    });
    pipeline.set_foreground_source([&](const ObservationRequest &, const OperationContext &) {
        return foreground_component(epoch);
    });
    std::atomic<std::uint64_t> published{0};
    pipeline.set_publish_observer([&published](const Observation &) { published.fetch_add(1); });

    ObservationRequest request;
    request.required.device = true;
    request.optional.foreground = true;
    request.max_component_skew = std::chrono::milliseconds(10);

    const auto observation =
        pipeline.observe(request, context_with_deadline(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    MIRA_CHECK(observation.value().environment_epoch == epoch);
    MIRA_CHECK(observation.value().device.has_value());
    MIRA_CHECK(observation.value().foreground.has_value());
    // Skew of 0 ms is within the request bound, but the pipeline cannot
    // prove a platform transaction, so Atomic is never claimed.
    MIRA_CHECK(observation.value().atomicity == ObservationAtomicity::BoundedSkew);
    MIRA_CHECK(observation.value().quality.overall == ComponentQuality::Good);
    MIRA_CHECK(published.load() == 1);

    const auto stats = pipeline.stats();
    MIRA_CHECK(stats.observations_published == 1);
    MIRA_CHECK(stats.components_settled == 2);
    MIRA_CHECK(stats.components_timed_out == 0);
    MIRA_CHECK(stats.stale_components_dropped == 0);
    return 0;
}

int check_stale_epoch_components_dropped() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    const EnvironmentEpoch epoch = 3;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    // The device component reports an older epoch than the topology.
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &) {
        return device_component(epoch - 1);
    });

    ObservationRequest request;
    request.required.device = true;
    const auto observation =
        pipeline.observe(request, context_with_deadline(std::chrono::seconds(2)));
    // The stale component was dropped and the required request fails closed.
    MIRA_CHECK(!observation.has_value());
    MIRA_CHECK(observation.error().code == ErrorCode::InvalidObservation);
    MIRA_CHECK(pipeline.stats().stale_components_dropped == 1);
    return 0;
}

int check_deadline_partial_settlement() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    const EnvironmentEpoch epoch = 11;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    // The device source ignores both deadline and cancellation, so it must
    // be parked and counted instead of blocking the settlement.
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return device_component(epoch);
    });
    pipeline.set_foreground_source([&](const ObservationRequest &, const OperationContext &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return foreground_component(epoch);
    });

    ObservationRequest request;
    request.required.device = true;
    request.optional.foreground = true;
    const auto failed =
        pipeline.observe(request, context_with_deadline(std::chrono::milliseconds(60)));
    MIRA_CHECK(!failed.has_value());
    MIRA_CHECK(failed.error().code == ErrorCode::InvalidObservation);
    MIRA_CHECK(pipeline.stats().components_timed_out >= 1);

    // The straggler settles after cancellation; a later drain consumes it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto drained = pipeline.drain_pending(std::chrono::milliseconds(500));
    MIRA_CHECK(drained.has_value());
    MIRA_CHECK(drained.value() >= 1);
    return 0;
}

int check_partial_publication_mode() {
    ExecutorFixture fixture;
    ObservationPipelineConfig config;
    config.fail_on_missing_required = false;
    ObservationPipeline pipeline(fixture.executor, config);
    const EnvironmentEpoch epoch = 5;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return device_component(epoch);
    });

    ObservationRequest request;
    request.required.device = true;
    request.max_component_skew = std::chrono::milliseconds(1);
    const auto partial =
        pipeline.observe(request, context_with_deadline(std::chrono::milliseconds(50)));
    // Explicit partial settlement: the observation is published with the
    // required component missing and the quality says so.
    MIRA_CHECK(partial.has_value());
    MIRA_CHECK(!partial.value().device.has_value());
    MIRA_CHECK(partial.value().quality.overall == ComponentQuality::Unavailable);
    bool missing_noted = false;
    for (const std::string &entry : partial.value().quality.degradations) {
        if (entry.find("device") != std::string::npos) {
            missing_noted = true;
        }
    }
    MIRA_CHECK(missing_noted);
    const auto evaluation = mira::evaluate_observation(partial.value(), request, Timestamp::now());
    MIRA_CHECK(!evaluation.satisfies_request);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto drained = pipeline.drain_pending(std::chrono::milliseconds(500));
    MIRA_CHECK(drained.has_value());
    return 0;
}

int check_cancellation_fails_fast() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    const EnvironmentEpoch epoch = 2;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &context) {
        while (!context.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return device_component(epoch);
    });

    ObservationRequest request;
    request.required.device = true;
    OperationContext context = context_with_deadline(std::chrono::seconds(5));
    std::atomic<bool> cancelled{false};
    context.cancellation_requested = [&cancelled]() noexcept {
        return cancelled.load(std::memory_order_acquire);
    };
    // Simulate cancellation racing with an in-flight capture.
    auto late = fixture.executor.submit_auto([&pipeline, request, context]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pipeline.observe(request, context);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancelled.store(true, std::memory_order_release);
    const auto outcome = late.get();
    MIRA_CHECK(!outcome.has_value());
    MIRA_CHECK(outcome.error().code == ErrorCode::Cancelled);
    const auto drained = pipeline.drain_pending(std::chrono::milliseconds(500));
    MIRA_CHECK(drained.has_value());
    return 0;
}

int check_observer_failure_is_isolated() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    const EnvironmentEpoch epoch = 9;
    pipeline.set_topology_source([&](const ObservationRequest &, const OperationContext &) {
        return static_topology(epoch);
    });
    pipeline.set_device_source([&](const ObservationRequest &, const OperationContext &) {
        return device_component(epoch);
    });
    pipeline.set_publish_observer(
        [](const Observation &) { throw std::runtime_error("observer"); });

    ObservationRequest request;
    request.required.device = true;
    const auto observation =
        pipeline.observe(request, context_with_deadline(std::chrono::seconds(2)));
    MIRA_CHECK(observation.has_value());
    MIRA_CHECK(pipeline.stats().publish_callback_failures == 1);
    return 0;
}

int check_missing_source_and_bad_request() {
    ExecutorFixture fixture;
    ObservationPipeline pipeline(fixture.executor);
    pipeline.set_topology_source(
        [&](const ObservationRequest &, const OperationContext &) { return static_topology(1); });

    ObservationRequest request;
    request.required.structure = true;
    const auto unsupported =
        pipeline.observe(request, context_with_deadline(std::chrono::seconds(1)));
    MIRA_CHECK(!unsupported.has_value());
    MIRA_CHECK(unsupported.error().code == ErrorCode::UnsupportedCapability);

    ObservationRequest roi;
    roi.mode = mira::ObservationMode::Roi;
    const auto invalid = pipeline.observe(roi, context_with_deadline(std::chrono::seconds(1)));
    MIRA_CHECK(!invalid.has_value());
    MIRA_CHECK(invalid.error().code == ErrorCode::InvalidArgument);

    ObservationRequest fine;
    fine.optional.device = true;
    const auto ok = pipeline.observe(fine, context_with_deadline(std::chrono::seconds(1)));
    MIRA_CHECK(ok.has_value());
    return 0;
}

} // namespace

int main() {
    if (const int code = check_settlement_and_quality(); code != 0)
        return code;
    if (const int code = check_stale_epoch_components_dropped(); code != 0)
        return code;
    if (const int code = check_deadline_partial_settlement(); code != 0)
        return code;
    if (const int code = check_partial_publication_mode(); code != 0)
        return code;
    if (const int code = check_cancellation_fails_fast(); code != 0)
        return code;
    if (const int code = check_observer_failure_is_isolated(); code != 0)
        return code;
    if (const int code = check_missing_source_and_bad_request(); code != 0)
        return code;
    return 0;
}
