#include <mira/memory_consolidation.hpp>

#include "context_text.hpp"

#include <mira/json.hpp>
#include <mira/verified_fact.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] std::chrono::system_clock::time_point event_wall(const EventEnvelope &event) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(wall_nanos(event.timestamp.wall))));
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>>
parse_verified_fact(const EventEnvelope &event) {
    if (event.payload.type != "TaskFactVerified") {
        return std::nullopt;
    }
    auto parsed = parse_json(event.payload.data);
    if (!parsed) {
        return std::nullopt;
    }
    const auto *key = parsed.value().find("key");
    const auto *value = parsed.value().find("value");
    if (key == nullptr || !key->is_string() || value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return std::make_pair(*key->as_string(), *value->as_string());
}

} // namespace

std::string candidate_disposition_name(CandidateDisposition disposition) {
    switch (disposition) {
    case CandidateDisposition::Applied:
        return "Applied";
    case CandidateDisposition::PendingApproval:
        return "PendingApproval";
    case CandidateDisposition::RejectedForbidden:
        return "RejectedForbidden";
    case CandidateDisposition::RejectedInjection:
        return "RejectedInjection";
    case CandidateDisposition::RejectedInvalid:
        return "RejectedInvalid";
    case CandidateDisposition::RejectedConflict:
        return "RejectedConflict";
    }
    return "Unknown";
}

Result<void> ConsolidationPolicy::validate() const {
    if (max_candidates_per_run == 0) {
        return make_memory_error(MemoryDomainCode::InvalidMutation,
                                 "candidate bound must be positive");
    }
    for (const auto &marker : forbidden_markers) {
        if (marker.empty()) {
            return make_memory_error(MemoryDomainCode::InvalidMutation,
                                     "forbidden markers must not be empty");
        }
    }
    for (const auto &marker : injection_markers) {
        if (marker.empty()) {
            return make_memory_error(MemoryDomainCode::InvalidMutation,
                                     "injection markers must not be empty");
        }
    }
    return Result<void>{};
}

std::size_t ConsolidationReport::count_of(CandidateDisposition disposition) const {
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
                                                  [disposition](const ConsolidationEntry &entry) {
                                                      return entry.disposition == disposition;
                                                  }));
}

MemoryConsolidator::MemoryConsolidator(ConsolidationPolicy policy, IConsolidationModel *model)
    : policy_(std::move(policy)), model_(model) {}

