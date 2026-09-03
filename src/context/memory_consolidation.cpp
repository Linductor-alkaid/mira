#include <mira/memory_consolidation.hpp>

#include <mira/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] bool contains_any_marker(const std::string &text,
                                       const std::vector<std::string> &markers) {
    std::string lower;
    lower.reserve(text.size());
    for (const char character : text) {
        lower += static_cast<char>(
            character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    }
    for (const auto &marker : markers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::int64_t wall_nanos(const std::chrono::system_clock::time_point &stamp) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count());
}

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

[[nodiscard]] std::string lowercase(const std::string &text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char character : text) {
        lower += static_cast<char>(
            character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    }
    return lower;
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
    return Result<void>{};
}

std::size_t ConsolidationReport::count_of(CandidateDisposition disposition) const {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(),
                      [disposition](const ConsolidationEntry &entry) {
                          return entry.disposition == disposition;
                      }));
}

MemoryConsolidator::MemoryConsolidator(ConsolidationPolicy policy, IConsolidationModel *model)
    : policy_(std::move(policy)), model_(model) {}

std::vector<MemoryCandidate>
MemoryConsolidator::extract_deterministic(std::span<const EventEnvelope> events,
                                          const MemoryScope &scope) const {
    std::vector<MemoryCandidate> candidates;
    // Verified facts become environment/application facts; a settled loop
    // becomes an episode. Only verified event classes contribute — model
    // guesses and unverified observations never enter on their own.
    for (const auto &event : events) {
        if (const auto fact = parse_verified_fact(event)) {
            MemoryRecord record;
            record.id = MemoryId::generate();
            record.scope = scope;
            record.kind = scope.kind == MemoryScopeKind::Application
                              ? MemoryKind::ApplicationFact
                              : (scope.kind == MemoryScopeKind::User ? MemoryKind::Preference
                                                                      : MemoryKind::EnvironmentFact);
            record.statement = fact->first + "=" + fact->second;
            record.validity.valid_from = event_wall(event);
            record.recorded_at = event_wall(event);
            record.provenance = {event.event_id};
            record.verification = MemoryVerification::Verified;
            record.confidence = 0.9F;
            record.sensitivity = Sensitivity::Internal;
            MemoryCandidate candidate;
            candidate.proposed = std::move(record);
            candidate.evidence = {event.event_id};
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
    ConsolidationReport report;
    std::vector<MemoryCandidate> candidates = extract_deterministic(events, scope);
    if (model_ != nullptr) {
        // Model proposals are untrusted: strip any HumanConfirmed claim and
        // mark them model-assisted so the policy applies stricter checks.
        for (auto &proposal : model_->propose(events, scope)) {
            proposal.model_assisted = true;
            if (proposal.proposed.verification == MemoryVerification::HumanConfirmed) {
                proposal.proposed.verification = MemoryVerification::Unverified;
            }
            if (!proposal.proposed.source_namespace.has_value()) {
                proposal.proposed.source_namespace = "consolidation-model";
            }
            candidates.push_back(std::move(proposal));
        }
    }
    if (candidates.size() > policy_.max_candidates_per_run) {
        candidates.resize(policy_.max_candidates_per_run);
    }
    report.candidates_examined = candidates.size();

    for (auto &candidate : candidates) {
        MemoryRecord proposed = std::move(candidate.proposed);
        proposed.scope = scope;
        proposed.recorded_at =
            proposed.recorded_at.time_since_epoch().count() == 0 ? now.wall : proposed.recorded_at;
        const std::string key =
            memory_kind_name(proposed.kind) + ":" + lowercase(proposed.statement);
        ConsolidationEntry entry;
        entry.key = key;
        entry.mutation = MutationId::generate();

        // 1. Forbidden content never persists.
        if (contains_any_marker(proposed.statement, policy_.forbidden_markers)) {
            entry.disposition = CandidateDisposition::RejectedForbidden;
            entry.reason_code = "forbidden-content-marker";
            report.entries.push_back(std::move(entry));
            continue;
        }
        // 2. Untrusted instruction-shaped text is rejected before it can
        // become a persistent prompt injection.
        if (candidate.model_assisted &&
            contains_any_marker(proposed.statement, policy_.injection_markers)) {
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

        // 4. Conflict retrieval inside the same scope: facts conflict when
        // their key prefix matches ("volume=..." vs "volume=..."), so a new
        // value supersedes the old record instead of duplicating it.
        const auto separator = proposed.statement.find('=');
        const std::string conflict_term =
            separator == std::string::npos ? proposed.statement
                                           : proposed.statement.substr(0, separator + 1);
        MemoryQuery conflict_query;
        conflict_query.scopes = {scope};
        conflict_query.kinds = std::vector<MemoryKind>{proposed.kind};
        conflict_query.exact_terms = {conflict_term};
        conflict_query.max_results = 4;
        std::optional<MemoryRecord> conflict;
        {
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
                const auto record_separator = record.statement.find('=');
                const bool same_key =
                    separator == std::string::npos
                        ? lowercase(record.statement) == lowercase(proposed.statement)
                        : record_separator == separator &&
                              lowercase(record.statement.substr(0, separator)) ==
                                  lowercase(proposed.statement.substr(0, separator));
                if (same_key) {
                    conflict = record;
                    break;
                }
            }
        }
        if (conflict.has_value() && conflict->statement == proposed.statement &&
            conflict->verification == proposed.verification) {
            // Exact duplicate: no new memory, no churn.
            entry.disposition = CandidateDisposition::Applied;
            entry.reason_code = "duplicate-noop";
            entry.resulting_version = conflict->version;
            report.entries.push_back(std::move(entry));
            continue;
        }

        // 5. High-risk kinds wait for human approval before apply().
        const bool needs_approval = std::find(policy_.approval_required_kinds.begin(),
                                              policy_.approval_required_kinds.end(),
                                              proposed.kind) !=
                                    policy_.approval_required_kinds.end();
        if (needs_approval && candidate.reason != MutationReasonCode::HumanCorrection) {
            MemoryMutation mutation;
            mutation.id = entry.mutation;
            mutation.scope = scope;
            mutation.proposed = proposed;
            mutation.evidence =
                    candidate.evidence.empty() ? proposed.provenance : candidate.evidence;
            mutation.reason = candidate.reason;
            if (conflict.has_value()) {
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
        mutation.evidence =
                    candidate.evidence.empty() ? proposed.provenance : candidate.evidence;
        mutation.reason = candidate.reason;
        if (conflict.has_value()) {
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
        entry.reason_code = conflict.has_value() ? "superseded-existing" : "added";
        entry.resulting_version = applied.value().new_version;
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
