#include "support/test.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

namespace {

using mira::CoordinateSpaceId;
using mira::CoordinateTransform;
using mira::DisplayTopology;
using mira::EnvironmentEpoch;
using mira::ErrorCode;
using mira::ObservationAtomicity;
using mira::ObservationRequest;
using mira::OperationContext;
using mira::PointF;
using mira::RectF;
using mira::Rotation;
using mira::TransformChain;
using mira::adapters::simulator::SimulatorComponentPlan;
using mira::adapters::simulator::SimulatorDisplaySetup;
using mira::adapters::simulator::SimulatorEnvironment;
using mira::adapters::simulator::SimulatorSetup;

[[nodiscard]] bool near(double lhs, double rhs, double epsilon = 1e-9) {
    return std::fabs(lhs - rhs) <= epsilon;
}

OperationContext context() {
    OperationContext value;
    value.operation = mira::OperationId::generate();
    value.started_at = mira::Timestamp::now();
    return value;
}

ObservationRequest full_request() {
    ObservationRequest request;
    request.required.screen = true;
    request.required.structure = true;
    request.required.foreground = true;
    request.required.device = true;
    return request;
}

// Walks the published chain from native pixels to the canonical viewport.
// Simulator space ids encode their kind in byte 14 (1 = native, 3 = canonical),
// so tests can pick endpoints without hardcoding ids.
std::optional<TransformChain> resolve_canonical(const SimulatorEnvironment &environment,
                                                EnvironmentEpoch epoch) {
    const auto transforms = environment.coordinate_transforms(epoch);
    CoordinateSpaceId from;
    CoordinateSpaceId to;
    for (const CoordinateTransform &transform : transforms) {
        if (from.is_nil() && transform.from.value.bytes()[14] == 1 &&
            transform.from.value.bytes()[15] == 0xA5) {
            from = transform.from;
        }
        if (to.is_nil() && transform.to.value.bytes()[14] == 3 &&
            transform.to.value.bytes()[15] == 0xA5) {
            to = transform.to;
        }
    }
    if (from.is_nil() || to.is_nil()) {
        return std::nullopt;
    }
    auto chain = mira::resolve_transform_chain(transforms, from, to, epoch);
    if (!chain.has_value()) {
        return std::nullopt;
    }
    return chain.value();
}

int check_single_display_transform_roundtrip() {
    SimulatorEnvironment environment{SimulatorSetup::single_display()};
    const EnvironmentEpoch epoch = environment.environment_epoch();
    const auto topology = environment.topology();
    MIRA_CHECK(topology.has_value());
    MIRA_CHECK(topology.value().displays.size() == 1);
    const auto &display = topology.value().displays.front();
    // 240x320 native, 2 px per logical -> 120x160 logical.
    MIRA_CHECK(near(display.logical_width, 120.0));
    MIRA_CHECK(near(display.logical_height, 160.0));

    const auto chain = resolve_canonical(environment, epoch);
    MIRA_CHECK(chain.has_value());
    const auto &transforms = environment.coordinate_transforms(epoch);
    const mira::PointF center_native{120.0, 160.0};
    const auto canonical = mira::transform_point_through(transforms, chain.value(), center_native);
    MIRA_CHECK(canonical.has_value());
    MIRA_CHECK(near(canonical.value().point.x, 0.5, 1e-9));
    MIRA_CHECK(near(canonical.value().point.y, 0.5, 1e-9));
    MIRA_CHECK(canonical.value().error_bound <= 1e-12);

    // Frame pixels map onto native pixels one-to-one.
    const CoordinateSpaceId frame_space = [&] {
        for (const CoordinateTransform &transform : transforms) {
            if (transform.from.value.bytes()[14] == 5) {
                return transform.from;
            }
        }
        return CoordinateSpaceId{};
    }();
    const CoordinateSpaceId native_space = [&] {
        for (const CoordinateTransform &transform : transforms) {
            if (transform.from.value.bytes()[14] == 5) {
                return transform.to;
            }
        }
        return CoordinateSpaceId{};
    }();
    const auto frame_chain =
        mira::resolve_transform_chain(transforms, frame_space, native_space, epoch);
    MIRA_CHECK(frame_chain.has_value());
    const auto frame_point =
        mira::transform_point_through(transforms, frame_chain.value(), mira::PointF{100.0, 200.0});
    MIRA_CHECK(frame_point.has_value());
    MIRA_CHECK(near(frame_point.value().point.x, 100.0));
    MIRA_CHECK(near(frame_point.value().point.y, 200.0));
    return 0;
}

int check_rotation_invalidates_epoch() {
    SimulatorEnvironment environment{SimulatorSetup::single_display()};
    const EnvironmentEpoch before = environment.environment_epoch();
    const auto transforms_before = environment.coordinate_transforms(before);
    MIRA_CHECK(!transforms_before.empty());

    const auto topology = environment.topology();
    const mira::DisplayId display = topology.value().displays.front().id;
    const auto rotated = environment.rotate_display(display, Rotation::Rotation90);
    MIRA_CHECK(rotated.has_value());
    MIRA_CHECK(rotated.value() == before + 1);
    MIRA_CHECK(!environment.epoch_history().empty());

    // The environment no longer publishes transforms for the old epoch;
    // resolving an old-epoch chain against the current table fails closed.
    MIRA_CHECK(environment.coordinate_transforms(before).empty());
    const auto current_transforms = environment.coordinate_transforms(rotated.value());
    MIRA_CHECK(!current_transforms.empty());
    const auto stale_resolution = mira::resolve_transform_chain(
        current_transforms, current_transforms.front().from, current_transforms.front().to, before);
    MIRA_CHECK(!stale_resolution.has_value());
    MIRA_CHECK(stale_resolution.error().code == ErrorCode::StaleObservation);

    // The new epoch maps the rotated panel: native is now 320x240.
    const auto topology_after = environment.topology();
    MIRA_CHECK(topology_after.has_value());
    MIRA_CHECK(topology_after.value().displays.front().native_width_pixels == 320);
    MIRA_CHECK(topology_after.value().displays.front().native_height_pixels == 240);
    const auto transforms_after = environment.coordinate_transforms(rotated.value());
    MIRA_CHECK(transforms_after.size() == transforms_before.size());

    // Rotating to the same orientation is a no-op that keeps the epoch.
    const auto again = environment.rotate_display(display, Rotation::Rotation90);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value() == rotated.value());
    return 0;
}

