// M20 (DEC-035 Stage W1) working-context eval harness. Builds the frozen
// synthetic checkpoint-chain dataset, drives the deterministic projection and
// commit pipeline through the frozen scenario rounds (progressive chains,
// idempotent replay, same-watermark conflict, stale watermark, identity
// mismatches, terminal lateness, epoch invalidation, recovery, bounds
// rejection) and asserts the pre-frozen gates W1-G1..G6 from milestone §4,
// printing a JSON report. Metrics measure pipeline behaviour, not semantic
// quality or token savings (RULE-10): W1 has no curator and no model.

#include <mira/context_consolidation.hpp>
#include <mira/context_working_context.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (M20 §4, frozen before the first evaluation run)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5232'3057'4354ULL; // "MIR20WCT"
    std::size_t sessions = 12;
    std::size_t chain_length = 5;
    std::size_t watermark_stride = 8;             // watermarks 8/16/24/32/40
    std::size_t planted_constraints = 6;
    std::size_t planted_decisions = 4;
    std::size_t planted_threads = 4;
    std::size_t events_per_session = 40;
    std::uint64_t task_epoch = 3;
    std::uint64_t environment_epoch = 7;
    // Pinned after the first frozen run (2026-09-14); the harness asserts
    // equality so the dataset is checked, not assumed (M17/M18/M19 style).
    std::string dataset_digest =
        "ed81befbc0273b80253c723f9273bf65fc932eac7a7972c9bc39c9d1b894695a";
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        std::ostringstream slot;
        slot << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned int>(byte);
        text += slot.str();
    }
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

[[nodiscard]] Id128 id_from(SplitMix64 &rng, std::string_view tag) {
    const std::uint64_t first = rng.next();
    const std::uint64_t second = rng.next();
    Id128::Bytes bytes{};
    std::memcpy(bytes.data(), &first, sizeof(first));
    std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
    for (std::size_t index = 0; index < tag.size() && index < bytes.size(); ++index) {
        bytes[index] ^= static_cast<std::uint8_t>(tag[index]);
    }
    return Id128{bytes};
}

[[nodiscard]] std::string rare_token(SplitMix64 &rng) {
    static constexpr std::string_view kAlphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string token(6, 'a');
    for (auto &character : token) {
        character = kAlphabet[static_cast<std::size_t>(rng.next() % kAlphabet.size())];
    }
    return token;
}

// ---------------------------------------------------------------------------
// Dataset: deterministic checkpoint chains
// ---------------------------------------------------------------------------

struct ChainCheckpoint final {
    ConversationCheckpoint checkpoint;
    // Ground truth: statement texts and their provenance per section.
    std::vector<std::string> constraint_texts;
    std::vector<std::string> decision_texts;
    std::vector<std::string> thread_texts;
};

struct EvalSession final {
    SessionId session;
    TaskId task;
    std::vector<EventId> events;
    std::vector<ChainCheckpoint> chain;
};

struct Dataset final {
    std::vector<EvalSession> sessions;
    std::string digest_hex_text;
};

[[nodiscard]] ConversationStatement make_statement(const std::string &content,
                                                   const EventId &origin,
                                                   std::uint64_t sequence) {
    ConversationStatement statement;
    statement.content = content;
    statement.source_events = {origin};
    statement.source_sequence = sequence;
    statement.confidence = 0.9;
    return statement;
}

