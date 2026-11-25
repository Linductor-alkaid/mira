#pragma once

#include <mira/memory_contracts.hpp>
#include <mira/social_identity.hpp>

namespace mira {

// These helpers consume an identity and source event already authorized by
// the host. They do not resolve aliases, grant access, or verify event origin.
[[nodiscard]] Result<MemoryScope> social_object_scope(const SocialObjectKey &key);
[[nodiscard]] Result<MemoryScope> social_environment_scope(const SocialEnvironmentKey &key);
[[nodiscard]] std::optional<MemoryScope> social_environment_scope_of(const MemoryScope &scope);
[[nodiscard]] Result<MemoryScope> social_relationship_scope(const SocialRelationshipKey &key);
[[nodiscard]] std::optional<SocialRelationshipKey>
social_relationship_endpoints(const MemoryScope &scope);

[[nodiscard]] Result<MemoryRecord>
make_social_attribute(const SocialObjectKey &key, SocialAttribute attribute, std::string value,
                      EventId source_event, ValidityInterval validity,
                      std::chrono::system_clock::time_point recorded_at);

[[nodiscard]] Result<MemoryRecord>
make_social_relationship(const SocialRelationshipKey &key, SocialRelationship relationship,
                         std::string statement, EventId source_event, ValidityInterval validity,
                         std::chrono::system_clock::time_point recorded_at);

[[nodiscard]] Result<MemoryRecord>
make_social_relationship(const SocialRelationshipKey &key, SocialRelationship relationship,
                         std::string statement, EventId source_event, ValidityInterval validity,
                         std::chrono::system_clock::time_point recorded_at,
                         std::optional<EventId> referent_event);

[[nodiscard]] bool is_social_attribute_field(std::string_view field) noexcept;
[[nodiscard]] bool is_social_object_scope(const MemoryScope &scope);
[[nodiscard]] std::string_view social_attribute_source_name(SocialAttributeSource source) noexcept;
[[nodiscard]] std::optional<SocialAttributeSource>
social_attribute_source_from(std::string_view name) noexcept;

} // namespace mira
