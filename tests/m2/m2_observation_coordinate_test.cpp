#include "support/test.hpp"

#include <mira/coordinates.hpp>
#include <mira/observation.hpp>

#include <cmath>
#include <cstdint>

namespace {

using mira::CaptureSpan;
using mira::CoordinateSpaceId;
using mira::CoordinateTransform;
using mira::DisplayInfo;
using mira::Matrix3x3;
using mira::Observation;
using mira::ObservationAtomicity;
using mira::ObservationComponent;
using mira::ObservationRequest;
using mira::PointF;
using mira::RectF;
using mira::Rotation;
using mira::ScreenFrameDescriptor;
using mira::Timestamp;
using mira::TransformQuality;
using mira::UiNode;
using mira::UiNodeState;
using mira::UiTreeSnapshot;

[[nodiscard]] bool near(double lhs, double rhs, double epsilon = 1e-9) {
    return std::fabs(lhs - rhs) <= epsilon;
}

[[nodiscard]] Timestamp at_ms(std::int64_t ms) {
    Timestamp stamp;
    stamp.monotonic = std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
    stamp.wall = std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
    return stamp;
}

[[nodiscard]] CaptureSpan span_at(std::int64_t begin_ms, std::int64_t end_ms) {
    CaptureSpan span;
    span.normalized_begin = at_ms(begin_ms);
    span.normalized_end = at_ms(end_ms);
    return span;
}

[[nodiscard]] CoordinateSpaceId space_id(std::uint8_t seed) {
    mira::Id128::Bytes bytes{};
    bytes[0] = seed;
    return CoordinateSpaceId{mira::Id128{bytes}};
}

int check_matrix_basics() {
    const PointF point{3.0, 5.0};
    const PointF identity_point = Matrix3x3{}.apply(point);
    MIRA_CHECK(near(identity_point.x, 3.0) && near(identity_point.y, 5.0));

    const Matrix3x3 scaled = Matrix3x3::scaling(2.0, 4.0);
    const PointF scaled_point = scaled.apply(point);
    MIRA_CHECK(near(scaled_point.x, 6.0) && near(scaled_point.y, 20.0));

    // Composition applies the right-hand matrix first.
    const Matrix3x3 combined = Matrix3x3::translation(10.0, 20.0) * scaled;
    const PointF combined_point = combined.apply(point);
    MIRA_CHECK(near(combined_point.x, 16.0) && near(combined_point.y, 40.0));

    const Matrix3x3 rotated = Matrix3x3::rotation_degrees(90.0);
    const PointF rotated_point = rotated.apply(PointF{1.0, 0.0});
    MIRA_CHECK(near(rotated_point.x, 0.0, 1e-12) && near(rotated_point.y, 1.0, 1e-12));

    MIRA_CHECK(!Matrix3x3{}.is_singular());
    MIRA_CHECK(Matrix3x3::scaling(0.0, 1.0).is_singular());

    const CoordinateTransform degenerate = [] {
        CoordinateTransform transform;
        transform.from = space_id(1);
        transform.to = space_id(2);
        transform.matrix = Matrix3x3::scaling(0.0, 1.0);
        transform.valid_source_region = RectF::from_origin_size(10.0, 10.0);
        transform.valid_target_region = RectF::from_origin_size(10.0, 10.0);
        return transform;
    }();
    const auto rejected = mira::validate_coordinate_transform(degenerate);
    MIRA_CHECK(!rejected.has_value());

    const CoordinateTransform with_nan = [&] {
        CoordinateTransform transform;
        transform.from = space_id(1);
        transform.to = space_id(2);
        const double nan_value = std::numeric_limits<double>::quiet_NaN();
        transform.matrix = Matrix3x3::affine(nan_value, 0.0, 0.0, 0.0, 1.0, 0.0);
        transform.valid_source_region = RectF::from_origin_size(10.0, 10.0);
        transform.valid_target_region = RectF::from_origin_size(10.0, 10.0);
        return transform;
    }();
    MIRA_CHECK(!mira::validate_coordinate_transform(with_nan).has_value());
    return 0;
}

int check_native_to_logical_rotations() {
    const CoordinateSpaceId native = space_id(10);
    const CoordinateSpaceId logical = space_id(11);
    const RectF native_region = RectF{0.0, 0.0, 2400.0, 1080.0};
    constexpr double pixels_per_logical = 2.0;

    struct Case {
        Rotation rotation;
        double native_width;
        double native_height;
        PointF probe;
        PointF expected;
    };
    const Case cases[] = {
        {Rotation::Rotation0, 1080.0, 1920.0, PointF{1080.0, 960.0}, PointF{540.0, 480.0}},
        {Rotation::Rotation90, 2400.0, 1080.0, PointF{0.0, 0.0}, PointF{540.0, 0.0}},
        {Rotation::Rotation180, 1080.0, 1920.0, PointF{1080.0, 1920.0}, PointF{0.0, 0.0}},
        {Rotation::Rotation270, 2400.0, 1080.0, PointF{0.0, 1080.0}, PointF{540.0, 1200.0}},
    };
    for (const Case &scenario : cases) {
        const auto transform = mira::make_native_to_logical_transform(
            native, logical, native_region, scenario.native_width, scenario.native_height,
            pixels_per_logical, scenario.rotation);
        MIRA_CHECK(transform.has_value());
        const PointF mapped = transform.value().matrix.apply(scenario.probe);
        MIRA_CHECK(near(mapped.x, scenario.expected.x, 1e-9));
        MIRA_CHECK(near(mapped.y, scenario.expected.y, 1e-9));
        MIRA_CHECK(transform.value().valid_target_region.contains_inclusive(mapped));
        // Round trip through the same matrix family stays within 1e-9.
        const PointF round_trip = Matrix3x3::scaling(1.0, 1.0).apply(mapped);
        MIRA_CHECK(round_trip.is_finite());
    }

    MIRA_CHECK(!mira::make_native_to_logical_transform(native, logical, native_region, 2400.0,
                                                       1080.0, -1.0, Rotation::Rotation0)
                    .has_value());
    MIRA_CHECK(!mira::make_native_to_logical_transform(native, logical, native_region, 2400.0,
                                                       1080.0, 2.0, Rotation::Unknown)
                    .has_value());
    return 0;
}

int check_chain_resolution_and_failures() {
    const CoordinateSpaceId frame_space = space_id(20);
    const CoordinateSpaceId native_space = space_id(21);
    const CoordinateSpaceId logical_space = space_id(22);
    const CoordinateSpaceId canonical_space = space_id(23);

    // Letterboxed capture: 1080x2340 frame holds 1080x1920 content with
    // 210px bars top and bottom.
    const CoordinateTransform frame_to_native = [&] {
        CoordinateTransform transform;
        transform.from = frame_space;
        transform.to = native_space;
        transform.environment_epoch = 7;
        transform.matrix = Matrix3x3::affine(1.0, 0.0, 0.0, 0.0, 1.0, -210.0);
        transform.valid_source_region = RectF{0.0, 210.0, 1080.0, 2130.0};
        transform.valid_target_region = RectF::from_origin_size(1080.0, 1920.0);
        return transform;
    }();
    const auto native_to_logical = mira::make_native_to_logical_transform(
        native_space, logical_space, RectF::from_origin_size(1080.0, 1920.0), 1080.0, 1920.0, 2.0,
        Rotation::Rotation0);
    MIRA_CHECK(native_to_logical.has_value());
    const auto logical_to_canonical = mira::make_logical_to_canonical_transform(
        logical_space, canonical_space, RectF::from_origin_size(540.0, 960.0), 540.0, 960.0);
    MIRA_CHECK(logical_to_canonical.has_value());
    constexpr std::uint64_t epoch = 7;

    std::vector<CoordinateTransform> table = {frame_to_native, native_to_logical.value(),
                                              logical_to_canonical.value()};
    for (CoordinateTransform &entry : table) {
        entry.environment_epoch = epoch;
    }

    const auto chain = mira::resolve_transform_chain(table, frame_space, canonical_space, epoch);
    MIRA_CHECK(chain.has_value());
    MIRA_CHECK(chain.value().path.size() == 4);
    MIRA_CHECK(chain.value().transform_indices.size() == 3);
    MIRA_CHECK(chain.value().quality == TransformQuality::Exact);

    const auto canonical_point =
        mira::transform_point_through(table, chain.value(), PointF{540.0, 1050.0});
    MIRA_CHECK(canonical_point.has_value());
    MIRA_CHECK(near(canonical_point.value().point.x, 0.5));
    MIRA_CHECK(near(canonical_point.value().point.y, 0.4375));

    // Point inside the letterbox bar is outside the declared content region.
    const auto bar_point =
        mira::transform_point_through(table, chain.value(), PointF{540.0, 100.0});
    MIRA_CHECK(!bar_point.has_value());
    MIRA_CHECK(bar_point.error().code == mira::ErrorCode::InvalidObservation);

    // Non-finite input is rejected before any transform runs.
    const auto nan_point = mira::transform_point_through(
        table, chain.value(), PointF{std::numeric_limits<double>::quiet_NaN(), 0.0});
    MIRA_CHECK(!nan_point.has_value());
    MIRA_CHECK(nan_point.error().code == mira::ErrorCode::InvalidArgument);

    // Stale epoch: no transforms exist for the requested epoch.
    const auto stale_chain =
        mira::resolve_transform_chain(table, frame_space, canonical_space, epoch + 1);
    MIRA_CHECK(!stale_chain.has_value());
    MIRA_CHECK(stale_chain.error().code == mira::ErrorCode::StaleObservation);

    // Missing link: reverse direction has no path.
    const auto missing_chain =
        mira::resolve_transform_chain(table, canonical_space, frame_space, epoch);
    MIRA_CHECK(!missing_chain.has_value());
    MIRA_CHECK(missing_chain.error().code == mira::ErrorCode::InvalidObservation);

    // Quality roll-up: the worst segment wins and error bounds accumulate.
    std::vector<CoordinateTransform> degraded_table = table;
    degraded_table[2].quality = TransformQuality::Approximate;
    degraded_table[2].max_abs_error = 0.25;
    degraded_table[1].max_abs_error = 0.5;
    const auto degraded_chain =
        mira::resolve_transform_chain(degraded_table, frame_space, canonical_space, epoch);
    MIRA_CHECK(degraded_chain.has_value());
    MIRA_CHECK(degraded_chain.value().quality == TransformQuality::Approximate);
    MIRA_CHECK(near(degraded_chain.value().max_abs_error, 0.75));
    const auto degraded_point = mira::transform_point_through(
        degraded_table, degraded_chain.value(), PointF{540.0, 1050.0});
    MIRA_CHECK(degraded_point.has_value());
    MIRA_CHECK(near(degraded_point.value().error_bound, 0.75));
    return 0;
}

int check_box_transform() {
    const CoordinateSpaceId native = space_id(30);
    const CoordinateSpaceId logical = space_id(31);
    const auto transform = mira::make_native_to_logical_transform(
        native, logical, RectF::from_origin_size(2400.0, 1080.0), 2400.0, 1080.0, 2.0,
        Rotation::Rotation90);
    MIRA_CHECK(transform.has_value());

    std::vector<CoordinateTransform> table{transform.value()};
    constexpr std::uint64_t epoch = 3;
    table[0].environment_epoch = epoch;
    const auto chain = mira::resolve_transform_chain(table, native, logical, epoch);
    MIRA_CHECK(chain.has_value());

    const auto box =
        mira::transform_box_through(table, chain.value(), RectF{100.0, 100.0, 200.0, 300.0});
    MIRA_CHECK(box.has_value());
    // (x, y) -> ((1080 - y) / 2, x / 2): the hull of all four corners.
    MIRA_CHECK(!box.value().axis_aligned);
    MIRA_CHECK(near(box.value().bounds.left, 390.0));
    MIRA_CHECK(near(box.value().bounds.top, 50.0));
    MIRA_CHECK(near(box.value().bounds.right, 490.0));
    MIRA_CHECK(near(box.value().bounds.bottom, 100.0));

    const auto bad_box =
        mira::transform_box_through(table, chain.value(), RectF{100.0, 100.0, 2500.0, 300.0});
    MIRA_CHECK(!bad_box.has_value());
    return 0;
}

int check_frame_descriptor_validation() {
    const auto valid_descriptor = [] {
        ScreenFrameDescriptor descriptor;
        descriptor.frame_id = mira::FrameId::generate();
        descriptor.display_id = mira::DisplayId::generate();
        descriptor.width_pixels = 4;
        descriptor.height_pixels = 4;
        descriptor.pixel_format = mira::PixelFormat::RGBA8888;
        descriptor.color_space = mira::ColorSpace::SRGB;
        descriptor.alpha_mode = mira::AlphaMode::Opaque;
        descriptor.native_rotation = Rotation::Rotation0;
        descriptor.planes.push_back(mira::PlaneLayout{0, 16, 4, 4, 4});
        descriptor.pixel_space = space_id(40);
        descriptor.capture = span_at(0, 5);
        descriptor.payload_artifact = mira::ArtifactId::generate();
        return descriptor;
    }();
    MIRA_CHECK(mira::validate_frame_descriptor(valid_descriptor).has_value());

    auto wrong_planes = valid_descriptor;
    wrong_planes.planes.push_back(mira::PlaneLayout{64, 16, 4, 4, 4});
    MIRA_CHECK(!mira::validate_frame_descriptor(wrong_planes).has_value());

    auto tight_stride = valid_descriptor;
    tight_stride.planes[0].row_stride = 8;
    MIRA_CHECK(!mira::validate_frame_descriptor(tight_stride).has_value());

    auto nv12 = valid_descriptor;
    nv12.pixel_format = mira::PixelFormat::NV12;
    nv12.planes = {mira::PlaneLayout{0, 4, 1, 4, 4}, mira::PlaneLayout{16, 4, 2, 2, 2}};
    MIRA_CHECK(mira::validate_frame_descriptor(nv12).has_value());

    // Offset/stride arithmetic must be overflow checked in 64-bit space: this
    // offset places the plane's last row past the addressable size range.
    auto hostile = valid_descriptor;
    hostile.width_pixels = 0xFFFFFFFFU;
    hostile.height_pixels = 0xFFFFFFFFU;
    hostile.planes = {
        mira::PlaneLayout{0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFU, 4, 0x10000000U, 0xFFFFFFFFU}};
    const auto hostile_result = mira::validate_frame_descriptor(hostile);
    MIRA_CHECK(!hostile_result.has_value());
    MIRA_CHECK(hostile_result.error().code == mira::ErrorCode::InvalidArgument);

    auto unknown_format = valid_descriptor;
    unknown_format.pixel_format = mira::PixelFormat::Unknown;
    MIRA_CHECK(!mira::validate_frame_descriptor(unknown_format).has_value());
    return 0;
}

int check_display_topology() {
    const auto make_display = [](std::uint8_t seed, double density) {
        DisplayInfo display;
        display.id = mira::DisplayId{
            mira::Id128{mira::Id128::Bytes{seed, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}};
        display.name = "display";
        display.native_width_pixels = 1080;
        display.native_height_pixels = 1920;
        display.native_rotation = Rotation::Rotation0;
        display.density_scale = density;
        display.logical_width = 1080.0 * density;
        display.logical_height = 1920.0 * density;
        return display;
    };

    const auto first = mira::make_display_topology(1, {make_display(1, 0.5), make_display(2, 0.5)});
    const auto reordered =
        mira::make_display_topology(1, {make_display(2, 0.5), make_display(1, 0.5)});
    const auto changed =
        mira::make_display_topology(1, {make_display(1, 0.5), make_display(2, 0.75)});
    MIRA_CHECK(first.has_value() && reordered.has_value() && changed.has_value());
    MIRA_CHECK(first.value().topology_hash == reordered.value().topology_hash);
    MIRA_CHECK(!(first.value().topology_hash == changed.value().topology_hash));
    MIRA_CHECK(first.value().environment_epoch == 1);

    const auto duplicated =
        mira::make_display_topology(1, {make_display(1, 0.5), make_display(1, 0.5)});
    MIRA_CHECK(!duplicated.has_value());

    auto inactive = make_display(9, 0.5);
    inactive.active = false;
    const auto no_active = mira::make_display_topology(1, {inactive});
    MIRA_CHECK(!no_active.has_value());
    return 0;
}

int check_ui_tree_validation() {
    const auto valid_snapshot = [] {
        UiTreeSnapshot snapshot;
        snapshot.complete = true;
        snapshot.space = space_id(50);
        snapshot.capture = span_at(0, 5);
        UiNode root;
        root.id = mira::UiNodeId::generate();
        root.role = mira::UiRole::Root;
        root.bounds = RectF::from_origin_size(540.0, 960.0);
        root.space = snapshot.space;
        root.state = UiNodeState::Visible | UiNodeState::Enabled;
        UiNode button;
        button.id = mira::UiNodeId::generate();
        button.parent = root.id;
        button.role = mira::UiRole::Button;
        button.text = "continue";
        button.bounds = RectF{10.0, 20.0, 110.0, 80.0};
        button.space = snapshot.space;
        button.supported_actions = mira::UiNodeAction::Click;
        snapshot.nodes = {root, button};
        return snapshot;
    }();
    MIRA_CHECK(mira::validate_ui_tree_snapshot(valid_snapshot).has_value());

    auto password_leak = valid_snapshot;
    password_leak.nodes[1].state = UiNodeState::Visible | UiNodeState::Password;
    MIRA_CHECK(!mira::validate_ui_tree_snapshot(password_leak).has_value());

    auto dangling_parent = valid_snapshot;
    dangling_parent.nodes[1].parent = mira::UiNodeId::generate();
    MIRA_CHECK(!mira::validate_ui_tree_snapshot(dangling_parent).has_value());

    auto space_mismatch = valid_snapshot;
    space_mismatch.nodes[1].space = space_id(51);
    MIRA_CHECK(!mira::validate_ui_tree_snapshot(space_mismatch).has_value());

    auto incremental_without_base = valid_snapshot;
    incremental_without_base.complete = false;
    incremental_without_base.base_snapshot = mira::ObservationId::generate();
    incremental_without_base.base_sequence = 0;
    MIRA_CHECK(!mira::validate_ui_tree_snapshot(incremental_without_base).has_value());

    auto truncated_complete = valid_snapshot;
    truncated_complete.truncated = true;
    MIRA_CHECK(!mira::validate_ui_tree_snapshot(truncated_complete).has_value());
    return 0;
}

int check_observation_evaluation() {
    Observation observation;
    observation.id = mira::ObservationId::generate();
    observation.session_id = mira::SessionId::generate();
    observation.environment_epoch = 4;
    observation.aggregate_span = span_at(100, 180);

    ObservationComponent<ScreenFrameDescriptor> screen;
    screen.environment_epoch = 4;
    screen.capture = span_at(100, 160);
    screen.value.width_pixels = 4;
    screen.value.height_pixels = 4;
    screen.value.pixel_format = mira::PixelFormat::RGBA8888;
    screen.value.planes.push_back(mira::PlaneLayout{0, 16, 4, 4, 4});
    screen.value.pixel_space = space_id(60);
    screen.value.capture = screen.capture;
    observation.screen = screen;

    ObservationComponent<UiTreeSnapshot> structure;
    structure.environment_epoch = 4;
    // 150ms after the screen capture: a real, declared component skew.
    structure.capture = span_at(250, 300);
    structure.value.space = space_id(60);
    structure.value.capture = structure.capture;
    observation.structure = structure;

    ObservationRequest request;
    request.required.screen = true;
    request.required.structure = true;
    request.max_component_skew = std::chrono::milliseconds{100};

    const auto bounded = mira::evaluate_observation(observation, request, at_ms(400));
    MIRA_CHECK(bounded.component_skew == std::chrono::milliseconds{150});
    MIRA_CHECK(bounded.effective_atomicity == ObservationAtomicity::NonAtomic);
    MIRA_CHECK(!bounded.satisfies_request);

    request.max_component_skew = std::chrono::milliseconds{200};
    const auto satisfied = mira::evaluate_observation(observation, request, at_ms(400));
    MIRA_CHECK(satisfied.effective_atomicity == ObservationAtomicity::BoundedSkew);
    MIRA_CHECK(satisfied.satisfies_request);

    observation.atomicity = ObservationAtomicity::Atomic;
    const auto platform_atomic = mira::evaluate_observation(observation, request, at_ms(400));
    MIRA_CHECK(platform_atomic.effective_atomicity == ObservationAtomicity::Atomic);
    MIRA_CHECK(platform_atomic.satisfies_request);

    const auto missing_screen = [&] {
        Observation partial = observation;
        partial.screen.reset();
        return mira::evaluate_observation(partial, request, at_ms(400));
    }();
    MIRA_CHECK(!missing_screen.satisfies_request);
    MIRA_CHECK(!missing_screen.deficiencies.empty());

    const auto expired = mira::evaluate_observation(observation, request, at_ms(5000));
    MIRA_CHECK(!expired.fresh_enough);
    MIRA_CHECK(!expired.satisfies_request);
    return 0;
}

int check_request_validation_and_parsers() {
    ObservationRequest request;
    MIRA_CHECK(mira::validate_observation_request(request).has_value());

    ObservationRequest roi_without_regions;
    roi_without_regions.mode = mira::ObservationMode::Roi;
    MIRA_CHECK(!mira::validate_observation_request(roi_without_regions).has_value());

    ObservationRequest diff_without_baseline;
    diff_without_baseline.mode = mira::ObservationMode::Diff;
    MIRA_CHECK(!mira::validate_observation_request(diff_without_baseline).has_value());

    MIRA_CHECK(
        mira::parse_observation_mode(static_cast<std::uint8_t>(mira::ObservationMode::Verification))
            .has_value());
    MIRA_CHECK(!mira::parse_observation_mode(99).has_value());
    MIRA_CHECK(mira::parse_pixel_format(static_cast<std::uint8_t>(mira::PixelFormat::RGBA8888))
                   .has_value());
    MIRA_CHECK(!mira::parse_pixel_format(200).has_value());
    MIRA_CHECK(
        mira::parse_color_space(static_cast<std::uint8_t>(mira::ColorSpace::SRGB)).has_value());
    MIRA_CHECK(!mira::parse_color_space(200).has_value());
    MIRA_CHECK(mira::parse_rotation(static_cast<std::uint8_t>(Rotation::Rotation270)).has_value());
    MIRA_CHECK(!mira::parse_rotation(200).has_value());
    MIRA_CHECK(mira::parse_coordinate_space_kind(
                   static_cast<std::uint8_t>(mira::CoordinateSpaceKind::CanonicalViewport))
                   .has_value());
    MIRA_CHECK(!mira::parse_coordinate_space_kind(200).has_value());
    return 0;
}

} // namespace

int main() {
    if (const int status = check_matrix_basics(); status != 0) {
        return status;
    }
    if (const int status = check_native_to_logical_rotations(); status != 0) {
        return status;
    }
    if (const int status = check_chain_resolution_and_failures(); status != 0) {
        return status;
    }
    if (const int status = check_box_transform(); status != 0) {
        return status;
    }
    if (const int status = check_frame_descriptor_validation(); status != 0) {
        return status;
    }
    if (const int status = check_display_topology(); status != 0) {
        return status;
    }
    if (const int status = check_ui_tree_validation(); status != 0) {
        return status;
    }
    if (const int status = check_observation_evaluation(); status != 0) {
        return status;
    }
    if (const int status = check_request_validation_and_parsers(); status != 0) {
        return status;
    }
    return 0;
}
