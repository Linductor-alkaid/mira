#include <mira/adapters/android/android_host_adapter.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace mira::adapters::android {
namespace {

Error adapter_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.android.adapter";
    error.safe_message = std::move(message);
    return error;
}

constexpr std::chrono::milliseconds kWaitSlice{2};

Id128 id_from_u64(std::uint64_t value) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFU);
    }
    bytes[15] = 0xB7;
    return Id128{bytes};
}

std::uint64_t u64_from_id(const Id128 &id) {
    const Id128::Bytes &bytes = id.bytes();
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::optional<PixelFormat> host_pixel_format_to_mira(std::uint32_t format) {
    switch (format) {
    case MIRA_HOST_PIXEL_RGBA8888:
        return PixelFormat::RGBA8888;
    case MIRA_HOST_PIXEL_BGRA8888:
        return PixelFormat::BGRA8888;
    case MIRA_HOST_PIXEL_RGBX8888:
        return PixelFormat::RGBX8888;
    case MIRA_HOST_PIXEL_NV12:
        return PixelFormat::NV12;
    case MIRA_HOST_PIXEL_YUV420P:
        return PixelFormat::YUV420P;
    case MIRA_HOST_PIXEL_GRAY8:
        return PixelFormat::Gray8;
    }
    return std::nullopt;
}

std::optional<Rotation> host_rotation_to_mira(std::uint32_t rotation) {
    switch (rotation) {
    case MIRA_HOST_ROTATION_0:
        return Rotation::Rotation0;
    case MIRA_HOST_ROTATION_90:
        return Rotation::Rotation90;
    case MIRA_HOST_ROTATION_180:
        return Rotation::Rotation180;
    case MIRA_HOST_ROTATION_270:
        return Rotation::Rotation270;
    }
    return std::nullopt;
}

// Bounded cooperative wait for a bridge future: polls the caller-supplied
// cancellation probe while waiting for the host settlement.
template <typename T>
Result<T> wait_for_outcome(std::future<Result<T>> &future, const OperationContext &context,
                           MiraAndroidHostV1 *host, HostDispatcherBridge &bridge) {
    const auto deadline =
        context.deadline.value_or(std::chrono::steady_clock::now() + std::chrono::seconds(10));
    bool cancellation_sent = false;
    auto cancel_grace = std::chrono::steady_clock::time_point{};
    while (true) {
        if (future.wait_for(kWaitSlice) == std::future_status::ready) {
            return future.get();
        }
        const auto now = std::chrono::steady_clock::now();
        if (context.cancelled()) {
            if (!cancellation_sent) {
                static_cast<void>(bridge.cancel_outstanding(host));
                cancellation_sent = true;
                cancel_grace = now + std::chrono::milliseconds(200);
            }
            if (now >= cancel_grace) {
                return adapter_error(ErrorCode::Cancelled,
                                     "host operation was cancelled before settlement");
            }
            continue;
        }
        if (now >= deadline) {
            static_cast<void>(bridge.cancel_outstanding(host));
            // Give a cancelled operation a short, bounded window to deliver
            // its terminal callback before failing the caller.
            const auto grace = deadline + std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < grace) {
                if (future.wait_for(kWaitSlice) == std::future_status::ready) {
                    return future.get();
                }
            }
            return adapter_error(ErrorCode::DeadlineExceeded,
                                 "host operation did not settle before the deadline");
        }
    }
}

bool parse_canonical_pair(const std::string &payload, double &x, double &y) {
    std::string normalized = payload;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream stream(normalized);
    std::vector<double> values;
    double value = 0.0;
    while (stream >> value) {
        values.push_back(value);
    }
    std::string extra;
    if (stream >> extra || values.size() != 2) {
        return false;
    }
    for (const double coordinate : values) {
        if (!std::isfinite(coordinate) || coordinate < 0.0 || coordinate > 1.0) {
            return false;
        }
    }
    x = values[0];
    y = values[1];
    return true;
}

bool parse_canonical_quad(const std::string &payload, double &x1, double &y1, double &x2,
                          double &y2) {
    std::string normalized = payload;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream stream(normalized);
    std::vector<double> values;
    double value = 0.0;
    while (stream >> value) {
        values.push_back(value);
    }
    std::string extra;
    if (stream >> extra || values.size() != 4) {
        return false;
    }
    for (const double coordinate : values) {
        if (!std::isfinite(coordinate) || coordinate < 0.0 || coordinate > 1.0) {
            return false;
        }
    }
    x1 = values[0];
    y1 = values[1];
    x2 = values[2];
    y2 = values[3];
    return true;
}

} // namespace

