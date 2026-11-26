#pragma once

#include <mira/environment.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/world_fact.hpp>

#include <functional>
#include <memory>

namespace mira {

struct WorldFactRecordingBinding final {
    RuntimeId runtime;
    MemoryScope verified_scope;
    std::string verified_key;
    MemoryScope world_scope;
    WorldProperty property;
};

struct WorldFactSource final {
    SessionSequence sequence = 0;
    EventId event;
};

// Pins the host's source and destination authorization through the mutation.
// The host serializes erasure and source invalidation with the lease lifetime.
class IWorldFactRecordingLease {
  public:
    virtual ~IWorldFactRecordingLease() = default;
    [[nodiscard]] virtual Result<void> validate() = 0;
};

using WorldFactRecordingLeaseAcquirer =
    std::function<Result<std::unique_ptr<IWorldFactRecordingLease>>(
        const WorldFactRecordingBinding &, const WorldFactSource &, const OperationContext &)>;

// Called in an Executor-managed host operation with an authorized store pair.
// The source is a committed v1 TaskFactVerified event in the context's task.
[[nodiscard]] Result<ConsolidationReport>
record_verified_world_fact(IMemory &memory, const IEventStore &events,
                           const WorldFactRecordingBinding &binding, const WorldFactSource &source,
                           const OperationContext &context,
                           const WorldFactRecordingLeaseAcquirer &acquire);

} // namespace mira