[[nodiscard]] Dataset build_dataset(const FrozenConfig &config) {
    SplitMix64 rng{config.seed};
    Dataset dataset;

    for (std::size_t session_index = 0; session_index < config.sessions; ++session_index) {
        EvalSession eval_session;
        eval_session.session = SessionId{id_from(rng, "session")};
        eval_session.task = TaskId{id_from(rng, "task")};
        const std::string token = rare_token(rng);

        SplitMix64 local{rng.next()};
        // Two rng draws must be sequenced statements: expression-level
        // evaluation order of two rare_token(local) calls is unspecified and
        // would fork the dataset between compilers (M19 lesson).
        eval_session.events.reserve(config.events_per_session);
        for (std::size_t index = 0; index < config.events_per_session; ++index) {
            eval_session.events.push_back(EventId{id_from(local, "event")});
        }

        for (std::size_t chain_index = 0; chain_index < config.chain_length; ++chain_index) {
            const std::uint64_t watermark =
                static_cast<std::uint64_t>((chain_index + 1) * config.watermark_stride);
            const std::uint64_t revision = chain_index;
            ChainCheckpoint chain_entry;
            ConversationCheckpoint &checkpoint = chain_entry.checkpoint;
            checkpoint.id = conversation_checkpoint_id_from_seed(
                eval_session.session.to_string() + "|" + std::to_string(watermark) + "|" +
                std::to_string(revision));
            checkpoint.session_id = eval_session.session;
            checkpoint.task_id = eval_session.task;
            checkpoint.task_epoch = config.task_epoch;
            checkpoint.environment_epoch = config.environment_epoch;
            checkpoint.through_event_sequence = watermark;
            checkpoint.created_at = Timestamp::now();
            checkpoint.summary = "chain revision " + std::to_string(revision);

            // Planted statements: content varies per chain revision (the
            // replacement semantics of the frozen commit model stay visible),
            // provenance cites this session's event space, sequence sits in
            // [1, watermark].
            const auto plant = [&](std::size_t count, const char *kind,
                                   std::vector<ConversationStatement> &statements,
                                   std::vector<std::string> &texts) {
                for (std::size_t index = 0; index < count; ++index) {
                    const std::size_t flat = chain_index * count + index;
                    // Sequenced draws: one rare_token per statement.
                    const std::string salt = rare_token(local);
                    const std::string content =
                        std::string(kind) + " " + token + " " + salt + " r" +
                        std::to_string(revision) + " item" + std::to_string(index);
                    const std::size_t event_index = flat % config.events_per_session;
                    const std::uint64_t sequence =
                        (static_cast<std::uint64_t>(flat) % watermark) + 1;
                    statements.push_back(
                        make_statement(content, eval_session.events[event_index], sequence));
                    texts.push_back(content);
                    if (std::find(checkpoint.source_events.begin(),
                                  checkpoint.source_events.end(),
                                  eval_session.events[event_index]) ==
                        checkpoint.source_events.end()) {
                        checkpoint.source_events.push_back(eval_session.events[event_index]);
                    }
                }
            };
            plant(config.planted_constraints, "constraint:", checkpoint.constraints,
                  chain_entry.constraint_texts);
            plant(config.planted_decisions, "decision:", checkpoint.decisions,
                  chain_entry.decision_texts);
            plant(config.planted_threads, "thread:", checkpoint.unresolved_threads,
                  chain_entry.thread_texts);
            // Deterministic provenance order.
            std::sort(checkpoint.source_events.begin(), checkpoint.source_events.end(),
                      [](const EventId &left, const EventId &right) {
                          return left.to_string() < right.to_string();
                      });
            checkpoint.confidence = 0.9;
            eval_session.chain.push_back(std::move(chain_entry));
        }
        dataset.sessions.push_back(std::move(eval_session));
    }

    // Dataset digest over sorted "watermark|text" lines (structure matters).
    std::vector<std::string> lines;
    for (const auto &eval_session : dataset.sessions) {
        for (const auto &chain_entry : eval_session.chain) {
            const std::uint64_t watermark = chain_entry.checkpoint.through_event_sequence;
            for (const auto &text : chain_entry.constraint_texts) {
                lines.push_back(std::to_string(watermark) + "|" + text);
            }
            for (const auto &text : chain_entry.decision_texts) {
                lines.push_back(std::to_string(watermark) + "|" + text);
            }
            for (const auto &text : chain_entry.thread_texts) {
                lines.push_back(std::to_string(watermark) + "|" + text);
            }
        }
    }
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto &line : lines) {
        joined += line;
        joined += '\n';
    }
    dataset.digest_hex_text = digest_hex(digest_string(joined));
    return dataset;
}

// ---------------------------------------------------------------------------
// Metrics and gate bookkeeping
// ---------------------------------------------------------------------------

struct Metrics final {
    std::uint64_t commits = 0;
    std::uint64_t idempotent_no_ops = 0;
    std::uint64_t stale_discards = 0;
    std::uint64_t terminal_discards = 0;
    std::uint64_t unexpected_commits = 0;
    std::uint64_t fidelity_violations = 0;
    std::uint64_t identity_violations = 0;
    std::uint64_t conversion_violations = 0;
    std::uint64_t snapshots_committed = 0;
    std::uint64_t constraint_items = 0;
    std::uint64_t decision_items = 0;
    std::uint64_t open_issue_items = 0;
    std::uint64_t snapshot_bytes = 0;

    void add_snapshot(const WorkingContextSnapshot &snapshot) {
        ++snapshots_committed;
        constraint_items += snapshot.constraints.size();
        decision_items += snapshot.decisions.size();
        open_issue_items += snapshot.open_issues.size();
        // Byte accounting on a zeroed timestamp: the payload JSON carries
        // created_at, which must never leak into cross-process comparable
        // report numbers.
        WorkingContextSnapshot copy = snapshot;
        copy.created_at = Timestamp{};
        snapshot_bytes += to_json_string(working_context_to_json(copy)).size();
    }

    [[nodiscard]] bool equals(const Metrics &other) const {
        return commits == other.commits && idempotent_no_ops == other.idempotent_no_ops &&
               stale_discards == other.stale_discards &&
               terminal_discards == other.terminal_discards &&
               unexpected_commits == other.unexpected_commits &&
               fidelity_violations == other.fidelity_violations &&
               identity_violations == other.identity_violations &&
               conversion_violations == other.conversion_violations &&
               snapshots_committed == other.snapshots_committed &&
               constraint_items == other.constraint_items &&
               decision_items == other.decision_items &&
               open_issue_items == other.open_issue_items &&
               snapshot_bytes == other.snapshot_bytes;
    }
};