Result<std::unique_ptr<AndroidHostAdapter>>
AndroidHostAdapter::create(executor::Executor &executor) {
    auto adapter = std::unique_ptr<AndroidHostAdapter>(new AndroidHostAdapter(executor, nullptr));
    MiraAndroidHostConfigV1 config{};
    config.struct_size = sizeof(MiraAndroidHostConfigV1);
    config.abi_version = MIRA_ANDROID_ABI_VERSION;
    MiraAndroidHostV1 *host = nullptr;
    const MiraHostStatus created =
        mira_android_host_create_v1(&config, adapter->bridge_.host_callbacks(), &host);
    if (created != MIRA_HOST_OK) {
        return adapter_error(ErrorCode::PlatformError, "host create failed");
    }
    adapter->host_ = host;
    const MiraHostStatus started = mira_android_host_start_v1(host);
    if (started != MIRA_HOST_OK) {
        static_cast<void>(mira_android_host_destroy_v1(host));
        adapter->host_ = nullptr;
        return adapter_error(ErrorCode::PlatformError, "host start failed");
    }
    MiraHostCapabilitiesV1 capabilities{};
    const MiraHostStatus probed = mira_android_host_get_capabilities_v1(host, &capabilities);
    if (probed != MIRA_HOST_OK) {
        static_cast<void>(mira_android_host_stop_v1(host));
        static_cast<void>(mira_android_host_destroy_v1(host));
        adapter->host_ = nullptr;
        return adapter_error(ErrorCode::PlatformError, "host capability probe failed");
    }
    adapter->bridge_.note_capabilities(capabilities);
    return adapter;
}

AndroidHostAdapter::AndroidHostAdapter(executor::Executor &executor, MiraAndroidHostV1 *host)
    : executor_(executor), bridge_(executor), host_(host) {}

AndroidHostAdapter::~AndroidHostAdapter() {
    if (host_ != nullptr) {
        static_cast<void>(mira_android_host_stop_v1(host_));
        static_cast<void>(mira_android_host_destroy_v1(host_));
        host_ = nullptr;
    }
}

EnvironmentCapabilities AndroidHostAdapter::capabilities() const {
    EnvironmentCapabilities capabilities;
    const auto latest = bridge_.latest_capabilities();
    if (!latest.has_value()) {
        return capabilities;
    }
    capabilities.screen_capture = latest->screenshot_pixel_formats_mask != 0;
    capabilities.discrete_input = latest->input_capabilities_mask != 0;
    capabilities.input_release =
        (latest->input_capabilities_mask &
         (1U << static_cast<std::uint32_t>(MIRA_HOST_INPUT_RELEASE_ALL))) != 0;
    capabilities.epoch_invalidation = true;
    return capabilities;
}

