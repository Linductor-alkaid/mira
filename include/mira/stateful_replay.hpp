#pragma once

#include <mira/artifact_store.hpp>
#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/task_checkpoint.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Analysis replay over checkpoints, memory and artifacts (M4-15)
// ---------------------------------------------------------------------------

// Bitemporal "as of" coordinates; nullopt fields default to the latest state.
struct ReplayAsOf final {
    std::optional<std::uint64_t> through_event_sequence;
    std::optional<std::chrono::system_clock::time_point> recorded_at;
    std::optional<std::chrono::system_clock::time_point> valid_at;
};

// One artifact the replay could not open: referenced but missing or deleted.
// Replays degrade quality; they never fabricate content in its place.
struct MissingArtifactNote final {
    ArtifactId id;
    std::string reason;
};

struct AnalysisReplayReport final {
    std::optional<TaskCheckpoint> checkpoint;
    std::vector<EventEnvelope> events_after_checkpoint;
    MemoryQueryResult memory;
    std::vector<MissingArtifactNote> missing_artifacts;
    bool checkpoint_present = false;
    bool store_read_only = false;
    bool memory_degraded = false;
    std::string note;

    // Replay invariants (asserted by tests): no capability that can produce
    // real side effects is ever exercised.
    static constexpr std::uint32_t capability_mask = 0; // no Network/Tool/Input
};

// Read-only analysis view over recorded state. It composes the durable
// stores that are already loaded; it never dispatches input, network or tool
// actions, never captures, and never loads provider continuations (rebuilds
// go through the local checkpoint instead).
class AnalysisReplay final {
  public:
    AnalysisReplay(IEventStore &events, ICheckpointStore *checkpoints, IMemory *memory,
                   IArtifactStore *artifacts);

    // Assembles the replay view for one task. Missing stores simply yield
    // empty sections with explicit notes; missing artifacts degrade.
    [[nodiscard]] Result<AnalysisReplayReport>
    inspect(TaskId task, SessionId session, const MemoryScope &memory_scope,
            const ReplayAsOf &as_of = ReplayAsOf{}) const;

  private:
    [[nodiscard]] Result<std::vector<MissingArtifactNote>>
    collect_missing_artifacts(const TaskCheckpoint &checkpoint) const;

    IEventStore &events_;
    ICheckpointStore *checkpoints_ = nullptr;
    IMemory *memory_ = nullptr;
    IArtifactStore *artifacts_ = nullptr;
};

} // namespace mira