std::vector<MemoryCandidate>
MemoryConsolidator::extract_deterministic(std::span<const EventEnvelope> events,
                                          const MemoryScope &scope, const Timestamp &now) const {
    std::vector<MemoryCandidate> candidates;
    // Verified facts become environment/application facts; a settled loop
    // becomes an episode. Only verified event classes contribute — model
    // guesses and unverified observations never enter on their own.
    for (const auto &event : events) {
        const auto body = event.payload.type == "TaskFactVerified" ? parse_json(event.payload.data)
                                                                   : Result<JsonValue>{JsonValue{}};
        const bool versioned = body && body.value().find("schema") != nullptr;
        std::optional<PublishedTaskFact> published;
        if (versioned) {
            auto decoded = parse_published_task_fact(event);
            if (!decoded || decoded.value().scope != scope || !decoded.value().valid_at(now.wall))
                continue;
            published = std::move(decoded).value();
        }
        const auto legacy = versioned ? std::nullopt : parse_verified_fact(event);
        if (published || legacy) {
            MemoryRecord record;
            record.id = MemoryId::generate();
            record.scope = scope;
            record.kind = scope.kind == MemoryScopeKind::Application
                              ? MemoryKind::ApplicationFact
                              : (scope.kind == MemoryScopeKind::User ? MemoryKind::Preference
                                                                     : MemoryKind::EnvironmentFact);
            record.statement = published ? published->claim.key + "=" + published->claim.value
                                         : legacy->first + "=" + legacy->second;
            record.validity.valid_from =
                published ? std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                published->claim.valid_from)
                          : event_wall(event);
            // V1 attestation time is persisted in the payload. A file-store
            // envelope may be re-timestamped by legacy recovery (BUG-20261106-001).
            record.recorded_at = published ? record.validity.valid_from : event_wall(event);
            record.provenance = {event.event_id};
            if (published) {
                auto until = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    published->claim.valid_until);
                record.validity.valid_until = until;
                record.expires_at = until;
                record.provenance.push_back(published->claim.evidence_event);
            }
            record.verification = MemoryVerification::Verified;
            record.confidence = 0.9F;
            record.sensitivity = Sensitivity::Internal;
            MemoryCandidate candidate;
            candidate.proposed = std::move(record);
            candidate.evidence = candidate.proposed.provenance;
            candidate.reason = MutationReasonCode::VerifiedEvent;
            candidates.push_back(std::move(candidate));
            continue;
        }
        if (event.payload.type == "LoopSettled") {
            auto parsed = parse_json(event.payload.data);
            if (!parsed || parsed.value().find("outcome") == nullptr ||
                !parsed.value().find("outcome")->is_string() ||
                *parsed.value().find("outcome")->as_string() != "Completed") {
                continue;
            }
            MemoryRecord record;
            record.id = MemoryId::generate();
            record.scope = scope;
            record.kind = MemoryKind::Episode;
            record.statement = "task settled: outcome=completed sequence=" +
                               std::to_string(event.session_sequence);
            record.validity.valid_from = event_wall(event);
            record.recorded_at = event_wall(event);
            record.provenance = {event.event_id};
            record.verification = MemoryVerification::Observed;
            record.confidence = 0.6F;
            record.sensitivity = Sensitivity::Internal;
            MemoryCandidate candidate;
            candidate.proposed = std::move(record);
            candidate.evidence = {event.event_id};
            candidate.reason = MutationReasonCode::VerifiedEvent;
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

Result<ConsolidationReport> MemoryConsolidator::consolidate(IMemory &memory,
                                                            std::span<const EventEnvelope> events,
                                                            const MemoryScope &scope,
                                                            const Timestamp &now) const {
    const auto valid_policy = policy_.validate();
    if (!valid_policy) {
        return valid_policy.error();
    }
    // Missing v1 file time must not become "now" via the generic default.
    for (const auto &event : events) {
        if (event.timestamp_origin != EventTimestampOrigin::UnavailableLegacy)
            continue;
        auto body = parse_json(event.payload.data);
        if (!body || body.value().find("schema"))
            continue;
        const bool fact = parse_verified_fact(event).has_value();
        const auto *outcome = body.value().find("outcome");
        const bool episode = event.payload.type == "LoopSettled" && outcome &&
                             outcome->is_string() && *outcome->as_string() == "Completed";
        if (fact || episode)
            return make_memory_error(
                MemoryDomainCode::InvalidMutation,
                "source event time is unavailable; explicit validation required");
    }
    std::vector<MemoryCandidate> candidates = extract_deterministic(events, scope, now);
    if (model_ != nullptr) {
        // Model proposals are untrusted: strip any HumanConfirmed claim and
        // mark them model-assisted so the policy applies stricter checks.
        for (auto &proposal : model_->propose(events, scope)) {
            proposal.model_assisted = true;
            if (proposal.proposed.verification == MemoryVerification::HumanConfirmed ||
                !policy_.allow_model_import_exemption)
                proposal.proposed.verification = MemoryVerification::Unverified;
            if (!proposal.proposed.source_namespace.has_value())
                proposal.proposed.source_namespace = "consolidation-model";
            candidates.push_back(std::move(proposal));
        }
    }
    return consolidate_candidates(memory, std::move(candidates), scope, now);
}

Result<ConsolidationReport>
MemoryConsolidator::consolidate_candidates(IMemory &memory, std::vector<MemoryCandidate> candidates,
                                           const MemoryScope &scope, const Timestamp &now) const {
    const auto valid_policy = policy_.validate();
    if (!valid_policy) {
        return valid_policy.error();
    }
    ConsolidationReport report;
    // DEC-115: the report counts what arrived, and truncation is explicit.
    report.candidates_examined = candidates.size();
    if (candidates.size() > policy_.max_candidates_per_run) {
        report.candidates_truncated = candidates.size() - policy_.max_candidates_per_run;
        candidates.resize(policy_.max_candidates_per_run);
    }

    for (auto &candidate : candidates) {
        MemoryRecord proposed = std::move(candidate.proposed);
        const bool social = is_social_assertion(proposed.kind);
        const bool world = proposed.world_property.has_value() || is_world_scope(scope.kind);
        const bool immutable = social || world || candidate.immutable_add_id.has_value();
        if (candidate.model_assisted && !policy_.allow_model_import_exemption)
            proposed.verification = MemoryVerification::Unverified;
        proposed.scope = scope;
        if (proposed.recorded_at.time_since_epoch().count() == 0) {
            proposed.recorded_at =
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(now.wall);
        }
        // The fact-validity era must exist too: a zero valid_from would make
        // every era comparison call this candidate historical (DEC-115).
        if (proposed.validity.valid_from.time_since_epoch().count() == 0) {
            proposed.validity.valid_from = proposed.recorded_at;
        }
        const std::string key = memory_kind_name(proposed.kind) + ":" +
                                (immutable ? proposed.id.to_string()
                                           : context_text::to_lower_ascii(proposed.statement));
        ConsolidationEntry entry;
        entry.key = key;
        entry.mutation =
            candidate.immutable_add_id ? *candidate.immutable_add_id : MutationId::generate();

        if (candidate.immutable_add_id &&
            (candidate.immutable_add_id->is_nil() || proposed.kind != MemoryKind::SkillHint ||
             candidate.model_assisted || proposed.source_namespace ||
             proposed.verification != MemoryVerification::Observed ||
             candidate.evidence.size() != 1 || candidate.evidence != proposed.provenance ||
             candidate.evidence.front().is_nil())) {
            entry.disposition = CandidateDisposition::RejectedInvalid;
            entry.reason_code = "invalid-immutable-skill-hint";
            report.entries.push_back(std::move(entry));
            continue;
        }

        // 1. Forbidden content never persists.
        if (context_text::carries_marker(proposed.statement, policy_.forbidden_markers)) {
            entry.disposition = CandidateDisposition::RejectedForbidden;
            entry.reason_code = "forbidden-content-marker";
            report.entries.push_back(std::move(entry));
            continue;
        }
        // 2. Untrusted instruction-shaped text is rejected before it can
        // become a persistent prompt injection.
        if (candidate.model_assisted && immutable) {
            entry.disposition = CandidateDisposition::RejectedInvalid;
            entry.reason_code = social ? "social-assertion-requires-host-source"
                                       : "world-observation-requires-host-source";
            report.entries.push_back(std::move(entry));
            continue;
        }
        if ((candidate.model_assisted || immutable) &&
            context_text::carries_marker(proposed.statement, policy_.injection_markers)) {
            entry.disposition = CandidateDisposition::RejectedInjection;
            entry.reason_code = "instruction-shaped-model-text";
            report.entries.push_back(std::move(entry));
            continue;
        }
        // 3. Record-level validation (schema, confidence bounds, provenance).
        proposed.version = 1;
        if (const auto record_valid = proposed.validate(); !record_valid) {
            entry.disposition = CandidateDisposition::RejectedInvalid;
            entry.reason_code = "invalid-record";
            report.entries.push_back(std::move(entry));
            continue;
        }
        // The per-kind policy is a Core gate, not a store detail: a candidate
        // whose kind does not belong in this scope, or whose verification is
        // below that kind's floor, never reaches the store (design §6.2).
        auto policy_record = proposed;
        // Keep the provenance tag, but never let a model manufacture the
        // trusted-import exemption from per-kind verification/scope floors.
        if (candidate.model_assisted && !policy_.allow_model_import_exemption)
            policy_record.source_namespace.reset();
        if (const auto kind_policy = check_memory_kind_policy(policy_record); !kind_policy) {
            entry.disposition = CandidateDisposition::RejectedInvalid;
            entry.reason_code = "kind-policy";
            report.entries.push_back(std::move(entry));
            continue;
        }

        // 4. DEC-115 conflict analysis: same-key actives split into
        // duplicates (the same normalized claim — "volume=30" and
        // "volume = 30") and alternatives (same subject, different value).
        // Retention and authority are separate decisions: evidence
        // accumulates on duplicates, conflicting views coexist as preserved
        // alternatives, and only era succession replaces the authoritative
        // claim (design Context/Memory §13.5).
        const auto claim = memory_claim_key(proposed.statement);
        MemoryQuery conflict_query;
        conflict_query.scopes = {scope};
        conflict_query.kinds = std::vector<MemoryKind>{proposed.kind};
        conflict_query.claim_key = claim.key;
        conflict_query.max_results = 64;
        std::vector<MemoryRecord> duplicates;
        std::vector<MemoryRecord> alternatives;
        if (!immutable) {
            auto found = memory.query(conflict_query);
            if (!found) {
                entry.disposition = CandidateDisposition::RejectedConflict;
                entry.reason_code = "conflict-retrieval-unavailable";
                report.entries.push_back(std::move(entry));
                continue;
            }
            for (const auto &record : found.value().records) {
                if (record.status != MemoryStatus::Active) {
                    continue;
                }
                // The store narrows the candidates by claim key; the Core
                // verifies it, because a backend that ignores the filter
                // must not turn an unrelated record into a conflict.
                if (memory_claim_key(record.statement).key != claim.key) {
                    continue;
                }
                if (memory_same_claim(record.statement, proposed.statement)) {
                    duplicates.push_back(record);
                } else {
                    alternatives.push_back(record);
                }
            }
        }
        // Duplicate incumbent: the newest observation of the same claim —
        // observation time orders records that state the identical fact.
        std::optional<MemoryRecord> conflict;
        for (const auto &record : duplicates) {
            if (!conflict.has_value() || record.recorded_at > conflict->recorded_at) {
                conflict = record;
            }
        }
        // Era head: the newest fact-validity era among all same-key claims.
        // Succession replaces it; alternatives join beside it.
        std::optional<MemoryRecord> era_head;
        for (const auto *family : {&duplicates, &alternatives}) {
            for (const auto &record : *family) {
                if (!era_head.has_value() ||
                    record.validity.valid_from > era_head->validity.valid_from ||
                    (record.validity.valid_from == era_head->validity.valid_from &&
                     record.recorded_at > era_head->recorded_at)) {
                    era_head = record;
                }
            }
        }

        if (conflict.has_value()) {
            // Duplicate family: evidence and verification ride on the same
            // claim without approval — nothing new is claimed, so nothing
            // needs human sign-off. Entry outcomes: duplicate-noop,
            // evidence-appended, verification-promoted.
            const auto &candidate_evidence =
                candidate.evidence.empty() ? proposed.provenance : candidate.evidence;
            std::vector<EventId> fresh_evidence;
            std::set<std::string> known_evidence;
            for (const auto &event : conflict->provenance) {
                known_evidence.insert(event.to_string());
            }
            for (const auto &event : candidate_evidence) {
                if (known_evidence.insert(event.to_string()).second) {
                    fresh_evidence.push_back(event);
                }
            }
            const bool strictly_higher =
                memory_verification_rank(proposed.verification) >
                    memory_verification_rank(conflict->verification) &&
                conflict->verification != MemoryVerification::HumanConfirmed;
            if (strictly_higher && !fresh_evidence.empty()) {
                // Strict authoritative promotion (DEC-115): same claim, more
                // verified, evidence-backed — raised in place, never lowered,
                // HumanConfirmed incumbents are terminal.
                MemoryMutation mutation;
                mutation.id = entry.mutation;
                mutation.scope = scope;
                mutation.type = MemoryMutationType::Update;
                mutation.target = conflict->id;
                mutation.expected_version = conflict->version;
                mutation.proposed = *conflict;
                mutation.proposed.verification = proposed.verification;
                for (const auto &event : fresh_evidence) {
                    mutation.proposed.provenance.push_back(event);
                }
                mutation.evidence = fresh_evidence;
                mutation.reason = candidate.reason;
                const auto mutation_valid = mutation.validate();
                if (!mutation_valid) {
                    entry.disposition = CandidateDisposition::RejectedInvalid;
                    entry.reason_code = "invalid-mutation";
                    report.entries.push_back(std::move(entry));
                    continue;
                }
                auto applied = memory.apply(mutation);
                if (!applied) {
                    entry.disposition = CandidateDisposition::RejectedConflict;
                    entry.reason_code = "store-rejected-mutation";
                    report.entries.push_back(std::move(entry));
                    continue;
                }
                entry.disposition = CandidateDisposition::Applied;
                entry.reason_code = "verification-promoted";
                entry.resulting_version = applied.value().new_version;
                entry.incumbent = conflict->id;
                report.entries.push_back(std::move(entry));
                continue;
            }
            if (!fresh_evidence.empty()) {
                // New independent evidence for the same claim: accumulate it
                // without disturbing the fact identity or version chain.
                MemoryMutation mutation;
                mutation.id = entry.mutation;
                mutation.scope = scope;
                mutation.type = MemoryMutationType::AppendEvidence;
                mutation.target = conflict->id;
                mutation.expected_version = conflict->version;
                mutation.proposed = *conflict;
                mutation.evidence = fresh_evidence;
                mutation.reason = candidate.reason;
                const auto mutation_valid = mutation.validate();
                if (!mutation_valid) {
                    entry.disposition = CandidateDisposition::RejectedInvalid;
                    entry.reason_code = "invalid-mutation";
                    report.entries.push_back(std::move(entry));
                    continue;
                }
                auto applied = memory.apply(mutation);
                if (!applied) {
                    entry.disposition = CandidateDisposition::RejectedConflict;
                    entry.reason_code = "store-rejected-mutation";
                    report.entries.push_back(std::move(entry));
                    continue;
                }
                entry.disposition = CandidateDisposition::Applied;
                entry.reason_code = "evidence-appended";
                entry.resulting_version = applied.value().new_version;
                entry.incumbent = conflict->id;
                report.entries.push_back(std::move(entry));
                continue;
            }
            // Fully known repeat: no new memory, no churn — and never a
            // downgrade (an Unverified candidate must not touch a
            // HumanConfirmed record, M23 plan §4.2).
            entry.disposition = CandidateDisposition::Applied;
            entry.reason_code = "duplicate-noop";
            entry.resulting_version = conflict->version;
            entry.incumbent = conflict->id;
            report.entries.push_back(std::move(entry));
            continue;
        }

        // Alternative family / fresh key: supersede is earned only by era
        // succession — a strictly later fact era at sufficient verification
        // from an authoritative source (VerifiedEvent, an approved
        // HumanCorrection, or a write behind require_explicit_approval).
        // Everything else coexists as a preserved alternative; model text
        // never supersedes an authoritative claim (DEC-114/DEC-115).
        const bool approval_backed = candidate.reason == MutationReasonCode::HumanCorrection ||
                                     policy_.require_explicit_approval;
        const bool era_succession =
            era_head.has_value() &&
            (candidate.reason == MutationReasonCode::VerifiedEvent || approval_backed) &&
            (!candidate.model_assisted || approval_backed) &&
            memory_verification_rank(proposed.verification) >=
                memory_verification_rank(era_head->verification) &&
            proposed.validity.valid_from > era_head->validity.valid_from &&
            !(!proposed.validity.valid_until.has_value() &&
              era_head->validity.valid_until.has_value());
        std::optional<MemoryId> preserve_incumbent;
        std::string preserve_reason = "alternative-preserved";
        if (era_head.has_value()) {
            if (era_succession) {
                // Supersede targets the era head, not merely the newest
                // observation.
                conflict = era_head;
            } else {
                // The era head is the authoritative view, not an
                // alternative: the cap bounds the coexisting alternatives
                // beside it.
                const std::size_t existing_alternatives = alternatives.size() - 1;
                if (existing_alternatives >= policy_.max_alternatives_per_claim) {
                    entry.disposition = CandidateDisposition::RejectedConflict;
                    entry.reason_code = "alternative-cap-reached";
                    entry.incumbent = era_head->id;
                    report.entries.push_back(std::move(entry));
                    continue;
                }
                // An unbounded proposal cannot succeed a bounded era, but it
                // is still preserved: the information is not lost, only the
                // overwrite is forbidden.
                const bool boundedness_blocked = !proposed.validity.valid_until.has_value() &&
                                                 era_head->validity.valid_until.has_value();
                if (boundedness_blocked) {
                    preserve_reason = "legacy-coexists-bounded";
                }
                preserve_incumbent = era_head->id;
                conflict = era_head;
            }
        }

        // 5. High-risk kinds wait for human approval before apply().
        const bool needs_approval =
            std::find(policy_.approval_required_kinds.begin(),
                      policy_.approval_required_kinds.end(),
                      proposed.kind) != policy_.approval_required_kinds.end();
        if (policy_.require_explicit_approval ||
            (needs_approval && candidate.reason != MutationReasonCode::HumanCorrection)) {
            MemoryMutation mutation;
            mutation.id = entry.mutation;
            mutation.scope = scope;
            mutation.proposed = proposed;
            mutation.evidence =
                candidate.evidence.empty() ? proposed.provenance : candidate.evidence;
            mutation.reason = candidate.reason;
            if (preserve_incumbent.has_value()) {
                mutation.type = MemoryMutationType::PreserveAlternative;
                mutation.proposed.alternative_of = preserve_incumbent;
            } else if (conflict.has_value()) {
                mutation.type = MemoryMutationType::Supersede;
                mutation.target = conflict->id;
                mutation.expected_version = conflict->version;
                mutation.proposed.supersedes = conflict->id;
            } else {
                mutation.type = MemoryMutationType::Add;
            }
            const auto mutation_valid = mutation.validate();
            if (!mutation_valid) {
                entry.disposition = CandidateDisposition::RejectedInvalid;
                entry.reason_code = "invalid-mutation";
                report.entries.push_back(std::move(entry));
                continue;
            }
            entry.disposition = CandidateDisposition::PendingApproval;
            entry.reason_code = "awaiting-human-approval";
            report.pending_approval.push_back(std::move(mutation));
            report.entries.push_back(std::move(entry));
            continue;
        }

        // 6. Plan and apply.
        MemoryMutation mutation;
        mutation.id = entry.mutation;
        mutation.scope = scope;
        mutation.proposed = proposed;
        mutation.evidence = candidate.evidence.empty() ? proposed.provenance : candidate.evidence;
        mutation.reason = candidate.reason;
        if (preserve_incumbent.has_value()) {
            mutation.type = MemoryMutationType::PreserveAlternative;
            mutation.proposed.alternative_of = preserve_incumbent;
        } else if (conflict.has_value()) {
            mutation.type = MemoryMutationType::Supersede;
            mutation.target = conflict->id;
            mutation.expected_version = conflict->version;
            mutation.proposed.supersedes = conflict->id;
            // Supersede requires a fresh record id.
            mutation.proposed.id = MemoryId::generate();
        } else {
            mutation.type = MemoryMutationType::Add;
        }
        const auto mutation_valid = mutation.validate();
        if (!mutation_valid) {
            entry.disposition = CandidateDisposition::RejectedInvalid;
            entry.reason_code = "invalid-mutation";
            report.entries.push_back(std::move(entry));
            continue;
        }
        auto applied = memory.apply(mutation);
        if (!applied) {
            entry.disposition = CandidateDisposition::RejectedConflict;
            entry.reason_code = "store-rejected-mutation";
            report.entries.push_back(std::move(entry));
            continue;
        }
        entry.disposition = CandidateDisposition::Applied;
        entry.reason_code = preserve_incumbent.has_value() ? preserve_reason
                            : conflict.has_value()         ? "superseded-existing"
                                                           : "added";
        entry.resulting_version = applied.value().new_version;
        entry.incumbent = preserve_incumbent.has_value() ? preserve_incumbent
                          : conflict.has_value()         ? conflict->id
                                                         : std::optional<MemoryId>{};
        entry.idempotent_replay = applied.value().idempotent_replay;
        report.entries.push_back(std::move(entry));
    }
    return report;
}

Result<MemoryMutationResult> MemoryConsolidator::apply_pending(IMemory &memory,
                                                               const MemoryMutation &mutation) {
    return memory.apply(mutation);
}

} // namespace mira
