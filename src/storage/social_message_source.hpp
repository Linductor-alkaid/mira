#pragma once

#include <mira/sqlite_social_directory.hpp>

namespace mira::storage {

[[nodiscard]] Result<EventEnvelope> read_social_message_source(const IEventStore &,
                                                               const SocialMessageEvidence &);
[[nodiscard]] Result<std::vector<MemoryCandidate>>
prepare_social_message_attributes(const SocialMessageEvidence &, const EventEnvelope &,
                                  std::span<const SocialMessageAttributeCandidate>);

} // namespace mira::storage
