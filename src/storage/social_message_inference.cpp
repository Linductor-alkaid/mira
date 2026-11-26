#include <mira/social_attribute_inference.hpp>
#include <mira/social_mention_inference.hpp>
#include <mira/social_relationship_inference.hpp>

#include "social_message_source.hpp"
#include "sqlite_social_aliases.hpp"

#include <mira/environment.hpp>
#include <mira/social_profile.hpp>

#include <algorithm>
#include <cstddef>

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

SocialClaimRow consolidation_row(std::size_t row, const ConsolidationEntry &entry) {
    const auto disposition = entry.disposition == CandidateDisposition::Applied
                                 ? SocialClaimDisposition::Applied
                                 : SocialClaimDisposition::Dropped;
    return {row, disposition, entry.reason_code};
}

template <typename Candidate>
void restore_model_rows(SocialClaimReport &report, const SocialClaimExtraction<Candidate> &source) {
    for (auto &row : report.rows)
        row.row = source.proposals[row.row].row;
    for (const auto &row : source.rejected)
        report.rows.push_back({row.row, SocialClaimDisposition::Dropped, row.reason});
    report.truncated_rows = source.truncated_rows;
    std::ranges::sort(report.rows, {}, &SocialClaimRow::row);
}

struct SocialTranscript final {
    SocialMessageEvidence speaker;
    std::vector<SocialMessageEvidence> history;
    std::vector<SocialMentionUtterance> rows;
};

Result<SocialTranscript>
load_social_transcript(const SqliteSocialDirectory &directory, const IEventStore &events,
                       const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
                       const OperationContext &context, bool skip_non_text_history,
                       bool include_history = true) {
    const auto participants = directory.resolve_message_participants(environment, message, {});
    if (!participants)
        return participants.error();
    const auto &current = participants.value().speaker;
    std::vector<SocialMessageEvidence> earlier;
    if (include_history && message.sequence > 1) {
        const auto through = message.sequence - 1;
        const auto after =
            through > kSocialMentionHistoryLimit ? through - kSocialMentionHistoryLimit : 0;
        auto page = directory.read_message_evidence(environment, message.session, after, through,
                                                    kSocialMentionHistoryLimit);
        if (!page)
            return page.error();
        earlier = std::move(page).value().evidence;
    }

    SocialTranscript transcript{current, {}, {}};
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
        transcript.rows.push_back({slot, std::move(evidence).value().payload.data});
        return {};
    };
    for (const auto &previous : earlier) {
        if (auto added = add_row(previous); !added) {
            if (skip_non_text_history && added.error().code == ErrorCode::UnsupportedCapability)
                continue;
            return added.error();
        }
        transcript.history.push_back(previous);
        references.emplace_back(previous.message);
    }
    if (auto added = add_row(current); !added)
        return added.error();
    const auto admitted = directory.resolve_message_participants(environment, message, references);
    if (!admitted)
        return admitted.error();
    if (auto current_sources = check_sources(admitted.value(), current, transcript.history);
        !current_sources)
        return current_sources.error();
    return transcript;
}
} // namespace

Result<SocialInferredMentions>
infer_social_message_mentions(const SqliteSocialDirectory &directory, const IEventStore &events,
                              IModelProvider &provider, const SocialEnvironmentKey &environment,
                              const SocialMessageLocator &message,
                              const OperationContext &operation) {
    return infer_social_message_mentions(directory, events, &provider, environment, message,
                                         operation);
}

