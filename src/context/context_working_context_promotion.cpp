#include <mira/context_working_context_promotion.hpp>

#include <mira/context_retrieval.hpp>

#include <array>
#include <string>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error promotion_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context.working_context_promotion";
    error.safe_message = std::move(message);
    return error;
}

struct PromotableSection final {
    std::string_view tag; // seed-space section tag
    MemoryKind kind;
    const std::vector<WorkingContextItem> *items;
};

} // namespace

Result<void> WorkingContextPromotionPolicy::validate() const {
    // The range comparison also rejects NaN (every relational compare is
    // false).
    if (!(min_confidence >= 0.0 && min_confidence <= 1.0)) {
        return promotion_error(ErrorCode::InvalidArgument,
                               "promotion confidence floor must be within [0, 1]");
    }
    if (max_candidates_per_run == 0) {
        return promotion_error(ErrorCode::InvalidArgument, "candidate bound must be positive");
    }
    return Result<void>{};
}

MemoryId promotion_record_id_from_seed(std::string_view seed) {
    const ContextAssetId asset =
        context_asset_id_from_seed("mira.memory.promotion|" + std::string(seed));
    return MemoryId{asset.value};
}

Result<WorkingContextPromotionProjection>
memory_candidates_from_working_context(const WorkingContextSnapshot &snapshot,
                                       const MemoryScope &scope,
                                       const WorkingContextPromotionPolicy &policy) {
    if (const auto valid_policy = policy.validate(); !valid_policy) {
        return valid_policy.error();
    }
    if (const auto valid_snapshot = snapshot.validate(); !valid_snapshot) {
        return valid_snapshot.error();
    }

    // Frozen section order and kind mapping (M23 plan §4.1). The
    // task-oriented sections and important_refs are deliberately absent:
    // promotion must not be able to persist current task state.
    const std::array<PromotableSection, 4> sections = {
        PromotableSection{"constraints", MemoryKind::Preference, &snapshot.constraints},
        PromotableSection{"decisions", MemoryKind::ApplicationFact, &snapshot.decisions},
        PromotableSection{"verified_facts", MemoryKind::EnvironmentFact, &snapshot.verified_facts},
        PromotableSection{"failed_attempts", MemoryKind::RecoveryLesson, &snapshot.failed_attempts},
    };

    WorkingContextPromotionProjection projection;
    for (const auto &section : sections) {
        for (const auto &item : *section.items) {
            // Per-item evaluation order is frozen: drops are counted for
            // every item (even past the cap), truncation only skips valid
            // surplus candidates.
            if (item.confidence < policy.min_confidence) {
                projection.dropped_low_confidence += 1;
                continue;
            }
            if (item.source_events.empty()) {
                // Upper contract (M4): every Add carries event evidence.
                projection.dropped_missing_evidence += 1;
                continue;
            }
            if (projection.candidates.size() >= policy.max_candidates_per_run) {
                continue;
            }

            MemoryRecord record;
            record.id = promotion_record_id_from_seed(
                std::string(memory_scope_kind_name(scope.kind)) + "|" + scope.subject_id + "|" +
                std::string(section.tag) + "|" + item.content);
            record.scope = scope;
            record.kind = section.kind;
            record.statement = item.content;
            record.validity.valid_from = snapshot.created_at.wall;
            record.provenance = item.source_events;
            // Model-mediated derived projection (RULE-09): hard-wired, not
            // caller-tunable — a promotion candidate is unverified model
            // output no matter where the caller thinks it came from.
            record.verification = MemoryVerification::Unverified;
            record.confidence = static_cast<float>(item.confidence);
            record.sensitivity = Sensitivity::Internal;
            record.source_namespace = "working-context";

            MemoryCandidate candidate;
            candidate.proposed = std::move(record);
            candidate.evidence = item.source_events;
            candidate.reason = MutationReasonCode::Consolidation;
            candidate.model_assisted = true;
            projection.candidates.push_back(std::move(candidate));
        }
    }
    return projection;
}

Result<WorkingContextPromotionReport> promote_working_context_to_memory(
    const MemoryConsolidator &consolidator, IMemory &memory, const WorkingContextSnapshot &snapshot,
    const MemoryScope &scope, const Timestamp &now, const WorkingContextPromotionPolicy &policy) {
    auto projection = memory_candidates_from_working_context(snapshot, scope, policy);
    if (!projection) {
        return projection.error();
    }
    WorkingContextPromotionReport report;
    // The report owns the projection (candidates included); the pipeline
    // works on a copy, so a consumed report still audits what was projected.
    report.projection = std::move(projection.value());
    auto consolidation =
        consolidator.consolidate_candidates(memory, report.projection.candidates, scope, now);
    if (!consolidation) {
        return consolidation.error();
    }
    report.consolidation = std::move(consolidation.value());
    return report;
}

} // namespace mira
