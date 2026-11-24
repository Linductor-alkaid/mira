#include <mira/skill_environment.hpp>

namespace mira {

Result<MemoryScope> skill_environment_scope(const SkillEnvironmentKey &key) {
    if (key.tenant.is_nil() || key.environment.is_nil() ||
        (key.kind != SkillEnvironmentKind::Work && key.kind != SkillEnvironmentKind::Social))
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid skill environment key");
    const auto prefix = key.kind == SkillEnvironmentKind::Work ? "work/" : "social/";
    return MemoryScope{MemoryScopeKind::SkillEnvironment, prefix + key.environment.to_string(),
                       key.tenant.to_string()};
}

std::optional<SkillEnvironmentKey> skill_environment_key_of(const MemoryScope &scope) {
    if (scope.kind != MemoryScopeKind::SkillEnvironment || !scope.tenant_id)
        return std::nullopt;
    const auto tenant = TenantId::parse(*scope.tenant_id);
    const std::string_view subject = scope.subject_id;
    const auto slash = subject.find('/');
    if (!tenant || slash == std::string_view::npos)
        return std::nullopt;
    const auto domain = subject.substr(0, slash);
    if (domain != "work" && domain != "social")
        return std::nullopt;
    const auto environment = Id128::parse(subject.substr(slash + 1));
    if (!environment)
        return std::nullopt;
    const SkillEnvironmentKey key{
        *tenant, domain == "work" ? SkillEnvironmentKind::Work : SkillEnvironmentKind::Social,
        *environment};
    const auto canonical = skill_environment_scope(key);
    return canonical && canonical.value() == scope ? std::optional{key} : std::nullopt;
}

} // namespace mira