Result<SocialInferredMentions>
infer_social_message_mentions(const SqliteSocialDirectory &directory, const IEventStore &events,
                              IModelProvider *provider, const SocialEnvironmentKey &environment,
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
    const auto transcript = load_social_transcript(directory, events, environment, message, context,
                                                   false, provider != nullptr);
    if (!transcript)
        return transcript.error();
    const auto &current = transcript.value().speaker;
    const auto &history = transcript.value().history;
    const auto &text = transcript.value().rows.back().text;
    const auto found = directory.scan_message_aliases(environment, text, context);
    if (!found)
        return found.error();
    struct Mention final {
        std::string surface;
        SocialMentionReference reference;
        std::optional<SocialMessageLocator> prior{};
        std::size_t offset = 0;
        std::optional<std::size_t> model_row{};
    };
    constexpr std::size_t kMaxMentions = 8;
    std::vector<Mention> mentions;
    for (const auto &alias : found.value().matches)
        mentions.push_back({alias.surface, alias.key, {}, alias.offset});
    SocialInferredMentions result{current, {}};
    result.truncated = found.value().truncated;

    if (provider) {
        auto proposed =
            extract_social_mentions_with_report(*provider, transcript.value().rows, context);
        if (!proposed) {
            const auto code = proposed.error().code;
            if (mentions.empty() ||
                (code != ErrorCode::InvalidModelOutput &&
                 code != ErrorCode::UnsupportedCapability && code != ErrorCode::Unavailable))
                return proposed.error();
            result.model_error = proposed.error();
        } else {
            result.rejected = std::move(proposed.value().rejected);
            result.claims.truncated_rows = proposed.value().truncated_rows;
            result.truncated = result.truncated || result.claims.truncated_rows != 0;
            for (const auto &row : result.rejected)
                result.claims.rows.push_back(
                    {row.row, SocialClaimDisposition::Dropped, row.reason});
            for (const auto &mention : proposed.value().mentions) {
                const auto key = storage::social_aliases::normalize_alias(mention.surface);
                if (std::ranges::any_of(found.value().matches,
                                        [&](const auto &alias) { return alias.key == key; })) {
                    result.claims.rows.push_back(
                        {mention.model_row, SocialClaimDisposition::Dropped, "literal-alias"});
                    continue;
                }
                if (mentions.size() == kMaxMentions) {
                    result.truncated = true;
                    result.claims.rows.push_back(
                        {mention.model_row, SocialClaimDisposition::Dropped, "mention-capacity"});
                    continue;
                }
                const auto offset = text.find(mention.surface);
                if (mention.prior_row) {
                    const auto &prior = history.at(*mention.prior_row).message;
                    mentions.push_back({mention.surface, prior, prior, offset, mention.model_row});
                } else {
                    mentions.push_back({mention.surface, key, {}, offset, mention.model_row});
                }
            }
        }
    }
    std::ranges::stable_sort(mentions, {}, &Mention::offset);
    std::vector<SocialMentionReference> references;
    references.reserve(history.size() + mentions.size());
    for (const auto &previous : history)
        references.emplace_back(previous.message);

    for (const auto &mention : mentions)
        references.push_back(mention.reference);
    auto resolved = directory.resolve_message_participants(environment, message, references);
    if (!resolved)
        return resolved.error();
    if (auto current_sources = check_sources(resolved.value(), current, history); !current_sources)
        return current_sources.error();
    if (auto active = check_operation(context); !active)
        return active.error();
    for (std::size_t index = 0; index < mentions.size(); ++index) {
        auto &mention = mentions[index];
        auto &candidate = resolved.value().mentions.at(history.size() + index);
        if (const auto *alias = std::get_if<SocialAliasResult>(&candidate)) {
            const bool resolved_name = alias->resolution != SocialAliasResolution::Missing;
            const bool literal = !mention.model_row;
            if (mention.model_row)
                result.claims.rows.push_back({*mention.model_row,
                                              SocialClaimDisposition::Unresolved,
                                              resolved_name ? "alias-changed" : "missing-target"});
            if (resolved_name != literal)
                continue;
        } else if (mention.model_row) {
            result.claims.rows.push_back(
                {*mention.model_row, SocialClaimDisposition::Applied, "prior-speaker"});
        }
        result.mentions.push_back(
            {std::move(mention.surface), mention.prior, std::move(candidate)});
    }
    std::ranges::sort(result.claims.rows, {}, &SocialClaimRow::row);
    return result;
}

Result<SocialMessageAttributeReport> infer_and_record_social_message_attributes(
    SqliteSocialDirectory &directory, const IEventStore &events, IModelProvider &provider,
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    std::span<const std::string> fields, const OperationContext &operation) {
    std::vector<SocialAttributeField> described;
    described.reserve(fields.size());
    for (const auto &field : fields)
        described.push_back({field});
    return infer_and_record_social_message_attributes(directory, events, provider, environment,
                                                      message, described, operation);
}

