#include <mira/task_recovery.hpp>

#include <algorithm>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error recovery_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.recovery";
    error.safe_message = std::move(message);
    return error;
}

} // namespace

std::string recovery_action_name(RecoveryAction action) {
    switch (action) {
    case RecoveryAction::ResumeObserving:
        return "ResumeObserving";
    case RecoveryAction::ResumeVerifying:
        return "ResumeVerifying";
    case RecoveryAction::AlreadyTerminal:
        return "AlreadyTerminal";
    case RecoveryAction::NoState:
        return "NoState";
    }
    return "Unknown";
}

RecoveryPlanner::RecoveryPlanner(IEventStore &events, ICheckpointStore &checkpoints,
                                 RecoveryPolicy policy)
    : events_(events), checkpoints_(checkpoints), policy_(policy),
      builder_(CheckpointBuilder::Config{}) {}

Result<std::vector<EventEnvelope>> RecoveryPlanner::read_task_events(TaskId task,
                                                                     SessionId session) const {
    std::vector<EventEnvelope> collected;
    std::optional<SessionSequence> after;
    while (true) {
        EventQuery query;
        query.session_id = session;
        query.after_sequence = after;
        query.limit = 1024;
        auto page = events_.read(query);
        if (!page) {
            return page.error();
        }
        if (page.value().events.empty()) {
            break;
        }
        for (const auto &event : page.value().events) {
            if (event.task_id.has_value() && *event.task_id == task) {
                collected.push_back(event);
            }
        }
        if (!page.value().has_more) {
            break;
        }
        after = page.value().events.back().session_sequence;
        if (collected.size() > policy_.max_events_scanned) {
            return recovery_error(ErrorCode::ResourceExhausted,
                                  "recovery scan exceeded the event bound");
        }
    }
    return collected;
}

Result<RecoveryOutcome> RecoveryPlanner::plan(TaskId task, SessionId session,
                                              const Timestamp &now) const {
    if (task.is_nil() || session.is_nil()) {
        return recovery_error(ErrorCode::InvalidArgument, "recovery requires task and session ids");
    }

    auto events = read_task_events(task, session);
    if (!events) {
        return events.error();
    }

    std::uint64_t durable_sequence = 0;
    for (const auto &event : events.value()) {
        durable_sequence = std::max(durable_sequence, event.session_sequence);
    }

    auto stored = checkpoints_.latest_at_or_before(task, durable_sequence);
    if (!stored) {
        return stored.error();
    }
    std::optional<TaskCheckpoint> base = std::move(stored).value();
    if (base.has_value()) {
        const auto schema =
            validate_schema_version(base->schema_version, policy_.max_supported_schema);
        if (!schema) {
            return schema.error();
        }
        // A checkpoint from another session is not a valid projection base.
        if (base->session_id != session || base->task_id != task) {
            return recovery_error(ErrorCode::InvalidState,
                                  "checkpoint does not belong to the recovering task");
        }
    }

    if (!base.has_value() && events.value().empty()) {
        RecoveryOutcome outcome;
        outcome.action = RecoveryAction::NoState;
        outcome.durable_sequence = durable_sequence;
        return outcome;
    }

    auto built = builder_.build(base, events.value(), now);
    RecoveryOutcome outcome;
    outcome.checkpoint = built.checkpoint;
    outcome.durable_sequence = durable_sequence;
    outcome.task_epoch = built.checkpoint.task_epoch;
    outcome.environment_epoch = built.checkpoint.environment_epoch;
    outcome.stats = built.stats;
    outcome.pending_side_effects = built.checkpoint.uncertain_side_effects;

    if (built.checkpoint.terminal_state.has_value()) {
        // The durable log already settled this task. A late model response or
        // action completion must never revive it.
        outcome.action = RecoveryAction::AlreadyTerminal;
        return outcome;
    }
    if (!outcome.pending_side_effects.empty()) {
        // Uncertain side effects are resolved by observing and verifying the
        // environment, never by re-dispatching the action.
        outcome.action = RecoveryAction::ResumeObserving;
        return outcome;
    }
    outcome.action = built.checkpoint.verification_pending ? RecoveryAction::ResumeVerifying
                                                           : RecoveryAction::ResumeObserving;
    return outcome;
}

} // namespace mira