int check_letterbox_and_inset_regions() {
    SimulatorEnvironment letterboxed{SimulatorSetup::letterboxed_display()};
    const EnvironmentEpoch epoch = letterboxed.environment_epoch();
    const auto transforms = letterboxed.coordinate_transforms(epoch);
    // Resolve logical -> canonical directly; the window region is x in
    // [48, 72], so its left edge maps to 0 and its right edge to 1, proving
    // canonical covers the window rather than the whole display.
    const CoordinateSpaceId logical_space = [&] {
        for (const CoordinateTransform &transform : transforms) {
            if (transform.from.value.bytes()[14] == 2 && transform.from.value.bytes()[15] == 0xA5) {
                return transform.from;
            }
        }
        return CoordinateSpaceId{};
    }();
    const CoordinateSpaceId canonical_space = [&] {
        for (const CoordinateTransform &transform : transforms) {
            if (transform.to.value.bytes()[14] == 3 && transform.to.value.bytes()[15] == 0xA5) {
                return transform.to;
            }
        }
        return CoordinateSpaceId{};
    }();
    MIRA_CHECK(!logical_space.is_nil());
    MIRA_CHECK(!canonical_space.is_nil());
    const auto chain =
        mira::resolve_transform_chain(transforms, logical_space, canonical_space, epoch);
    MIRA_CHECK(chain.has_value());
    const auto left = mira::transform_point_through(transforms, chain.value(), PointF{48.0, 80.0});
    const auto right = mira::transform_point_through(transforms, chain.value(), PointF{72.0, 80.0});
    MIRA_CHECK(left.has_value());
    MIRA_CHECK(near(left.value().point.x, 0.0));
    MIRA_CHECK(right.has_value());
    MIRA_CHECK(near(right.value().point.x, 1.0));
    // Points inside the letterbox bars are outside the valid source region.
    const auto outside =
        mira::transform_point_through(transforms, chain.value(), PointF{10.0, 80.0});
    MIRA_CHECK(!outside.has_value());
    MIRA_CHECK(outside.error().code == ErrorCode::InvalidObservation);

    // Insets: the content edge is translated by the inset origin when going
    // content -> logical.
    SimulatorEnvironment inset{SimulatorSetup::inset_display()};
    const auto inset_transforms = inset.coordinate_transforms(inset.environment_epoch());
    const auto content_edge = [&] {
        for (const CoordinateTransform &transform : inset_transforms) {
            if (transform.from.value.bytes()[14] == 4) {
                return transform;
            }
        }
        return CoordinateTransform{};
    }();
    MIRA_CHECK(!content_edge.from.is_nil());
    // Content space is anchored at the content region's own origin, so the
    // inset top of 24 logical units appears as a pure translation.
    const auto mapped = content_edge.matrix.apply(mira::PointF{0.0, 0.0});
    MIRA_CHECK(near(mapped.x, 0.0));
    MIRA_CHECK(near(mapped.y, 24.0));
    return 0;
}