Result<SocialMessageAttributeReport> infer_and_record_social_message_attributes(
    SqliteSocialDirectory &directory, const IEventStore &events, IModelProvider &provider,
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    std::span<const SocialAttributeField> fields, const OperationContext &operation) {
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
    auto proposed = extract_social_message_attributes_with_report(
        provider, event.value().payload.data, fields, context);
    if (!proposed)
        return proposed.error();
    std::vector<SocialMessageAttributeCandidate> accepted;
    accepted.reserve(proposed.value().proposals.size());
    for (auto &row : proposed.value().proposals)
        accepted.push_back(std::move(row.candidate));
    auto recorded = directory.record_inferred_message_attributes(environment, message, events,
                                                                 accepted, context);
    if (!recorded)
        return recorded.error();
    auto &report = recorded.value();
    if (report.consolidation.entries.size() != accepted.size())
        return source_error(ErrorCode::DataLoss, "social attribute write report is incomplete");
    for (std::size_t i = 0; i < accepted.size(); ++i)
        report.claims.rows.push_back(consolidation_row(i, report.consolidation.entries[i]));
    restore_model_rows(report.claims, proposed.value());
    return recorded;
}

Result<SocialMessageRelationshipReport>
SqliteSocialDirectory::record_inferred_message_relationships(
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    const IEventStore &events, std::span<const SocialMessageRelationshipCandidate> relationships,
    const OperationContext &context) {
    return record_message_relationship_candidates(environment, message, events, relationships,
                                                  context, nullptr, {});
}