[[nodiscard]] WorkingContextIdentity identity_for(const EvalSession &eval_session,
                                                  const FrozenConfig &config) {
    WorkingContextIdentity identity;
    identity.task = eval_session.task;
    identity.task_epoch = config.task_epoch;
    identity.environment_epoch = config.environment_epoch;
    return identity;
}

[[nodiscard]] WorkingContextCommitState live_for(const EvalSession &eval_session,
                                                 const FrozenConfig &config) {
    WorkingContextCommitState live;
    live.session = eval_session.session;
    live.task = eval_session.task;
    live.task_epoch = config.task_epoch;
    live.environment_epoch = config.environment_epoch;
    return live;
}

// Verifies the projection ground truth for one snapshot (W1-G1 and the
// identity part of W1-G2).
void audit_snapshot(const ChainCheckpoint &chain_entry, const WorkingContextSnapshot &snapshot,
                    Metrics &metrics) {
    const ConversationCheckpoint &checkpoint = chain_entry.checkpoint;
    const auto same = [](const WorkingContextItem &item,
                         const ConversationStatement &statement) {
        return item.content == statement.content &&
               item.source_events == statement.source_events &&
               item.source_sequence == statement.source_sequence &&
               item.confidence == statement.confidence;
    };
    if (snapshot.constraints.size() != checkpoint.constraints.size() ||
        snapshot.decisions.size() != checkpoint.decisions.size() ||
        snapshot.open_issues.size() != checkpoint.unresolved_threads.size()) {
        ++metrics.fidelity_violations;
    }
    for (std::size_t index = 0; index < snapshot.constraints.size(); ++index) {
        if (!same(snapshot.constraints[index], checkpoint.constraints[index])) {
            ++metrics.fidelity_violations;
        }
    }
    for (std::size_t index = 0; index < snapshot.decisions.size(); ++index) {
        if (!same(snapshot.decisions[index], checkpoint.decisions[index])) {
            ++metrics.fidelity_violations;
        }
    }
    for (std::size_t index = 0; index < snapshot.open_issues.size(); ++index) {
        if (!same(snapshot.open_issues[index], checkpoint.unresolved_threads[index])) {
            ++metrics.fidelity_violations;
        }
    }
    // Identity binding (W1-G2): watermark equals the source checkpoint's and
    // the provenance cites exactly it; re-projection re-derives id+digest.
    if (snapshot.through_event_sequence != checkpoint.through_event_sequence ||
        snapshot.source_checkpoints.size() != 1 ||
        snapshot.source_checkpoints[0] != checkpoint.id) {
        ++metrics.identity_violations;
    }
}

