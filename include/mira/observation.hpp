#pragma once

#include <mira/coordinates.hpp>
#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Capture primitives
// ---------------------------------------------------------------------------

enum class ClockSyncQuality : std::uint8_t {
    Synced,    // Same clock domain as the runtime clock.
    SameClock, // Same domain family, no measurable offset.
    Estimated, // Offset estimated across domains.
    Unknown,
};

enum class ComponentQuality : std::uint8_t {
    Good,
    Degraded,    // Captured with reduced fidelity or partial coverage.
    Partial,     // Captured but incomplete (e.g. truncated tree).
    Unavailable, // Requested but not delivered.
};

struct Provenance final {
    // Stable provider/backend identifier, e.g. "simulator.screen.v1".
    std::string source;
    // How the value was produced, e.g. "media-projection", "accessibility".
    std::string method;
    std::optional<Sha256Digest> input_digest;
};

// Capture interval in the component's own clock domain plus normalized
// timestamps. Mira never rewrites these after publication.
struct CaptureSpan final {
    ClockDomainId clock_domain;
    std::int64_t begin_ticks = 0;
    std::int64_t end_ticks = 0;
    Timestamp normalized_begin;
    Timestamp normalized_end;
    ClockSyncQuality sync_quality = ClockSyncQuality::Unknown;
    [[nodiscard]] std::chrono::nanoseconds normalized_duration() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(normalized_end.monotonic -
                                                                    normalized_begin.monotonic);
    }
};

enum class ObservationAtomicity : std::uint8_t {
    Atomic,      // Platform guarantees one transaction/frame id.
    BoundedSkew, // Skew within the capability-declared bound.
    NonAtomic,   // No reliable skew bound; consumers use per-component age.
};

enum class ObservationMode : std::uint8_t {
    Full,         // Screen, structure, app and device context.
    Verification, // Minimal evidence for an expected outcome.
    Diff,         // Diff against a baseline observation.
    Roi,          // Restricted to referenced regions.
};

[[nodiscard]] Result<ObservationMode> parse_observation_mode(std::uint8_t value) noexcept;

// Regions reference the observation and space they were defined in; they can
// never be applied to a new epoch without re-resolution.
struct RegionRef final {
    RegionId id;
    ObservationId source_observation;
    RectF region;
    CoordinateSpaceId space;
};

struct RequiredComponents final {
    bool screen = false;
    bool structure = false;
    bool foreground = false;
    bool device = false;
    std::size_t perception = 0;
};

struct ObservationRequest final {
    ObservationMode mode = ObservationMode::Full;
    RequiredComponents required;
    RequiredComponents optional;
    std::vector<RegionRef> regions;
    std::optional<ObservationId> baseline;
    // Provisional defaults pending DEC on environment alpha defaults.
    std::chrono::milliseconds max_age{2000};
    std::chrono::milliseconds max_component_skew{150};
};

[[nodiscard]] Result<void> validate_observation_request(const ObservationRequest &request);

template <typename T> struct ObservationComponent final {
    T value;
    CaptureSpan capture;
    ComponentQuality quality = ComponentQuality::Good;
    Provenance provenance;
    EnvironmentEpoch environment_epoch = 0;
};

// ---------------------------------------------------------------------------
// Screen frames
// ---------------------------------------------------------------------------

enum class PixelFormat : std::uint8_t {
    RGBA8888,
    BGRA8888,
    RGBX8888,
    NV12,
    YUV420P,
    Gray8,
    Unknown,
};

enum class ColorSpace : std::uint8_t { SRGB, DisplayP3, BT709, Unknown };

enum class AlphaMode : std::uint8_t { Opaque, Premultiplied, Postmultiplied, Unknown };

[[nodiscard]] Result<PixelFormat> parse_pixel_format(std::uint8_t value) noexcept;
[[nodiscard]] Result<ColorSpace> parse_color_space(std::uint8_t value) noexcept;
[[nodiscard]] std::uint32_t bytes_per_pixel_sample(PixelFormat format) noexcept;
[[nodiscard]] std::uint32_t expected_plane_count(PixelFormat format) noexcept;

