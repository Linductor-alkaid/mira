// M24 (DEC-035 Stage W5) subagent fork/merge contract/integration suite over
// the milestone §6 gate matrix, against the frozen §4 semantics (fixtures:
// tests/m24/m24_fork_merge_support.hpp):
//   W5-G1 fork contract — eight-section verbatim copy, child-identity rebase,
//     fork provenance, zero-watermark/invalid-parent all-or-nothing, schema
//     1.2 additive discipline (v1.0/v1.1 payloads keep their schema version
//     and a bit-identical digest; the fork field enters the canonical object
//     only when non-nil, no per-version branching), pure-function idempotency.
//   W5-G2 delta contract — three-way mechanical projection (inherited skip
//     counter, same-section lineage intersection -> Supersede at the minimal
//     baseline index, else Addition), canonical baseline numbering in
//     declaration order, frozen eight-section vocabulary, provenance/bounds
//     discipline, fork-base snapshot id required, delta schema v1 JSON round
//     trip, invalid child snapshot rejected whole.
//   W5-G3 merge policy — in-place supersede resolved through the fork-base
//     reference and preserving parent order, stale supersede reclassified as
//     a counted addition, additions in delta order, per-section fixed-order
//     truncation, generated_by inherited from the parent (nil and curated
//     forms), the three precondition rejections, zero-effect delta
//     digest-identical to the parent, byte determinism.
//   W5-G4 commit integration — merge candidates commit only on a strictly
//     advancing parent watermark; the same watermark with a different digest
//     fails closed as "conflicting-watermark" with the store untouched;
//     zero-effect same-watermark is IdempotentNoOp for both parent forms;
//     stale/terminal lateness unchanged; parent/child chains isolated; a
//     parent epoch change rejects the stale-identity merge candidate.
//   W5-G5 lifecycle — supervisor Deferrable routing (consumed future,
//     in-flight cancel resolves Cancelled with zero partial writes, post-
//     close rejection), erase_session(child) leaves the parent and merged
//     content untouched, W3 AutoCurator chains stay per-session independent,
//     W4 promotion maps the merged parent and never the unmerged branch, and
//     the child never writes the parent store.
//   W5-G6 recovery (test half) — the fork baseline rebuilds from the rebuilt
//     parent baseline with the same id and digest.
//
// Determinism: all synchronization gates on mutex/condition_variable; no
// sleep-based sequencing anywhere.

#include "m24_fork_merge_cases.hpp"
#include "m24_fork_merge_support.hpp"

#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using mira::testing::GatedWorkingContextStore;
using mira::testing::m24_event_from_seed;
using mira::testing::m24_fill_all_sections;
using mira::testing::m24_fixed_now;
using mira::testing::m24_identity_from_seed;
using mira::testing::m24_item;
using mira::testing::m24_profile_from_seed;
using mira::testing::m24_section_names;
using mira::testing::m24_session_from_seed;
using mira::testing::m24_snapshot;
using mira::testing::m24_task_from_seed;
using mira::testing::StoreGateReleaser;
using namespace mira::m24_cases;
// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// W5-G1: the eight sections copy verbatim into the child baseline and the
// identity rebase carries the child five-tuple
// ---------------------------------------------------------------------------

