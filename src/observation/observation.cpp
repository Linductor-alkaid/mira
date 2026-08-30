#include <mira/observation.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace mira {

namespace {

[[nodiscard]] Error observation_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.observation";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] bool span_is_ordered(const CaptureSpan &span) noexcept {
    return span.normalized_begin.monotonic <= span.normalized_end.monotonic;
}

template <typename T>
[[nodiscard]] const T *component_value(const std::optional<ObservationComponent<T>> &component) {
    if (!component.has_value() || component->quality == ComponentQuality::Unavailable) {
        return nullptr;
    }
    return &component->value;
}

} // namespace

Result<ObservationMode> parse_observation_mode(std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t>(ObservationMode::Roi)) {
        return Error{ErrorCode::UnsupportedVersion, "mira.observation", 0,           false,
                     "unknown observation mode",    std::nullopt,       std::nullopt};
    }
    return static_cast<ObservationMode>(value);
}

Result<PixelFormat> parse_pixel_format(std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t>(PixelFormat::Unknown)) {
        return Error{ErrorCode::UnsupportedVersion, "mira.observation", 0,           false,
                     "unknown pixel format",        std::nullopt,       std::nullopt};
    }
    return static_cast<PixelFormat>(value);
}

Result<ColorSpace> parse_color_space(std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t>(ColorSpace::Unknown)) {
        return Error{ErrorCode::UnsupportedVersion, "mira.observation", 0,           false,
                     "unknown color space",         std::nullopt,       std::nullopt};
    }
    return static_cast<ColorSpace>(value);
}

std::uint32_t bytes_per_pixel_sample(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::RGBA8888:
    case PixelFormat::BGRA8888:
    case PixelFormat::RGBX8888:
        return 4U;
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
    case PixelFormat::Gray8:
        return 1U;
    case PixelFormat::Unknown:
        break;
    }
    return 0U;
}

std::uint32_t expected_plane_count(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::RGBA8888:
    case PixelFormat::BGRA8888:
    case PixelFormat::RGBX8888:
    case PixelFormat::Gray8:
        return 1U;
    case PixelFormat::NV12:
        return 2U;
    case PixelFormat::YUV420P:
        return 3U;
    case PixelFormat::Unknown:
        break;
    }
    return 0U;
}

Result<void> validate_observation_request(const ObservationRequest &request) {
    if (request.max_age < std::chrono::milliseconds::zero() ||
        request.max_component_skew < std::chrono::milliseconds::zero()) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "request age and skew bounds must be non-negative");
    }
    if (request.mode == ObservationMode::Roi) {
        if (request.regions.empty()) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "roi request must reference at least one region");
        }
        for (const RegionRef &region : request.regions) {
            if (region.source_observation.is_nil() || region.space.is_nil()) {
                return observation_error(ErrorCode::InvalidArgument,
                                         "roi region must reference an observation and space");
            }
            if (!region.region.is_well_formed()) {
                return observation_error(ErrorCode::InvalidArgument,
                                         "roi region must be well formed");
            }
        }
    }
    if (request.mode == ObservationMode::Diff && !request.baseline.has_value()) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "diff request must reference a baseline observation");
    }
    return Result<void>{};
}

Result<void> validate_frame_descriptor(const ScreenFrameDescriptor &descriptor) {
    if (descriptor.width_pixels == 0U || descriptor.height_pixels == 0U) {
        return observation_error(ErrorCode::InvalidArgument, "frame dimensions must be positive");
    }
    if (descriptor.pixel_format == PixelFormat::Unknown) {
        return observation_error(ErrorCode::InvalidArgument, "frame must declare a pixel format");
    }
    if (descriptor.native_rotation == Rotation::Unknown) {
        return observation_error(ErrorCode::InvalidArgument, "frame must declare rotation");
    }
    if (descriptor.display_id.is_nil() || descriptor.frame_id.is_nil() ||
        descriptor.pixel_space.is_nil() || descriptor.payload_artifact.is_nil()) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "frame ids, pixel space and payload artifact are required");
    }
    if (!span_is_ordered(descriptor.capture)) {
        return observation_error(ErrorCode::InvalidArgument, "capture span must be ordered");
    }

    const std::uint32_t expected_planes = expected_plane_count(descriptor.pixel_format);
    if (descriptor.planes.size() != expected_planes) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "frame plane count does not match pixel format");
    }
    const std::uint32_t sample_bytes = bytes_per_pixel_sample(descriptor.pixel_format);
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
    for (const PlaneLayout &plane : descriptor.planes) {
        if (plane.width == 0U || plane.height == 0U) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "plane dimensions must be positive");
        }
        if (plane.width > descriptor.width_pixels || plane.height > descriptor.height_pixels) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "plane dimensions exceed frame dimensions");
        }
        if (plane.pixel_stride < sample_bytes) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "plane pixel stride is smaller than the format requires");
        }
        const auto stride_span =
            static_cast<std::uint64_t>(plane.row_stride) * static_cast<std::uint64_t>(plane.height);
        const auto row_extent = static_cast<std::uint64_t>(plane.width) *
                                static_cast<std::uint64_t>(plane.pixel_stride);
        if (row_extent > static_cast<std::uint64_t>(plane.row_stride)) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "plane row stride is smaller than the row extent");
        }
        if (stride_span > limit - row_extent || plane.offset > limit - stride_span - row_extent) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "plane layout overflows the payload size space");
        }
    }
    for (const FrameMaskedRegion &masked : descriptor.coverage.masked_regions) {
        if (!masked.bounds.is_well_formed() || masked.space.is_nil()) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "masked region must be well formed and carry a space");
        }
    }
    return Result<void>{};
}

