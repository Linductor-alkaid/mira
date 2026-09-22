#pragma once

#include <mira/context_working_context.hpp>
#include <mira/core_contracts.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>

#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Curator Stage W4: Memory Promotion (DEC-035 decision 3, curator
// design §2 principle 3 / §13, M23)
//
// The only bridge from the Working State Plane into long-term memory: durable
// statements of a committed `WorkingContextSnapshot` are projected — purely
// deterministically — into `MemoryCandidate`s that then run the *existing*
// `MemoryConsolidator` policy pipeline (marker filters, record validation,
// in-scope conflict retrieval, duplicate detection, human-approval gating,
// mutation planning and apply). The snapshot and its curator never write
// memory directly; promotion is an explicit host operation (typically at a
// task-terminal boundary), never an automatic side effect of curation.
//
// Every promoted candidate is model-mediated derived projection (RULE-09):
// it is hard-wired to `Unverified` + `model_assisted` + the
// "working-context" source namespace regardless of what a caller passes in,
// and it can never claim `HumanConfirmed` — human confirmation happens only
// through the existing approval flow on the memory side. The shared
// pipeline's duplicate rule keeps a stored copy that is at least as verified
// as the proposal, so a re-promotion can never downgrade a human-confirmed
// record (M23 plan §4.2).
//
// Promotion is read-only on the snapshot plane: it never writes the
// `IWorkingContextStore`, mints snapshots or advances watermarks, and
// `erase_session` does not reach promoted records — they are long-term
// memory facts governed by the memory-side retention/erasure/approval
// discipline, and hosts orchestrate any cross-store erasure through the
// existing `IMemory` erasure path themselves.
// ---------------------------------------------------------------------------

// Bounds and floors for one promotion projection. Documented defaults, not a
// frozen contract (mirrors `ConsolidationOptions`): promotion raises a
// task-scoped statement into cross-task memory, so the confidence floor
// defaults to a majority line and the candidate cap keeps one run bounded.
struct WorkingContextPromotionPolicy final {
    // Items below this confidence are dropped (counted), never clamped.
    double min_confidence = 0.5;
    // Projection-stage candidate cap; surplus *valid* candidates beyond it
    // are truncated in the frozen section order (the pipeline's own
    // `max_candidates_per_run` still applies downstream). Drop counters stay
    // exact for every item regardless of truncation.
    std::size_t max_candidates_per_run = 32;

    [[nodiscard]] Result<void> validate() const;
};

// Deterministic projection output. `candidates` is empty for an empty
// snapshot — an empty projection is a normal outcome, not an error.
struct WorkingContextPromotionProjection final {
    std::vector<MemoryCandidate> candidates;
    std::size_t dropped_low_confidence = 0;   // item.confidence < policy floor
    std::size_t dropped_missing_evidence = 0; // item.source_events empty
};

// Projects the durable snapshot sections into memory candidates (plan §4.1,
// frozen mapping): constraints -> Preference (falls into the pipeline's
// default human-approval gate), decisions -> ApplicationFact, verified_facts
// -> EnvironmentFact, failed_attempts -> RecoveryLesson. The task-oriented
// sections (active_tasks / next_actions / open_issues) and important_refs
// are never read — there is no caller-visible way to promote current task
// state into long-term memory. Section order is frozen; record ids derive
// deterministically from the promotion seed space, so the same snapshot
// always projects to the same candidates. Pure function; hosts run it inside
// an Executor-supervised operation.
[[nodiscard]] Result<WorkingContextPromotionProjection>
memory_candidates_from_working_context(const WorkingContextSnapshot &snapshot,
                                       const MemoryScope &scope,
                                       const WorkingContextPromotionPolicy &policy = {});

// Deterministic record identity for one promoted statement: first 16 bytes
// of SHA-256 over `mira.memory.promotion|<seed>` (the same derivation shape
// as the workflow-learning ids; the seed space is promotion-specific).
// Exposed so hosts and tests can audit record ids without re-deriving them
// from the projection internals.
[[nodiscard]] MemoryId promotion_record_id_from_seed(std::string_view seed);

struct WorkingContextPromotionReport final {
    WorkingContextPromotionProjection projection;
    ConsolidationReport consolidation; // the shared pipeline's verdicts
};

// Composition entry: project the snapshot, then run the candidates through
// `MemoryConsolidator::consolidate_candidates` — the single existing policy
// pipeline. The consolidator's `ConsolidationPolicy` is the only
// configuration surface for promotion discipline (markers, approval kinds,
// caps); this function adds no policy of its own. The scope is
// host-authoritative ACL: records become retrievable exactly in the scopes
// the host names, with the snapshot's source-event provenance attached.
[[nodiscard]] Result<WorkingContextPromotionReport>
promote_working_context_to_memory(const MemoryConsolidator &consolidator, IMemory &memory,
                                  const WorkingContextSnapshot &snapshot, const MemoryScope &scope,
                                  const Timestamp &now,
                                  const WorkingContextPromotionPolicy &policy = {});

} // namespace mira
