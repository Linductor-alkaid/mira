#pragma once

#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/observation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// A component source captures one component. Sources run as Executor tasks;
// they must honor the context deadline and cancellation probe and return a
// component stamped with the environment epoch they captured under.
template <typename T>
using ObservationComponentSource = std::function<Result<ObservationComponent<T>>(
    const ObservationRequest &, const OperationContext &)>;

// Supplies the display topology the observation aggregates against.
using TopologySource =
    std::function<Result<DisplayTopology>(const ObservationRequest &, const OperationContext &)>;

struct ObservationPipelineConfig final {
    // Grace period after the deadline during which straggler captures are
    // still consumed before they move to the pending-drain set.
    std::chrono::milliseconds drain_grace{200};
    // Fail closed when required components miss the deadline. When false the
    // pipeline publishes an explicitly partial observation instead.
    bool fail_on_missing_required = true;
    // Bound on futures parked for later drains; excess is a hard failure.
    std::size_t max_pending_drains = 64;
    // Bound on sources per observation.
    std::size_t max_sources = 8;
};

struct ObservationPipelineStats final {
    std::uint64_t observations_published = 0;
    std::uint64_t components_settled = 0;
    std::uint64_t components_timed_out = 0;
    std::uint64_t components_failed = 0;
    std::uint64_t stale_components_dropped = 0;
    std::uint64_t submission_rejections = 0;
    std::uint64_t publish_callback_failures = 0;
};

// Orchestrates component captures into one honest Observation: sources run
// as Executor tasks, settle by the operation deadline, and late or stale
// components are dropped and counted instead of mixed in. The pipeline never
// marks an observation Atomic: it cannot prove a platform transaction, only
// a platform environment can.
class ObservationPipeline final {
  public:
    explicit ObservationPipeline(executor::Executor &executor,
                                 ObservationPipelineConfig config = {});
    ~ObservationPipeline();

    ObservationPipeline(const ObservationPipeline &) = delete;
    ObservationPipeline &operator=(const ObservationPipeline &) = delete;
    ObservationPipeline(ObservationPipeline &&) = delete;
    ObservationPipeline &operator=(ObservationPipeline &&) = delete;

    void set_topology_source(TopologySource source);
    void set_screen_source(ObservationComponentSource<ScreenFrameDescriptor> source);
    void set_structure_source(ObservationComponentSource<UiTreeSnapshot> source);
    void set_foreground_source(ObservationComponentSource<AppContext> source);
    void set_device_source(ObservationComponentSource<DeviceState> source);
    // Called after an observation is constructed. Must be bounded and
    // non-blocking; exceptions are isolated and counted as diagnostics.
    void set_publish_observer(std::function<void(const Observation &)> observer);

    // Runs one aggregation. Observations are serialized per pipeline
    // instance; call from an Executor task or a dedicated owner thread.
    Result<Observation> observe(const ObservationRequest &request, const OperationContext &context);

    // Waits for and consumes straggler futures parked by earlier observes.
    // The owner must call this before shutting the Executor down.
    Result<std::size_t> drain_pending(std::chrono::milliseconds timeout);

    [[nodiscard]] ObservationPipelineStats stats() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
