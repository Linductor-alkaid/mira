#include <mira/social_attribute_inference.hpp>
#include <mira/social_mention_inference.hpp>
#include <mira/social_relationship_inference.hpp>

#include "social_message_source.hpp"

#include <mira/environment.hpp>
#include <mira/social_profile.hpp>

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

} // namespace

Result<EventEnvelope> storage::read_social_message_source(const IEventStore &events,
                                                          const SocialMessageEvidence &source) {
    EventSliceQuery query;
    query.session_id = source.message.session;
    query.after_sequence = source.message.sequence - 1;
    query.through_sequence = source.message.sequence;
    query.limit = 1;
    query.scan_limit = 1;
    query.byte_limit = 4096 + kSocialMessageTextEvent.size();
    auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (page.value().events.size() != 1 || page.value().has_more ||
        page.value().snapshot_through != source.message.sequence ||
        page.value().scanned_through != source.message.sequence)
        return source_error(ErrorCode::DataLoss, "social message evidence has no matching source");
    auto &event = page.value().events.front();
    if (event.session_id != source.message.session ||
        event.session_sequence != source.message.sequence ||
        event.event_id != source.message.event || event.integrity != source.event_integrity)
        return source_error(ErrorCode::DataLoss, "social message source changed");
    if (event.payload.type != kSocialMessageTextEvent)
        return source_error(ErrorCode::UnsupportedCapability,
                            "social inference needs a message text event");
    return std::move(event);
}

Result<std::vector<MemoryCandidate>> storage::prepare_social_message_attributes(
    const SocialMessageEvidence &source, const EventEnvelope &event,
    std::span<const SocialMessageAttributeCandidate> attributes) {
    constexpr std::size_t kMaxAttributes = 8;
    constexpr std::size_t kMaxQuoteBytes = 256;
    if (attributes.size() > kMaxAttributes)
        return source_error(ErrorCode::ResourceExhausted,
                            "too many social attributes for one message");
    if (event.timestamp_origin == EventTimestampOrigin::UnavailableLegacy ||
        event.timestamp.wall.time_since_epoch().count() <= 0)
        return source_error(ErrorCode::DataLoss, "social message source time is unavailable");
    const auto recorded =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(event.timestamp.wall);
    std::vector<MemoryCandidate> prepared;
    prepared.reserve(attributes.size());
    for (const auto &attribute : attributes) {
        if (!is_social_attribute_field(attribute.field) || attribute.quote.empty() ||
            attribute.quote.size() > kMaxQuoteBytes ||
            event.payload.data.find(attribute.quote) == std::string::npos ||
            std::ranges::any_of(prepared, [&](const MemoryCandidate &candidate) {
                return candidate.proposed.social_attribute->field == attribute.field;
            }))
            return source_error(ErrorCode::InvalidArgument,
                                "social attribute requires a unique field and a source excerpt");
        auto record = make_social_attribute(
            source.speaker, {attribute.field, SocialAttributeSource::Inferred}, attribute.quote,
            source.message.event, {recorded, {}}, recorded);
        if (!record)
            return record.error();
        MemoryCandidate candidate;
        candidate.proposed = std::move(record).value();
        candidate.evidence = {source.message.event};
        candidate.reason = MutationReasonCode::Consolidation;
        prepared.push_back(std::move(candidate));
    }
    return prepared;
}