Result<SocialMessageRelationshipReport>
SqliteSocialDirectory::record_message_relationship_candidates(
    const SocialEnvironmentKey &environment, const SocialMessageLocator &message,
    const IEventStore &events, std::span<const SocialMessageRelationshipCandidate> relationships,
    const OperationContext &context, const SocialMessageEvidence *expected_speaker,
    std::span<const SocialMessageEvidence> history) {
    if (context.session.is_nil() || context.session != message.session)
        return source_error(ErrorCode::PermissionDenied,
                            "social relationship recording needs its authorized session");
    if (relationships.size() > 8)
        return source_error(ErrorCode::ResourceExhausted,
                            "too many social relationships for one message");
    if (auto active = check_operation(context); !active)
        return active.error();
    std::vector<SocialMentionReference> references;
    references.reserve(history.size() + relationships.size());
    for (const auto &previous : history)
        references.emplace_back(previous.message);
    for (const auto &relationship : relationships) {
        if (!is_social_attribute_field(relationship.predicate) || relationship.alias.empty() ||
            relationship.quote.empty() || relationship.quote.size() > 256 ||
            relationship.quote.find(relationship.alias) == std::string::npos)
            return source_error(ErrorCode::InvalidArgument,
                                "social relationship requires a predicate and quoted alias");
        if (relationship.prior_message)
            references.emplace_back(*relationship.prior_message);
        else
            references.emplace_back(relationship.alias);
    }
    auto indexed = resolve_message_participants(environment, message, references);
    if (!indexed)
        return indexed.error();
    SocialMessageRelationshipReport report{std::move(indexed).value(), {}, {}};
    if (report.participants.mentions.size() != references.size())
        return source_error(ErrorCode::DataLoss,
                            "social relationship references do not match candidates");
    if (expected_speaker) {
        if (auto current = check_sources(report.participants, *expected_speaker, history); !current)
            return current.error();
    }
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
    std::vector<std::size_t> prepared_rows;
    prepared.reserve(relationships.size());
    for (std::size_t index = 0; index < relationships.size(); ++index) {
        if (auto active = check_operation(context); !active)
            return active.error();
        const auto &relationship = relationships[index];
        if (event.value().payload.data.find(relationship.quote) == std::string::npos)
            return source_error(ErrorCode::InvalidArgument,
                                "social relationship excerpt is absent from the message");
        const auto &reference = report.participants.mentions[history.size() + index];
        std::optional<SocialObjectKey> target;
        std::optional<EventId> referent_event;
        if (const auto *prior = std::get_if<SocialMessageEvidence>(&reference)) {
            const auto proof = storage::read_social_message_source(events, *prior);
            if (!proof)
                return proof.error();
            target = prior->speaker;
            referent_event = prior->message.event;
        } else if (const auto *alias = std::get_if<SocialAliasResult>(&reference);
                   alias && alias->resolution == SocialAliasResolution::Unique &&
                   !alias->truncated && alias->matches.size() == 1) {
            target = alias->matches.front().key;
        }
        if (!target || *target == speaker.speaker) {
            report.unresolved.push_back(index);
            const auto *alias = std::get_if<SocialAliasResult>(&reference);
            const auto reason = target ? "self-reference"
                                : alias && alias->resolution == SocialAliasResolution::Ambiguous
                                    ? "ambiguous-target"
                                    : "missing-target";
            report.claims.rows.push_back({index, SocialClaimDisposition::Unresolved, reason});
            continue;
        }
        const SocialRelationshipKey endpoints =
            relationship.speaker_is_subject ? SocialRelationshipKey{speaker.speaker, *target}
                                            : SocialRelationshipKey{*target, speaker.speaker};
        auto record = make_social_relationship(
            endpoints, {relationship.predicate, SocialAttributeSource::Inferred},
            relationship.quote, message.event, {recorded, {}}, recorded, referent_event);
        if (!record)
            return record.error();
        if (std::ranges::any_of(prepared, [&](const MemoryCandidate &candidate) {
                return candidate.proposed.id == record.value().id;
            })) {
            if (!expected_speaker)
                return source_error(ErrorCode::InvalidArgument,
                                    "social message contains a duplicate relationship candidate");
            report.claims.rows.push_back(
                {index, SocialClaimDisposition::Dropped, "duplicate-target"});
            continue;
        }
        MemoryCandidate candidate;
        candidate.proposed = std::move(record).value();
        candidate.evidence = candidate.proposed.provenance;
        candidate.reason = MutationReasonCode::Consolidation;
        prepared.push_back(std::move(candidate));
        prepared_rows.push_back(index);
    }
    auto consolidated = commit_inferred_message_candidates(
        environment, report.participants, references, std::move(prepared), context);
    if (!consolidated)
        return consolidated.error();
    auto &mentions = report.participants.mentions;
    mentions.erase(mentions.begin(),
                   mentions.begin() + static_cast<std::ptrdiff_t>(history.size()));
    report.consolidation = std::move(consolidated).value();
    if (report.consolidation.entries.size() != prepared_rows.size())
        return source_error(ErrorCode::DataLoss, "social relationship write report is incomplete");
    for (std::size_t i = 0; i < prepared_rows.size(); ++i)
        report.claims.rows.push_back(
            consolidation_row(prepared_rows[i], report.consolidation.entries[i]));
    std::ranges::sort(report.claims.rows, {}, &SocialClaimRow::row);
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
    const auto transcript =
        load_social_transcript(directory, events, environment, message, context, true);
    if (!transcript)
        return transcript.error();
    const auto &source = transcript.value();
    auto proposed = extract_social_message_relationships_with_report(provider, source.rows,
                                                                     predicates, context);
    if (!proposed)
        return proposed.error();
    std::vector<SocialMessageRelationshipCandidate> candidates;
    candidates.reserve(proposed.value().proposals.size());
    for (auto &row : proposed.value().proposals) {
        auto &relationship = row.candidate;
        if (relationship.prior_row)
            relationship.candidate.prior_message = source.history[*relationship.prior_row].message;
        candidates.push_back(std::move(relationship.candidate));
    }
    auto recorded = directory.record_message_relationship_candidates(
        environment, message, events, candidates, context, &source.speaker, source.history);
    if (!recorded)
        return recorded.error();
    for (auto &row : recorded.value().unresolved)
        row = proposed.value().proposals[row].row;
    restore_model_rows(recorded.value().claims, proposed.value());
    return recorded;
}

} // namespace mira
