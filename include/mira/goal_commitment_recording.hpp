#pragma once

#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/sqlite_goal_store.hpp>

#include <span>

namespace mira {

struct GoalCommitmentBinding final {
    RuntimeId runtime;
    GoalOwner owner;
};

struct GoalCommitmentSource final {
    SessionSequence settlement_sequence = 0;
    EventId settlement_event;
};

struct GoalCommitmentCandidate final {
    std::string quote;
    GoalTime due_at;
    std::optional<GoalTime> expires_at;
};

struct GoalCommitmentExcerpt final {
    GoalCommitmentCandidate candidate;
    std::optional<std::size_t> quote_offset = std::nullopt; // UTF-8 byte offset in the answer.
};

// Checks live access to the source work and its exact goal owner. The
// authorized SqliteGoalStore separately guards creation on its worker.
using GoalCommitmentAuthorizer =
    std::function<bool(const GoalCommitmentBinding &, const OperationContext &)>;

// The trusted host interprets the commitment and its dates, then runs this
// bounded operation on its Executor. Uses the first commitment identity.
[[nodiscard]] Result<GoalCreationReceipt> record_conversation_commitment(
    SqliteGoalStore &goals, const IEventStore &events, const GoalCommitmentBinding &binding,
    const GoalCommitmentSource &source, const GoalCommitmentCandidate &candidate,
    const OperationContext &operation, const GoalCommitmentAuthorizer &authorize);

// Receipts follow answer order. An omitted offset must identify a unique
// excerpt in a multi-candidate batch. Replays retain the original positions.
[[nodiscard]] Result<std::vector<GoalCreationReceipt>> record_conversation_commitments(
    SqliteGoalStore &goals, const IEventStore &events, const GoalCommitmentBinding &binding,
    const GoalCommitmentSource &source, std::span<const GoalCommitmentExcerpt> excerpts,
    const OperationContext &operation, const GoalCommitmentAuthorizer &authorize);

} // namespace mira
