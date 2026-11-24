#pragma once

#include <mira/memory_contracts.hpp>

namespace mira {

enum class SkillEnvironmentKind : std::uint8_t { Work, Social };

struct SkillEnvironmentKey final {
    TenantId tenant;
    SkillEnvironmentKind kind = SkillEnvironmentKind::Work;
    Id128 environment;
    friend bool operator==(const SkillEnvironmentKey &, const SkillEnvironmentKey &) = default;
};

[[nodiscard]] Result<MemoryScope> skill_environment_scope(const SkillEnvironmentKey &key);
[[nodiscard]] std::optional<SkillEnvironmentKey> skill_environment_key_of(const MemoryScope &scope);

} // namespace mira
