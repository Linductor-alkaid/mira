#pragma once

#include <mira/sqlite_social_directory.hpp>

#include <string_view>

namespace mira {

class IModelProvider;

// Extracts from one authenticated speaker's message, using the host's field vocabulary.
[[nodiscard]] Result<std::vector<SocialMessageAttributeCandidate>>
extract_social_message_attributes(IModelProvider &, std::string_view message,
                                  std::span<const std::string> fields, const OperationContext &);

// Run in a managed host task after indexing the authorized message.
[[nodiscard]] Result<SocialMessageAttributeReport> infer_and_record_social_message_attributes(
    SqliteSocialDirectory &, const IEventStore &, IModelProvider &, const SocialEnvironmentKey &,
    const SocialMessageLocator &, std::span<const std::string> fields, const OperationContext &);

} // namespace mira
