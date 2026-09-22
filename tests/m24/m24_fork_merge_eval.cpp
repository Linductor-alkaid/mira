// M24 (DEC-035 Stage W5) fork/merge deterministic eval harness. Builds the
// frozen synthetic fork/merge chain dataset (one parent session with a
// committed snapshot chain, one child fork with its own curated rounds), then
// drives the full mechanical pipeline through the frozen scenario rounds —
// fork baseline, delta projection and three-way classification, merge with
// supersede/stale/addition mixes, §5.2 commit rounds, zero-effect idempotent
// replay, same-watermark conflict, recovery re-fork — asserting the W5-G6
// gate from milestone §6 and printing a JSON report.
//
// Metrics measure deterministic pipeline behaviour, not semantic quality or
// token savings (RULE-10): W5 has no model and makes no quality claims.
// The report embeds only deterministic fields (no wall-clock), so two runs —
// in-process here, cross-process on the gate runners — must be byte
// identical. The dataset digest is computed, not assumed; once the first
// frozen green run pins it (M20/M21 precedent), the harness asserts equality
// against the pin.

#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (M24 §4, frozen before the first evaluation run)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5232'3457'3546ULL; // "MIR24W5F"
    std::uint64_t parent_watermark = 48;
    std::uint64_t child_watermark = 4;
    std::size_t baseline_items_per_section = 3; // fork-base canonical numbering 0..23
    std::size_t child_inherited = 6;            // verbatim baseline copies -> skipped
    std::size_t child_supersedes = 4;           // lineage-bound restatements
    std::size_t child_additions = 5;            // child-fresh entries
    std::uint64_t task_epoch = 3;
    std::uint64_t environment_epoch = 7;
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    std::ostringstream stream;
    for (const auto byte : digest.bytes) {
        stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
    }
    text = stream.str();
    return text;
}

struct SplitMix64 final {
    std::uint64_t state = 0;
    [[nodiscard]] std::uint64_t next() {
        state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return mixed ^ (mixed >> 31U);
    }
};

// ---------------------------------------------------------------------------
// Deterministic dataset builders (m24 seed spaces, all pure)
// ---------------------------------------------------------------------------

[[nodiscard]] Id128 id_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

struct DatasetItem final {
    std::size_t section; // 0..7, declaration order
    std::string content;
    std::uint64_t event_seed;
    std::uint64_t lineage_event_seed = 0; // set on restated rows (supersede evidence)
    double confidence;
};

struct Dataset final {
    WorkingContextSnapshot parent;
    std::vector<DatasetItem> baseline_items; // ground truth for the fork base
    std::vector<DatasetItem> child_items;    // ground truth for the child rounds
    std::string digest_hex_text;
};

[[nodiscard]] std::vector<WorkingContextItem> items_of(const std::vector<DatasetItem> &items,
                                                       std::size_t section) {
    std::vector<WorkingContextItem> converted;
    for (const DatasetItem &item : items) {
        if (item.section != section) {
            continue;
        }
        WorkingContextItem converted_item;
        converted_item.content = item.content;
        converted_item.source_events = {EventId{id_from_seed(item.event_seed)}};
        if (item.lineage_event_seed != 0) {
            // Restated row: the baseline entry's event rides along as the
            // lineage evidence the supersede classification keys on (W2
            // numbered-transcript provenance binding).
            converted_item.source_events.push_back(EventId{id_from_seed(item.lineage_event_seed)});
        }
        converted_item.source_sequence = item.event_seed;
        converted_item.confidence = item.confidence;
        converted.push_back(converted_item);
    }
    return converted;
}

[[nodiscard]] const char *section_name(std::size_t index) {
    static const char *names[] = {"constraints",    "decisions",      "open_issues",
                                  "active_tasks",   "verified_facts", "failed_attempts",
                                  "important_refs", "next_actions"};
    return names[index];
}