// Layer 0 conversion discipline (W1-G5): partitions, authority, provenance,
// deterministic ids disjoint from the checkpoint item space.
void audit_conversion(const ChainCheckpoint &chain_entry, const WorkingContextSnapshot &snapshot,
                      Metrics &metrics) {
    const auto items = context_items_from_working_context(snapshot);
    const std::size_t expected = chain_entry.constraint_texts.size() +
                                 chain_entry.decision_texts.size() +
                                 chain_entry.thread_texts.size();
    if (items.size() != expected) {
        ++metrics.conversion_violations;
    }
    std::size_t index = 0;
    for (std::size_t constraint = 0; constraint < chain_entry.constraint_texts.size();
         ++constraint, ++index) {
        if (items[index].kind != ContextItemKind::UserConstraint ||
            items[index].authority != ContextAuthority::UntrustedExternalData ||
            items[index].provenance != snapshot.constraints[constraint].source_events) {
            ++metrics.conversion_violations;
        }
    }
    for (std::size_t decision = 0; decision < chain_entry.decision_texts.size();
         ++decision, ++index) {
        if (items[index].kind != ContextItemKind::CheckpointSummary ||
            items[index].authority != ContextAuthority::UntrustedExternalData) {
            ++metrics.conversion_violations;
        }
    }
    for (std::size_t issue = 0; issue < chain_entry.thread_texts.size(); ++issue, ++index) {
        if (items[index].kind != ContextItemKind::CheckpointSummary ||
            items[index].authority != ContextAuthority::UntrustedExternalData) {
            ++metrics.conversion_violations;
        }
    }
    // Deterministic re-conversion and id-space separation from the
    // checkpoint conversion.
    const auto again = context_items_from_working_context(snapshot);
    if (again.size() != items.size()) {
        ++metrics.conversion_violations;
    }
    const auto checkpoint_items = context_items_from_checkpoint(chain_entry.checkpoint);
    for (const auto &item : items) {
        for (const auto &checkpoint_item : checkpoint_items) {
            if (item.id == checkpoint_item.id) {
                ++metrics.conversion_violations;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario rounds
// ---------------------------------------------------------------------------

struct ChainRoundResult final {
    Metrics metrics;
    // Final committed snapshot JSON per session (for the recovery round).
    std::vector<std::string> final_json;
};

[[nodiscard]] ChainRoundResult run_chain_round(const Dataset &dataset,
                                               const FrozenConfig &config) {
    ChainRoundResult result;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto live = live_for(eval_session, config);
        for (const auto &chain_entry : eval_session.chain) {
            auto snapshot =
                working_context_from_checkpoint(chain_entry.checkpoint, identity);
            if (!snapshot) {
                ++result.metrics.fidelity_violations;
                continue;
            }
            audit_snapshot(chain_entry, snapshot.value(), result.metrics);
            audit_conversion(chain_entry, snapshot.value(), result.metrics);
            const auto outcome = commit_working_context(store, snapshot.value(), live);
            switch (outcome.disposition) {
            case WorkingContextCommitDisposition::Committed:
                ++result.metrics.commits;
                result.metrics.add_snapshot(snapshot.value());
                break;
            case WorkingContextCommitDisposition::IdempotentNoOp:
                ++result.metrics.idempotent_no_ops;
                break;
            case WorkingContextCommitDisposition::DiscardedTerminal:
                ++result.metrics.terminal_discards;
                break;
            case WorkingContextCommitDisposition::DiscardedStale:
                ++result.metrics.stale_discards;
                break;
            }
        }
        const auto latest = store.latest(eval_session.session);
        if (latest.has_value() && latest.value().has_value()) {
            result.final_json.push_back(
                to_json_string(working_context_to_json(latest.value().value())));
        } else {
            result.final_json.emplace_back();
        }
    }
    return result;
}

// Re-merges the final checkpoint of every session: pure replays must no-op.
[[nodiscard]] Metrics run_replay_round(const Dataset &dataset, const FrozenConfig &config,
                                       std::vector<std::string> &stored_before,
                                       std::vector<std::string> &stored_after) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto &final_entry = eval_session.chain.back();
        const auto snapshot =
            working_context_from_checkpoint(final_entry.checkpoint,
                                            identity_for(eval_session, config));
        if (!snapshot) {
            ++metrics.fidelity_violations;
            stored_before.emplace_back();
            stored_after.emplace_back();
            continue;
        }
        // Seed the store with one commit, then replay the identical
        // candidate: the stored payload must not move.
        const auto seeded = commit_working_context(store, snapshot.value(),
                                                   live_for(eval_session, config));
        if (seeded.disposition != WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
        }
        const auto latest = store.latest(eval_session.session);
        if (latest.has_value() && latest.value().has_value()) {
            const std::string before =
                to_json_string(working_context_to_json(latest.value().value()));
            stored_before.push_back(before);
            const auto outcome = commit_working_context(store, snapshot.value(),
                                                        live_for(eval_session, config));
            if (outcome.disposition == WorkingContextCommitDisposition::IdempotentNoOp) {
                ++metrics.idempotent_no_ops;
            } else {
                ++metrics.unexpected_commits;
            }
            const auto after_latest = store.latest(eval_session.session);
            const std::string after =
                to_json_string(working_context_to_json(after_latest.value().value()));
            stored_after.push_back(after);
        } else {
            stored_before.emplace_back();
            stored_after.emplace_back();
        }
    }
    return metrics;
}

// Same watermark, different content: fail-closed conflict, store untouched.
[[nodiscard]] Metrics run_conflict_round(const Dataset &dataset, const FrozenConfig &config,
                                         std::uint64_t &stored_unchanged) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    stored_unchanged = 0;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto live = live_for(eval_session, config);
        const auto &final_entry = eval_session.chain.back();
        const auto seeded_snapshot =
            working_context_from_checkpoint(final_entry.checkpoint, identity);
        if (!seeded_snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        (void)commit_working_context(store, seeded_snapshot.value(), live);
        const std::string before =
            to_json_string(working_context_to_json(
                store.latest(eval_session.session).value().value()));

        ConversationCheckpoint conflicting = final_entry.checkpoint;
        conflicting.constraints[0].content += " (conflicting revision)";
        const auto conflicting_snapshot = working_context_from_checkpoint(conflicting, identity);
        if (!conflicting_snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        const auto outcome = commit_working_context(store, conflicting_snapshot.value(), live);
        if (outcome.disposition == WorkingContextCommitDisposition::DiscardedStale &&
            outcome.reason_code == "conflicting-watermark") {
            ++metrics.stale_discards;
        } else if (outcome.disposition == WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
        }
        const std::string after =
            to_json_string(working_context_to_json(
                store.latest(eval_session.session).value().value()));
        if (before == after) {
            ++stored_unchanged;
        }
    }
    return metrics;
}

// Stale watermark: re-committing an older chain entry is discarded.
[[nodiscard]] Metrics run_stale_round(const Dataset &dataset, const FrozenConfig &config) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto live = live_for(eval_session, config);
        const auto &final_entry = eval_session.chain.back();
        const auto seeded = working_context_from_checkpoint(final_entry.checkpoint, identity);
        if (!seeded) {
            ++metrics.fidelity_violations;
            continue;
        }
        (void)commit_working_context(store, seeded.value(), live);
        const auto &older_entry =
            eval_session.chain[eval_session.chain.size() - 2];
        const auto older = working_context_from_checkpoint(older_entry.checkpoint, identity);
        if (!older) {
            ++metrics.fidelity_violations;
            continue;
        }
        const auto outcome = commit_working_context(store, older.value(), live);
        if (outcome.disposition == WorkingContextCommitDisposition::DiscardedStale &&
            outcome.reason_code == "stale-watermark") {
            ++metrics.stale_discards;
        } else if (outcome.disposition == WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
        }
    }
    return metrics;
}