Result<DisplayTopology> make_display_topology(EnvironmentEpoch epoch,
                                              std::vector<DisplayInfo> displays) {
    if (displays.empty()) {
        return observation_error(ErrorCode::InvalidArgument, "topology needs at least one display");
    }
    std::unordered_set<Id128, Id128Hash> seen;
    bool active = false;
    for (const DisplayInfo &display : displays) {
        if (display.id.is_nil()) {
            return observation_error(ErrorCode::InvalidArgument, "display id must be non-nil");
        }
        if (!seen.insert(display.id.value).second) {
            return observation_error(ErrorCode::InvalidArgument, "duplicate display id");
        }
        if (display.native_width_pixels == 0U || display.native_height_pixels == 0U) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "display native dimensions must be positive");
        }
        if (display.native_rotation == Rotation::Unknown) {
            return observation_error(ErrorCode::InvalidArgument, "display rotation must be known");
        }
        if (!std::isfinite(display.density_scale) || display.density_scale <= 0.0 ||
            !std::isfinite(display.logical_width) || display.logical_width <= 0.0 ||
            !std::isfinite(display.logical_height) || display.logical_height <= 0.0) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "display density and logical extent must be positive");
        }
        active = active || display.active;
    }
    if (!active) {
        return observation_error(ErrorCode::InvalidArgument, "topology needs an active display");
    }

    // Canonical hash input: stable field order, display order by id so equal
    // topologies hash equal regardless of vector order.
    std::vector<const DisplayInfo *> ordered;
    ordered.reserve(displays.size());
    for (const DisplayInfo &display : displays) {
        ordered.push_back(&display);
    }
    std::sort(ordered.begin(), ordered.end(), [](const DisplayInfo *lhs, const DisplayInfo *rhs) {
        return lhs->id.value < rhs->id.value;
    });
    std::ostringstream canonical;
    canonical << "mira.topology.v1;epoch=" << epoch;
    for (const DisplayInfo *display : ordered) {
        canonical << ";d=" << display->id.to_string() << "," << display->native_width_pixels << "x"
                  << display->native_height_pixels
                  << ",rot=" << static_cast<unsigned int>(display->native_rotation)
                  << ",scale=" << display->density_scale << ",logical=" << display->logical_width
                  << "x" << display->logical_height << ",insets=" << display->system_insets.left
                  << "," << display->system_insets.top << "," << display->system_insets.right << ","
                  << display->system_insets.bottom << ",active=" << display->active;
    }

    DisplayTopology topology;
    topology.environment_epoch = epoch;
    topology.displays = std::move(displays);
    topology.topology_hash = digest_string(canonical.str());
    return topology;
}

