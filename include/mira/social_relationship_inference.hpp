#pragma once

#include <mira/social_mention_inference.hpp>
#include <mira/sqlite_social_directory.hpp>

#include <string_view>

namespace mira {

class IModelProvider;

struct SocialTextRelationship final {
    SocialMessageRelationshipCandidate candidate;
    std::optional<std::size_t> prior_row;
};

[[nodiscard]] Result<std::vector<SocialMessageRelationshipCandidate>>
extract_social_message_relationships(IModelProvider &, std::string_view message,
                                     std::span<const std::string> predicates,
                                     const OperationContext &);

// Last row is current. Prior row numbers and speaker slots are request-local.
[[nodiscard]] Result<std::vector<SocialTextRelationship>>
extract_social_message_relationships(IModelProvider &, std::span<const SocialMentionUtterance>,
                                     std::span<const std::string> predicates,
                                     const OperationContext &);

// Run in a managed host task after indexing the authorized message.
[[nodiscard]] Result<SocialMessageRelationshipReport> infer_and_record_social_message_relationships(
    SqliteSocialDirectory &, const IEventStore &, IModelProvider &, const SocialEnvironmentKey &,
    const SocialMessageLocator &, std::span<const std::string> predicates,
    const OperationContext &);

} // namespace mira