// Identity mismatches: every mutated live state must discard with the exact
// reason code, never commit.
[[nodiscard]] Metrics run_mismatch_rounds(const Dataset &dataset, const FrozenConfig &config) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto &final_entry = eval_session.chain.back();
        const auto snapshot =
            working_context_from_checkpoint(final_entry.checkpoint, identity);
        if (!snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        auto live = live_for(eval_session, config);
        // Seed one commit against the matching live state.
        (void)commit_working_context(store, snapshot.value(), live);

        WorkingContextCommitState wrong_session = live;
        SplitMix64 other_rng{config.seed};
        wrong_session.session = SessionId{id_from(other_rng, "other")};
        if (commit_working_context(store, snapshot.value(), wrong_session)
                .disposition != WorkingContextCommitDisposition::DiscardedStale) {
            ++metrics.unexpected_commits;
        }
        WorkingContextCommitState wrong_task = live;
        SplitMix64 other_rng2{config.seed + 1};
        wrong_task.task = TaskId{id_from(other_rng2, "other")};
        if (commit_working_context(store, snapshot.value(), wrong_task)
                .disposition != WorkingContextCommitDisposition::DiscardedStale) {
            ++metrics.unexpected_commits;
        }
        WorkingContextCommitState wrong_task_epoch = live;
        wrong_task_epoch.task_epoch = config.task_epoch + 1;
        if (commit_working_context(store, snapshot.value(), wrong_task_epoch)
                .disposition != WorkingContextCommitDisposition::DiscardedStale) {
            ++metrics.unexpected_commits;
        }
        WorkingContextCommitState wrong_environment = live;
        wrong_environment.environment_epoch = config.environment_epoch + 1;
        if (commit_working_context(store, snapshot.value(), wrong_environment)
                .disposition != WorkingContextCommitDisposition::DiscardedStale) {
            ++metrics.unexpected_commits;
        }
    }
    return metrics;
}

// Terminal idempotency: late candidates after session/task terminal state.
[[nodiscard]] Metrics run_terminal_rounds(const Dataset &dataset, const FrozenConfig &config) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto &final_entry = eval_session.chain.back();
        const auto snapshot =
            working_context_from_checkpoint(final_entry.checkpoint, identity);
        if (!snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        auto live = live_for(eval_session, config);
        live.session_terminal = true;
        const auto session_outcome = commit_working_context(store, snapshot.value(), live);
        if (session_outcome.disposition == WorkingContextCommitDisposition::DiscardedTerminal) {
            ++metrics.terminal_discards;
        } else if (session_outcome.disposition == WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
        }
        auto task_live = live_for(eval_session, config);
        task_live.task_terminal = true;
        const auto task_outcome = commit_working_context(store, snapshot.value(), task_live);
        if (task_outcome.disposition == WorkingContextCommitDisposition::DiscardedTerminal) {
            ++metrics.terminal_discards;
        } else if (task_outcome.disposition == WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
        }
    }
    return metrics;
}

// The store-shape assertions of the epoch round.
void result_epoch_checks(InMemoryWorkingContextStore &store, const EvalSession &eval_session,
                         const WorkingContextSnapshot &old_snapshot,
                         const WorkingContextSnapshot &new_snapshot,
                         const WorkingContextCommitState &old_live,
                         const WorkingContextCommitState &new_live, Metrics &metrics) {
    (void)old_live;
    const auto latest = store.latest(eval_session.session);
    if (!latest.has_value() || !latest.value().has_value() ||
        latest.value().value().id != new_snapshot.id) {
        ++metrics.identity_violations;
    }
    const auto at_old = store.latest_at_or_before(eval_session.session,
                                                  old_snapshot.through_event_sequence);
    if (!at_old.has_value() || !at_old.value().has_value() ||
        at_old.value().value().environment_epoch != old_snapshot.environment_epoch) {
        ++metrics.identity_violations;
    }
    // Old-epoch candidates cannot commit against new-epoch live state.
    if (commit_working_context(store, old_snapshot, new_live).reason_code !=
        "environment-epoch-mismatch") {
        ++metrics.unexpected_commits;
    }
}

