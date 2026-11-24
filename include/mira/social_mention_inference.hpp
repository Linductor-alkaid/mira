#pragma once

#include <mira/sqlite_social_directory.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

class IModelProvider;
struct OperationContext;

inline constexpr std::string_view kSocialMessageTextEvent = "mira.social.message.received.v1";
inline constexpr std::size_t kSocialMentionHistoryLimit = 6;

struct SocialMentionUtterance final {
    std::size_t speaker_slot = 0;
    std::string text;
};

struct SocialTextMention final {
    std::string surface;
    std::optional<std::size_t> prior_row;
};

// Last utterance is current. Speaker slots are local to the request.
[[nodiscard]] Result<std::vector<SocialTextMention>>
extract_social_mentions(IModelProvider &, std::span<const SocialMentionUtterance>,
                        const OperationContext &);

struct SocialInferredMention final {
    std::string surface;
    std::optional<SocialMessageLocator> inferred_prior_message;
    SocialMentionResult candidate;
};

struct SocialInferredMentions final {
    SocialMessageEvidence speaker;
    std::vector<SocialInferredMention> mentions;
};

// Host entry point in Mira::state_store. Invoke in the existing managed task
// for a session and provider authorized to process this conversation.
[[nodiscard]] Result<SocialInferredMentions>
infer_social_message_mentions(const SqliteSocialDirectory &, const IEventStore &, IModelProvider &,
                              const SocialEnvironmentKey &, const SocialMessageLocator &,
                              const OperationContext &);

} // namespace mira