namespace {
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
        auto evidence = storage::read_social_message_source(events, source);
        if (!evidence)
            return evidence.error();
        const auto existing = std::ranges::find(speakers, source.speaker);
        const auto slot = static_cast<std::size_t>(existing - speakers.begin());
        if (existing == speakers.end())
            speakers.push_back(source.speaker);
        transcript.push_back({slot, std::move(evidence).value().payload.data});
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

Result<SocialMessageAttributeReport> infer_and_record_social_message_attributes(
    SqliteSocialDirectory &directory, const IEventStore &events, IModelProvider &provider,
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    std::span<const std::string> fields, const OperationContext &operation) {
    if (operation.session != message.session || operation.session.is_nil())
        return source_error(ErrorCode::PermissionDenied,
                            "social attribute inference needs its authorized session");
    auto context = operation;
    const auto limit = Timestamp::now().monotonic + std::chrono::seconds{15};
    context.deadline = std::min(context.deadline.value_or(limit), limit);
    if (auto active = check_operation(context); !active)
        return active.error();
    const auto indexed = directory.resolve_message_participants(environment, message, {});
    if (!indexed)
        return indexed.error();
    const auto event = storage::read_social_message_source(events, indexed.value().speaker);
    if (!event)
        return event.error();
    auto proposed =
        extract_social_message_attributes(provider, event.value().payload.data, fields, context);
    if (!proposed)
        return proposed.error();
    return directory.record_inferred_message_attributes(environment, message, events,
                                                        proposed.value(), context);
}

Result<SocialMessageRelationshipReport>
SqliteSocialDirectory::record_inferred_message_relationships(
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    const IEventStore &events, std::span<const SocialMessageRelationshipCandidate> relationships,
    const OperationContext &context) {
    if (context.session.is_nil() || context.session != message.session)
        return source_error(ErrorCode::PermissionDenied,
                            "social relationship recording needs its authorized session");
    if (relationships.size() > 8)
        return source_error(ErrorCode::ResourceExhausted,
                            "too many social relationships for one message");
    if (auto active = check_operation(context); !active)
        return active.error();
    std::vector<SocialMentionReference> references;
    for (const auto &relationship : relationships) {
        if (!is_social_attribute_field(relationship.predicate) || relationship.alias.empty() ||
            relationship.quote.empty() || relationship.quote.size() > 256 ||
            relationship.quote.find(relationship.alias) == std::string::npos)
            return source_error(ErrorCode::InvalidArgument,
                                "social relationship requires a predicate and quoted alias");
        references.emplace_back(relationship.alias);
    }
    auto indexed = resolve_message_participants(environment, message, references);
    if (!indexed)
        return indexed.error();
    SocialMessageRelationshipReport report{std::move(indexed).value(), {}, {}};
    if (report.participants.mentions.size() != relationships.size())
        return source_error(ErrorCode::DataLoss,
                            "social relationship aliases do not match candidates");
    const auto &speaker = report.participants.speaker;
    const auto event = storage::read_social_message_source(events, speaker);
    if (!event)
        return event.error();
    if (event.value().timestamp_origin == EventTimestampOrigin::UnavailableLegacy ||
        event.value().timestamp.wall.time_since_epoch().count() <= 0)
        return source_error(ErrorCode::DataLoss, "social message source time is unavailable");
    const auto recorded = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        event.value().timestamp.wall);
    std::vector<MemoryCandidate> prepared;
    for (std::size_t index = 0; index < relationships.size(); ++index) {
        const auto &relationship = relationships[index];
        if (event.value().payload.data.find(relationship.quote) == std::string::npos)
            return source_error(ErrorCode::InvalidArgument,
                                "social relationship excerpt is absent from the message");
        const auto *alias = std::get_if<SocialAliasResult>(&report.participants.mentions[index]);
        if (!alias)
            return source_error(ErrorCode::DataLoss,
                                "social relationship received invalid alias evidence");
        if (alias->resolution != SocialAliasResolution::Unique || alias->truncated ||
            alias->matches.size() != 1 || alias->matches.front().key == speaker.speaker) {
            report.unresolved.push_back(index);
            continue;
        }
        const auto target = alias->matches.front().key;
        const SocialRelationshipKey endpoints =
            relationship.speaker_is_subject ? SocialRelationshipKey{speaker.speaker, target}
                                            : SocialRelationshipKey{target, speaker.speaker};
        auto record = make_social_relationship(
            endpoints, {relationship.predicate, SocialAttributeSource::Inferred},
            relationship.quote, message.event, {recorded, {}}, recorded);
        if (!record)
            return record.error();
        if (std::ranges::any_of(prepared, [&](const MemoryCandidate &candidate) {
                return candidate.proposed.id == record.value().id;
            }))
            return source_error(ErrorCode::InvalidArgument,
                                "social message contains a duplicate relationship candidate");
        MemoryCandidate candidate;
        candidate.proposed = std::move(record).value();
        candidate.evidence = {message.event};
        candidate.reason = MutationReasonCode::Consolidation;
        prepared.push_back(std::move(candidate));
    }
    auto consolidated = commit_inferred_message_candidates(
        environment, report.participants, references, std::move(prepared), context);
    if (!consolidated)
        return consolidated.error();
    report.consolidation = std::move(consolidated).value();
    return report;
}

Result<SocialMessageRelationshipReport> infer_and_record_social_message_relationships(
    SqliteSocialDirectory &directory, const IEventStore &events, IModelProvider &provider,
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    std::span<const std::string> predicates, const OperationContext &operation) {
    if (operation.session != message.session || operation.session.is_nil())
        return source_error(ErrorCode::PermissionDenied,
                            "social relationship inference needs its authorized session");
    auto context = operation;
    const auto limit = Timestamp::now().monotonic + std::chrono::seconds{15};
    context.deadline = std::min(context.deadline.value_or(limit), limit);
    if (auto active = check_operation(context); !active)
        return active.error();
    const auto indexed = directory.resolve_message_participants(environment, message, {});
    if (!indexed)
        return indexed.error();
    const auto event = storage::read_social_message_source(events, indexed.value().speaker);
    if (!event)
        return event.error();
    auto proposed = extract_social_message_relationships(provider, event.value().payload.data,
                                                         predicates, context);
    if (!proposed)
        return proposed.error();
    return directory.record_inferred_message_relationships(environment, message, events,
                                                           proposed.value(), context);
}

} // namespace mira