Result<Observation> AndroidHostAdapter::observe(const ObservationRequest &request,
                                                const OperationContext &context) {
    if (const auto validated = validate_observation_request(request); !validated) {
        return validated.error();
    }
    if (context.cancelled()) {
        return adapter_error(ErrorCode::Cancelled, "observe was cancelled before capture");
    }
    const auto snapshot = bridge_.latest_capabilities();
    if (!snapshot.has_value()) {
        return adapter_error(ErrorCode::InvalidState, "adapter has no capability snapshot");
    }
    const EnvironmentCapabilities declared = capabilities();
    if (const auto unsupported = unsupported_required_components(declared, request);
        !unsupported.empty()) {
        std::string message = "environment cannot provide required components: ";
        for (std::size_t index = 0; index < unsupported.size(); ++index) {
            if (index != 0) {
                message += ", ";
            }
            message += unsupported[index];
        }
        return adapter_error(ErrorCode::UnsupportedCapability, message);
    }
    if (!request.required.screen && !request.optional.screen) {
        return adapter_error(ErrorCode::UnsupportedCapability,
                             "the M2 host skeleton only captures screen components");
    }

    MiraHostTopologyV1 topology{};
    const MiraHostStatus topology_status = mira_android_host_get_topology_v1(host_, &topology);
    if (topology_status != MIRA_HOST_OK) {
        return adapter_error(ErrorCode::InvalidObservation, "host topology was unavailable");
    }

    MiraHostFrameRequestV1 frame_request{};
    frame_request.struct_size = sizeof(MiraHostFrameRequestV1);
    if (topology.display_count > 0) {
        frame_request.display_id = topology.displays[0].display_id;
    }
    if (context.deadline.has_value()) {
        frame_request.deadline_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           context.deadline->time_since_epoch())
                                           .count());
    }

    auto future = bridge_.capture_frame(host_, frame_request);
    auto settled = wait_for_outcome(future, context, host_, bridge_);
    if (!settled) {
        return settled.error();
    }
    HostFrameOutcome outcome = std::move(settled).value();

    // A frame captured under an older epoch carries coordinates Mira can no
    // longer vouch for; the lease is released and the observation fails.
    // The comparison uses the freshest capability snapshot so a rotation
    // that landed while the capture was in flight is still detected.
    const auto fresh = bridge_.latest_capabilities();
    const std::uint64_t current_epoch =
        fresh.has_value() ? fresh->environment_epoch : snapshot->environment_epoch;
    if (outcome.environment_epoch != current_epoch) {
        outcome.lease.release();
        return adapter_error(ErrorCode::StaleObservation,
                             "host epoch changed during frame capture");
    }

    const auto pixel_format = host_pixel_format_to_mira(outcome.pixel_format);
    const auto rotation = host_rotation_to_mira(outcome.rotation);
    if (!pixel_format.has_value() || !rotation.has_value()) {
        return adapter_error(ErrorCode::InvalidObservation, "frame format was not usable");
    }

    // Fail closed on any plane layout that escapes the leased buffer.
    const MiraHostBufferLeaseV1 &lease = outcome.lease.lease();
    for (std::uint32_t index = 0; index < lease.plane_count; ++index) {
        const MiraPlaneV1 &plane = lease.planes[index];
        const std::uint64_t stride_span =
            static_cast<std::uint64_t>(plane.row_stride) * static_cast<std::uint64_t>(plane.height);
        const std::uint64_t row_extent = static_cast<std::uint64_t>(plane.width) *
                                         static_cast<std::uint64_t>(plane.pixel_stride);
        if (plane.width == 0 || plane.height == 0 || plane.width > outcome.width ||
            plane.height > outcome.height || row_extent > plane.row_stride ||
            stride_span > lease.size || plane.offset > lease.size - stride_span) {
            return adapter_error(ErrorCode::InvalidObservation,
                                 "frame plane layout escapes the leased buffer");
        }
    }

    ScreenFrameDescriptor descriptor;
    descriptor.frame_id = FrameId{id_from_u64(outcome.frame_id)};
    descriptor.display_id = DisplayId{id_from_u64(outcome.display_id)};
    descriptor.width_pixels = outcome.width;
    descriptor.height_pixels = outcome.height;
    descriptor.pixel_format = *pixel_format;
    descriptor.color_space = ColorSpace::SRGB;
    descriptor.alpha_mode = AlphaMode::Opaque;
    descriptor.native_rotation = *rotation;
    for (std::uint32_t index = 0; index < lease.plane_count; ++index) {
        const MiraPlaneV1 &plane = lease.planes[index];
        descriptor.planes.push_back(PlaneLayout{plane.offset, plane.row_stride, plane.pixel_stride,
                                                plane.width, plane.height});
    }
    descriptor.pixel_space = CoordinateSpaceId{id_from_u64(0xF1A7ULL + outcome.display_id)};
    descriptor.capture.clock_domain = ClockDomainId::generate();
    const auto begin_ticks =
        std::chrono::steady_clock::duration(std::chrono::nanoseconds(outcome.capture_begin_ns));
    const auto end_ticks =
        std::chrono::steady_clock::duration(std::chrono::nanoseconds(outcome.capture_end_ns));
    descriptor.capture.normalized_begin.monotonic =
        std::chrono::steady_clock::time_point(begin_ticks);
    descriptor.capture.normalized_end.monotonic = std::chrono::steady_clock::time_point(end_ticks);
    descriptor.capture.sync_quality = ClockSyncQuality::Estimated;
    descriptor.coverage.includes_system_bars = snapshot->secure_surface_policy == 0;

    ArtifactWriteSpec spec;
    spec.media_type = "image/x-host-frame";
    auto writer = artifacts_.begin(spec);
    if (!writer.has_value()) {
        return writer.error();
    }
    if (const auto written = writer.value().write(lease.data, lease.size); !written) {
        return written.error();
    }
    const auto committed = artifacts_.commit(writer.value());
    if (!committed) {
        return committed.error();
    }
    descriptor.payload_artifact = committed.value().id;
    if (const auto validated = validate_frame_descriptor(descriptor); !validated) {
        return validated.error();
    }
    outcome.lease.release();

    Observation observation;
    observation.id = ObservationId::generate();
    observation.session_id = context.session;
    observation.environment_epoch = outcome.environment_epoch;
    observation.aggregate_span.normalized_begin = descriptor.capture.normalized_begin;
    observation.aggregate_span.normalized_end = descriptor.capture.normalized_end;
    observation.aggregate_span.sync_quality = ClockSyncQuality::Estimated;
    // One component captured through an async host callback: no platform
    // transaction is proven, so the observation is never Atomic.
    observation.atomicity = ObservationAtomicity::BoundedSkew;
    {
        std::vector<DisplayInfo> displays;
        for (std::uint32_t index = 0;
             index < topology.display_count && index < MIRA_MAX_TOPOLOGY_DISPLAYS; ++index) {
            const MiraHostDisplayV1 &host_display = topology.displays[index];
            DisplayInfo info;
            info.id = DisplayId{id_from_u64(host_display.display_id)};
            info.name = "android-host-" + info.id.to_string();
            info.native_width_pixels = host_display.native_width;
            info.native_height_pixels = host_display.native_height;
            const auto host_rotation = host_rotation_to_mira(host_display.rotation);
            info.native_rotation = host_rotation.value_or(Rotation::Unknown);
            info.density_scale =
                host_display.pixels_per_logical > 0.0 ? 1.0 / host_display.pixels_per_logical : 1.0;
            const bool rotated = info.native_rotation == Rotation::Rotation90 ||
                                 info.native_rotation == Rotation::Rotation270;
            info.logical_width = (rotated ? static_cast<double>(host_display.native_height)
                                          : static_cast<double>(host_display.native_width)) *
                                 info.density_scale;
            info.logical_height = (rotated ? static_cast<double>(host_display.native_width)
                                           : static_cast<double>(host_display.native_height)) *
                                  info.density_scale;
            info.system_insets = Insets{host_display.inset_left, host_display.inset_top,
                                        host_display.inset_right, host_display.inset_bottom};
            info.active = host_display.active != 0;
            displays.push_back(info);
        }
        auto topology_result =
            make_display_topology(outcome.environment_epoch, std::move(displays));
        if (!topology_result) {
            return topology_result.error();
        }
        observation.topology = std::move(topology_result).value();
    }
    observation.screen = ObservationComponent<ScreenFrameDescriptor>{
        std::move(descriptor), observation.aggregate_span, ComponentQuality::Good,
        Provenance{"android.host.frame.v1", "host-capture", std::nullopt},
        outcome.environment_epoch};
    const auto evaluation = evaluate_observation(observation, request, Timestamp::now());
    observation.quality.overall = ComponentQuality::Good;
    observation.quality.components_skewed = false;
    for (const std::string &deficiency : evaluation.deficiencies) {
        observation.quality.degradations.push_back(deficiency);
    }
    return observation;
}

