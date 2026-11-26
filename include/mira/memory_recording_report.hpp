#pragma once

#include <mira/agent_persona_suggestion.hpp>
#include <mira/memory_agent.hpp>
#include <mira/sqlite_goal_store.hpp>
#include <mira/sqlite_social_directory.hpp>
#include <mira/workflow_runtime.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mira {

enum class MemoryRecordingSystem : std::uint8_t { Persona, Event, Social, World, Goal, Skill };

struct MemoryRecordingAttempt final {
    MemoryRecordingSystem system;
    MemoryRecordingCounts counts{};
    bool more_pending = false; // The Record cursor has further work.
    std::optional<ErrorCode> error{};
    bool retryable = false; // The owner marked the error retryable.
};

// Host-side collection of settled owner results. One attempt per collection,
// in call order, so a failed attempt cannot erase preceding receipts.
class MemoryRecordingReport final {
  public:
    void collect(const Result<AgentPersonaSuggestionReceipt> &result) {
        add(MemoryRecordingSystem::Persona, result, [](const auto &receipt) {
            MemoryRecordingCounts counts;
            counts.pending_approval = 1;
            counts.replayed = receipt.idempotent_replay;
            return counts;
        });
    }

    void collect(const Result<MemoryAgentResult> &result) {
        if (result && result.value().mode != MemoryAgentMode::Record) {
            attempts_.push_back(
                {MemoryRecordingSystem::Event, {}, false, ErrorCode::InvalidArgument});
            return;
        }
        add(
            MemoryRecordingSystem::Event, result,
            [](const auto &receipt) { return receipt.recording_counts(); },
            result && result.value().more_pending);
    }

    void collect(const Result<SocialMessageAttributeReport> &result) {
        add(MemoryRecordingSystem::Social, result, [](const auto &receipt) {
            return social_counts(receipt.claims, receipt.consolidation, 0);
        });
    }

    void collect(const Result<SocialMessageRelationshipReport> &result) {
        add(MemoryRecordingSystem::Social, result, [](const auto &receipt) {
            return social_counts(receipt.claims, receipt.consolidation, receipt.unresolved.size());
        });
    }

    void collect_world_fact(const Result<ConsolidationReport> &result) {
        add(MemoryRecordingSystem::World, result,
            [](const auto &receipt) { return receipt.recording_counts(); });
    }

    void collect(const Result<GoalCreationReceipt> &result) { collect_goal(result); }

    void collect(const Result<std::vector<GoalCreationReceipt>> &result) {
        add(MemoryRecordingSystem::Goal, result, [](const auto &receipts) {
            MemoryRecordingCounts counts;
            for (const auto &receipt : receipts)
                counts.replayed += receipt.idempotent_replay;
            counts.unchanged = counts.replayed;
            counts.applied = receipts.size() - counts.unchanged;
            return counts;
        });
    }

    void collect(const Result<RecurringPlanCreationReceipt> &result) { collect_goal(result); }

    void collect(const Result<WorkflowRuntime::SkillHintRecordReport> &result) {
        add(MemoryRecordingSystem::Skill, result,
            [](const auto &receipt) { return receipt.consolidation.recording_counts(); });
    }

    [[nodiscard]] std::span<const MemoryRecordingAttempt> attempts() const noexcept {
        return attempts_;
    }

    [[nodiscard]] MemoryRecordingCounts totals(MemoryRecordingSystem system) const noexcept {
        MemoryRecordingCounts total;
        for (const auto &attempt : attempts_) {
            if (attempt.system != system)
                continue;
            total.applied += attempt.counts.applied;
            total.unchanged += attempt.counts.unchanged;
            total.pending_approval += attempt.counts.pending_approval;
            total.rejected += attempt.counts.rejected;
            total.unresolved += attempt.counts.unresolved;
            total.truncated += attempt.counts.truncated;
            total.replayed += attempt.counts.replayed;
        }
        return total;
    }

  private:
    template <typename Receipt, typename Summarize>
    void add(MemoryRecordingSystem system, const Result<Receipt> &result, Summarize summarize,
             bool more_pending = false) {
        MemoryRecordingAttempt attempt{system};
        if (result) {
            attempt.counts = summarize(result.value());
            attempt.more_pending = more_pending;
        } else {
            attempt.error = result.error().code;
            attempt.retryable = result.error().retryable;
        }
        attempts_.push_back(attempt);
    }

    template <typename Receipt> void collect_goal(const Result<Receipt> &result) {
        add(MemoryRecordingSystem::Goal, result, [](const auto &receipt) {
            MemoryRecordingCounts counts;
            counts.applied = !receipt.idempotent_replay;
            counts.unchanged = receipt.idempotent_replay;
            counts.replayed = receipt.idempotent_replay;
            return counts;
        });
    }

    static MemoryRecordingCounts social_counts(const SocialClaimReport &claims,
                                               const ConsolidationReport &consolidation,
                                               std::size_t unresolved) {
        auto counts = consolidation.recording_counts();
        const auto accounted = consolidation.entries.size() + unresolved;
        if (claims.rows.size() > accounted)
            counts.rejected += claims.rows.size() - accounted;
        counts.unresolved = unresolved;
        counts.truncated += claims.truncated_rows;
        return counts;
    }

    std::vector<MemoryRecordingAttempt> attempts_;
};

} // namespace mira