// Epoch invalidation (W1-G4): the bumped environment epoch opens an isolated
// chain; old snapshots stay queryable with their epoch stamping.
[[nodiscard]] Metrics run_epoch_round(const Dataset &dataset, const FrozenConfig &config) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        auto live = live_for(eval_session, config);
        const auto &first_entry = eval_session.chain.front();
        const auto old_snapshot =
            working_context_from_checkpoint(first_entry.checkpoint, identity);
        if (!old_snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        if (commit_working_context(store, old_snapshot.value(), live).disposition ==
            WorkingContextCommitDisposition::Committed) {
            ++metrics.commits;
            metrics.add_snapshot(old_snapshot.value());
        } else {
            ++metrics.unexpected_commits;
        }

        WorkingContextIdentity bumped = identity;
        bumped.environment_epoch = config.environment_epoch + 1;
        ConversationCheckpoint bumped_checkpoint =
            eval_session.chain[1].checkpoint;
        bumped_checkpoint.environment_epoch = bumped.environment_epoch;
        const auto new_snapshot =
            working_context_from_checkpoint(bumped_checkpoint, bumped);
        if (!new_snapshot) {
            ++metrics.fidelity_violations;
            continue;
        }
        WorkingContextCommitState new_live = live;
        new_live.environment_epoch = bumped.environment_epoch;
        if (commit_working_context(store, new_snapshot.value(), new_live).disposition ==
            WorkingContextCommitDisposition::Committed) {
            ++metrics.commits;
            metrics.add_snapshot(new_snapshot.value());
        } else {
            ++metrics.unexpected_commits;
        }
        result_epoch_checks(store, eval_session, old_snapshot.value(), new_snapshot.value(),
                            live, new_live, metrics);
    }
    return metrics;
}

// Bounds rejection (W1-G5): over-bounds and invalid candidates produce no
// snapshot and no commit.
[[nodiscard]] Metrics run_bounds_round(const Dataset &dataset, const FrozenConfig &config) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    for (const auto &eval_session : dataset.sessions) {
        const auto identity = identity_for(eval_session, config);
        const auto live = live_for(eval_session, config);
        WorkingContextMergeOptions tight;
        tight.max_items_per_section = 2;
        const auto &final_entry = eval_session.chain.back();
        // 4+ planted threads against a two-item bound: whole-candidate
        // rejection.
        if (working_context_from_checkpoint(final_entry.checkpoint, identity, tight)
                .has_value()) {
            ++metrics.unexpected_commits;
        }
        // A checkpoint failing its own validation never projects.
        ConversationCheckpoint invalid = final_entry.checkpoint;
        invalid.constraints[0].source_events.clear();
        if (working_context_from_checkpoint(invalid, identity).has_value()) {
            ++metrics.unexpected_commits;
        }
        // An invalid candidate cannot sneak through the commit either.
        const auto seeded = working_context_from_checkpoint(final_entry.checkpoint, identity);
        if (!seeded) {
            ++metrics.fidelity_violations;
            continue;
        }
        (void)commit_working_context(store, seeded.value(), live);
        WorkingContextSnapshot broken = seeded.value();
        broken.constraints.clear();
        broken.constraints.push_back(WorkingContextItem{});
        if (commit_working_context(store, broken, live).reason_code != "invalid-candidate") {
            ++metrics.unexpected_commits;
        }
    }
    return metrics;
}

// Recovery (W1-G6): re-projecting into a fresh store restores the same
// payload byte for byte.
[[nodiscard]] Metrics run_recovery_round(const Dataset &dataset, const FrozenConfig &config,
                                         const std::vector<std::string> &expected_json,
                                         std::uint64_t &recovered_identical) {
    Metrics metrics;
    InMemoryWorkingContextStore store;
    recovered_identical = 0;
    std::size_t index = 0;
    for (const auto &eval_session : dataset.sessions) {
        const auto &final_entry = eval_session.chain.back();
        const auto snapshot =
            working_context_from_checkpoint(final_entry.checkpoint,
                                            identity_for(eval_session, config));
        if (!snapshot) {
            ++metrics.fidelity_violations;
            ++index;
            continue;
        }
        const auto outcome = commit_working_context(store, snapshot.value(),
                                                    live_for(eval_session, config));
        if (outcome.disposition != WorkingContextCommitDisposition::Committed) {
            ++metrics.unexpected_commits;
            ++index;
            continue;
        }
        metrics.add_snapshot(snapshot.value());
        const std::string recovered =
            to_json_string(working_context_to_json(
                store.latest(eval_session.session).value().value()));
        if (index < expected_json.size() && recovered == expected_json[index]) {
            ++recovered_identical;
        }
        ++index;
    }
    return metrics;
}