Result<ExecutionReceipt> AndroidHostAdapter::execute(const InputSequence &input,
                                                     const OperationContext &context) {
    if (input.events.empty()) {
        return adapter_error(ErrorCode::InvalidArgument, "input sequence must not be empty");
    }
    if (context.cancelled()) {
        return adapter_error(ErrorCode::Cancelled, "execute was cancelled before dispatch");
    }
    if (input.events.size() > MIRA_MAX_INPUT_EVENTS) {
        return adapter_error(ErrorCode::InvalidArgument, "input sequence exceeds host bounds");
    }
    const auto snapshot = bridge_.latest_capabilities();
    if (!snapshot.has_value()) {
        return adapter_error(ErrorCode::InvalidState, "adapter has no capability snapshot");
    }

    MiraHostTopologyV1 topology{};
    if (mira_android_host_get_topology_v1(host_, &topology) != MIRA_HOST_OK ||
        topology.display_count == 0) {
        return adapter_error(ErrorCode::InvalidObservation, "host topology was unavailable");
    }

    MiraHostInputRequestV1 request{};
    request.struct_size = sizeof(MiraHostInputRequestV1);
    if (input.display.has_value()) {
        const std::uint64_t target_id = u64_from_id(input.display->value);
        bool found = false;
        for (std::uint32_t index = 0;
             index < topology.display_count && index < MIRA_MAX_TOPOLOGY_DISPLAYS; ++index) {
            if (topology.displays[index].display_id == target_id &&
                topology.displays[index].active != 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return adapter_error(ErrorCode::InvalidArgument,
                                 "input targets an unknown or inactive display");
        }
        request.display_id = target_id;
    } else {
        request.display_id = topology.displays[0].display_id;
    }
    request.event_count = static_cast<std::uint32_t>(input.events.size());
    for (std::size_t index = 0; index < input.events.size(); ++index) {
        const InputEvent &event = input.events[index];
        MiraHostInputEventV1 &target = request.events[index];
        if (event.kind == "tap") {
            target.kind = MIRA_HOST_INPUT_TAP;
            if (!parse_canonical_pair(event.payload, target.x, target.y)) {
                return adapter_error(ErrorCode::InvalidArgument, "tap payload was invalid");
            }
        } else if (event.kind == "long_press") {
            target.kind = MIRA_HOST_INPUT_LONG_PRESS;
            if (!parse_canonical_pair(event.payload, target.x, target.y)) {
                return adapter_error(ErrorCode::InvalidArgument, "long_press payload was invalid");
            }
        } else if (event.kind == "swipe") {
            target.kind = MIRA_HOST_INPUT_SWIPE;
            if (!parse_canonical_quad(event.payload, target.x, target.y, target.x2, target.y2)) {
                return adapter_error(ErrorCode::InvalidArgument, "swipe payload was invalid");
            }
        } else if (event.kind == "type") {
            target.kind = MIRA_HOST_INPUT_TYPE;
            if (event.payload.empty()) {
                return adapter_error(ErrorCode::InvalidArgument, "type event must carry text");
            }
            target.text = event.payload.data();
            target.text_length = static_cast<std::uint32_t>(event.payload.size());
        } else if (event.kind == "back") {
            target.kind = MIRA_HOST_INPUT_BACK;
        } else if (event.kind == "home") {
            target.kind = MIRA_HOST_INPUT_HOME;
        } else {
            return adapter_error(ErrorCode::InvalidArgument, "unknown input kind");
        }
    }
    if (context.deadline.has_value()) {
        request.deadline_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           context.deadline->time_since_epoch())
                                           .count());
    }

    auto future = bridge_.dispatch_input(host_, request);
    auto settled = wait_for_outcome(future, context, host_, bridge_);
    if (!settled) {
        return settled.error();
    }
    const HostInputOutcome outcome = std::move(settled).value();
    ExecutionReceipt receipt;
    switch (outcome.receipt) {
    case MIRA_HOST_INPUT_RECEIPT_DISPATCHED:
        receipt.status = ExecutionStatus::Dispatched;
        break;
    case MIRA_HOST_INPUT_RECEIPT_COMPLETED:
        receipt.status = ExecutionStatus::Completed;
        break;
    case MIRA_HOST_INPUT_RECEIPT_REJECTED:
        receipt.status = ExecutionStatus::Rejected;
        break;
    case MIRA_HOST_INPUT_RECEIPT_UNKNOWN:
        receipt.status = ExecutionStatus::Unknown;
        break;
    default:
        return adapter_error(ErrorCode::Internal, "host returned an unknown input receipt");
    }
    receipt.side_effect_may_have_occurred = outcome.side_effect_may_have_occurred != 0;
    receipt.environment_epoch = outcome.environment_epoch;
    receipt.safe_message = "host input receipt " + std::to_string(outcome.receipt);
    return receipt;
}

Result<void> AndroidHostAdapter::interrupt(const OperationContext & /*context*/) {
    static_cast<void>(bridge_.cancel_outstanding(host_));
    return Result<void>{};
}

HostBridgeStats AndroidHostAdapter::bridge_stats() const { return bridge_.stats(); }

EnvironmentEpoch AndroidHostAdapter::environment_epoch() const {
    const auto snapshot = bridge_.latest_capabilities();
    return snapshot.has_value() ? snapshot->environment_epoch : 0;
}

HostDispatcherBridge &AndroidHostAdapter::bridge() { return bridge_; }

} // namespace mira::adapters::android
