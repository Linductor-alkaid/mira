#include <mira/adapters/simulator/simulator_environment.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

namespace mira::adapters::simulator {
namespace {

Error simulator_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.simulator";
    error.safe_message = std::move(message);
    return error;
}

constexpr std::chrono::milliseconds kCaptureDuration{5};
constexpr std::uint8_t kSpaceKindNative = 1;
constexpr std::uint8_t kSpaceKindLogical = 2;
constexpr std::uint8_t kSpaceKindCanonical = 3;
constexpr std::uint8_t kSpaceKindContent = 4;
constexpr std::uint8_t kSpaceKindFrame = 5;

// Space identity is a pure function of display, kind and epoch so tests can
// correlate observations, chains and frames without persistent registries.
CoordinateSpaceId make_space_id(const DisplayId &display, std::uint8_t kind,
                                EnvironmentEpoch epoch) {
    Id128::Bytes bytes{};
    const Id128::Bytes &source = display.value.bytes();
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = source[index];
    }
    for (std::size_t index = 0; index < 6; ++index) {
        bytes[8 + index] = static_cast<std::uint8_t>((epoch >> (index * 8)) & 0xFFU);
    }
    bytes[14] = kind;
    bytes[15] = 0xA5;
    return CoordinateSpaceId{Id128{bytes}};
}

void logical_extent(const SimulatorDisplaySetup &display, double &width, double &height) {
    const double inverse = 1.0 / display.pixels_per_logical;
    const bool rotated = display.native_rotation == Rotation::Rotation90 ||
                         display.native_rotation == Rotation::Rotation270;
    const double native_width = static_cast<double>(display.native_width_pixels);
    const double native_height = static_cast<double>(display.native_height_pixels);
    width = (rotated ? native_height : native_width) * inverse;
    height = (rotated ? native_width : native_height) * inverse;
}

RectF content_region_of(const SimulatorDisplaySetup &display) {
    double logical_width = 0.0;
    double logical_height = 0.0;
    logical_extent(display, logical_width, logical_height);
    return RectF{display.system_insets.left, display.system_insets.top,
                 logical_width - display.system_insets.right,
                 logical_height - display.system_insets.bottom};
}

RectF action_region_of(const SimulatorDisplaySetup &display) {
    if (display.window_region.has_value()) {
        return *display.window_region;
    }
    double logical_width = 0.0;
    double logical_height = 0.0;
    logical_extent(display, logical_width, logical_height);
    return RectF::from_origin_size(logical_width, logical_height);
}

DisplayInfo to_display_info(const SimulatorDisplaySetup &setup) {
    DisplayInfo info;
    info.id = setup.id;
    info.name = "simulator-" + setup.id.to_string();
    info.native_width_pixels = setup.native_width_pixels;
    info.native_height_pixels = setup.native_height_pixels;
    info.native_rotation = setup.native_rotation;
    info.density_scale = 1.0 / setup.pixels_per_logical;
    logical_extent(setup, info.logical_width, info.logical_height);
    info.system_insets = setup.system_insets;
    info.active = setup.active;
    return info;
}

const SimulatorDisplaySetup *find_display(const std::vector<SimulatorDisplaySetup> &displays,
                                          const DisplayId &id) {
    for (const SimulatorDisplaySetup &display : displays) {
        if (display.id == id) {
            return &display;
        }
    }
    return nullptr;
}

// Strict canonical-coordinate parsing: exactly the expected number of finite
// doubles in [0, 1]; anything else is rejected instead of guessed.
bool parse_canonical_payload(const std::string &payload, std::size_t expected,
                             std::string &failure) {
    std::string normalized = payload;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream stream(normalized);
    std::vector<double> values;
    double value = 0.0;
    while (stream >> value) {
        values.push_back(value);
    }
    std::string extra;
    if (stream >> extra) {
        failure = "payload contains a non-numeric token";
        return false;
    }
    if (values.size() != expected) {
        failure = "payload must contain " + std::to_string(expected) + " numbers";
        return false;
    }
    for (const double coordinate : values) {
        if (!std::isfinite(coordinate) || coordinate < 0.0 || coordinate > 1.0) {
            failure = "canonical coordinates must be finite and within [0, 1]";
            return false;
        }
    }
    return true;
}

