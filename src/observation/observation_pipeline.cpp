#include <mira/observation_pipeline.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mira {

namespace {

Error pipeline_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.observation.pipeline";
    error.safe_message = std::move(message);
    return error;
}

template <typename T> struct PendingSlot final {
    std::future<Result<ObservationComponent<T>>> future;
    std::shared_ptr<std::atomic_bool> cancel;
};

template <typename T>
void consume_component(Result<ObservationComponent<T>> &&result,
                       std::optional<ObservationComponent<T>> &destination, bool required,
                       const char *name, EnvironmentEpoch epoch,
                       std::vector<std::string> &missing_required,
                       std::vector<std::string> &missing_optional,
                       ObservationPipelineStats &stats) {
    if (!result.has_value()) {
        ++stats.components_failed;
        (required ? missing_required : missing_optional).emplace_back(name);
        return;
    }
    ObservationComponent<T> component = std::move(result).value();
    if (component.environment_epoch != epoch) {
        // A component captured under an older epoch carries coordinates Mira
        // can no longer vouch for; it is dropped, never mixed in.
        ++stats.stale_components_dropped;
        (required ? missing_required : missing_optional).emplace_back(name);
        return;
    }
    ++stats.components_settled;
    destination = std::move(component);
}

ComponentQuality worst_present_quality(const Observation &observation) {
    const auto rank = [](ComponentQuality quality) noexcept {
        switch (quality) {
        case ComponentQuality::Good:
            return 0;
        case ComponentQuality::Degraded:
            return 1;
        case ComponentQuality::Partial:
            return 2;
        case ComponentQuality::Unavailable:
            break;
        }
        return 3;
    };
    ComponentQuality overall = ComponentQuality::Good;
    int overall_rank = 0;
    const auto consider = [&](const ComponentQuality quality) {
        const int quality_rank = rank(quality);
        if (quality_rank > overall_rank) {
            overall_rank = quality_rank;
            overall = quality;
        }
    };
    if (observation.screen.has_value()) {
        consider(observation.screen->quality);
    }
    if (observation.structure.has_value()) {
        consider(observation.structure->quality);
    }
    if (observation.foreground.has_value()) {
        consider(observation.foreground->quality);
    }
    if (observation.device.has_value()) {
        consider(observation.device->quality);
    }
    return overall;
}

} // namespace

class ObservationPipeline::Impl final {
  public:
    Impl(executor::Executor &executor_ref, ObservationPipelineConfig config_ref)
        : executor(executor_ref), config(config_ref) {}

    executor::Executor &executor;
    ObservationPipelineConfig config;

    mutable std::mutex mutex;
    TopologySource topology_source;
    ObservationComponentSource<ScreenFrameDescriptor> screen_source;
    ObservationComponentSource<UiTreeSnapshot> structure_source;
    ObservationComponentSource<AppContext> foreground_source;
    ObservationComponentSource<DeviceState> device_source;
    std::function<void(const Observation &)> publish_observer;
    ObservationPipelineStats stats;

    std::vector<PendingSlot<ScreenFrameDescriptor>> pending_screen;
    std::vector<PendingSlot<UiTreeSnapshot>> pending_structure;
    std::vector<PendingSlot<AppContext>> pending_foreground;
    std::vector<PendingSlot<DeviceState>> pending_device;

    template <typename T>
    std::optional<PendingSlot<T>> submit(const ObservationComponentSource<T> &source,
                                         const ObservationRequest &request,
                                         const OperationContext &context) {
        auto cancel = std::make_shared<std::atomic_bool>(false);
        OperationContext source_context = context;
        source_context.cancellation_requested = [cancel]() noexcept {
            return cancel->load(std::memory_order_acquire);
        };
        try {
            auto future = executor.submit_auto([source, request, source_context]() mutable {
                return source(request, source_context);
            });
            return PendingSlot<T>{std::move(future), std::move(cancel)};
        } catch (const executor::CapacityExhaustedException &) {
            ++stats.submission_rejections;
            return std::nullopt;
        } catch (const executor::ExecutorStopping &) {
            ++stats.submission_rejections;
            return std::nullopt;
        } catch (...) {
            ++stats.submission_rejections;
            return std::nullopt;
        }
    }

