#include <mira/social_mention_inference.hpp>

#include <mira/environment.hpp>

#include <algorithm>

namespace mira {
namespace {
Error source_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.social.mention_inference";
    error.safe_message = std::move(message);
    return error;
}

Result<void> check_operation(const OperationContext &context) {
    if (context.cancelled())
        return source_error(ErrorCode::Cancelled, "social mention inference was cancelled");
    if (context.expired(Timestamp::now()))
        return source_error(ErrorCode::DeadlineExceeded, "social mention inference expired");
    return {};
}

Result<std::string> read_message_text(const IEventStore &events,
                                      const SocialMessageEvidence &source) {
    EventSliceQuery query;
    query.session_id = source.message.session;
    query.after_sequence = source.message.sequence - 1;
    query.through_sequence = source.message.sequence;
    query.limit = 1;
    query.scan_limit = 1;
    query.byte_limit = 4096 + kSocialMessageTextEvent.size();
    const auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (page.value().events.size() != 1 || page.value().has_more ||
        page.value().snapshot_through != source.message.sequence ||
        page.value().scanned_through != source.message.sequence)
        return source_error(ErrorCode::DataLoss, "social message evidence has no matching source");
    const auto &event = page.value().events.front();
    if (event.session_id != source.message.session ||
        event.session_sequence != source.message.sequence ||
        event.event_id != source.message.event || event.integrity != source.event_integrity)
        return source_error(ErrorCode::DataLoss, "social message source changed");
    if (event.payload.type != kSocialMessageTextEvent)
        return source_error(ErrorCode::UnsupportedCapability,
                            "social inference needs a message text event");
    return event.payload.data;
}

Result<void> check_sources(const SocialMessageParticipants &participants,
                           const SocialMessageEvidence &current,
                           std::span<const SocialMessageEvidence> history) {
    if (participants.speaker != current || participants.mentions.size() < history.size())
        return source_error(ErrorCode::InvalidState, "social inference source was invalidated");
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto *message = std::get_if<SocialMessageEvidence>(&participants.mentions[index]);
        if (message == nullptr || *message != history[index])
            return source_error(ErrorCode::InvalidState,
                                "social inference history was invalidated");
    }
    return {};
}
} // namespace

Result<SocialInferredMentions>
infer_social_message_mentions(const SqliteSocialDirectory &directory, const IEventStore &events,
                              IModelProvider &provider, const SocialEnvironmentKey &environment,
                              const SocialMessageLocator &message,
                              const OperationContext &operation) {
    if (operation.session != message.session || operation.session.is_nil())
        return source_error(ErrorCode::PermissionDenied,
                            "social inference needs its authorized session");
    auto context = operation;
    const auto limit = Timestamp::now().monotonic + std::chrono::seconds{15};
    context.deadline = std::min(context.deadline.value_or(limit), limit);
    if (auto active = check_operation(context); !active)
        return active.error();
    const auto participants = directory.resolve_message_participants(environment, message, {});
    if (!participants)
        return participants.error();
    const auto &current = participants.value().speaker;
    std::vector<SocialMessageEvidence> history;
    if (message.sequence > 1) {
        const auto through = message.sequence - 1;
        const auto after =
            through > kSocialMentionHistoryLimit ? through - kSocialMentionHistoryLimit : 0;
        auto page = directory.read_message_evidence(environment, message.session, after, through,
                                                    kSocialMentionHistoryLimit);
        if (!page)
            return page.error();
        history = std::move(page).value().evidence;
    }

    std::vector<SocialMentionUtterance> transcript;
    std::vector<SocialMentionReference> references;
    std::vector<SocialObjectKey> speakers;
    const auto add_row = [&](const SocialMessageEvidence &source) -> Result<void> {
        if (auto active = check_operation(context); !active)
            return active.error();
        auto text = read_message_text(events, source);
        if (!text)
            return text.error();
        const auto existing = std::ranges::find(speakers, source.speaker);
        const auto slot = static_cast<std::size_t>(existing - speakers.begin());
        if (existing == speakers.end())
            speakers.push_back(source.speaker);
        transcript.push_back({slot, std::move(text).value()});
        return {};
    };
    for (const auto &previous : history) {
        if (auto added = add_row(previous); !added)
            return added.error();
        references.emplace_back(previous.message);
    }
    if (auto added = add_row(current); !added)
        return added.error();
    const auto admitted = directory.resolve_message_participants(environment, message, references);
    if (!admitted)
        return admitted.error();
    if (auto current_sources = check_sources(admitted.value(), current, history); !current_sources)
        return current_sources.error();

    auto proposed = extract_social_mentions(provider, transcript, context);
    if (!proposed)
        return proposed.error();
    for (const auto &mention : proposed.value()) {
        if (mention.prior_row)
            references.emplace_back(history.at(*mention.prior_row).message);
        else
            references.emplace_back(mention.surface);
    }
    auto resolved = directory.resolve_message_participants(environment, message, references);
    if (!resolved)
        return resolved.error();
    if (auto current_sources = check_sources(resolved.value(), current, history); !current_sources)
        return current_sources.error();
    if (auto active = check_operation(context); !active)
        return active.error();
    SocialInferredMentions result{current, {}};
    for (std::size_t index = 0; index < proposed.value().size(); ++index) {
        auto &mention = proposed.value()[index];
        std::optional<SocialMessageLocator> prior;
        if (mention.prior_row)
            prior = history.at(*mention.prior_row).message;
        result.mentions.push_back(
            {std::move(mention.surface), prior,
             std::move(resolved.value().mentions.at(history.size() + index))});
    }
    return result;
}

} // namespace mira