int fork_copies_all_eight_sections_verbatim() {
    WorkingContextSnapshot parent = m24_snapshot(100);
    m24_fill_all_sections(parent, 900);
    parent.constraints.push_back(m24_item("constraint: second entry", 950, 0.7));

    const WorkingContextForkSeed seed = m24_fork_seed(101, 102, 5);
    const auto baseline = fork_working_context(parent, seed);
    MIRA_CHECK(baseline.has_value());
    const WorkingContextSnapshot &child = baseline.value();
    MIRA_CHECK(child.validate().has_value());

    // Additive minor: a new-produced fork baseline is stamped schema 1.2.
    MIRA_CHECK((working_context_schema_current() == SchemaVersion{1, 2}));
    MIRA_CHECK((child.schema_version == SchemaVersion{1, 2}));

    // Identity rebase: the child five-tuple, never the parent's.
    MIRA_CHECK(child.session_id == seed.child_session);
    MIRA_CHECK(child.task_id == seed.child_identity.task);
    MIRA_CHECK(child.task_epoch == seed.child_identity.task_epoch);
    MIRA_CHECK(child.environment_epoch == seed.child_identity.environment_epoch);
    MIRA_CHECK(child.through_event_sequence == 5);

    // Verbatim copy, per section in declaration order and vector order:
    // content, provenance, source sequence and confidence all carry over.
    MIRA_CHECK(child.constraints.size() == 2);
    MIRA_CHECK(child.constraints[0].content == "constraint: deploy only on tuesdays");
    MIRA_CHECK(child.constraints[0].source_events == parent.constraints[0].source_events);
    MIRA_CHECK(child.constraints[0].source_sequence == parent.constraints[0].source_sequence);
    MIRA_CHECK(child.constraints[0].confidence == parent.constraints[0].confidence);
    MIRA_CHECK(child.constraints[1].content == "constraint: second entry");
    MIRA_CHECK(child.decisions.size() == 1);
    MIRA_CHECK(child.decisions.front().content ==
               "decision: batch provider after the rate limit incident");
    MIRA_CHECK(child.decisions.front().source_events == parent.decisions.front().source_events);
    MIRA_CHECK(child.open_issues.size() == 1);
    MIRA_CHECK(child.open_issues.front().content == "issue: tenant quota still unanswered");
    MIRA_CHECK(child.active_tasks.size() == 1);
    MIRA_CHECK(child.active_tasks.front().content == "task: finish the tenant onboarding flow");
    MIRA_CHECK(child.verified_facts.size() == 1);
    MIRA_CHECK(child.verified_facts.front().content == "fact: staging accepts the v2 payload");
    MIRA_CHECK(child.failed_attempts.size() == 1);
    MIRA_CHECK(child.failed_attempts.front().content == "attempt: retry loop hit rate limit twice");
    MIRA_CHECK(child.important_refs.size() == 1);
    MIRA_CHECK(child.important_refs.front().content == "ref: tenant quota thread in ticket 482");
    MIRA_CHECK(child.next_actions.size() == 1);
    MIRA_CHECK(child.next_actions.front().content == "action: ask user about the deploy window");

    // Cross-session checkpoint references stay legal (global 128-bit ids):
    // the source checkpoint chain is inherited verbatim.
    MIRA_CHECK(child.source_checkpoints == parent.source_checkpoints);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G1: fork provenance fields; invalid parents and a zero child watermark
// reject the whole operation with no partial baseline
// ---------------------------------------------------------------------------

int fork_provenance_and_negative_inputs() {
    WorkingContextSnapshot parent = m24_snapshot(110);
    parent.constraints.push_back(m24_item("constraint: deploy only on tuesdays", 301, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(111, 112, 7);

    const auto baseline = fork_working_context(parent, seed);
    MIRA_CHECK(baseline.has_value());
    MIRA_CHECK(baseline.value().fork.has_value());
    const WorkingContextForkProvenance &provenance = baseline.value().fork.value();
    MIRA_CHECK(provenance.base_snapshot_id == parent.id);
    MIRA_CHECK(provenance.parent_session_id == parent.session_id);
    MIRA_CHECK(provenance.parent_through_event_sequence == parent.through_event_sequence);
    MIRA_CHECK(baseline.value().validate().has_value());

    // Zero child watermark: the child session has not even recorded the fork
    // instruction event — the whole operation rejects.
    WorkingContextForkSeed zero_watermark = seed;
    zero_watermark.child_watermark = 0;
    const auto rejected_zero = fork_working_context(parent, zero_watermark);
    MIRA_CHECK(!rejected_zero.has_value());
    MIRA_CHECK(rejected_zero.error().code == ErrorCode::InvalidArgument);

    // Invalid parents reject whole, before any baseline exists.
    WorkingContextSnapshot nil_id = m24_snapshot(113);
    nil_id.id = WorkingContextSnapshotId{};
    WorkingContextSnapshot zero_parent_watermark = m24_snapshot(114);
    zero_parent_watermark.through_event_sequence = 0;
    WorkingContextSnapshot nil_session = m24_snapshot(115);
    nil_session.session_id = SessionId{};
    WorkingContextSnapshot empty_content = m24_snapshot(116);
    empty_content.constraints.push_back(m24_item("", 311, 0.9));
    for (const WorkingContextSnapshot *broken :
         {&nil_id, &zero_parent_watermark, &nil_session, &empty_content}) {
        MIRA_CHECK(!broken->validate().has_value());
        const auto rejected = fork_working_context(*broken, seed);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G1: schema 1.2 additive digest discipline — the fork field enters the
// canonical object only when non-nil, and v1.0/v1.1 payloads read back with
// their own schema version and a bit-identical pre-upgrade digest
// ---------------------------------------------------------------------------

int fork_schema_12_digest_discipline() {
    WorkingContextSnapshot parent = m24_snapshot(120);
    parent.constraints.push_back(m24_item("constraint: deploy only on tuesdays", 321, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(121, 122, 3);
    const auto baseline = fork_working_context(parent, seed);
    MIRA_CHECK(baseline.has_value());
    WorkingContextSnapshot child = baseline.value();

    // (a) The non-nil fork field participates: clearing it changes the
    // digest — no silent dropping from the canonical object, and no digest
    // branching on the schema version either (the formula stays the M21
    // frozen full-inclusion one).
    const Hash with_fork = child.state_digest();
    child.fork.reset();
    const Hash without_fork = child.state_digest();
    MIRA_CHECK(!(with_fork == without_fork));

    // (b) v1.0/v1.1 payload read-back: a fork=nil snapshot serialized today,
    // its schema stamp rewritten to the pre-upgrade versions, reads back
    // with the original schema version preserved and a digest equal to the
    // same in-memory payload computed under that stamp — proving the read
    // path keeps the payload's version (no silent upgrade) and the digest
    // has no per-version branching beyond the stamp itself.
    WorkingContextSnapshot legacy = m24_snapshot(123);
    legacy.constraints.push_back(m24_item("constraint: legacy payload", 331, 0.9));
    legacy.fork.reset();
    const JsonValue payload = working_context_to_json(legacy);
    for (const SchemaVersion legacy_version : {SchemaVersion{1, 0}, SchemaVersion{1, 1}}) {
        JsonValue rewritten = payload;
        JsonValue *schema = rewritten.find("schema_version");
        MIRA_CHECK(schema != nullptr);
        JsonValue *major = schema->find("major");
        JsonValue *minor = schema->find("minor");
        MIRA_CHECK(major != nullptr && minor != nullptr);
        *major = JsonValue(static_cast<std::int64_t>(legacy_version.major));
        *minor = JsonValue(static_cast<std::int64_t>(legacy_version.minor));
        const auto read_back = working_context_from_json(rewritten);
        MIRA_CHECK(read_back.has_value());
        MIRA_CHECK(read_back.value().schema_version == legacy_version);
        WorkingContextSnapshot in_memory = legacy;
        in_memory.schema_version = legacy_version;
        MIRA_CHECK(read_back.value().state_digest() == in_memory.state_digest());
    }

    // (c) A fork baseline round-trips through JSON with its provenance
    // intact and the fork field still non-nil on read-back.
    const JsonValue forked_payload = working_context_to_json(baseline.value());
    const auto forked_read_back = working_context_from_json(forked_payload);
    MIRA_CHECK(forked_read_back.has_value());
    MIRA_CHECK(forked_read_back.value().fork.has_value());
    MIRA_CHECK(forked_read_back.value().fork.value().base_snapshot_id == parent.id);
    MIRA_CHECK(forked_read_back.value().fork.value().parent_session_id == parent.session_id);
    MIRA_CHECK(forked_read_back.value().fork.value().parent_through_event_sequence ==
               parent.through_event_sequence);
    MIRA_CHECK(forked_read_back.value().state_digest() == baseline.value().state_digest());
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G1: fork is a pure function — the same (parent, seed) pair always
// derives the same id, digest and bytes
// ---------------------------------------------------------------------------

int fork_is_pure_same_input_same_output() {
    WorkingContextSnapshot parent = m24_snapshot(130);
    m24_fill_all_sections(parent, 340);
    const WorkingContextForkSeed seed = m24_fork_seed(131, 132, 9);

    const auto first = fork_working_context(parent, seed);
    const auto second = fork_working_context(parent, seed);
    MIRA_CHECK(first.has_value() && second.has_value());
    MIRA_CHECK(first.value().id == second.value().id);
    MIRA_CHECK(first.value().state_digest() == second.value().state_digest());
    MIRA_CHECK(normalized_json(first.value()) == normalized_json(second.value()));

    // A different child watermark is a different fork point and must not
    // collide with the original baseline identity.
    WorkingContextForkSeed moved = seed;
    moved.child_watermark = 10;
    const auto other = fork_working_context(parent, moved);
    MIRA_CHECK(other.has_value());
    MIRA_CHECK(other.value().id != first.value().id);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G2: the mechanical delta projection classifies inherited / supersede /
// addition per the frozen per-entry order
// ---------------------------------------------------------------------------

int delta_projection_three_way_classification() {
    WorkingContextSnapshot parent = m24_snapshot(140);
    parent.constraints.push_back(m24_item("constraint: volume=30", 401, 0.9));
    parent.decisions.push_back(m24_item("decision: batch provider", 402, 0.8));
    const WorkingContextForkSeed seed = m24_fork_seed(141, 142, 4);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());
    const WorkingContextSnapshot &baseline = forked.value();

    // The child curated its own chain on top of the baseline: one entry is a
    // verbatim baseline copy (inherited), one re-states the baseline content
    // carrying the baseline event in its provenance (supersede), one is
    // child-fresh (addition); the decision section supersedes too.
    WorkingContextSnapshot child = baseline; // the child chain previous
    child.id = working_context_snapshot_id_from_seed("m24-child-curated-140");
    child.generated_by = m24_profile_from_seed(143);
    child.constraints.clear();
    child.constraints.push_back(m24_item("constraint: volume=30", 401, 0.9)); // inherited copy
    WorkingContextItem supersede = m24_item("constraint: volume=60", 404, 0.85);
    supersede.source_events.push_back(m24_event_from_seed(401)); // baseline lineage evidence
    child.constraints.push_back(supersede);
    child.constraints.push_back(m24_item("constraint: child fresh rule", 405, 0.75));
    child.decisions.clear();
    WorkingContextItem decision_update = m24_item("decision: streaming provider", 406, 0.8);
    decision_update.source_events.push_back(m24_event_from_seed(402));
    child.decisions.push_back(decision_update);

    const auto delta = working_context_delta_from_fork(baseline, child);
    MIRA_CHECK(delta.has_value());
    const WorkingContextDelta &report = delta.value();
    MIRA_CHECK(report.validate().has_value());
    MIRA_CHECK((report.schema_version == SchemaVersion{1, 0}));
    MIRA_CHECK(report.fork_base_snapshot_id == baseline.id);
    MIRA_CHECK(report.child_session_id == child.session_id);
    MIRA_CHECK(report.generated_by == child.generated_by); // the child-side profile (DEC-036)
    MIRA_CHECK(report.inherited_skipped == 1);

    // Fixed emission order: section declaration order, vector order inside.
    MIRA_CHECK(report.entries.size() == 3);
    MIRA_CHECK(report.entries[0].kind == WorkingContextDeltaEntryKind::Supersede);
    MIRA_CHECK(report.entries[0].section == "constraints");
    MIRA_CHECK(report.entries[0].content == "constraint: volume=60");
    MIRA_CHECK(report.entries[0].superseded_base_index == 0); // baseline constraints[0]
    MIRA_CHECK(report.entries[1].kind == WorkingContextDeltaEntryKind::Addition);
    MIRA_CHECK(report.entries[1].section == "constraints");
    MIRA_CHECK(report.entries[1].content == "constraint: child fresh rule");
    MIRA_CHECK(report.entries[2].kind == WorkingContextDeltaEntryKind::Supersede);
    MIRA_CHECK(report.entries[2].section == "decisions");
    MIRA_CHECK(report.entries[2].content == "decision: streaming provider");
    MIRA_CHECK(report.entries[2].superseded_base_index == 1); // baseline decisions[0]
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G2: baseline canonical numbering — declaration order times vector order
// ---------------------------------------------------------------------------

int delta_baseline_numbering_declaration_order() {
    WorkingContextSnapshot parent = m24_snapshot(150);
    parent.constraints.push_back(m24_item("constraint: a0", 501, 0.9));
    parent.constraints.push_back(m24_item("constraint: a1", 502, 0.9));
    parent.decisions.push_back(m24_item("decision: b0", 503, 0.9));
    parent.verified_facts.push_back(m24_item("fact: v0", 504, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(151, 152, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());
    const WorkingContextSnapshot &baseline = forked.value();

    // The child restates one entry per section with fresh wording but the
    // baseline entry's source event — each classifies as a Supersede of the
    // baseline entry its lineage points at, exposing the canonical numbers:
    // constraints occupy 0..1, decisions 2, verified_facts 3.
    WorkingContextSnapshot child = baseline;
    child.id = working_context_snapshot_id_from_seed("m24-child-curated-150");
    const auto restated = [](const WorkingContextItem &origin, std::uint64_t event_seed,
                             const char *suffix) {
        WorkingContextItem item = origin;
        item.content += suffix;
        item.source_events.push_back(m24_event_from_seed(event_seed)); // child-side event too
        return item;
    };
    child.constraints[1] = restated(baseline.constraints[1], 511, " (updated)");
    child.decisions[0] = restated(baseline.decisions[0], 512, " (updated)");
    child.verified_facts[0] = restated(baseline.verified_facts[0], 513, " (updated)");

    const auto delta = working_context_delta_from_fork(baseline, child);
    MIRA_CHECK(delta.has_value());
    MIRA_CHECK(delta.value().inherited_skipped == 1); // constraints[0] copied verbatim
    MIRA_CHECK(delta.value().entries.size() == 3);
    const WorkingContextDeltaEntry &first = delta.value().entries[0];
    const WorkingContextDeltaEntry &second = delta.value().entries[1];
    const WorkingContextDeltaEntry &third = delta.value().entries[2];
    MIRA_CHECK(first.kind == WorkingContextDeltaEntryKind::Supersede);
    MIRA_CHECK(first.section == "constraints");
    MIRA_CHECK(first.content == "constraint: a1 (updated)");
    MIRA_CHECK(first.superseded_base_index == 1);
    MIRA_CHECK(second.kind == WorkingContextDeltaEntryKind::Supersede);
    MIRA_CHECK(second.section == "decisions");
    MIRA_CHECK(second.content == "decision: b0 (updated)");
    MIRA_CHECK(second.superseded_base_index == 2);
    MIRA_CHECK(third.kind == WorkingContextDeltaEntryKind::Supersede);
    MIRA_CHECK(third.section == "verified_facts");
    MIRA_CHECK(third.content == "fact: v0 (updated)");
    MIRA_CHECK(third.superseded_base_index == 3);

    // The frozen vocabulary is exactly the eight snapshot section names, in
    // declaration order — the numbering derives from it.
    MIRA_CHECK(m24_section_names().size() == 8);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G2: the delta validate() discipline — frozen vocabulary, per-entry
// provenance, content bounds, required fork-base id, all-or-nothing
// ---------------------------------------------------------------------------

int delta_validation_discipline() {
    WorkingContextSnapshot parent = m24_snapshot(160);
    parent.constraints.push_back(m24_item("constraint: volume=30", 601, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(161, 162, 3);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    // A valid hand-built delta anchors the discipline checks.
    WorkingContextDelta valid;
    valid.schema_version = SchemaVersion{1, 0};
    valid.fork_base_snapshot_id = forked.value().id;
    valid.child_session_id = seed.child_session;
    WorkingContextDeltaEntry entry;
    entry.kind = WorkingContextDeltaEntryKind::Addition;
    entry.section = "constraints";
    entry.content = "constraint: fresh from child";
    entry.source_events = {m24_event_from_seed(603)};
    entry.source_sequence = 603;
    entry.confidence = 0.8;
    valid.entries.push_back(entry);
    MIRA_CHECK(valid.validate().has_value());

    const auto reject_mutant = [](WorkingContextDelta &mutant) {
        const auto verdict = mutant.validate();
        MIRA_CHECK(!verdict.has_value());
        MIRA_CHECK(verdict.error().code == ErrorCode::InvalidArgument);
        return 0;
    };

    // Frozen eight-section vocabulary only.
    WorkingContextDelta unknown_section = valid;
    unknown_section.entries.front().section = "hunches";
    reject_mutant(unknown_section);

    // Per-entry provenance: every entry carries at least one source event.
    WorkingContextDelta no_events = valid;
    no_events.entries.front().source_events.clear();
    reject_mutant(no_events);

    // Content bounds: empty and over-long contents reject.
    WorkingContextDelta empty_content = valid;
    empty_content.entries.front().content = "";
    reject_mutant(empty_content);
    WorkingContextDelta long_content = valid;
    const WorkingContextMergeOptions bounds;
    long_content.entries.front().content = std::string(bounds.max_item_chars + 1, 'x');
    reject_mutant(long_content);

    // The fork-base snapshot id is required.
    WorkingContextDelta nil_base = valid;
    nil_base.fork_base_snapshot_id = WorkingContextSnapshotId{};
    reject_mutant(nil_base);

    // Per-section entry bounds (RULE-08) reject.
    WorkingContextDelta too_many = valid;
    for (std::size_t index = 0; index < bounds.max_items_per_section + 1; ++index) {
        WorkingContextDeltaEntry extra = entry;
        extra.content = "constraint: overflow " + std::to_string(index);
        too_many.entries.push_back(extra);
    }
    reject_mutant(too_many);

    // Projection all-or-nothing: an invalid child snapshot rejects whole.
    WorkingContextSnapshot broken_child = forked.value();
    broken_child.constraints.push_back(m24_item("", 604, 0.9));
    MIRA_CHECK(!broken_child.validate().has_value());
    const auto rejected = working_context_delta_from_fork(forked.value(), broken_child);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G2: the delta schema v1 JSON round trip preserves every field and
// enforces the frozen vocabulary on read-back
// ---------------------------------------------------------------------------

int delta_json_round_trip() {
    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = working_context_snapshot_id_from_seed("m24-delta-roundtrip-base");
    delta.child_session_id = m24_session_from_seed(170);
    delta.generated_by = m24_profile_from_seed(171);
    delta.inherited_skipped = 2;

    WorkingContextDeltaEntry supersede;
    supersede.kind = WorkingContextDeltaEntryKind::Supersede;
    supersede.section = "verified_facts";
    supersede.content = "fact: staging accepts the v3 payload";
    supersede.superseded_base_index = 5;
    supersede.source_events = {m24_event_from_seed(701), m24_event_from_seed(702)};
    supersede.source_sequence = 702;
    supersede.confidence = 0.82;
    delta.entries.push_back(supersede);

    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "next_actions";
    addition.content = "action: schedule the deploy window";
    addition.source_events = {m24_event_from_seed(703)};
    addition.source_sequence = 703;
    addition.confidence = 0.7;
    delta.entries.push_back(addition);
    MIRA_CHECK(delta.validate().has_value());

    const JsonValue payload = working_context_delta_to_json(delta);
    const auto read_back = working_context_delta_from_json(payload);
    MIRA_CHECK(read_back.has_value());
    MIRA_CHECK(read_back.value().schema_version == delta.schema_version);
    MIRA_CHECK(read_back.value().fork_base_snapshot_id == delta.fork_base_snapshot_id);
    MIRA_CHECK(read_back.value().child_session_id == delta.child_session_id);
    MIRA_CHECK(read_back.value().generated_by == delta.generated_by);
    MIRA_CHECK(read_back.value().inherited_skipped == delta.inherited_skipped);
    MIRA_CHECK(read_back.value().entries.size() == delta.entries.size());
    for (std::size_t index = 0; index < delta.entries.size(); ++index) {
        const WorkingContextDeltaEntry &left = delta.entries[index];
        const WorkingContextDeltaEntry &right = read_back.value().entries[index];
        MIRA_CHECK(left.kind == right.kind);
        MIRA_CHECK(left.section == right.section);
        MIRA_CHECK(left.content == right.content);
        MIRA_CHECK(left.superseded_base_index == right.superseded_base_index);
        MIRA_CHECK(left.source_events == right.source_events);
        MIRA_CHECK(left.source_sequence == right.source_sequence);
        MIRA_CHECK(left.confidence == right.confidence);
    }
    MIRA_CHECK(read_back.value().validate().has_value());

    // The delta vocabulary is frozen: an unknown section in a payload
    // rejects on read-back too.
    const std::string serialized = to_json_string(payload);
    const std::size_t slot = serialized.find("\"section\":\"verified_facts\"");
    MIRA_CHECK(slot != std::string::npos);
    std::string tampered = serialized;
    constexpr std::string_view kPoisonedProbe = "\"section\":\"verified_facts\"";
    tampered.replace(slot, kPoisonedProbe.size(), "\"section\":\"hunches\"");
    const auto parsed = parse_json(tampered);
    MIRA_CHECK(parsed.has_value());
    const auto rejected = working_context_delta_from_json(parsed.value());
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G3: in-place supersede resolves through the fork-base reference and
// keeps the parent position; a stale supersede reclassifies as a counted
// addition appended in delta order
// ---------------------------------------------------------------------------

int merge_supersede_in_place_and_stale_reclassified() {
    // Fork base: the baseline carries four constraints (indices 0..3). The
    // parent advanced past the fork point and dropped index 3 ("X").
    WorkingContextSnapshot fork_point = m24_snapshot(180);
    fork_point.constraints.push_back(m24_item("constraint: A", 801, 0.9));
    fork_point.constraints.push_back(m24_item("constraint: B", 802, 0.9));
    fork_point.constraints.push_back(m24_item("constraint: C", 803, 0.9));
    fork_point.constraints.push_back(m24_item("constraint: X", 804, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(181, 182, 2);
    const auto forked = fork_working_context(fork_point, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextSnapshot parent = fork_point; // advanced chain tip
    parent.id = working_context_snapshot_id_from_seed("m24-parent-advanced-180");
    parent.through_event_sequence = 50; // past the fork point 48
    parent.constraints.pop_back();      // the parent's curation removed X

    // Delta: supersede B (still in the parent — resolves in place) and
    // supersede X (gone from the parent — stale), plus one plain addition.
    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    delta.inherited_skipped = 0;

    WorkingContextDeltaEntry supersede_b;
    supersede_b.kind = WorkingContextDeltaEntryKind::Supersede;
    supersede_b.section = "constraints";
    supersede_b.content = "constraint: B v2";
    supersede_b.superseded_base_index = 1;
    supersede_b.source_events = {m24_event_from_seed(802), m24_event_from_seed(810)};
    supersede_b.source_sequence = 810;
    supersede_b.confidence = 0.85;
    delta.entries.push_back(supersede_b);

    WorkingContextDeltaEntry stale_x;
    stale_x.kind = WorkingContextDeltaEntryKind::Supersede;
    stale_x.section = "constraints";
    stale_x.content = "constraint: X v2";
    stale_x.superseded_base_index = 3;
    stale_x.source_events = {m24_event_from_seed(804), m24_event_from_seed(811)};
    stale_x.source_sequence = 811;
    stale_x.confidence = 0.85;
    delta.entries.push_back(stale_x);

    WorkingContextDeltaEntry addition_d;
    addition_d.kind = WorkingContextDeltaEntryKind::Addition;
    addition_d.section = "constraints";
    addition_d.content = "constraint: D";
    addition_d.source_events = {m24_event_from_seed(812)};
    addition_d.source_sequence = 812;
    addition_d.confidence = 0.8;
    delta.entries.push_back(addition_d);
    MIRA_CHECK(delta.validate().has_value());

    const auto merged =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(merged.has_value());
    const WorkingContextMergeReport &report = merged.value();
    MIRA_CHECK(report.merged.validate().has_value());

    // In-place replacement keeps the parent position and order; the stale
    // supersede and the addition append in delta order at the tail.
    MIRA_CHECK(section_contents(report.merged.constraints) ==
               "constraint: A|constraint: B v2|constraint: C|constraint: X v2|constraint: D");
    MIRA_CHECK(report.supersedes_resolved == 1);
    MIRA_CHECK(report.supersedes_dropped_stale == 1);
    MIRA_CHECK(report.additions_appended == 2); // the reclassified stale + the addition
    MIRA_CHECK(report.truncated_entries == 0);

    // Candidate identity: the parent five-tuple at the advanced watermark,
    // fork field nil — an ordinary snapshot candidate for §5.2.
    MIRA_CHECK(report.merged.session_id == parent.session_id);
    MIRA_CHECK(report.merged.task_id == parent.task_id);
    MIRA_CHECK(report.merged.task_epoch == parent.task_epoch);
    MIRA_CHECK(report.merged.environment_epoch == parent.environment_epoch);
    MIRA_CHECK(report.merged.through_event_sequence == parent.through_event_sequence + 1);
    MIRA_CHECK(!report.merged.fork.has_value());
    MIRA_CHECK(report.merged.source_checkpoints == parent.source_checkpoints);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G3: additions append in delta order; per-section truncation keeps the
// parent entries and in-place supersedes first and counts the overflow
// ---------------------------------------------------------------------------

int merge_additions_append_and_truncate_fixed_order() {
    WorkingContextSnapshot parent = m24_snapshot(190);
    parent.constraints.push_back(m24_item("constraint: P0", 901, 0.9));
    parent.constraints.push_back(m24_item("constraint: P1", 902, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(191, 192, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    for (const char *content : {"constraint: C0", "constraint: C1", "constraint: C2"}) {
        WorkingContextDeltaEntry addition;
        addition.kind = WorkingContextDeltaEntryKind::Addition;
        addition.section = "constraints";
        addition.content = content;
        addition.source_events = {m24_event_from_seed(903)};
        addition.source_sequence = 903;
        addition.confidence = 0.8;
        delta.entries.push_back(addition);
    }
    MIRA_CHECK(delta.validate().has_value());

    // Uncapped: all three additions append in delta order.
    const auto uncapped =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(uncapped.has_value());
    MIRA_CHECK(section_contents(uncapped.value().merged.constraints) ==
               "constraint: P0|constraint: P1|constraint: C0|constraint: C1|constraint: C2");
    MIRA_CHECK(uncapped.value().additions_appended == 3);
    MIRA_CHECK(uncapped.value().truncated_entries == 0);

    // Cap of three: the parent entries keep their slots, additions fill the
    // remainder in delta order and the overflow is counted (M23 fixed-order
    // precedent).
    WorkingContextMergeOptions capped;
    capped.max_items_per_section = 3;
    const auto truncated =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1, capped);
    MIRA_CHECK(truncated.has_value());
    MIRA_CHECK(section_contents(truncated.value().merged.constraints) ==
               "constraint: P0|constraint: P1|constraint: C0");
    MIRA_CHECK(truncated.value().additions_appended == 1);
    MIRA_CHECK(truncated.value().truncated_entries == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G3: the merged candidate inherits generated_by from the parent — both
// the deterministic (nil) and the curated (non-nil) parent forms — and the
// three precondition rejections hold with their frozen tokens
// ---------------------------------------------------------------------------

int merge_candidate_fields_and_preconditions() {
    WorkingContextSnapshot parent = m24_snapshot(200);
    parent.constraints.push_back(m24_item("constraint: A", 1001, 0.9));
    const WorkingContextForkSeed seed = m24_fork_seed(201, 202, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: from child";
    addition.source_events = {m24_event_from_seed(1002)};
    addition.source_sequence = 1002;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    // (a) Deterministic parent: the nil generated_by is inherited.
    const auto from_deterministic =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(from_deterministic.has_value());
    MIRA_CHECK(from_deterministic.value().merged.generated_by.is_nil());

    // (b) Curated parent: the non-nil profile carries over (§4.4 field
    // decision; keeps the zero-effect IdempotentNoOp reachable), while the
    // child-side model attribution stays on the delta itself.
    WorkingContextSnapshot curated_parent = parent;
    curated_parent.generated_by = m24_profile_from_seed(203);
    delta.generated_by = m24_profile_from_seed(204);
    const auto from_curated = merge_working_context_delta(
        forked.value(), curated_parent, delta, m24_parent_identity(curated_parent),
        curated_parent.through_event_sequence + 1);
    MIRA_CHECK(from_curated.has_value());
    MIRA_CHECK(from_curated.value().merged.generated_by == curated_parent.generated_by);

    // Precondition 1: the delta anchors a different fork base.
    WorkingContextDelta mismatched = delta;
    mismatched.fork_base_snapshot_id =
        working_context_snapshot_id_from_seed("m24-not-the-fork-base");
    const auto rejected_base =
        merge_working_context_delta(forked.value(), parent, mismatched, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(!rejected_base.has_value());
    MIRA_CHECK(rejected_base.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(has_token(rejected_base.error(), "fork-base-mismatch"));

    // Precondition 2: the same session may not merge into itself (DEC-044
    // negative freeze).
    WorkingContextSnapshot same_session_parent = parent;
    same_session_parent.session_id = seed.child_session;
    same_session_parent.task_id = seed.child_identity.task;
    const auto rejected_same_session = merge_working_context_delta(
        forked.value(), same_session_parent, delta, m24_parent_identity(same_session_parent),
        same_session_parent.through_event_sequence);
    MIRA_CHECK(!rejected_same_session.has_value());
    MIRA_CHECK(rejected_same_session.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(has_token(rejected_same_session.error(), "same-session-fork"));

    // Precondition 3: the supplied parent identity must match the parent
    // snapshot's own stamping.
    WorkingContextIdentity wrong_identity = m24_parent_identity(parent);
    wrong_identity.task_epoch = parent.task_epoch + 1;
    const auto rejected_identity = merge_working_context_delta(
        forked.value(), parent, delta, wrong_identity, parent.through_event_sequence + 1);
    MIRA_CHECK(!rejected_identity.has_value());
    MIRA_CHECK(rejected_identity.error().code == ErrorCode::InvalidArgument);

    // Precondition 4: an invalid delta rejects whole as well.
    WorkingContextDelta invalid = delta;
    invalid.entries.front().source_events.clear();
    const auto rejected_delta =
        merge_working_context_delta(forked.value(), parent, invalid, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(!rejected_delta.has_value());
    MIRA_CHECK(rejected_delta.error().code == ErrorCode::InvalidArgument);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G3: a zero-effect delta produces a candidate digest-identical to the
// parent — for both parent forms — so the §5.2 same-watermark commit rules
// it IdempotentNoOp
// ---------------------------------------------------------------------------

int merge_zero_effect_delta_is_field_identical() {
    const WorkingContextForkSeed seed = m24_fork_seed(211, 212, 2);
    for (const bool curated : {false, true}) {
        WorkingContextSnapshot parent = m24_snapshot(curated ? 213 : 210);
        parent.constraints.push_back(m24_item("constraint: unchanged", 1101, 0.9));
        parent.decisions.push_back(m24_item("decision: unchanged too", 1102, 0.85));
        if (curated) {
            parent.generated_by = m24_profile_from_seed(214);
        }
        const auto forked = fork_working_context(parent, seed);
        MIRA_CHECK(forked.has_value());

        // The child copied the parent verbatim: every entry is inherited.
        WorkingContextSnapshot child = forked.value();
        child.id = working_context_snapshot_id_from_seed("m24-child-quiet");
        child.generated_by = m24_profile_from_seed(215);
        const auto delta = working_context_delta_from_fork(forked.value(), child);
        MIRA_CHECK(delta.has_value());
        MIRA_CHECK(delta.value().entries.empty());
        MIRA_CHECK(delta.value().inherited_skipped == 2);

        const auto merged = merge_working_context_delta(
            forked.value(), parent, delta.value(), m24_parent_identity(parent),
            parent.through_event_sequence); // same watermark, zero effect
        MIRA_CHECK(merged.has_value());
        // Field-identical over every digest-covered field, including the
        // inherited generated_by.
        MIRA_CHECK(merged.value().merged.state_digest() == parent.state_digest());
        MIRA_CHECK(merged.value().additions_appended == 0);
        MIRA_CHECK(merged.value().supersedes_resolved == 0);
        MIRA_CHECK(merged.value().supersedes_dropped_stale == 0);
        MIRA_CHECK(merged.value().truncated_entries == 0);

        // The commit rules the zero-effect candidate at the same watermark
        // an idempotent no-op against the stored parent.
        InMemoryWorkingContextStore store;
        const auto base = commit_working_context(store, parent, m24_live(parent));
        MIRA_CHECK(base.disposition == WorkingContextCommitDisposition::Committed);
        const auto noop = commit_working_context(store, merged.value().merged, m24_live(parent));
        MIRA_CHECK(noop.disposition == WorkingContextCommitDisposition::IdempotentNoOp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G3: the same input group always produces byte-identical candidates and
// report counts
// ---------------------------------------------------------------------------

int merge_is_byte_deterministic() {
    WorkingContextSnapshot parent = m24_snapshot(220);
    parent.constraints.push_back(m24_item("constraint: volume=30", 1201, 0.9));
    parent.verified_facts.push_back(m24_item("fact: staging accepts the v2 payload", 1202, 0.8));
    const WorkingContextForkSeed seed = m24_fork_seed(221, 222, 2);
    const auto forked = fork_working_context(parent, seed);
    MIRA_CHECK(forked.has_value());

    WorkingContextDelta delta;
    delta.schema_version = SchemaVersion{1, 0};
    delta.fork_base_snapshot_id = forked.value().id;
    delta.child_session_id = seed.child_session;
    delta.inherited_skipped = 1;
    WorkingContextDeltaEntry supersede;
    supersede.kind = WorkingContextDeltaEntryKind::Supersede;
    supersede.section = "verified_facts";
    supersede.content = "fact: staging accepts the v3 payload";
    supersede.superseded_base_index = 0;
    supersede.source_events = {m24_event_from_seed(1202), m24_event_from_seed(1203)};
    supersede.source_sequence = 1203;
    supersede.confidence = 0.83;
    delta.entries.push_back(supersede);
    WorkingContextDeltaEntry addition;
    addition.kind = WorkingContextDeltaEntryKind::Addition;
    addition.section = "constraints";
    addition.content = "constraint: child fresh";
    addition.source_events = {m24_event_from_seed(1204)};
    addition.source_sequence = 1204;
    addition.confidence = 0.8;
    delta.entries.push_back(addition);

    const auto first =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    const auto second =
        merge_working_context_delta(forked.value(), parent, delta, m24_parent_identity(parent),
                                    parent.through_event_sequence + 1);
    MIRA_CHECK(first.has_value() && second.has_value());
    MIRA_CHECK(snapshot_json(first.value().merged) == snapshot_json(second.value().merged));
    MIRA_CHECK(first.value().additions_appended == second.value().additions_appended);
    MIRA_CHECK(first.value().supersedes_resolved == second.value().supersedes_resolved);
    MIRA_CHECK(first.value().supersedes_dropped_stale == second.value().supersedes_dropped_stale);
    MIRA_CHECK(first.value().truncated_entries == second.value().truncated_entries);
    return 0;
}

// ---------------------------------------------------------------------------
// W5-G4: the merge candidate commits on a strictly advancing parent
// watermark; the same watermark with a different digest fails closed and
// leaves the store untouched; zero effect at the same watermark is a no-op
// ---------------------------------------------------------------------------

} // namespace

// The G4-G6 integration & lifecycle cases live in
// m24_fork_merge_lifecycle.cpp (max-file-lines split); the case table below
// drives them unchanged.
int merge_commit_watermark_discipline();
int merge_commit_stale_and_terminal_discards();
int parent_child_chains_isolated();
int fork_then_parent_epoch_change_rejects_stale_merge();
int supervisor_routes_merge_commit_and_shuts_down();
int erase_child_session_preserves_parent_and_merge();
int w3_auto_curator_chains_independent();
int w4_promotion_maps_merged_parent_only();
int recovery_rebuilds_fork_baseline_idempotently();

int main() {
    const struct {
        const char *name;
        int (*run)();
    } cases[] = {
        {"fork_copies_all_eight_sections_verbatim", fork_copies_all_eight_sections_verbatim},
        {"fork_provenance_and_negative_inputs", fork_provenance_and_negative_inputs},
        {"fork_schema_12_digest_discipline", fork_schema_12_digest_discipline},
        {"fork_is_pure_same_input_same_output", fork_is_pure_same_input_same_output},
        {"delta_projection_three_way_classification", delta_projection_three_way_classification},
        {"delta_baseline_numbering_declaration_order", delta_baseline_numbering_declaration_order},
        {"delta_validation_discipline", delta_validation_discipline},
        {"delta_json_round_trip", delta_json_round_trip},
        {"merge_supersede_in_place_and_stale_reclassified",
         merge_supersede_in_place_and_stale_reclassified},
        {"merge_additions_append_and_truncate_fixed_order",
         merge_additions_append_and_truncate_fixed_order},
        {"merge_candidate_fields_and_preconditions", merge_candidate_fields_and_preconditions},
        {"merge_zero_effect_delta_is_field_identical", merge_zero_effect_delta_is_field_identical},
        {"merge_is_byte_deterministic", merge_is_byte_deterministic},
        {"merge_commit_watermark_discipline", merge_commit_watermark_discipline},
        {"merge_commit_stale_and_terminal_discards", merge_commit_stale_and_terminal_discards},
        {"parent_child_chains_isolated", parent_child_chains_isolated},
        {"fork_then_parent_epoch_change_rejects_stale_merge",
         fork_then_parent_epoch_change_rejects_stale_merge},
        {"supervisor_routes_merge_commit_and_shuts_down",
         supervisor_routes_merge_commit_and_shuts_down},
        {"erase_child_session_preserves_parent_and_merge",
         erase_child_session_preserves_parent_and_merge},
        {"w3_auto_curator_chains_independent", w3_auto_curator_chains_independent},
        {"w4_promotion_maps_merged_parent_only", w4_promotion_maps_merged_parent_only},
        {"recovery_rebuilds_fork_baseline_idempotently",
         recovery_rebuilds_fork_baseline_idempotently},
    };
    constexpr std::size_t case_count = sizeof(cases) / sizeof(cases[0]);
    std::size_t failed = 0;
    for (const auto &item : cases) {
        const int result = item.run();
        if (result != 0) {
            ++failed;
            std::cerr << "FAIL " << item.name << '\n';
        } else {
            std::cout << "PASS " << item.name << '\n';
        }
    }
    if (failed == 0) {
        std::cout << "m24 fork/merge: OK\n";
        return 0;
    }
    std::cerr << "m24 fork/merge: " << failed << "/" << case_count << " cases failed\n";
    return 1;
}
