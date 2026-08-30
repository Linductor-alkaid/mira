#pragma once

#include <mira/artifact_store.hpp>
#include <mira/environment.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira::adapters::simulator {

// One simulated display. Native extents describe the current native pixel
// space (what a captured frame contains); the logical extent follows from
// the rotation and pixels-per-logical factor.
struct SimulatorDisplaySetup final {
    DisplayId id;
    std::uint32_t native_width_pixels = 240;
    std::uint32_t native_height_pixels = 320;
    Rotation native_rotation = Rotation::Rotation0;
    // Native pixels per logical unit (px/dp); must be positive.
    double pixels_per_logical = 1.0;
    // System insets in logical units carve out the content viewport.
    Insets system_insets;
    // Letterbox: the interactable window region in logical units. When set,
    // the canonical action space covers this region instead of the whole
    // logical display.
    std::optional<RectF> window_region;
    bool active = true;
};

// Per-component behaviour injection. Skew offsets only shift the reported
// capture span; the simulator never sleeps, so tests stay deterministic.
struct SimulatorComponentPlan final {
    std::chrono::milliseconds capture_skew{0};
    ComponentQuality quality = ComponentQuality::Good;
    std::optional<ErrorCode> failure_code;
    std::string failure_message;
    [[nodiscard]] bool fails() const noexcept { return failure_code.has_value(); }
};

struct SimulatorSetup final {
    std::vector<SimulatorDisplaySetup> displays;
    std::size_t primary_display = 0;
    SimulatorComponentPlan screen_plan;
    SimulatorComponentPlan structure_plan;
    SimulatorComponentPlan foreground_plan;
    SimulatorComponentPlan device_plan;
    // All components share one capture span, simulating a platform that
    // returns them from a single transaction.
    bool atomic_components = false;
    // Declared worst-case component skew published through capabilities.
    std::chrono::nanoseconds declared_component_skew{0};
    // Bump the environment epoch after the first captured component; the
    // observe must then settle StaleObservation instead of mixing epochs.
    bool inject_mid_capture_epoch_bump = false;
    // Include a password text field with empty text in the UI tree.
    bool structure_password_field = false;

    // Contract-test fixtures. Dimensions are intentionally small so frame
    // payloads stay cheap while exercising rotation, density, letterbox,
    // inset and multi-display behaviour.
    [[nodiscard]] static SimulatorSetup single_display();
    [[nodiscard]] static SimulatorSetup letterboxed_display();
    [[nodiscard]] static SimulatorSetup inset_display();
    [[nodiscard]] static SimulatorSetup dual_display();
};

// Deterministic RGBA8888 pixel the simulator writes at (x, y); tests use it
// to verify frame payloads survive artifact publication unchanged.
[[nodiscard]] std::array<std::uint8_t, 4> simulator_frame_pixel(std::uint32_t x,
                                                                std::uint32_t y) noexcept;

// Reference environment implementing the M2 observation and coordinate
// contracts: request-driven observe, explicit transform chains per epoch,
// honest atomicity classification and injectable epoch invalidation.
class SimulatorEnvironment final : public IEnvironment {
  public:
    explicit SimulatorEnvironment(SimulatorSetup setup = SimulatorSetup::single_display());
    ~SimulatorEnvironment() override;

    SimulatorEnvironment(const SimulatorEnvironment &) = delete;
    SimulatorEnvironment &operator=(const SimulatorEnvironment &) = delete;
    SimulatorEnvironment(SimulatorEnvironment &&) noexcept;
    SimulatorEnvironment &operator=(SimulatorEnvironment &&) noexcept;

    EnvironmentCapabilities capabilities() const override;
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override;
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override;
    Result<void> interrupt(const OperationContext &context) override;

    // --- contract-test surface -------------------------------------------

    [[nodiscard]] EnvironmentEpoch environment_epoch() const;
    [[nodiscard]] std::vector<std::string> epoch_history() const;
    [[nodiscard]] Result<DisplayTopology> topology() const;
    // All validated transform edges for the requested epoch; transforms from
    // older epochs are absent, so stale chains fail resolution by design.
    [[nodiscard]] std::vector<CoordinateTransform>
    coordinate_transforms(EnvironmentEpoch epoch) const;
    [[nodiscard]] std::vector<InputSequence> executed_inputs() const;
    [[nodiscard]] bool interrupted() const noexcept;
    [[nodiscard]] std::uint64_t observation_count() const;
    [[nodiscard]] Result<std::size_t> open_artifact(const ArtifactId &id,
                                                    std::vector<std::byte> &out_bytes) const;

    // Mutations that invalidate coordinates; each bumps the epoch.
    [[nodiscard]] Result<EnvironmentEpoch> rotate_display(DisplayId display, Rotation rotation);
    [[nodiscard]] Result<EnvironmentEpoch> set_density(DisplayId display,
                                                       double pixels_per_logical);
    [[nodiscard]] Result<EnvironmentEpoch> add_display(SimulatorDisplaySetup setup);
    [[nodiscard]] Result<EnvironmentEpoch> remove_display(DisplayId display);
    EnvironmentEpoch invalidate_epoch(std::string reason);
    void set_component_plans(SimulatorComponentPlan screen, SimulatorComponentPlan structure);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira::adapters::simulator