// Builds the frozen dataset: a parent snapshot with a full baseline, the
// child rounds (inherited copies, lineage-bound supersedes, fresh additions)
// and the dataset digest over the sorted canonical content rows.
[[nodiscard]] Dataset build_dataset(const FrozenConfig &config) {
    SplitMix64 rng{config.seed};

    Dataset dataset;
    WorkingContextSnapshot &parent = dataset.parent;
    parent.id = working_context_snapshot_id_from_seed("m24-eval-parent");
    parent.session_id = SessionId{id_from_seed(0xE1A1)};
    parent.task_id = TaskId{id_from_seed(0xE1A2)};
    parent.task_epoch = config.task_epoch;
    parent.environment_epoch = config.environment_epoch;
    parent.through_event_sequence = config.parent_watermark;
    parent.source_checkpoints = {
        conversation_checkpoint_id_from_seed("m24-eval-parent-checkpoint")};

    const auto emit = [&rng](std::size_t section, std::uint64_t index) {
        DatasetItem item;
        item.section = section;
        item.content = std::string(section_name(section)) + " baseline entry " +
                       std::to_string(index) + " token" + std::to_string(rng.next() % 1000);
        item.event_seed = 1'000 + section * 100 + index;
        item.confidence = 0.8;
        return item;
    };
    for (std::size_t section = 0; section < 8; ++section) {
        for (std::size_t index = 0; index < config.baseline_items_per_section; ++index) {
            dataset.baseline_items.push_back(emit(section, index));
        }
    }

    // Child rounds: the first `child_inherited` baseline rows are copied
    // verbatim; the next `child_supersedes` restate earlier rows with fresh
    // wording plus the original lineage event; the rest are child-fresh.
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < config.child_inherited; ++index) {
        dataset.child_items.push_back(dataset.baseline_items[cursor++]);
    }
    for (std::size_t index = 0; index < config.child_supersedes; ++index) {
        DatasetItem restated = dataset.baseline_items[cursor++];
        restated.content += " superseded by the child";
        restated.lineage_event_seed = dataset.baseline_items[cursor - 1].event_seed;
        restated.event_seed = 2'000 + index; // the child-side event rides on top
        dataset.child_items.push_back(restated);
    }
    for (std::size_t index = 0; index < config.child_additions; ++index) {
        DatasetItem fresh;
        fresh.section = index % 8;
        fresh.content = std::string(section_name(fresh.section)) + " child addition " +
                        std::to_string(index) + " token" + std::to_string(rng.next() % 1000);
        fresh.event_seed = 3'000 + index;
        fresh.confidence = 0.75;
        dataset.child_items.push_back(fresh);
    }

    // Materialize the ground truth into the snapshots.
    for (std::size_t section = 0; section < 8; ++section) {
        const auto baseline = items_of(dataset.baseline_items, section);
        switch (section) {
        case 0:
            parent.constraints = baseline;
            break;
        case 1:
            parent.decisions = baseline;
            break;
        case 2:
            parent.open_issues = baseline;
            break;
        case 3:
            parent.active_tasks = baseline;
            break;
        case 4:
            parent.verified_facts = baseline;
            break;
        case 5:
            parent.failed_attempts = baseline;
            break;
        case 6:
            parent.important_refs = baseline;
            break;
        case 7:
            parent.next_actions = baseline;
            break;
        default:
            break;
        }
    }

    // Dataset digest: sorted canonical rows over section|content|event seed —
    // checkpoint-data-level, independent of any snapshot schema evolution
    // (m20 dataset-digest precedent).
    std::vector<std::string> rows;
    for (const DatasetItem &item : dataset.baseline_items) {
        rows.push_back("baseline|" + std::string(section_name(item.section)) + "|" + item.content +
                       "|" + std::to_string(item.event_seed));
    }
    for (const DatasetItem &item : dataset.child_items) {
        rows.push_back("child|" + std::string(section_name(item.section)) + "|" + item.content +
                       "|" + std::to_string(item.event_seed));
    }
    std::sort(rows.begin(), rows.end());
    JsonValue::Object rows_object;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        rows_object.emplace_back("row_" + std::to_string(index), JsonValue(rows[index]));
    }
    dataset.digest_hex_text = digest_hex(canonical_json_digest(JsonValue(rows_object)));
    return dataset;
}

// ---------------------------------------------------------------------------
// The frozen fork/merge chain over the dataset (one full pass)
// ---------------------------------------------------------------------------

struct ChainOutcome final {
    bool fork_ok = false;
    std::string fork_id;
    std::string fork_digest;
    std::size_t inherited_skipped = 0;
    std::size_t supersedes = 0;
    std::size_t additions = 0;
    std::size_t resolved = 0;
    std::size_t stale = 0;
    std::size_t appended = 0;
    std::size_t truncated = 0;
    std::string merged_digest;
    std::string merged_bytes;
    std::string commit_disposition;
    std::string commit_reason;
    std::string noop_disposition;
    std::string conflict_disposition;
    std::string conflict_reason;
    std::string refork_id;
    std::string refork_digest;
};