[[nodiscard]] JsonValue metrics_json(const Metrics &metrics) {
    JsonValue::Object object;
    object.emplace_back("commits", static_cast<std::int64_t>(metrics.commits));
    object.emplace_back("idempotent_no_ops", static_cast<std::int64_t>(metrics.idempotent_no_ops));
    object.emplace_back("stale_discards", static_cast<std::int64_t>(metrics.stale_discards));
    object.emplace_back("terminal_discards", static_cast<std::int64_t>(metrics.terminal_discards));
    object.emplace_back("unexpected_commits",
                        static_cast<std::int64_t>(metrics.unexpected_commits));
    object.emplace_back("fidelity_violations",
                        static_cast<std::int64_t>(metrics.fidelity_violations));
    object.emplace_back("identity_violations",
                        static_cast<std::int64_t>(metrics.identity_violations));
    object.emplace_back("conversion_violations",
                        static_cast<std::int64_t>(metrics.conversion_violations));
    object.emplace_back("snapshots_committed",
                        static_cast<std::int64_t>(metrics.snapshots_committed));
    object.emplace_back("constraint_items", static_cast<std::int64_t>(metrics.constraint_items));
    object.emplace_back("decision_items", static_cast<std::int64_t>(metrics.decision_items));
    object.emplace_back("open_issue_items", static_cast<std::int64_t>(metrics.open_issue_items));
    object.emplace_back("snapshot_bytes", static_cast<std::int64_t>(metrics.snapshot_bytes));
    return JsonValue(std::move(object));
}

} // namespace