struct PlaneLayout final {
    std::uint64_t offset = 0;
    std::uint32_t row_stride = 0;
    std::uint32_t pixel_stride = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class FrameMaskReason : std::uint8_t {
    SecureSurface,
    SystemOverlay,
    Cutout,
    Offscreen,
    Unknown,
};

// Regions the platform withheld or cannot vouch for. Masked areas carry no
// trustworthy pixels and must never be read as black content.
struct FrameMaskedRegion final {
    RectF bounds;
    CoordinateSpaceId space;
    FrameMaskReason reason = FrameMaskReason::Unknown;
};

struct FrameCoverage final {
    bool includes_system_bars = false;
    bool includes_ime = false;
    bool includes_secure_surface = false;
    bool includes_overlays = false;
    std::vector<FrameMaskedRegion> masked_regions;
};

struct ScreenFrameDescriptor final {
    FrameId frame_id;
    DisplayId display_id;
    std::uint32_t width_pixels = 0;
    std::uint32_t height_pixels = 0;
    PixelFormat pixel_format = PixelFormat::Unknown;
    ColorSpace color_space = ColorSpace::Unknown;
    AlphaMode alpha_mode = AlphaMode::Unknown;
    Rotation native_rotation = Rotation::Unknown;
    std::vector<PlaneLayout> planes;
    CoordinateSpaceId pixel_space;
    CaptureSpan capture;
    // Published, immutable payload in the ArtifactStore.
    ArtifactId payload_artifact;
    FrameCoverage coverage;
};

[[nodiscard]] Result<void> validate_frame_descriptor(const ScreenFrameDescriptor &descriptor);

// ---------------------------------------------------------------------------
// Display topology
// ---------------------------------------------------------------------------

struct Insets final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    friend constexpr bool operator==(const Insets &, const Insets &) noexcept = default;
};

struct DisplayInfo final {
    DisplayId id;
    std::string name;
    std::uint32_t native_width_pixels = 0;
    std::uint32_t native_height_pixels = 0;
    Rotation native_rotation = Rotation::Rotation0;
    // Logical units per native pixel; e.g. 1/density where density is px/dp.
    double density_scale = 1.0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    Insets system_insets;
    bool active = true;
    friend constexpr bool operator==(const DisplayInfo &, const DisplayInfo &) noexcept = default;
};

struct DisplayTopology final {
    EnvironmentEpoch environment_epoch = 0;
    std::vector<DisplayInfo> displays;
    Sha256Digest topology_hash;
};

[[nodiscard]] Result<DisplayTopology> make_display_topology(EnvironmentEpoch epoch,
                                                            std::vector<DisplayInfo> displays);

// ---------------------------------------------------------------------------
// UI tree
// ---------------------------------------------------------------------------

enum class UiRole : std::uint8_t {
    Unknown,
    Root,
    Window,
    Pane,
    AppBar,
    Button,
    CheckBox,
    Switch,
    TextField,
    Text,
    Image,
    List,
    ListItem,
    Grid,
    Slider,
    Tab,
    Dialog,
    Menu,
    MenuItem,
    WebView,
    Custom,
};

enum class UiNodeState : std::uint32_t {
    None = 0U,
    Visible = 1U << 0U,
    Focused = 1U << 1U,
    Selected = 1U << 2U,
    Checked = 1U << 3U,
    Enabled = 1U << 4U,
    Scrollable = 1U << 5U,
    // Secure/password nodes must never carry plaintext text.
    Password = 1U << 6U,
};

[[nodiscard]] constexpr UiNodeState operator|(UiNodeState lhs, UiNodeState rhs) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<UiNodeState>(static_cast<std::uint32_t>(lhs) |
                                    static_cast<std::uint32_t>(rhs));
}
[[nodiscard]] constexpr UiNodeState operator&(UiNodeState lhs, UiNodeState rhs) noexcept {
    return static_cast<UiNodeState>(static_cast<std::uint32_t>(lhs) &
                                    static_cast<std::uint32_t>(rhs));
}
[[nodiscard]] constexpr bool has_state(UiNodeState flags, UiNodeState flag) noexcept {
    return (flags & flag) == flag;
}

enum class UiNodeAction : std::uint32_t {
    None = 0U,
    Click = 1U << 0U,
    LongPress = 1U << 1U,
    Scroll = 1U << 2U,
    TypeText = 1U << 3U,
    ClearSelection = 1U << 4U,
    Select = 1U << 5U,
    Dismiss = 1U << 6U,
    Custom = 1U << 7U,
};

[[nodiscard]] constexpr UiNodeAction operator|(UiNodeAction lhs, UiNodeAction rhs) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<UiNodeAction>(static_cast<std::uint32_t>(lhs) |
                                     static_cast<std::uint32_t>(rhs));
}
[[nodiscard]] constexpr bool supports_action(UiNodeAction flags, UiNodeAction action) noexcept {
    return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(action)) != 0U;
}