[[nodiscard]] ChainOutcome run_chain(const Dataset &dataset, const FrozenConfig &config) {
    ChainOutcome outcome;

    // 1. Fork baseline: pure, deterministic, schema 1.2 with provenance.
    WorkingContextForkSeed seed;
    seed.child_session = SessionId{id_from_seed(0xE1B1)};
    seed.child_identity.task = TaskId{id_from_seed(0xE1B2)};
    seed.child_identity.task_epoch = config.task_epoch;
    seed.child_identity.environment_epoch = config.environment_epoch;
    seed.child_watermark = config.child_watermark;
    const auto forked = fork_working_context(dataset.parent, seed);
    if (!forked.has_value()) {
        return outcome;
    }
    outcome.fork_ok = true;
    outcome.fork_id = forked.value().id.to_string();
    outcome.fork_digest = digest_hex(forked.value().state_digest());

    // 2. Child curated tip: the baseline rows materialized per ground truth
    // (the child chain's own committed snapshot — mechanically derived here
    // from the same frozen dataset rows the eval asserts on).
    WorkingContextSnapshot child = forked.value();
    child.id = working_context_snapshot_id_from_seed("m24-eval-child-tip");
    child.generated_by = ModelProfileId{id_from_seed(0xE1B3)};
    for (std::size_t section = 0; section < 8; ++section) {
        const auto rows = items_of(dataset.child_items, section);
        switch (section) {
        case 0:
            child.constraints = rows;
            break;
        case 1:
            child.decisions = rows;
            break;
        case 2:
            child.open_issues = rows;
            break;
        case 3:
            child.active_tasks = rows;
            break;
        case 4:
            child.verified_facts = rows;
            break;
        case 5:
            child.failed_attempts = rows;
            break;
        case 6:
            child.important_refs = rows;
            break;
        case 7:
            child.next_actions = rows;
            break;
        default:
            break;
        }
    }

    // 3. Delta projection: inherited skipped, supersedes with lineage,
    // additions counted.
    const auto delta = working_context_delta_from_fork(forked.value(), child);
    if (!delta.has_value()) {
        outcome.fork_ok = false;
        return outcome;
    }
    outcome.inherited_skipped = delta.value().inherited_skipped;
    for (const WorkingContextDeltaEntry &entry : delta.value().entries) {
        if (entry.kind == WorkingContextDeltaEntryKind::Supersede) {
            ++outcome.supersedes;
        } else {
            ++outcome.additions;
        }
    }

    // 4. Merge onto a parent tip that dropped two of the supersede targets
    // — the stale branch of the frozen policy — and commit at a strictly
    // advancing watermark.
    WorkingContextSnapshot parent_tip = dataset.parent;
    parent_tip.id = working_context_snapshot_id_from_seed("m24-eval-parent-tip");
    parent_tip.open_issues.pop_back(); // drops supersede target 8
    parent_tip.open_issues.pop_back(); // drops supersede target 7
    parent_tip.through_event_sequence = config.parent_watermark + 8;

    const WorkingContextIdentity tip_identity{parent_tip.task_id, parent_tip.task_epoch,
                                              parent_tip.environment_epoch};
    const auto merged =
        merge_working_context_delta(forked.value(), parent_tip, delta.value(), tip_identity,
                                    parent_tip.through_event_sequence + 8);
    if (!merged.has_value()) {
        outcome.fork_ok = false;
        return outcome;
    }
    outcome.resolved = merged.value().supersedes_resolved;
    outcome.stale = merged.value().supersedes_dropped_stale;
    outcome.appended = merged.value().additions_appended;
    outcome.truncated = merged.value().truncated_entries;
    outcome.merged_digest = digest_hex(merged.value().merged.state_digest());

    InMemoryWorkingContextStore store;
    WorkingContextCommitState live;
    live.session = parent_tip.session_id;
    live.task = parent_tip.task_id;
    live.task_epoch = parent_tip.task_epoch;
    live.environment_epoch = parent_tip.environment_epoch;

    // 5. Zero-effect replay at the parent tip's own watermark:
    // IdempotentNoOp (the candidate is field-identical to the stored tip).
    WorkingContextSnapshot quiet_child = forked.value();
    quiet_child.id = working_context_snapshot_id_from_seed("m24-eval-child-quiet");
    const auto quiet_delta = working_context_delta_from_fork(forked.value(), quiet_child);
    if (!quiet_delta.has_value()) {
        outcome.fork_ok = false;
        return outcome;
    }
    const auto quiet_merge =
        merge_working_context_delta(forked.value(), parent_tip, quiet_delta.value(), tip_identity,
                                    parent_tip.through_event_sequence);
    if (!quiet_merge.has_value()) {
        outcome.fork_ok = false;
        return outcome;
    }
    const auto base_commit = commit_working_context(store, parent_tip, live);
    if (base_commit.disposition != WorkingContextCommitDisposition::Committed) {
        outcome.fork_ok = false;
        return outcome;
    }
    const auto noop = commit_working_context(store, quiet_merge.value().merged, live);
    outcome.noop_disposition = working_context_commit_disposition_name(noop.disposition);

    // 6. The merged candidate commits at the strictly advancing watermark.
    const auto commit = commit_working_context(store, merged.value().merged, live);
    outcome.commit_disposition = working_context_commit_disposition_name(commit.disposition);
    outcome.commit_reason = commit.reason_code;
    const std::string merged_bytes = to_json_string(working_context_to_json(merged.value().merged));

    // 7. A different delta at the same watermark: equal watermark, different
    // digest — fail closed as a conflict, the merge gets no §5.2 waiver.
    WorkingContextDelta conflicting = delta.value();
    WorkingContextDeltaEntry probe;
    probe.kind = WorkingContextDeltaEntryKind::Addition;
    probe.section = "constraints";
    probe.content = "constraint: conflict probe at the same watermark";
    probe.source_events = {EventId{id_from_seed(9'001)}};
    probe.source_sequence = 9'001;
    probe.confidence = 0.8;
    conflicting.entries.push_back(probe);
    const auto conflict_merge =
        merge_working_context_delta(forked.value(), parent_tip, conflicting, tip_identity,
                                    parent_tip.through_event_sequence + 8);
    if (conflict_merge.has_value()) {
        const auto conflict = commit_working_context(store, conflict_merge.value().merged, live);
        outcome.conflict_disposition =
            working_context_commit_disposition_name(conflict.disposition);
        outcome.conflict_reason = conflict.reason_code;
    }

    // 8. Recovery re-fork: same seed over the rebuilt parent baseline.
    const auto reforked = fork_working_context(dataset.parent, seed);
    if (reforked.has_value()) {
        outcome.refork_id = reforked.value().id.to_string();
        outcome.refork_digest = digest_hex(reforked.value().state_digest());
    }

    outcome.merged_bytes = merged_bytes;
    return outcome;
}

[[nodiscard]] std::string render_report(const FrozenConfig &config, const Dataset &dataset,
                                        const ChainOutcome &first, const ChainOutcome &second) {
    JsonValue::Object report;
    report.emplace_back("eval", JsonValue("m24-fork-merge-w5"));
    report.emplace_back("schema", JsonValue("mira.working_context.delta.v1"));
    report.emplace_back("snapshot_schema",
                        JsonValue(std::to_string(working_context_schema_current().major) + "." +
                                  std::to_string(working_context_schema_current().minor)));
    report.emplace_back("dataset_digest", JsonValue(dataset.digest_hex_text));
    report.emplace_back("parent_watermark",
                        JsonValue(static_cast<std::int64_t>(config.parent_watermark)));
    report.emplace_back("child_watermark",
                        JsonValue(static_cast<std::int64_t>(config.child_watermark)));

    const auto chain_object = [](const ChainOutcome &outcome) {
        JsonValue::Object object;
        object.emplace_back("fork_ok", JsonValue(outcome.fork_ok));
        object.emplace_back("fork_id", JsonValue(outcome.fork_id));
        object.emplace_back("fork_digest", JsonValue(outcome.fork_digest));
        object.emplace_back("inherited_skipped",
                            JsonValue(static_cast<std::int64_t>(outcome.inherited_skipped)));
        object.emplace_back("supersedes", JsonValue(static_cast<std::int64_t>(outcome.supersedes)));
        object.emplace_back("additions", JsonValue(static_cast<std::int64_t>(outcome.additions)));
        object.emplace_back("supersedes_resolved",
                            JsonValue(static_cast<std::int64_t>(outcome.resolved)));
        object.emplace_back("supersedes_dropped_stale",
                            JsonValue(static_cast<std::int64_t>(outcome.stale)));
        object.emplace_back("additions_appended",
                            JsonValue(static_cast<std::int64_t>(outcome.appended)));
        object.emplace_back("truncated_entries",
                            JsonValue(static_cast<std::int64_t>(outcome.truncated)));
        object.emplace_back("merged_digest", JsonValue(outcome.merged_digest));
        object.emplace_back("merged_bytes", JsonValue(outcome.merged_bytes));
        object.emplace_back("commit_disposition", JsonValue(outcome.commit_disposition));
        object.emplace_back("commit_reason", JsonValue(outcome.commit_reason));
        object.emplace_back("noop_disposition", JsonValue(outcome.noop_disposition));
        object.emplace_back("conflict_disposition", JsonValue(outcome.conflict_disposition));
        object.emplace_back("conflict_reason", JsonValue(outcome.conflict_reason));
        object.emplace_back("refork_id", JsonValue(outcome.refork_id));
        object.emplace_back("refork_digest", JsonValue(outcome.refork_digest));
        return JsonValue(object);
    };
    report.emplace_back("run", chain_object(first));

    // The two runs must agree field by field; the report records the verdict
    // so cross-process byte equality also certifies run equality.
    bool runs_identical =
        first.fork_id == second.fork_id && first.fork_digest == second.fork_digest &&
        first.inherited_skipped == second.inherited_skipped &&
        first.supersedes == second.supersedes && first.additions == second.additions &&
        first.resolved == second.resolved && first.stale == second.stale &&
        first.appended == second.appended && first.truncated == second.truncated &&
        first.merged_digest == second.merged_digest && first.merged_bytes == second.merged_bytes &&
        first.commit_disposition == second.commit_disposition &&
        first.commit_reason == second.commit_reason &&
        first.noop_disposition == second.noop_disposition &&
        first.conflict_disposition == second.conflict_disposition &&
        first.conflict_reason == second.conflict_reason && first.refork_id == second.refork_id &&
        first.refork_digest == second.refork_digest;
    report.emplace_back("runs_byte_identical", JsonValue(runs_identical));
    return to_json_string(JsonValue(report));
}

int check(bool condition, const char *what) {
    if (!condition) {
        std::cerr << "m24 eval: gate failed: " << what << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const FrozenConfig config;
    const Dataset dataset = build_dataset(config);

    int failed = 0;
    failed += check(dataset.baseline_items.size() == 8 * config.baseline_items_per_section,
                    "dataset baseline size");
    failed += check(!dataset.digest_hex_text.empty(), "dataset digest computed");

    // The frozen chain runs twice; both passes must be byte identical.
    const ChainOutcome first = run_chain(dataset, config);
    const ChainOutcome second = run_chain(dataset, config);
    failed += check(first.fork_ok, "fork baseline produced");
    failed += check(first.fork_id == second.fork_id, "fork id stable across runs");
    failed += check(first.fork_digest == second.fork_digest, "fork digest stable across runs");

    // W5-G6 expectations from the frozen dataset ground truth:
    // six verbatim copies are skipped; the four lineage restatements are
    // supersedes; the rest are additions.
    failed += check(first.inherited_skipped == config.child_inherited, "inherited skip count");
    failed += check(first.supersedes == config.child_supersedes, "supersede count");
    failed += check(first.additions == config.child_additions, "addition count");

    // The parent tip dropped two supersede targets -> two stale supersedes;
    // the remaining two resolve in place; every addition appends.
    failed += check(first.resolved + first.stale == config.child_supersedes,
                    "supersede resolution partitions");
    failed += check(first.stale == 2, "stale supersede count");
    failed += check(first.appended == config.child_additions + first.stale,
                    "appended additions include reclassified stales");

    // Commit discipline on the merged candidate.
    failed += check(first.commit_disposition == "Committed", "merge commit committed");
    failed += check(first.noop_disposition == "IdempotentNoOp", "zero-effect replay noop");
    failed += check(first.conflict_disposition == "DiscardedStale" &&
                        first.conflict_reason == "conflicting-watermark",
                    "same-watermark different digest fails closed");

    // Recovery re-fork: same id and digest as the original baseline.
    failed += check(!first.refork_id.empty() && first.refork_id == first.fork_id,
                    "refork id equals original");
    failed += check(!first.refork_digest.empty() && first.refork_digest == first.fork_digest,
                    "refork digest equals original");

    const std::string report = render_report(config, dataset, first, second);
    failed += check(report.find("\"runs_byte_identical\":true") != std::string::npos,
                    "two chain runs byte identical");

    std::cout << report << '\n';
    if (failed == 0) {
        std::cout << "m24 fork/merge eval: OK\n";
        return 0;
    }
    std::cerr << "m24 fork/merge eval: " << failed << " checks failed\n";
    return 1;
}