    // Waits for the slot until `until`; after signalling cooperative
    // cancellation, parks still-unfinished captures for later drains.
    template <typename T>
    std::optional<Result<ObservationComponent<T>>>
    settle(PendingSlot<T> slot, std::vector<PendingSlot<T>> &pending,
           std::chrono::steady_clock::time_point until, bool &timed_out) {
        if (slot.future.wait_until(until) != std::future_status::ready) {
            slot.cancel->store(true, std::memory_order_release);
            if (slot.future.wait_until(until + config.drain_grace) != std::future_status::ready) {
                ++stats.components_timed_out;
                timed_out = true;
                pending.push_back(std::move(slot));
                return std::nullopt;
            }
        }
        try {
            return slot.future.get();
        } catch (const std::exception &) {
            return pipeline_error(ErrorCode::Internal, "component capture task failed");
        } catch (...) {
            return pipeline_error(ErrorCode::Internal, "component capture task failed");
        }
    }

    template <typename T> void cancel_pending(std::vector<PendingSlot<T>> &pending) {
        for (PendingSlot<T> &slot : pending) {
            if (slot.cancel) {
                slot.cancel->store(true, std::memory_order_release);
            }
        }
    }

    template <typename T>
    std::size_t drain(std::vector<PendingSlot<T>> &pending, std::chrono::milliseconds timeout) {
        std::size_t consumed = 0;
        for (auto iter = pending.begin(); iter != pending.end();) {
            if (iter->future.wait_for(timeout) == std::future_status::ready) {
                try {
                    static_cast<void>(iter->future.get());
                } catch (...) {
                }
                iter = pending.erase(iter);
                ++consumed;
            } else {
                ++iter;
            }
        }
        return consumed;
    }
};

ObservationPipeline::ObservationPipeline(executor::Executor &executor,
                                         ObservationPipelineConfig config)
    : impl_(std::make_unique<Impl>(executor, config)) {}

ObservationPipeline::~ObservationPipeline() {
    std::lock_guard lock(impl_->mutex);
    impl_->cancel_pending(impl_->pending_screen);
    impl_->cancel_pending(impl_->pending_structure);
    impl_->cancel_pending(impl_->pending_foreground);
    impl_->cancel_pending(impl_->pending_device);
    const auto grace = impl_->config.drain_grace;
    static_cast<void>(impl_->drain(impl_->pending_screen, grace));
    static_cast<void>(impl_->drain(impl_->pending_structure, grace));
    static_cast<void>(impl_->drain(impl_->pending_foreground, grace));
    static_cast<void>(impl_->drain(impl_->pending_device, grace));
}

void ObservationPipeline::set_topology_source(TopologySource source) {
    std::lock_guard lock(impl_->mutex);
    impl_->topology_source = std::move(source);
}

void ObservationPipeline::set_screen_source(
    ObservationComponentSource<ScreenFrameDescriptor> source) {
    std::lock_guard lock(impl_->mutex);
    impl_->screen_source = std::move(source);
}

void ObservationPipeline::set_structure_source(ObservationComponentSource<UiTreeSnapshot> source) {
    std::lock_guard lock(impl_->mutex);
    impl_->structure_source = std::move(source);
}

void ObservationPipeline::set_foreground_source(ObservationComponentSource<AppContext> source) {
    std::lock_guard lock(impl_->mutex);
    impl_->foreground_source = std::move(source);
}

void ObservationPipeline::set_device_source(ObservationComponentSource<DeviceState> source) {
    std::lock_guard lock(impl_->mutex);
    impl_->device_source = std::move(source);
}

void ObservationPipeline::set_publish_observer(std::function<void(const Observation &)> observer) {
    std::lock_guard lock(impl_->mutex);
    impl_->publish_observer = std::move(observer);
}

