#pragma once

#include <mira/goal_commitment_recording.hpp>
#include <mira/memory_recording_report.hpp>
#include <mira/world_fact_recording.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mira {

struct PersonaRecordingCandidate final {
    AgentPersonaSuggestionSource source;
    std::string quote;
    std::optional<std::size_t> quote_offset{};
};

struct EventRecordingCandidate final {
    MemoryScope write_scope;
    std::size_t page_limit = 1;
    std::optional<std::uint64_t> expected_revision{};
};

template <typename Claim> struct SocialRecordingCandidate final {
    SocialEnvironmentKey environment;
    SocialMessageLocator message;
    std::vector<Claim> claims;
};
using SocialAttributeRecordingCandidate = SocialRecordingCandidate<SocialMessageAttributeCandidate>;
using SocialRelationshipRecordingCandidate =
    SocialRecordingCandidate<SocialMessageRelationshipCandidate>;

struct WorldFactRecordingCandidate final {
    WorldFactRecordingBinding binding;
    WorldFactSource source;
};

struct GoalRecordingCandidate final {
    GoalCommitmentBinding binding;
    GoalCommitmentSource source;
    std::vector<GoalCommitmentExcerpt> commitments;
};

struct RecurringPlanRecordingCandidate final {
    GoalCommitmentBinding binding;
    GoalCommitmentSource source;
    GoalRecurringPlanCandidate plan;
};

struct SkillHintRecordingCandidate final {
    std::string skill;
    WorkflowRunId run;
    std::string hint;
};

using MemoryRecordingCandidate =
    std::variant<PersonaRecordingCandidate, EventRecordingCandidate,
                 SocialAttributeRecordingCandidate, SocialRelationshipRecordingCandidate,
                 WorldFactRecordingCandidate, GoalRecordingCandidate,
                 RecurringPlanRecordingCandidate, SkillHintRecordingCandidate>;

struct MemoryRecordingOwners final {
    const IEventStore &events;
    AgentPersonaSupplier bind_persona{};
    AgentPersonaSuggestionSink stage_persona{};
    MemoryAgentService *event_memory = nullptr;
    SqliteSocialDirectory *social = nullptr;
    std::function<bool(const SocialEnvironmentKey &, const SocialMessageLocator &,
                       const OperationContext &)>
        authorize_social{};
    IMemory *world_memory = nullptr;
    WorldFactRecordingLeaseAcquirer acquire_world{};
    SqliteGoalStore *goals = nullptr;
    GoalCommitmentAuthorizer authorize_goal{};
    WorkflowRuntime *workflow = nullptr;
    const SkillPublicationRegistry *skills = nullptr;
    WorkflowRuntime::SkillHintAuthorizer authorize_skill{};
};

using MemoryRecordingOutcome =
    std::variant<Result<AgentPersonaSuggestionReceipt>, Result<MemoryAgentResult>,
                 Result<SocialMessageAttributeReport>, Result<SocialMessageRelationshipReport>,
                 Result<ConsolidationReport>, Result<std::vector<GoalCreationReceipt>>,
                 Result<RecurringPlanCreationReceipt>,
                 Result<WorkflowRuntime::SkillHintRecordReport>>;

struct MemoryRecordingBatchResult final {
    // Same order as the processed input prefix, including per-candidate errors.
    std::vector<MemoryRecordingOutcome> outcomes;
    MemoryRecordingReport report;
    std::optional<ErrorCode> interruption{};
};

inline constexpr std::size_t kMaxMemoryRecordingBatchSize = 32;

namespace memory_recording_detail {

inline Error batch_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.memory_recording.batch";
    error.safe_message = message;
    return error;
}

struct Recorder final {
    const MemoryRecordingOwners &owners;
    const OperationContext &operation;

    static Error missing_owner() {
        return batch_error(ErrorCode::UnsupportedCapability, "memory recording owner unavailable");
    }

    Result<AgentPersonaSuggestionReceipt> operator()(const PersonaRecordingCandidate &row) const {
        if (!owners.bind_persona || !owners.stage_persona)
            return missing_owner();
        return submit_agent_persona_suggestion(owners.events, row.source, row.quote,
                                               row.quote_offset, operation, owners.bind_persona,
                                               owners.stage_persona);
    }

    Result<MemoryAgentResult> operator()(const EventRecordingCandidate &row) const {
        if (!owners.event_memory)
            return missing_owner();
        MemoryAgentRequest request;
        request.task = operation.task;
        request.mode = MemoryAgentMode::Record;
        request.write_scope = row.write_scope;
        request.record_page_limit = row.page_limit;
        request.expected_revision = row.expected_revision;
        auto job = owners.event_memory->submit(std::move(request), operation);
        if (!job)
            return job.error();
        return job.value().result.get();
    }

