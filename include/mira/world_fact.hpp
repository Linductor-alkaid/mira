#pragma once

#include <mira/memory_contracts.hpp>

namespace mira {

// The host supplies authorized stable IDs and a committed evidence event.
[[nodiscard]] Result<MemoryScope> world_environment_scope(TenantId tenant, Id128 environment);
[[nodiscard]] Result<MemoryScope> world_application_scope(TenantId tenant, Id128 environment,
                                                          Id128 application);
[[nodiscard]] Result<MemoryScope> world_application_model_scope(TenantId tenant, Id128 environment,
                                                                Id128 application,
                                                                const Sha256Digest &app_model);
[[nodiscard]] Result<MemoryRecord>
make_world_observation(const MemoryScope &scope, WorldProperty property, std::string value,
                       EventId source_event, ValidityInterval validity,
                       std::chrono::system_clock::time_point recorded_at);

struct WorldFactQuery final {
    MemoryScope scope;
    WorldProperty property;
    std::optional<std::chrono::system_clock::time_point> as_of_valid;
    std::optional<std::chrono::system_clock::time_point> as_of_recorded;

    [[nodiscard]] Result<void> validate() const;
};

enum class WorldFactState : std::uint8_t { Missing, Current, Expired, Ambiguous };

struct WorldFactLookup final {
    WorldFactState state = WorldFactState::Missing;
    std::optional<MemoryRecord> record; // Present only for Current.

    [[nodiscard]] bool requires_observation() const noexcept {
        return state != WorldFactState::Current;
    }
};

} // namespace mira