Result<Observation> ObservationPipeline::observe(const ObservationRequest &request,
                                                 const OperationContext &context) {
    if (const auto validated = validate_observation_request(request); !validated) {
        return validated.error();
    }
    std::lock_guard lock(impl_->mutex);
    if (context.cancelled()) {
        return pipeline_error(ErrorCode::Cancelled, "observation was cancelled before capture");
    }
    if (!impl_->topology_source) {
        return pipeline_error(ErrorCode::InvalidState, "pipeline has no topology source");
    }
    if (request.required.screen && !impl_->screen_source) {
        return pipeline_error(ErrorCode::UnsupportedCapability, "screen source is not configured");
    }
    if (request.required.structure && !impl_->structure_source) {
        return pipeline_error(ErrorCode::UnsupportedCapability,
                              "structure source is not configured");
    }
    if (request.required.foreground && !impl_->foreground_source) {
        return pipeline_error(ErrorCode::UnsupportedCapability,
                              "foreground source is not configured");
    }
    if (request.required.device && !impl_->device_source) {
        return pipeline_error(ErrorCode::UnsupportedCapability, "device source is not configured");
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        context.deadline.value_or(started + request.max_age + impl_->config.drain_grace);

    DisplayTopology topology;
    try {
        auto topology_result = impl_->topology_source(request, context);
        if (!topology_result) {
            return topology_result.error();
        }
        topology = std::move(topology_result).value();
    } catch (const std::exception &exception) {
        return pipeline_error(ErrorCode::Internal, exception.what());
    } catch (...) {
        return pipeline_error(ErrorCode::Internal, "topology source failed");
    }

    std::optional<PendingSlot<ScreenFrameDescriptor>> screen_slot;
    std::optional<PendingSlot<UiTreeSnapshot>> structure_slot;
    std::optional<PendingSlot<AppContext>> foreground_slot;
    std::optional<PendingSlot<DeviceState>> device_slot;
    if (request.required.screen || request.optional.screen) {
        screen_slot = impl_->submit(impl_->screen_source, request, context);
    }
    if (request.required.structure || request.optional.structure) {
        structure_slot = impl_->submit(impl_->structure_source, request, context);
    }
    if (request.required.foreground || request.optional.foreground) {
        foreground_slot = impl_->submit(impl_->foreground_source, request, context);
    }
    if (request.required.device || request.optional.device) {
        device_slot = impl_->submit(impl_->device_source, request, context);
    }

    const bool cancelled_mid_flight =
        context.cancelled() || std::chrono::steady_clock::now() > deadline;
    if (cancelled_mid_flight) {
        bool timed_out = false;
        if (screen_slot.has_value()) {
            impl_->settle(std::move(*screen_slot), impl_->pending_screen, deadline, timed_out);
        }
        if (structure_slot.has_value()) {
            impl_->settle(std::move(*structure_slot), impl_->pending_structure, deadline,
                          timed_out);
        }
        if (foreground_slot.has_value()) {
            impl_->settle(std::move(*foreground_slot), impl_->pending_foreground, deadline,
                          timed_out);
        }
        if (device_slot.has_value()) {
            impl_->settle(std::move(*device_slot), impl_->pending_device, deadline, timed_out);
        }
        return context.cancelled()
                   ? pipeline_error(ErrorCode::Cancelled, "observation was cancelled in flight")
                   : pipeline_error(ErrorCode::DeadlineExceeded,
                                    "observation deadline passed before capture settled");
    }

    Observation observation;
    observation.id = ObservationId::generate();
    observation.session_id = context.session;
    observation.topology = topology;
    observation.environment_epoch = topology.environment_epoch;

    std::vector<std::string> missing_required;
    std::vector<std::string> missing_optional;
    bool timed_out = false;

    if (screen_slot.has_value()) {
        auto result =
            impl_->settle(std::move(*screen_slot), impl_->pending_screen, deadline, timed_out);
        if (result.has_value()) {
            consume_component(std::move(*result), observation.screen, request.required.screen,
                              "screen", topology.environment_epoch, missing_required,
                              missing_optional, impl_->stats);
        } else {
            (request.required.screen ? missing_required : missing_optional).emplace_back("screen");
        }
    } else if (request.optional.screen) {
        missing_optional.emplace_back("screen");
    }

    if (structure_slot.has_value()) {
        auto result = impl_->settle(std::move(*structure_slot), impl_->pending_structure, deadline,
                                    timed_out);
        if (result.has_value()) {
            consume_component(std::move(*result), observation.structure, request.required.structure,
                              "structure", topology.environment_epoch, missing_required,
                              missing_optional, impl_->stats);
        } else {
            (request.required.structure ? missing_required : missing_optional)
                .emplace_back("structure");
        }
    } else if (request.optional.structure) {
        missing_optional.emplace_back("structure");
    }

    if (foreground_slot.has_value()) {
        auto result = impl_->settle(std::move(*foreground_slot), impl_->pending_foreground,
                                    deadline, timed_out);
        if (result.has_value()) {
            consume_component(std::move(*result), observation.foreground,
                              request.required.foreground, "foreground", topology.environment_epoch,
                              missing_required, missing_optional, impl_->stats);
        } else {
            (request.required.foreground ? missing_required : missing_optional)
                .emplace_back("foreground");
        }
    } else if (request.optional.foreground) {
        missing_optional.emplace_back("foreground");
    }

    if (device_slot.has_value()) {
        auto result =
            impl_->settle(std::move(*device_slot), impl_->pending_device, deadline, timed_out);
        if (result.has_value()) {
            consume_component(std::move(*result), observation.device, request.required.device,
                              "device", topology.environment_epoch, missing_required,
                              missing_optional, impl_->stats);
        } else {
            (request.required.device ? missing_required : missing_optional).emplace_back("device");
        }
    } else if (request.optional.device) {
        missing_optional.emplace_back("device");
    }

    // Cancellation may have arrived while captures were settling; a
    // cancelled observation is never published as a success.
    if (context.cancelled()) {
        return pipeline_error(ErrorCode::Cancelled, "observation was cancelled during settlement");
    }

    if (!missing_required.empty() && impl_->config.fail_on_missing_required) {
        std::string message = "missing required components: ";
        for (std::size_t index = 0; index < missing_required.size(); ++index) {
            if (index != 0) {
                message += ", ";
            }
            message += missing_required[index];
        }
        return pipeline_error(ErrorCode::InvalidObservation, message);
    }

    // Aggregate span: earliest begin to latest end across present components.
    std::optional<Timestamp> earliest;
    std::optional<Timestamp> latest;
    const auto consider = [&](const CaptureSpan &span) {
        if (!earliest.has_value() || span.normalized_begin.monotonic < earliest->monotonic) {
            earliest = span.normalized_begin;
        }
        if (!latest.has_value() || span.normalized_end.monotonic > latest->monotonic) {
            latest = span.normalized_end;
        }
    };
    if (observation.screen.has_value()) {
        consider(observation.screen->capture);
    }
    if (observation.structure.has_value()) {
        consider(observation.structure->capture);
    }
    if (observation.foreground.has_value()) {
        consider(observation.foreground->capture);
    }
    if (observation.device.has_value()) {
        consider(observation.device->capture);
    }
    const Timestamp publish_now = Timestamp::now();
    observation.aggregate_span.normalized_begin = earliest.value_or(publish_now);
    observation.aggregate_span.normalized_end = latest.value_or(publish_now);
    observation.aggregate_span.sync_quality = ClockSyncQuality::Unknown;

    // The pipeline aggregates independent captures; it cannot prove a
    // platform transaction, so it never claims Atomic.
    const auto skew = observation_component_skew(observation);
    observation.atomicity = skew <= request.max_component_skew ? ObservationAtomicity::BoundedSkew
                                                               : ObservationAtomicity::NonAtomic;

    observation.quality.overall = worst_present_quality(observation);
    if (!missing_required.empty()) {
        observation.quality.overall = ComponentQuality::Unavailable;
    }
    observation.quality.screen_missing =
        (request.required.screen || request.optional.screen) && !observation.screen.has_value();
    observation.quality.structure_truncated =
        observation.structure.has_value() &&
        (observation.structure->value.truncated ||
         observation.structure->quality == ComponentQuality::Partial);
    observation.quality.components_skewed =
        observation.atomicity == ObservationAtomicity::NonAtomic;
    if (timed_out) {
        observation.quality.degradations.emplace_back("component deadline exceeded");
    }
    for (const std::string &deficiency :
         evaluate_observation(observation, request, publish_now).deficiencies) {
        observation.quality.degradations.push_back(deficiency);
    }
    for (const std::string &name : missing_optional) {
        observation.quality.degradations.push_back("optional " + name + " unavailable");
    }

    ++impl_->stats.observations_published;
    if (impl_->publish_observer) {
        try {
            impl_->publish_observer(observation);
        } catch (...) {
            // Observer failures are isolated; they never fail the capture.
            ++impl_->stats.publish_callback_failures;
        }
    }
    return observation;
}

Result<std::size_t> ObservationPipeline::drain_pending(std::chrono::milliseconds timeout) {
    std::lock_guard lock(impl_->mutex);
    std::size_t consumed = 0;
    consumed += impl_->drain(impl_->pending_screen, timeout);
    consumed += impl_->drain(impl_->pending_structure, timeout);
    consumed += impl_->drain(impl_->pending_foreground, timeout);
    consumed += impl_->drain(impl_->pending_device, timeout);
    const std::size_t remaining = impl_->pending_screen.size() + impl_->pending_structure.size() +
                                  impl_->pending_foreground.size() + impl_->pending_device.size();
    if (remaining != 0) {
        return pipeline_error(ErrorCode::DeadlineExceeded,
                              std::to_string(remaining) + " straggler captures remain pending");
    }
    return consumed;
}

ObservationPipelineStats ObservationPipeline::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

} // namespace mira