    template <typename Claim>
    Result<void> check_social(const SocialRecordingCandidate<Claim> &row) const {
        if (!owners.social)
            return missing_owner();
        if (row.message.session != operation.session || !owners.authorize_social ||
            !owners.authorize_social(row.environment, row.message, operation))
            return batch_error(ErrorCode::PermissionDenied, "social recording is not authorized");
        return {};
    }

    Result<SocialMessageAttributeReport>
    operator()(const SocialAttributeRecordingCandidate &row) const {
        if (auto authorized = check_social(row); !authorized)
            return authorized.error();
        return owners.social->record_inferred_message_attributes(
            row.environment, row.message, owners.events, row.claims, operation);
    }

    Result<SocialMessageRelationshipReport>
    operator()(const SocialRelationshipRecordingCandidate &row) const {
        if (auto authorized = check_social(row); !authorized)
            return authorized.error();
        return owners.social->record_inferred_message_relationships(
            row.environment, row.message, owners.events, row.claims, operation);
    }

    Result<ConsolidationReport> operator()(const WorldFactRecordingCandidate &row) const {
        if (!owners.world_memory || !owners.acquire_world)
            return missing_owner();
        return record_verified_world_fact(*owners.world_memory, owners.events, row.binding,
                                          row.source, operation, owners.acquire_world);
    }

    Result<std::vector<GoalCreationReceipt>> operator()(const GoalRecordingCandidate &row) const {
        if (!owners.goals || !owners.authorize_goal)
            return missing_owner();
        return record_conversation_commitments(*owners.goals, owners.events, row.binding,
                                               row.source, row.commitments, operation,
                                               owners.authorize_goal);
    }

    Result<RecurringPlanCreationReceipt>
    operator()(const RecurringPlanRecordingCandidate &row) const {
        if (!owners.goals || !owners.authorize_goal)
            return missing_owner();
        return record_conversation_recurring_plan(*owners.goals, owners.events, row.binding,
                                                  row.source, row.plan, operation,
                                                  owners.authorize_goal);
    }

    Result<WorkflowRuntime::SkillHintRecordReport>
    operator()(const SkillHintRecordingCandidate &row) const {
        if (!owners.workflow || !owners.skills || !owners.authorize_skill)
            return missing_owner();
        return owners.workflow->record_skill_hint_candidate(
            *owners.skills, row.skill, row.run, row.hint, owners.authorize_skill, operation);
    }
};

} // namespace memory_recording_detail

// Host-only, called from an Executor-managed operation for one authorized work.
// Use a blocking worker for Event batches: a Record processor may use the finite pool.
// The caller owns dependencies through the last result and keeps destination
// access current during each owner operation.
[[nodiscard]] inline Result<MemoryRecordingBatchResult>
record_memory_batch(const MemoryRecordingOwners &owners,
                    std::span<const MemoryRecordingCandidate> candidates,
                    const OperationContext &operation) {
    using namespace memory_recording_detail;
    if (operation.session.is_nil() || operation.operation.is_nil())
        return batch_error(ErrorCode::InvalidArgument, "memory recording operation is incomplete");
    if (candidates.size() > kMaxMemoryRecordingBatchSize)
        return batch_error(ErrorCode::ResourceExhausted, "memory recording batch exceeds capacity");

    MemoryRecordingBatchResult result;
    result.outcomes.reserve(candidates.size());
    const Recorder record{owners, operation};
    for (const auto &candidate : candidates) {
        if (operation.cancelled()) {
            result.interruption = ErrorCode::Cancelled;
            break;
        }
        if (operation.expired(Timestamp::now())) {
            result.interruption = ErrorCode::DeadlineExceeded;
            break;
        }
        auto outcome = std::visit(
            [&record](const auto &row) -> MemoryRecordingOutcome {
                try {
                    return record(row);
                } catch (...) {
                    return decltype(record(row)){batch_error(ErrorCode::ExecutionUncertain,
                                                             "memory recording owner interrupted")};
                }
            },
            candidate);
        std::visit(
            [&report = result.report](const auto &receipt) {
                if constexpr (std::is_same_v<std::decay_t<decltype(receipt)>,
                                             Result<ConsolidationReport>>)
                    report.collect_world_fact(receipt);
                else
                    report.collect(receipt);
            },
            outcome);
        result.outcomes.push_back(std::move(outcome));
    }
    return result;
}

} // namespace mira
