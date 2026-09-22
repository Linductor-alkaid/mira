#pragma once

#include <mira/context_working_context.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Curator Stage W5: Subagent fork / merge (DEC-044, issue #48)
//
// A subagent is a host-created child Session beside the parent; these entry
// points are the Working State Plane projection operations behind that scene
// (M24 §4). All three are stateless deterministic pure functions — no model
// call, no store write, no hidden background work (fork/merge are explicit
// host operations; candidates reach the store only through the existing
// `commit_working_context` §5.2 pipeline, so the §5.2 discipline — same
// watermark + different digest fails closed as "conflicting-watermark" —
// applies to merge products without any waiver).
//
// Pipeline: `fork_working_context` derives the child's read-only baseline
// from a committed parent snapshot (verbatim eight-section copy + fork
// provenance); the child curates its own chain on top of that baseline with
// the unchanged W2/W3 contracts; `working_context_delta_from_fork` projects
// the child tip against the baseline into a bounded local delta (inherited
// skipped, lineage-bound supersede, addition); `merge_working_context_delta`
// folds the delta back into the parent by reference (in-place supersede,
// stale supersede reclassified as a counted addition, fixed-order
// truncation) producing an ordinary snapshot candidate.
// ---------------------------------------------------------------------------

// Host-supplied fork request: the child session (the baseline's ACL owner in
// the store) and the child five-tuple the baseline is stamped with. The
// child watermark must be >= 1 — the child session has already recorded its
// fork instruction event; the snapshot positive-watermark discipline applies
// to the derived baseline identically.
struct WorkingContextForkSeed final {
    SessionId child_session;
    WorkingContextIdentity child_identity; // task / task_epoch / environment_epoch
    std::uint64_t child_watermark = 0;
};

// Mechanical delta classification (M24 §4.3). The frozen per-entry order:
// (1) byte-equal content with a baseline entry -> inherited (skipped and
// counted, never re-transcribed); (2) else a source-event intersection with
// a same-section baseline entry -> Supersede of the minimal such baseline
// index (the W2 numbered-transcript provenance binding makes the carried
// parent event the lineage evidence; parent and child event spaces are
// disjoint); (3) else Addition.
enum class WorkingContextDeltaEntryKind : std::uint8_t {
    Addition,
    Supersede,
};

struct WorkingContextDeltaEntry final {
    WorkingContextDeltaEntryKind kind = WorkingContextDeltaEntryKind::Addition;
    // Supersede only: the fork baseline's canonical entry index (declaration
    // order x vector order, M24 §4.3); the merge resolves the target through
    // this reference.
    std::size_t superseded_base_index = 0;
    std::string section; // frozen vocabulary = the eight snapshot section names
    std::string content;
    std::vector<EventId> source_events; // per-entry non-empty (child events and/or lineage)
    SessionSequence source_sequence = 0;
    double confidence = 0.0;
};

// The child agent's curated result (issue #48 "局部 delta"): a versioned,
// bounded projection — never the full exploration history. `generated_by`
// records the child-side curator profile (DEC-036); the parent-side merge
// attribution stays on the parent snapshot.
struct WorkingContextDelta final {
    // mira.working_context.delta.v1
    SchemaVersion schema_version = SchemaVersion{1, 0};
    WorkingContextSnapshotId fork_base_snapshot_id; // required anchor
    SessionId child_session_id;
    std::vector<WorkingContextDeltaEntry> entries; // bounded, fixed emission order
    // Visibility counter: baseline entries removed as verbatim copies.
    std::size_t inherited_skipped = 0;
    ModelProfileId generated_by;

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] JsonValue working_context_delta_to_json(const WorkingContextDelta &delta);
[[nodiscard]] Result<WorkingContextDelta> working_context_delta_from_json(const JsonValue &json);

// Report of one mechanical merge: the candidate plus the full decision
// counters the host audits (and the promotion gate reads). The candidate is
// an ordinary parent-identity snapshot (fork provenance nil, `generated_by`
// inherited from the parent so a zero-effect merge stays digest-identical to
// its parent — M24 §4.4 item 5/7); this entry point never writes a store.
struct WorkingContextMergeReport final {
    WorkingContextSnapshot merged;
    std::size_t additions_appended = 0;       // additions (incl. reclassified stales) appended
    std::size_t supersedes_resolved = 0;      // replaced in place, parent order kept
    std::size_t supersedes_dropped_stale = 0; // target gone from the parent; reclassified
    std::size_t truncated_entries = 0;        // per-section fixed-order overflow
};

// Derives the child-session fork baseline from a committed parent snapshot
// (M24 §4.2): the eight sections copy verbatim (content / provenance /
// sequence / confidence and the source checkpoint chain), identity rebases to
// the child five-tuple, and the snapshot carries its `fork` provenance. Pure
// function: the same (parent_base, seed) pair always derives the same id,
// digest and bytes; an invalid parent, an out-of-bounds one or a zero child
// watermark rejects the whole operation with no partial baseline. Hosts run
// it inside an Executor-supervised operation.
[[nodiscard]] Result<WorkingContextSnapshot>
fork_working_context(const WorkingContextSnapshot &parent_base, const WorkingContextForkSeed &seed,
                     const WorkingContextMergeOptions &options = {});

// Projects the child tip against its fork baseline into a local delta (M24
// §4.3): entries classify per the frozen per-entry order above, emitted in
// section declaration order and vector order inside. All-or-nothing: an
// invalid baseline or child snapshot rejects the whole projection.
[[nodiscard]] Result<WorkingContextDelta>
working_context_delta_from_fork(const WorkingContextSnapshot &fork_base,
                                const WorkingContextSnapshot &child_snapshot,
                                const WorkingContextMergeOptions &options = {});

// Folds a delta into the parent snapshot by reference (M24 §4.4): supersede
// entries resolve against the fork-base entry content at their canonical
// index (first content-equal parent item -> in-place replacement; no hit ->
// counted stale reclassified as an addition), additions append in delta
// order, and each section truncates in fixed order at the option bound. The
// candidate carries the parent identity at `parent_watermark`, inherits
// `parent.generated_by` and `source_checkpoints`, and leaves `fork` nil —
// the host commits it through the existing `commit_working_context` pipeline
// (strictly advancing watermark; same-watermark different digest fails
// closed). Pure function; deterministic bytes and counters for the same
// input group.
[[nodiscard]] Result<WorkingContextMergeReport> merge_working_context_delta(
    const WorkingContextSnapshot &fork_base, const WorkingContextSnapshot &parent,
    const WorkingContextDelta &delta, const WorkingContextIdentity &parent_identity,
    std::uint64_t parent_watermark, const WorkingContextMergeOptions &options = {});

} // namespace mira