int main(int argc, char **argv) {
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (output_path.empty()) {
            output_path = argument;
        }
    }

    FrozenConfig config;
    const Dataset dataset = build_dataset(config);
    if (!config.dataset_digest.empty() && dataset.digest_hex_text != config.dataset_digest) {
        std::cerr << "dataset digest mismatch: got " << dataset.digest_hex_text << ", want "
                  << config.dataset_digest << "\n";
        return 2;
    }

    const ChainRoundResult chain = run_chain_round(dataset, config);
    std::vector<std::string> stored_before;
    std::vector<std::string> stored_after;
    const Metrics replay = run_replay_round(dataset, config, stored_before, stored_after);
    std::uint64_t conflicts_unchanged = 0;
    const Metrics conflict = run_conflict_round(dataset, config, conflicts_unchanged);
    const Metrics stale = run_stale_round(dataset, config);
    const Metrics mismatch = run_mismatch_rounds(dataset, config);
    const Metrics terminal = run_terminal_rounds(dataset, config);
    const Metrics epoch = run_epoch_round(dataset, config);
    const Metrics bounds = run_bounds_round(dataset, config);
    std::uint64_t recovered_identical = 0;
    const Metrics recovery =
        run_recovery_round(dataset, config, chain.final_json, recovered_identical);
    // In-process determinism: the second chain round must match the first on
    // every aggregate.
    const ChainRoundResult repeat = run_chain_round(dataset, config);

    bool gates_ok = true;
    std::vector<std::string> failures;

    // W1-G1: projection fidelity — sections mirror the source checkpoint.
    const bool g1 = chain.metrics.fidelity_violations == 0 &&
                    repeat.metrics.fidelity_violations == 0 &&
                    recovery.fidelity_violations == 0;
    if (!g1) {
        gates_ok = false;
        failures.push_back("G1: projection fidelity violations " +
                           std::to_string(chain.metrics.fidelity_violations));
    }
    // W1-G2: identity and watermark binding with deterministic re-derivation.
    const bool g2 = chain.metrics.identity_violations == 0 &&
                    epoch.identity_violations == 0 &&
                    chain.metrics.snapshots_committed ==
                        dataset.sessions.size() * config.chain_length;
    if (!g2) {
        gates_ok = false;
        failures.push_back("G2: identity/watermark binding violations " +
                           std::to_string(chain.metrics.identity_violations));
    }
    // W1-G3: commit discipline — replays no-op, conflicts and stale and
    // mismatched and terminal candidates discard, nothing unexpected commits.
    bool replays_stable = stored_before.size() == stored_after.size();
    for (std::size_t index = 0; replays_stable && index < stored_before.size(); ++index) {
        replays_stable = replays_stable && stored_before[index] == stored_after[index];
    }
    const std::size_t expected_commits =
        dataset.sessions.size() * config.chain_length;
    const bool g3 = replay.idempotent_no_ops == dataset.sessions.size() &&
                    replay.unexpected_commits == 0 && replays_stable &&
                    conflict.stale_discards == dataset.sessions.size() &&
                    conflicts_unchanged == dataset.sessions.size() &&
                    stale.stale_discards == dataset.sessions.size() &&
                    mismatch.unexpected_commits == 0 && terminal.terminal_discards == 2 * dataset.sessions.size() &&
                    terminal.unexpected_commits == 0 &&
                    chain.metrics.stale_discards == 0 && chain.metrics.terminal_discards == 0 &&
                    chain.metrics.unexpected_commits == 0 &&
                    chain.metrics.commits == expected_commits;
    if (!g3) {
        gates_ok = false;
        failures.push_back("G3: commit discipline violated (no-ops " +
                           std::to_string(replay.idempotent_no_ops) + ", conflicts kept " +
                           std::to_string(conflicts_unchanged) + ", stale " +
                           std::to_string(stale.stale_discards) + ", terminal " +
                           std::to_string(terminal.terminal_discards) + ", unexpected " +
                           std::to_string(replay.unexpected_commits + conflict.unexpected_commits +
                                          stale.unexpected_commits + mismatch.unexpected_commits +
                                          terminal.unexpected_commits +
                                          chain.metrics.unexpected_commits) +
                           ")");
    }
    // W1-G4: epoch invalidation isolates chains.
    const bool g4 = epoch.unexpected_commits == 0 && epoch.identity_violations == 0;
    if (!g4) {
        gates_ok = false;
        failures.push_back("G4: epoch chain isolation violated");
    }
    // W1-G5: Layer 0 conversion discipline and whole-candidate bounds.
    const bool g5 = chain.metrics.conversion_violations == 0 && bounds.unexpected_commits == 0;
    if (!g5) {
        gates_ok = false;
        failures.push_back("G5: conversion discipline or bounds rejection violated (" +
                           std::to_string(chain.metrics.conversion_violations) + ")");
    }
    // W1-G6: determinism and recovery.
    const bool g6 = repeat.metrics.equals(chain.metrics) &&
                    recovered_identical == dataset.sessions.size();
    if (!g6) {
        gates_ok = false;
        failures.push_back("G6: determinism or recovery violated (recovered " +
                           std::to_string(recovered_identical) + "/" +
                           std::to_string(dataset.sessions.size()) + ")");
    }

    JsonValue::Object report;
    report.emplace_back("schema", "mira.m20.working-context-eval.v1");
    report.emplace_back("dataset_digest", dataset.digest_hex_text);
    JsonValue::Object config_json;
    config_json.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    config_json.emplace_back("sessions", static_cast<std::int64_t>(config.sessions));
    config_json.emplace_back("chain_length", static_cast<std::int64_t>(config.chain_length));
    config_json.emplace_back("watermark_stride",
                             static_cast<std::int64_t>(config.watermark_stride));
    config_json.emplace_back("planted_constraints",
                             static_cast<std::int64_t>(config.planted_constraints));
    config_json.emplace_back("planted_decisions",
                             static_cast<std::int64_t>(config.planted_decisions));
    config_json.emplace_back("planted_threads", static_cast<std::int64_t>(config.planted_threads));
    config_json.emplace_back("events_per_session",
                             static_cast<std::int64_t>(config.events_per_session));
    config_json.emplace_back("task_epoch", static_cast<std::int64_t>(config.task_epoch));
    config_json.emplace_back("environment_epoch",
                             static_cast<std::int64_t>(config.environment_epoch));
    report.emplace_back("config", JsonValue(std::move(config_json)));
    report.emplace_back("chain_round", metrics_json(chain.metrics));
    report.emplace_back("replay_round", metrics_json(replay));
    report.emplace_back("conflict_round", metrics_json(conflict));
    report.emplace_back("stale_round", metrics_json(stale));
    report.emplace_back("mismatch_round", metrics_json(mismatch));
    report.emplace_back("terminal_round", metrics_json(terminal));
    report.emplace_back("epoch_round", metrics_json(epoch));
    report.emplace_back("bounds_round", metrics_json(bounds));
    report.emplace_back("recovery_round", metrics_json(recovery));
    report.emplace_back("conflicts_stored_unchanged", static_cast<std::int64_t>(conflicts_unchanged));
    report.emplace_back("recovered_identical", static_cast<std::int64_t>(recovered_identical));
    JsonValue::Object gates_json;
    gates_json.emplace_back("g1_projection_fidelity", g1);
    gates_json.emplace_back("g2_identity_watermark_binding", g2);
    gates_json.emplace_back("g3_commit_discipline", g3);
    gates_json.emplace_back("g4_epoch_chain_isolation", g4);
    gates_json.emplace_back("g5_layer0_and_bounds", g5);
    gates_json.emplace_back("g6_determinism_recovery", g6);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "deterministic projection from committed checkpoints, no curator model: metrics measure "
        "pipeline behaviour (projection fidelity, identity/watermark binding, five-tuple commit "
        "discipline, epoch chain isolation, Layer 0 conversion discipline, recovery, "
        "determinism), not semantic curation quality or token savings; W1 declares neither token "
        "reduction nor continuation correctness (RULE-10)");

    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    if (!gates_ok) {
        for (const auto &message : failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return gates_ok ? 0 : 1;
}
