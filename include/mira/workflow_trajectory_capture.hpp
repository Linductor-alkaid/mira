#pragma once

#include <mira/environment.hpp>
#include <mira/workflow_compiler.hpp>

#include <span>

namespace mira {

// Supplied by the trusted host after verification, using its authorized
// source. The raw loop event retains only the arguments digest.
struct VerifiedTrajectoryTool final {
    OperationId operation;
    EventId verification_event;
    JsonValue arguments;
};

struct ConversationTrajectoryCapture final {
    WorkflowTrajectory trajectory;
    EventId settlement_event;
    std::vector<EventId> execution_events;
    std::vector<EventId> verification_events;
};

// Bounded, read-only projection of one complete ConversationLoop execution.
// The caller authorizes the session and verification evidence and manages the
// synchronous EventStore read through its Executor. The result is a draft.
[[nodiscard]] Result<ConversationTrajectoryCapture>
capture_conversation_trajectory(const IEventStore &events, const OperationContext &context,
                                std::span<const VerifiedTrajectoryTool> verified,
                                std::size_t max_session_events = 1024);

} // namespace mira