Result<void> validate_ui_tree_snapshot(const UiTreeSnapshot &snapshot) {
    if (snapshot.base_snapshot.has_value()) {
        if (snapshot.base_snapshot->is_nil() || snapshot.base_sequence == 0U) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "incremental snapshot must reference a valid base");
        }
        if (snapshot.complete) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "incremental snapshot must not declare completeness");
        }
    }
    if (snapshot.truncated && snapshot.complete) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "truncated snapshot must not declare completeness");
    }
    if (snapshot.complete && snapshot.nodes.empty()) {
        return observation_error(ErrorCode::InvalidArgument,
                                 "complete snapshot must contain at least a root node");
    }
    if (snapshot.space.is_nil()) {
        return observation_error(ErrorCode::InvalidArgument, "snapshot must declare a space");
    }
    if (!span_is_ordered(snapshot.capture)) {
        return observation_error(ErrorCode::InvalidArgument, "capture span must be ordered");
    }

    std::unordered_set<Id128, Id128Hash> node_ids;
    for (const UiNode &node : snapshot.nodes) {
        if (node.id.is_nil()) {
            return observation_error(ErrorCode::InvalidArgument, "node id must be non-nil");
        }
        if (!node_ids.insert(node.id.value).second) {
            return observation_error(ErrorCode::InvalidArgument, "duplicate node id");
        }
        if (node.parent.has_value() && node.parent->is_nil()) {
            return observation_error(ErrorCode::InvalidArgument, "node parent must be non-nil");
        }
        if (node.space != snapshot.space) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "node space must match the snapshot space");
        }
        if (!node.bounds.is_well_formed()) {
            return observation_error(ErrorCode::InvalidArgument, "node bounds must be well formed");
        }
        if (has_state(node.state, UiNodeState::Password) && !node.text.empty()) {
            return observation_error(ErrorCode::InvalidArgument,
                                     "password node must not carry plaintext text");
        }
    }
    for (const UiNode &node : snapshot.nodes) {
        if (node.parent.has_value() && !node_ids.contains(node.parent->value)) {
            return observation_error(ErrorCode::InvalidArgument, "node parent is not in the tree");
        }
    }
    return Result<void>{};
}

std::chrono::nanoseconds observation_component_skew(const Observation &observation) noexcept {
    std::optional<std::chrono::steady_clock::time_point> earliest;
    std::optional<std::chrono::steady_clock::time_point> latest;
    const auto consider = [&](const CaptureSpan &span) noexcept {
        if (!earliest.has_value() || span.normalized_begin.monotonic < *earliest) {
            earliest = span.normalized_begin.monotonic;
        }
        if (!latest.has_value() || span.normalized_begin.monotonic > *latest) {
            latest = span.normalized_begin.monotonic;
        }
    };
    if (observation.screen.has_value()) {
        consider(observation.screen->capture);
    }
    if (observation.structure.has_value()) {
        consider(observation.structure->capture);
    }
    for (const auto &evidence : observation.perception) {
        consider(evidence.capture);
    }
    if (observation.foreground.has_value()) {
        consider(observation.foreground->capture);
    }
    if (observation.device.has_value()) {
        consider(observation.device->capture);
    }
    if (!earliest.has_value() || !latest.has_value()) {
        return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(*latest - *earliest);
}

ObservationEvaluation evaluate_observation(const Observation &observation,
                                           const ObservationRequest &request,
                                           const Timestamp &now) {
    ObservationEvaluation evaluation;
    evaluation.component_skew = observation_component_skew(observation);
    if (observation.atomicity == ObservationAtomicity::Atomic) {
        evaluation.effective_atomicity = ObservationAtomicity::Atomic;
    } else if (evaluation.component_skew <= request.max_component_skew) {
        evaluation.effective_atomicity = ObservationAtomicity::BoundedSkew;
    } else {
        evaluation.effective_atomicity = ObservationAtomicity::NonAtomic;
    }

    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.monotonic - observation.aggregate_span.normalized_end.monotonic);
    evaluation.fresh_enough = age >= std::chrono::milliseconds::zero() && age <= request.max_age;

    const auto check = [&](bool required, bool present, std::string_view name) {
        if (required && !present) {
            evaluation.deficiencies.emplace_back(name);
        }
    };
    check(request.required.screen, component_value(observation.screen) != nullptr, "screen");
    check(request.required.structure, component_value(observation.structure) != nullptr,
          "structure");
    check(request.required.foreground, component_value(observation.foreground) != nullptr,
          "foreground");
    check(request.required.device, component_value(observation.device) != nullptr, "device");
    if (observation.perception.size() < request.required.perception) {
        evaluation.deficiencies.emplace_back("perception");
    }

    if (observation.structure.has_value() &&
        (observation.structure->value.truncated ||
         observation.structure->quality == ComponentQuality::Partial)) {
        evaluation.deficiencies.emplace_back("structure_truncated");
    }
    if (evaluation.effective_atomicity == ObservationAtomicity::NonAtomic) {
        evaluation.deficiencies.emplace_back("component_skew_exceeded");
    }
    if (!evaluation.fresh_enough) {
        evaluation.deficiencies.emplace_back("observation_expired");
    }
    evaluation.satisfies_request = evaluation.deficiencies.empty();
    return evaluation;
}

} // namespace mira