bool parse_input_event(const InputEvent &event, std::string &failure) {
    if (event.kind == "tap" || event.kind == "long_press") {
        return parse_canonical_payload(event.payload, 2, failure);
    }
    if (event.kind == "swipe") {
        return parse_canonical_payload(event.payload, 4, failure);
    }
    if (event.kind == "type") {
        if (event.payload.empty()) {
            failure = "type event must carry text";
            return false;
        }
        return true;
    }
    if (event.kind == "back" || event.kind == "home") {
        if (!event.payload.empty()) {
            failure = event.kind + " event must not carry a payload";
            return false;
        }
        return true;
    }
    failure = "unknown input kind";
    return false;
}

} // namespace

class SimulatorEnvironment::Impl final {
  public:
    explicit Impl(SimulatorSetup initial) : setup(std::move(initial)) {}

    mutable std::mutex mutex;
    SimulatorSetup setup;
    EnvironmentEpoch epoch = 1;
    std::vector<std::string> epoch_reasons;
    std::uint64_t observations = 0;
    std::vector<InputSequence> executed;
    bool was_interrupted = false;
    MemoryArtifactStore artifacts{16ULL * 1024ULL * 1024ULL};
    std::unordered_map<ArtifactId, ArtifactDescriptor, StrongIdHash<ArtifactId>> published;

    EnvironmentEpoch bump(std::string reason) {
        ++epoch;
        epoch_reasons.push_back(std::move(reason));
        return epoch;
    }

    EnvironmentCapabilities capabilities_locked() const {
        EnvironmentCapabilities capabilities;
        capabilities.screen_capture = true;
        capabilities.ui_tree = true;
        capabilities.foreground_app = true;
        capabilities.device_state = true;
        capabilities.atomic_observation = setup.atomic_components;
        capabilities.max_component_skew = setup.declared_component_skew;
        capabilities.discrete_input = true;
        capabilities.input_release = true;
        capabilities.epoch_invalidation = true;
        return capabilities;
    }

    Result<DisplayTopology> make_topology_locked() const {
        std::vector<DisplayInfo> infos;
        infos.reserve(setup.displays.size());
        for (const SimulatorDisplaySetup &display : setup.displays) {
            infos.push_back(to_display_info(display));
        }
        return make_display_topology(epoch, std::move(infos));
    }
};

SimulatorSetup SimulatorSetup::single_display() {
    SimulatorDisplaySetup display;
    display.id = DisplayId::generate();
    display.native_width_pixels = 240;
    display.native_height_pixels = 320;
    display.native_rotation = Rotation::Rotation0;
    display.pixels_per_logical = 2.0;
    SimulatorSetup setup;
    setup.displays.push_back(display);
    return setup;
}

SimulatorSetup SimulatorSetup::letterboxed_display() {
    SimulatorSetup setup = single_display();
    SimulatorDisplaySetup &display = setup.displays.front();
    // 240x120 letterboxed window centered horizontally in the 120x160
    // logical display.
    display.window_region = RectF{48.0, 20.0, 72.0, 140.0};
    return setup;
}

SimulatorSetup SimulatorSetup::inset_display() {
    SimulatorSetup setup = single_display();
    SimulatorDisplaySetup &display = setup.displays.front();
    display.system_insets = Insets{0.0, 24.0, 0.0, 16.0};
    return setup;
}

SimulatorSetup SimulatorSetup::dual_display() {
    SimulatorSetup setup = single_display();
    SimulatorDisplaySetup second;
    second.id = DisplayId::generate();
    second.native_width_pixels = 320;
    second.native_height_pixels = 240;
    second.native_rotation = Rotation::Rotation90;
    second.pixels_per_logical = 2.5;
    setup.displays.push_back(second);
    return setup;
}

