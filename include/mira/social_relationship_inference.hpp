#pragma once

#include <mira/sqlite_social_directory.hpp>

#include <string_view>

namespace mira {

class IModelProvider;

[[nodiscard]] Result<std::vector<SocialMessageRelationshipCandidate>>
extract_social_message_relationships(IModelProvider &, std::string_view message,
                                     std::span<const std::string> predicates,
                                     const OperationContext &);

// Run in a managed host task after indexing the authorized message.
[[nodiscard]] Result<SocialMessageRelationshipReport> infer_and_record_social_message_relationships(
    SqliteSocialDirectory &, const IEventStore &, IModelProvider &, const SocialEnvironmentKey &,
    const SocialMessageLocator &, std::span<const std::string> predicates,
    const OperationContext &);

} // namespace mira