// A non-permanent identity hint (e.g. resource-id path). Node ids from the
// platform usually do not survive window updates.
struct StableNodeHint final {
    std::string hint;
};

struct UiNode final {
    UiNodeId id;
    std::optional<UiNodeId> parent;
    UiRole role = UiRole::Unknown;
    std::string text;
    std::string content_description;
    RectF bounds;
    CoordinateSpaceId space;
    UiNodeState state = UiNodeState::None;
    UiNodeAction supported_actions = UiNodeAction::None;
    std::optional<StableNodeHint> stable_hint;
    Provenance provenance;
};

struct UiTreeSnapshot final {
    std::vector<UiNode> nodes;
    // Declares whether the snapshot is authoritative for the full window.
    bool complete = false;
    bool truncated = false;
    bool visible_only = false;
    std::uint32_t max_depth_reached = 0;
    CaptureSpan capture;
    CoordinateSpaceId space;
    // Incremental snapshots reference their base; a missing base must be
    // rejected with a request for a full snapshot.
    std::optional<ObservationId> base_snapshot;
    std::uint32_t base_sequence = 0;
};

[[nodiscard]] Result<void> validate_ui_tree_snapshot(const UiTreeSnapshot &snapshot);

enum class ElementSource : std::uint8_t { UiTree, Ocr, Detector, Fused, Unknown };

// A reference to a UI element valid only within its observation, epoch and
// freshness policy. It is evidence, not permanent identity.
struct ElementRef final {
    ObservationId observation_id;
    EnvironmentEpoch environment_epoch = 0;
    ElementSource source = ElementSource::UiTree;
    StableNodeHint stable_hint;
    RectF bounds;
    CoordinateSpaceId space;
    Sha256Digest evidence_digest;
};

// ---------------------------------------------------------------------------
// Observation aggregate
// ---------------------------------------------------------------------------

// Minimal placeholder contracts; M5 extends perception evidence and device
// context without breaking these shapes.
struct PerceptionEvidence final {
    EvidenceId id;
    std::string kind; // e.g. "ocr.line", "detector.box"
    std::string label;
    RectF bounds;
    CoordinateSpaceId space;
    std::optional<ArtifactId> artifact;
    std::optional<FrameId> source_frame;
};

struct AppContext final {
    std::string package_name;
    std::string activity_name;
    bool sensitive = false;
};

struct DeviceState final {
    std::uint8_t battery_percent = 0;
    bool charging = false;
    std::string thermal_state;
};

struct ObservationQuality final {
    ComponentQuality overall = ComponentQuality::Good;
    bool screen_missing = false;
    bool structure_truncated = false;
    bool components_skewed = false;
    std::vector<std::string> degradations;
};

struct Observation final {
    ObservationId id;
    SessionId session_id;
    EnvironmentEpoch environment_epoch = 0;
    CaptureSpan aggregate_span;
    ObservationAtomicity atomicity = ObservationAtomicity::NonAtomic;
    DisplayTopology topology;
    std::optional<ObservationComponent<ScreenFrameDescriptor>> screen;
    std::optional<ObservationComponent<UiTreeSnapshot>> structure;
    std::vector<ObservationComponent<PerceptionEvidence>> perception;
    std::optional<ObservationComponent<AppContext>> foreground;
    std::optional<ObservationComponent<DeviceState>> device;
    ObservationQuality quality;
};

// Maximum pairwise gap between component capture starts, from normalized
// monotonic timestamps. Components captured together by construction report
// equal starts and therefore zero skew.
[[nodiscard]] std::chrono::nanoseconds
observation_component_skew(const Observation &observation) noexcept;

struct ObservationEvaluation final {
    bool satisfies_request = false;
    bool fresh_enough = false;
    ObservationAtomicity effective_atomicity = ObservationAtomicity::NonAtomic;
    std::chrono::nanoseconds component_skew{};
    std::vector<std::string> deficiencies;
};

// Checks an observation against a request: required components present, age
// within max_age, and component skew classified against max_component_skew.
// Returning false never authorizes guessing; the caller re-observes or
// degrades according to policy.
[[nodiscard]] ObservationEvaluation evaluate_observation(const Observation &observation,
                                                         const ObservationRequest &request,
                                                         const Timestamp &now);

} // namespace mira