std::array<std::uint8_t, 4> simulator_frame_pixel(std::uint32_t x, std::uint32_t y) noexcept {
    return {static_cast<std::uint8_t>(x & 0xFFU), static_cast<std::uint8_t>(y & 0xFFU),
            static_cast<std::uint8_t>((x * 7U + y * 13U) & 0xFFU), 0xFFU};
}

SimulatorEnvironment::SimulatorEnvironment(SimulatorSetup setup)
    : impl_(std::make_unique<Impl>(std::move(setup))) {}

SimulatorEnvironment::~SimulatorEnvironment() = default;
SimulatorEnvironment::SimulatorEnvironment(SimulatorEnvironment &&) noexcept = default;
SimulatorEnvironment &SimulatorEnvironment::operator=(SimulatorEnvironment &&) noexcept = default;

EnvironmentCapabilities SimulatorEnvironment::capabilities() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->capabilities_locked();
}

Result<Observation> SimulatorEnvironment::observe(const ObservationRequest &request,
                                                  const OperationContext &context) {
    if (const auto validated = validate_observation_request(request); !validated) {
        return validated.error();
    }
    std::lock_guard lock(impl_->mutex);
    if (context.cancelled()) {
        return simulator_error(ErrorCode::Cancelled, "observe was cancelled before capture");
    }
    const EnvironmentCapabilities capabilities = impl_->capabilities_locked();
    if (const auto unsupported = unsupported_required_components(capabilities, request);
        !unsupported.empty()) {
        std::string message = "environment cannot provide required components: ";
        for (std::size_t index = 0; index < unsupported.size(); ++index) {
            if (index != 0) {
                message += ", ";
            }
            message += unsupported[index];
        }
        return simulator_error(ErrorCode::UnsupportedCapability, message);
    }

    const auto topology = impl_->make_topology_locked();
    if (!topology) {
        return topology.error();
    }

    Observation observation;
    observation.id = ObservationId::generate();
    observation.session_id = context.session;
    observation.topology = topology.value();
    observation.environment_epoch = impl_->epoch;

    const Timestamp now = Timestamp::now();
    const auto span_for = [&](const SimulatorComponentPlan &plan) {
        CaptureSpan span;
        span.clock_domain = ClockDomainId::generate();
        if (impl_->setup.atomic_components) {
            // One synthesized transaction: every component shares the span.
            span.normalized_begin = now;
            span.normalized_end = now;
            span.normalized_end.monotonic += kCaptureDuration;
        } else {
            span.normalized_begin = now;
            span.normalized_begin.monotonic += plan.capture_skew;
            span.normalized_end = span.normalized_begin;
            span.normalized_end.monotonic += kCaptureDuration + plan.capture_skew;
        }
        span.sync_quality = ClockSyncQuality::SameClock;
        return span;
    };

    const bool want_screen = request.required.screen || request.optional.screen;
    const bool want_structure = request.required.structure || request.optional.structure;
    const bool want_foreground = request.required.foreground || request.optional.foreground;
    const bool want_device = request.required.device || request.optional.device;

    const SimulatorDisplaySetup &primary =
        impl_->setup
            .displays[std::min(impl_->setup.primary_display, impl_->setup.displays.size() - 1)];

    bool captured_any = false;
    bool bumped = false;
    std::vector<std::string> missing_required;
    std::vector<std::string> missing_optional;

    const auto settle_epoch_after_capture = [&] {
        if (captured_any && impl_->setup.inject_mid_capture_epoch_bump && !bumped) {
            impl_->bump("injected mid-capture epoch invalidation");
            bumped = true;
        }
    };

    if (want_screen) {
        const SimulatorComponentPlan &plan = impl_->setup.screen_plan;
        if (plan.fails()) {
            (request.required.screen ? missing_required : missing_optional).push_back("screen");
        } else {
            ScreenFrameDescriptor descriptor;
            descriptor.frame_id = FrameId::generate();
            descriptor.display_id = primary.id;
            descriptor.width_pixels = primary.native_width_pixels;
            descriptor.height_pixels = primary.native_height_pixels;
            descriptor.pixel_format = PixelFormat::RGBA8888;
            descriptor.color_space = ColorSpace::SRGB;
            descriptor.alpha_mode = AlphaMode::Opaque;
            descriptor.native_rotation = primary.native_rotation;
            descriptor.planes.push_back(PlaneLayout{0U, primary.native_width_pixels * 4U, 4U,
                                                    primary.native_width_pixels,
                                                    primary.native_height_pixels});
            descriptor.pixel_space = make_space_id(primary.id, kSpaceKindFrame, impl_->epoch);
            descriptor.capture = span_for(plan);
            descriptor.coverage.includes_system_bars = true;

            std::vector<std::byte> payload;
            payload.resize(static_cast<std::size_t>(primary.native_width_pixels) * 4U *
                           primary.native_height_pixels);
            for (std::uint32_t y = 0; y < primary.native_height_pixels; ++y) {
                for (std::uint32_t x = 0; x < primary.native_width_pixels; ++x) {
                    const auto pixel = simulator_frame_pixel(x, y);
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * primary.native_width_pixels + x) * 4U;
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        payload[offset + channel] = static_cast<std::byte>(pixel[channel]);
                    }
                }
            }

            ArtifactWriteSpec spec;
            spec.media_type = "image/x-rgba8888";
            auto writer = impl_->artifacts.begin(spec);
            bool artifact_ok = writer.has_value();
            if (artifact_ok) {
                const auto written = writer.value().write(payload.data(), payload.size());
                artifact_ok = written.has_value();
            }
            if (artifact_ok) {
                const auto committed = impl_->artifacts.commit(writer.value());
                artifact_ok = committed.has_value();
                if (artifact_ok) {
                    descriptor.payload_artifact = committed.value().id;
                    impl_->published.emplace(committed.value().id, committed.value());
                }
            }
            const auto screen_failed = [&] {
                if (!artifact_ok) {
                    return true;
                }
                return !validate_frame_descriptor(descriptor).has_value();
            }();
            if (screen_failed) {
                (request.required.screen ? missing_required : missing_optional).push_back("screen");
            } else {
                ObservationComponent<ScreenFrameDescriptor> component;
                component.value = std::move(descriptor);
                component.capture = component.value.capture;
                component.quality = plan.quality;
                component.provenance.source = "simulator.screen.v1";
                component.provenance.method = "synthesized-frame";
                component.environment_epoch = impl_->epoch;
                observation.screen = std::move(component);
                captured_any = true;
            }
        }
        settle_epoch_after_capture();
    }

    if (want_structure) {
        const SimulatorComponentPlan &plan = impl_->setup.structure_plan;
        if (plan.fails()) {
            (request.required.structure ? missing_required : missing_optional)
                .push_back("structure");
        } else {
            const RectF content = content_region_of(primary);
            UiTreeSnapshot snapshot;
            snapshot.space = make_space_id(primary.id, kSpaceKindContent, impl_->epoch);
            snapshot.complete = true;
            snapshot.capture = span_for(plan);
            snapshot.max_depth_reached = 2;

            const auto add_node = [&](UiNodeId id, std::optional<UiNodeId> parent, UiRole role,
                                      const RectF &bounds, const std::string &text,
                                      UiNodeState state, UiNodeAction actions) {
                UiNode node;
                node.id = id;
                node.parent = parent;
                node.role = role;
                node.text = text;
                node.bounds = bounds;
                node.space = snapshot.space;
                node.state = state;
                node.supported_actions = actions;
                node.provenance.source = "simulator.structure.v1";
                node.provenance.method = "synthesized-tree";
                snapshot.nodes.push_back(std::move(node));
            };

            UiNodeId root_id;
            root_id.value = Id128{Id128::Bytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
            UiNodeId text_id;
            text_id.value = Id128{Id128::Bytes{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
            UiNodeId button_id;
            button_id.value = Id128{Id128::Bytes{3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
            UiNodeId field_id;
            field_id.value = Id128{Id128::Bytes{4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

            add_node(root_id, std::nullopt, UiRole::Root, content, "",
                     UiNodeState::Visible | UiNodeState::Enabled, UiNodeAction::None);
            add_node(text_id, root_id, UiRole::Text,
                     RectF{content.left + 4.0, content.top + 4.0, content.right - 4.0,
                           content.top + 14.0},
                     "simulator headline", UiNodeState::Visible, UiNodeAction::None);
            add_node(button_id, root_id, UiRole::Button,
                     RectF{content.left + 4.0, content.top + 20.0, content.left + 44.0,
                           content.top + 36.0},
                     "continue", UiNodeState::Visible | UiNodeState::Enabled,
                     UiNodeAction::Click | UiNodeAction::LongPress);
            add_node(field_id, root_id, UiRole::TextField,
                     RectF{content.left + 4.0, content.top + 40.0, content.right - 4.0,
                           content.top + 52.0},
                     "",
                     UiNodeState::Visible | UiNodeState::Enabled |
                         (impl_->setup.structure_password_field ? UiNodeState::Password
                                                                : UiNodeState::None),
                     UiNodeAction::TypeText);

            if (content.is_empty() || !validate_ui_tree_snapshot(snapshot).has_value()) {
                (request.required.structure ? missing_required : missing_optional)
                    .push_back("structure");
            } else {
                ObservationComponent<UiTreeSnapshot> component;
                component.value = std::move(snapshot);
                component.capture = component.value.capture;
                component.quality = plan.quality;
                component.provenance.source = "simulator.structure.v1";
                component.provenance.method = "synthesized-tree";
                component.environment_epoch = impl_->epoch;
                observation.structure = std::move(component);
                captured_any = true;
            }
        }
        settle_epoch_after_capture();
    }

    if (want_foreground) {
        const SimulatorComponentPlan &plan = impl_->setup.foreground_plan;
        if (plan.fails()) {
            (request.required.foreground ? missing_required : missing_optional)
                .push_back("foreground");
        } else {
            ObservationComponent<AppContext> component;
            component.value.package_name = "mira.simulator.app";
            component.value.activity_name = ".MainActivity";
            component.capture = span_for(plan);
            component.quality = plan.quality;
            component.provenance.source = "simulator.foreground.v1";
            component.provenance.method = "synthesized-context";
            component.environment_epoch = impl_->epoch;
            observation.foreground = std::move(component);
            captured_any = true;
        }
        settle_epoch_after_capture();
    }

    if (want_device) {
        const SimulatorComponentPlan &plan = impl_->setup.device_plan;
        if (plan.fails()) {
            (request.required.device ? missing_required : missing_optional).push_back("device");
        } else {
            ObservationComponent<DeviceState> component;
            component.value.battery_percent = 77;
            component.value.charging = true;
            component.value.thermal_state = "nominal";
            component.capture = span_for(plan);
            component.quality = plan.quality;
            component.provenance.source = "simulator.device.v1";
            component.provenance.method = "synthesized-state";
            component.environment_epoch = impl_->epoch;
            observation.device = std::move(component);
            captured_any = true;
        }
        settle_epoch_after_capture();
    }

    // Any epoch change during capture invalidates every coordinate in this
    // observation; Mira drops the batch instead of mixing epochs.
    const auto stale_component = [&](const auto &component) {
        return component.environment_epoch != impl_->epoch;
    };
    bool has_stale = false;
    if (observation.screen.has_value() && stale_component(*observation.screen)) {
        has_stale = true;
    }
    if (observation.structure.has_value() && stale_component(*observation.structure)) {
        has_stale = true;
    }
    if (observation.foreground.has_value() && stale_component(*observation.foreground)) {
        has_stale = true;
    }
    if (observation.device.has_value() && stale_component(*observation.device)) {
        has_stale = true;
    }
    if (has_stale) {
        return simulator_error(ErrorCode::StaleObservation,
                               "environment epoch changed during capture");
    }
    observation.environment_epoch = impl_->epoch;

    if (!missing_required.empty()) {
        std::string message = "missing required components: ";
        for (std::size_t index = 0; index < missing_required.size(); ++index) {
            if (index != 0) {
                message += ", ";
            }
            message += missing_required[index];
        }
        return simulator_error(ErrorCode::InvalidObservation, message);
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
    observation.aggregate_span.normalized_begin = earliest.value_or(now);
    observation.aggregate_span.normalized_end = latest.value_or(now);
    observation.aggregate_span.sync_quality = ClockSyncQuality::SameClock;

    const auto skew = observation_component_skew(observation);
    if (impl_->setup.atomic_components) {
        // The simulator returns all components from one synthesized
        // transaction, so the atomic claim is a platform guarantee here.
        observation.atomicity = ObservationAtomicity::Atomic;
    } else if (impl_->setup.declared_component_skew > std::chrono::nanoseconds::zero() &&
               skew <= impl_->setup.declared_component_skew) {
        observation.atomicity = ObservationAtomicity::BoundedSkew;
    } else {
        observation.atomicity = ObservationAtomicity::NonAtomic;
    }

    const auto evaluation = evaluate_observation(observation, request, Timestamp::now());
    const auto quality_rank = [](ComponentQuality quality) noexcept {
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
    const auto worst = [&](const ComponentQuality quality) {
        const int rank = quality_rank(quality);
        if (rank > overall_rank) {
            overall_rank = rank;
            overall = quality;
        }
    };
    if (observation.screen.has_value()) {
        worst(observation.screen->quality);
    }
    if (observation.structure.has_value()) {
        worst(observation.structure->quality);
    }
    if (observation.foreground.has_value()) {
        worst(observation.foreground->quality);
    }
    if (observation.device.has_value()) {
        worst(observation.device->quality);
    }
    observation.quality.overall = overall;
    observation.quality.screen_missing = want_screen && !observation.screen.has_value();
    observation.quality.structure_truncated =
        observation.structure.has_value() &&
        (observation.structure->value.truncated ||
         observation.structure->quality == ComponentQuality::Partial);
    observation.quality.components_skewed =
        observation.atomicity == ObservationAtomicity::NonAtomic;
    for (const std::string &deficiency : evaluation.deficiencies) {
        observation.quality.degradations.push_back(deficiency);
    }
    for (const std::string &name : missing_optional) {
        observation.quality.degradations.push_back("optional " + name + " unavailable");
    }

    ++impl_->observations;
    return observation;
}

Result<ExecutionReceipt> SimulatorEnvironment::execute(const InputSequence &input,
                                                       const OperationContext &context) {
    if (input.events.empty()) {
        return simulator_error(ErrorCode::InvalidArgument, "input sequence must not be empty");
    }
    std::lock_guard lock(impl_->mutex);
    if (context.cancelled()) {
        return simulator_error(ErrorCode::Cancelled, "execute was cancelled before dispatch");
    }
    const SimulatorDisplaySetup *target = nullptr;
    if (input.display.has_value()) {
        target = find_display(impl_->setup.displays, *input.display);
        if (target == nullptr || !target->active) {
            return simulator_error(ErrorCode::InvalidArgument,
                                   "input targets an unknown or inactive display");
        }
    } else {
        target = &impl_->setup.displays[std::min(impl_->setup.primary_display,
                                                 impl_->setup.displays.size() - 1)];
    }
    for (const InputEvent &event : input.events) {
        std::string failure;
        if (!parse_input_event(event, failure)) {
            return simulator_error(ErrorCode::InvalidArgument, "input event rejected: " + failure);
        }
    }
    impl_->executed.push_back(input);
    impl_->was_interrupted = false;
    ExecutionReceipt receipt;
    receipt.status = ExecutionStatus::Completed;
    receipt.side_effect_may_have_occurred = false;
    receipt.environment_epoch = impl_->epoch;
    receipt.safe_message = "simulator applied " + std::to_string(input.events.size()) +
                           " events on display " + target->id.to_string();
    return receipt;
}

Result<void> SimulatorEnvironment::interrupt(const OperationContext & /*context*/) {
    std::lock_guard lock(impl_->mutex);
    impl_->was_interrupted = true;
    return Result<void>{};
}

EnvironmentEpoch SimulatorEnvironment::environment_epoch() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->epoch;
}

std::vector<std::string> SimulatorEnvironment::epoch_history() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->epoch_reasons;
}

Result<DisplayTopology> SimulatorEnvironment::topology() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->make_topology_locked();
}

std::vector<CoordinateTransform>
SimulatorEnvironment::coordinate_transforms(EnvironmentEpoch epoch) const {
    std::lock_guard lock(impl_->mutex);
    if (epoch != impl_->epoch) {
        return {};
    }
    std::vector<CoordinateTransform> transforms;
    for (const SimulatorDisplaySetup &display : impl_->setup.displays) {
        const CoordinateSpaceId native = make_space_id(display.id, kSpaceKindNative, epoch);
        const CoordinateSpaceId logical = make_space_id(display.id, kSpaceKindLogical, epoch);
        const CoordinateSpaceId canonical = make_space_id(display.id, kSpaceKindCanonical, epoch);
        const CoordinateSpaceId content = make_space_id(display.id, kSpaceKindContent, epoch);
        const CoordinateSpaceId frame = make_space_id(display.id, kSpaceKindFrame, epoch);

        const double native_width = static_cast<double>(display.native_width_pixels);
        const double native_height = static_cast<double>(display.native_height_pixels);
        if (auto edge = make_native_to_logical_transform(
                native, logical, RectF::from_origin_size(native_width, native_height), native_width,
                native_height, display.pixels_per_logical, display.native_rotation)) {
            edge.value().environment_epoch = epoch;
            transforms.push_back(edge.value());
        }

        const RectF action_region = action_region_of(display);
        if (auto edge = make_logical_to_canonical_transform(
                logical, canonical, action_region, action_region.width(), action_region.height())) {
            edge.value().environment_epoch = epoch;
            transforms.push_back(edge.value());
        }

        const RectF content_region = content_region_of(display);
        if (!content_region.is_empty()) {
            CoordinateTransform inset_edge;
            inset_edge.from = content;
            inset_edge.to = logical;
            inset_edge.matrix =
                Matrix3x3::translation(display.system_insets.left, display.system_insets.top);
            inset_edge.valid_source_region = content_region;
            double logical_width = 0.0;
            double logical_height = 0.0;
            logical_extent(display, logical_width, logical_height);
            inset_edge.valid_target_region = RectF::from_origin_size(logical_width, logical_height);
            inset_edge.environment_epoch = epoch;
            if (validate_coordinate_transform(inset_edge).has_value()) {
                transforms.push_back(inset_edge);
            }
        }

        if (auto edge = make_scale_transform(
                frame, native, RectF::from_origin_size(native_width, native_height), native_width,
                native_height, native_width, native_height)) {
            edge.value().environment_epoch = epoch;
            transforms.push_back(edge.value());
        }
    }
    return transforms;
}

std::vector<InputSequence> SimulatorEnvironment::executed_inputs() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->executed;
}

bool SimulatorEnvironment::interrupted() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->was_interrupted;
}

std::uint64_t SimulatorEnvironment::observation_count() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->observations;
}

Result<std::size_t> SimulatorEnvironment::open_artifact(const ArtifactId &id,
                                                        std::vector<std::byte> &out_bytes) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->published.find(id);
    if (found == impl_->published.end()) {
        return simulator_error(ErrorCode::NotFound, "artifact was not published by simulator");
    }
    auto reader = impl_->artifacts.open(found->second);
    if (!reader) {
        return reader.error();
    }
    out_bytes = reader.value().bytes();
    return out_bytes.size();
}

Result<EnvironmentEpoch> SimulatorEnvironment::rotate_display(DisplayId display,
                                                              Rotation rotation) {
    if (rotation == Rotation::Unknown) {
        return simulator_error(ErrorCode::InvalidArgument, "rotation must be known");
    }
    std::lock_guard lock(impl_->mutex);
    SimulatorDisplaySetup *target = nullptr;
    for (SimulatorDisplaySetup &candidate : impl_->setup.displays) {
        if (candidate.id == display) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr) {
        return simulator_error(ErrorCode::NotFound, "display was not found");
    }
    if (target->native_rotation == rotation) {
        return impl_->epoch;
    }
    // Rotations 90/270 read the panel transposed relative to 0/180; the
    // native extents swap when crossing between those groups.
    const bool was_transposed = target->native_rotation == Rotation::Rotation90 ||
                                target->native_rotation == Rotation::Rotation270;
    const bool now_transposed =
        rotation == Rotation::Rotation90 || rotation == Rotation::Rotation270;
    if (was_transposed != now_transposed) {
        std::swap(target->native_width_pixels, target->native_height_pixels);
    }
    target->native_rotation = rotation;
    return impl_->bump("display " + display.to_string() + " rotated");
}

Result<EnvironmentEpoch> SimulatorEnvironment::set_density(DisplayId display,
                                                           double pixels_per_logical) {
    if (!std::isfinite(pixels_per_logical) || pixels_per_logical <= 0.0) {
        return simulator_error(ErrorCode::InvalidArgument, "pixels per logical must be positive");
    }
    std::lock_guard lock(impl_->mutex);
    SimulatorDisplaySetup *target = nullptr;
    for (SimulatorDisplaySetup &candidate : impl_->setup.displays) {
        if (candidate.id == display) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr) {
        return simulator_error(ErrorCode::NotFound, "display was not found");
    }
    target->pixels_per_logical = pixels_per_logical;
    return impl_->bump("display " + display.to_string() + " density changed");
}

Result<EnvironmentEpoch> SimulatorEnvironment::add_display(SimulatorDisplaySetup setup) {
    if (setup.id.is_nil()) {
        return simulator_error(ErrorCode::InvalidArgument, "display id must be non-nil");
    }
    std::lock_guard lock(impl_->mutex);
    if (find_display(impl_->setup.displays, setup.id) != nullptr) {
        return simulator_error(ErrorCode::AlreadyExists, "display id already present");
    }
    impl_->setup.displays.push_back(setup);
    return impl_->bump("display added");
}

Result<EnvironmentEpoch> SimulatorEnvironment::remove_display(DisplayId display) {
    std::lock_guard lock(impl_->mutex);
    const auto found = std::find_if(
        impl_->setup.displays.begin(), impl_->setup.displays.end(),
        [&](const SimulatorDisplaySetup &candidate) { return candidate.id == display; });
    if (found == impl_->setup.displays.end()) {
        return simulator_error(ErrorCode::NotFound, "display was not found");
    }
    std::size_t others_active = 0;
    for (const SimulatorDisplaySetup &candidate : impl_->setup.displays) {
        if (&candidate != &*found && candidate.active) {
            ++others_active;
        }
    }
    if (others_active == 0) {
        return simulator_error(ErrorCode::InvalidArgument,
                               "topology requires at least one active display");
    }
    impl_->setup.displays.erase(found);
    if (impl_->setup.primary_display >= impl_->setup.displays.size()) {
        impl_->setup.primary_display = 0;
    }
    return impl_->bump("display removed");
}

EnvironmentEpoch SimulatorEnvironment::invalidate_epoch(std::string reason) {
    std::lock_guard lock(impl_->mutex);
    return impl_->bump(std::move(reason));
}

void SimulatorEnvironment::set_component_plans(SimulatorComponentPlan screen,
                                               SimulatorComponentPlan structure) {
    std::lock_guard lock(impl_->mutex);
    impl_->setup.screen_plan = std::move(screen);
    impl_->setup.structure_plan = std::move(structure);
}

} // namespace mira::adapters::simulator
