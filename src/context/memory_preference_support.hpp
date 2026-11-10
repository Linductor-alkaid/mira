#pragma once

#include "memory_agent_support.hpp"

#include <algorithm>
#include <charconv>

namespace mira::memory_preference {
using memory_agent_support::error;
inline const std::string *field(const JsonValue &json, const char *name) {
    const auto *value = json.find(name);
    return value ? value->as_string() : nullptr;
}
inline std::optional<std::uint64_t> number(const JsonValue &json, const char *name) {
    const auto *text = field(json, name);
    if (!text)
        return {};
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size())
        return {};
    return value;
}
template <typename Id> Id identity(std::string_view domain, MutationId request) {
    const auto digest = digest_string(std::string(domain) + request.to_string());
    std::array<std::uint8_t, 16> bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id{Id128{bytes}};
}
inline bool manage(const PrincipalContext &principal, const MemoryScope &scope) {
    if (principal.user_id.is_nil() || principal.tenant_id.is_nil() ||
        principal.identity_version == 0 ||
        principal.auth_strength == AuthenticationStrength::Anonymous ||
        scope != memory_user_scope(principal))
        return false;
    const auto now = std::chrono::system_clock::now();
    return std::any_of(principal.grants.begin(), principal.grants.end(), [&](const auto &grant) {
        const auto &resource = grant.resources;
        const bool all =
            resource.type.empty() && resource.scope.empty() && resource.pattern.empty();
        const bool exact = resource.type == "memory" && resource.scope == scope.subject_id &&
                           resource.pattern.empty();
        return grant.capability == "mira.memory.manage" &&
               grant.version == principal.identity_version && (all || exact) &&
               (grant.expires_at == std::chrono::system_clock::time_point{} ||
                grant.expires_at > now);
    });
}
inline bool owner(const PrincipalContext &left, const PrincipalContext &right) {
    return left.tenant_id == right.tenant_id && left.user_id == right.user_id &&
           left.identity_version == right.identity_version;
}
inline bool closed(PreferenceProposalState state) {
    return state == PreferenceProposalState::Rejected || state == PreferenceProposalState::Stale ||
           state == PreferenceProposalState::Erased;
}
inline JsonValue result_json(const MemoryMutationResult &result) {
    return JsonValue::Object{{"type", memory_mutation_type_name(result.applied)},
                             {"record", result.record.to_string()},
                             {"version", std::to_string(result.new_version)},
                             {"replayed", result.idempotent_replay}};
}
} // namespace mira::memory_preference