int check_dual_display_and_density() {
    SimulatorEnvironment environment{SimulatorSetup::dual_display()};
    const auto topology = environment.topology();
    MIRA_CHECK(topology.has_value());
    MIRA_CHECK(topology.value().displays.size() == 2);
    const auto &second = topology.value().displays[1];
    // 320x240 rotated 90 at 2.5 px/logical -> logical 96x128.
    MIRA_CHECK(second.native_rotation == Rotation::Rotation90);
    MIRA_CHECK(near(second.logical_width, 96.0));
    MIRA_CHECK(near(second.logical_height, 128.0));

    // A density change invalidates coordinates like any topology change.
    const auto density = environment.set_density(second.id, 4.0);
    MIRA_CHECK(density.has_value());
    const auto after = environment.topology();
    MIRA_CHECK(near(after.value().displays[1].logical_width, 60.0));
    MIRA_CHECK(environment.coordinate_transforms(density.value()).size() >= 4);

    // Removing the last remaining display is refused.
    const auto removed = environment.remove_display(second.id);
    MIRA_CHECK(removed.has_value());
    const auto topology_now = environment.topology();
    const mira::DisplayId last = topology_now.value().displays.front().id;
    const auto refused = environment.remove_display(last);
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int check_observation_atomicity_and_skew() {
    SimulatorSetup setup = SimulatorSetup::single_display();
    setup.declared_component_skew = std::chrono::milliseconds(500);
    SimulatorEnvironment environment{setup};
    const auto observation = environment.observe(full_request(), context());
    MIRA_CHECK(observation.has_value());
    MIRA_CHECK(observation.value().atomicity == ObservationAtomicity::BoundedSkew);
    MIRA_CHECK(observation.value().screen.has_value());
    MIRA_CHECK(observation.value().structure.has_value());
    MIRA_CHECK(observation.value().device.has_value());
    MIRA_CHECK(observation.value().foreground.has_value());
    // The password-free fixture tree carries no plaintext on secure nodes.
    const auto &tree = observation.value().structure.value().value;
    MIRA_CHECK(tree.complete);
    for (const auto &node : tree.nodes) {
        if (mira::has_state(node.state, mira::UiNodeState::Password)) {
            MIRA_CHECK(node.text.empty());
        }
    }

    // Declared skew below the actual skew downgrades to NonAtomic.
    SimulatorSetup skewed = SimulatorSetup::single_display();
    skewed.screen_plan.capture_skew = std::chrono::milliseconds(50);
    skewed.structure_plan.capture_skew = std::chrono::milliseconds(300);
    skewed.declared_component_skew = std::chrono::milliseconds(100);
    SimulatorEnvironment skew_environment{skewed};
    const auto skewed_observation = skew_environment.observe(full_request(), context());
    MIRA_CHECK(skewed_observation.has_value());
    MIRA_CHECK(skewed_observation.value().atomicity == ObservationAtomicity::NonAtomic);
    MIRA_CHECK(skewed_observation.value().quality.components_skewed);
    MIRA_CHECK(mira::observation_component_skew(skewed_observation.value()) >=
               std::chrono::milliseconds(250));

    // The atomic fixture shares one capture transaction.
    SimulatorSetup atomic = SimulatorSetup::single_display();
    atomic.atomic_components = true;
    SimulatorEnvironment atomic_environment{atomic};
    const auto atomic_observation = atomic_environment.observe(full_request(), context());
    MIRA_CHECK(atomic_observation.has_value());
    MIRA_CHECK(atomic_observation.value().atomicity == ObservationAtomicity::Atomic);
    MIRA_CHECK(atomic_environment.capabilities().atomic_observation);
    MIRA_CHECK(mira::observation_component_skew(atomic_observation.value()) ==
               std::chrono::nanoseconds::zero());
    return 0;
}

int check_mid_capture_invalidation_and_failures() {
    SimulatorSetup setup = SimulatorSetup::single_display();
    setup.inject_mid_capture_epoch_bump = true;
    SimulatorEnvironment environment{setup};
    const auto stale = environment.observe(full_request(), context());
    MIRA_CHECK(!stale.has_value());
    MIRA_CHECK(stale.error().code == ErrorCode::StaleObservation);

    // A required component that fails closes the request with a clear error.
    SimulatorSetup failing = SimulatorSetup::single_display();
    failing.screen_plan.failure_code = ErrorCode::Unavailable;
    failing.screen_plan.failure_message = "injected screen failure";
    SimulatorEnvironment failing_environment{failing};
    const auto failed = failing_environment.observe(full_request(), context());
    MIRA_CHECK(!failed.has_value());
    MIRA_CHECK(failed.error().code == ErrorCode::InvalidObservation);

    // The same failure on an optional component degrades instead of failing.
    SimulatorSetup optional_failure = SimulatorSetup::single_display();
    optional_failure.device_plan.failure_code = ErrorCode::Unavailable;
    SimulatorEnvironment degraded{optional_failure};
    ObservationRequest request = full_request();
    request.required.device = false;
    request.optional.device = true;
    const auto degraded_observation = degraded.observe(request, context());
    MIRA_CHECK(degraded_observation.has_value());
    MIRA_CHECK(!degraded_observation.value().device.has_value());
    bool noted = false;
    for (const std::string &entry : degraded_observation.value().quality.degradations) {
        if (entry.find("device") != std::string::npos) {
            noted = true;
        }
    }
    MIRA_CHECK(noted);
    return 0;
}

int check_frame_artifact_survives_publication() {
    SimulatorEnvironment environment{SimulatorSetup::single_display()};
    const auto observation = environment.observe(full_request(), context());
    MIRA_CHECK(observation.has_value());
    const auto &screen = observation.value().screen.value();
    MIRA_CHECK(!screen.value.payload_artifact.is_nil());
    std::vector<std::byte> bytes;
    const auto opened = environment.open_artifact(screen.value.payload_artifact, bytes);
    MIRA_CHECK(opened.has_value());
    const std::uint32_t width = screen.value.width_pixels;
    const std::uint32_t height = screen.value.height_pixels;
    MIRA_CHECK(opened.value() == static_cast<std::size_t>(width) * height * 4U);
    const auto pixel = mira::adapters::simulator::simulator_frame_pixel(width / 2, height / 2);
    const std::size_t offset = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4U;
    MIRA_CHECK(static_cast<std::uint8_t>(bytes[offset]) == pixel[0]);
    MIRA_CHECK(static_cast<std::uint8_t>(bytes[offset + 1]) == pixel[1]);
    MIRA_CHECK(static_cast<std::uint8_t>(bytes[offset + 3]) == 0xFFU);
    return 0;
}

int check_multi_display_input_targeting() {
    SimulatorEnvironment environment{SimulatorSetup::dual_display()};
    const auto topology = environment.topology();
    const mira::DisplayId second = topology.value().displays[1].id;

    mira::InputSequence sequence;
    sequence.display = second;
    sequence.events.push_back(mira::InputEvent{"tap", "0.25,0.75"});
    const auto receipt = environment.execute(sequence, context());
    MIRA_CHECK(receipt.has_value());
    MIRA_CHECK(receipt.value().status == mira::ExecutionStatus::Completed);
    const auto history = environment.executed_inputs();
    MIRA_CHECK(history.size() == 1);
    MIRA_CHECK(history.front().display.has_value());
    MIRA_CHECK(*history.front().display == second);

    // Unknown display targets are rejected before any side effect.
    mira::InputSequence lost;
    mira::Id128::Bytes bytes{};
    bytes[0] = 0xEE;
    lost.display = mira::DisplayId{mira::Id128{bytes}};
    lost.events.push_back(mira::InputEvent{"home", ""});
    const auto refused = environment.execute(lost, context());
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(environment.executed_inputs().size() == 1);
    return 0;
}

} // namespace

int main() {
    if (const int code = check_single_display_transform_roundtrip(); code != 0)
        return code;
    if (const int code = check_rotation_invalidates_epoch(); code != 0)
        return code;
    if (const int code = check_letterbox_and_inset_regions(); code != 0)
        return code;
    if (const int code = check_dual_display_and_density(); code != 0)
        return code;
    if (const int code = check_observation_atomicity_and_skew(); code != 0)
        return code;
    if (const int code = check_mid_capture_invalidation_and_failures(); code != 0)
        return code;
    if (const int code = check_frame_artifact_survives_publication(); code != 0)
        return code;
    if (const int code = check_multi_display_input_targeting(); code != 0)
        return code;
    return 0;
}
