#include <mira/task_checkpoint.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error checkpoint_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.checkpoint";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::int64_t wall_nanos(const Timestamp &stamp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.wall.time_since_epoch())
        .count();
}

[[nodiscard]] Timestamp timestamp_from_nanos(std::int64_t nanos, std::int64_t monotonic_nanos) {
    Timestamp stamp;
    stamp.wall = std::chrono::system_clock::time_point{} + std::chrono::nanoseconds(nanos);
    stamp.monotonic =
        std::chrono::steady_clock::time_point{} + std::chrono::nanoseconds(monotonic_nanos);
    return stamp;
}

[[nodiscard]] std::optional<TaskState> task_state_from_name(std::string_view name) {
    static const std::pair<std::string_view, TaskState> kStates[] = {
        {"Idle", TaskState::Idle},
        {"Observing", TaskState::Observing},
        {"Reasoning", TaskState::Reasoning},
        {"Planning", TaskState::Planning},
        {"Acting", TaskState::Acting},
        {"Verifying", TaskState::Verifying},
        {"Recovering", TaskState::Recovering},
        {"Pausing", TaskState::Pausing},
        {"Paused", TaskState::Paused},
        {"TakeoverSettling", TaskState::TakeoverSettling},
        {"SuspendedForTakeover", TaskState::SuspendedForTakeover},
        {"Cancelling", TaskState::Cancelling},
        {"Completed", TaskState::Completed},
        {"Failed", TaskState::Failed},
        {"Cancelled", TaskState::Cancelled},
    };
    for (const auto &[state_name, state] : kStates) {
        if (state_name == name) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] JsonValue event_ids_to_json(const std::vector<EventId> &ids) {
    JsonValue::Array array;
    for (const auto &id : ids) {
        array.emplace_back(id.to_string());
    }
    return JsonValue(std::move(array));
}

[[nodiscard]] std::vector<EventId> event_ids_from_json(const JsonValue &json) {
    std::vector<EventId> ids;
    if (json.is_array()) {
        for (const auto &entry : *json.as_array()) {
            if (auto parsed = EventId::parse(entry.as_string() ? *entry.as_string() : ""); parsed) {
                ids.emplace_back(*parsed);
            }
        }
    }
    return ids;
}

[[nodiscard]] JsonValue constraint_to_json(const CheckpointConstraint &constraint) {
    JsonValue::Object object;
    object.emplace_back("key", constraint.key);
    object.emplace_back("requirement", constraint.requirement);
    object.emplace_back("safety", constraint.safety);
    object.emplace_back("provenance", event_ids_to_json(constraint.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] CheckpointConstraint constraint_from(const JsonValue &json) {
    CheckpointConstraint constraint;
    if (const auto *key = json.find("key"); key != nullptr && key->is_string()) {
        constraint.key = *key->as_string();
    }
    if (const auto *requirement = json.find("requirement");
        requirement != nullptr && requirement->is_string()) {
        constraint.requirement = *requirement->as_string();
    }
    if (const auto *safety = json.find("safety"); safety != nullptr && safety->is_boolean()) {
        constraint.safety = safety->as_boolean().value();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        constraint.provenance = event_ids_from_json(*provenance);
    }
    return constraint;
}

[[nodiscard]] JsonValue fact_to_json(const VerifiedFact &fact) {
    JsonValue::Object object;
    object.emplace_back("key", fact.key);
    object.emplace_back("value", fact.value);
    object.emplace_back("provenance", event_ids_to_json(fact.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] VerifiedFact fact_from(const JsonValue &json) {
    VerifiedFact fact;
    if (const auto *key = json.find("key"); key != nullptr && key->is_string()) {
        fact.key = *key->as_string();
    }
    if (const auto *value = json.find("value"); value != nullptr && value->is_string()) {
        fact.value = *value->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        fact.provenance = event_ids_from_json(*provenance);
    }
    return fact;
}

[[nodiscard]] JsonValue step_to_json(const CompletedStep &step) {
    JsonValue::Object object;
    object.emplace_back("step_id", step.step_id.to_string());
    object.emplace_back("summary", step.summary);
    object.emplace_back("provenance", event_ids_to_json(step.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] CompletedStep step_from(const JsonValue &json) {
    CompletedStep step;
    if (const auto *id = json.find("step_id"); id != nullptr && id->is_string()) {
        if (auto parsed = StepId::parse(*id->as_string()); parsed) {
            step.step_id = *parsed;
        }
    }
    if (const auto *summary = json.find("summary"); summary != nullptr && summary->is_string()) {
        step.summary = *summary->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        step.provenance = event_ids_from_json(*provenance);
    }
    return step;
}

[[nodiscard]] JsonValue objective_to_json(const PendingObjective &objective) {
    JsonValue::Object object;
    object.emplace_back("objective", objective.objective);
    object.emplace_back("provenance", event_ids_to_json(objective.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] PendingObjective objective_from(const JsonValue &json) {
    PendingObjective objective;
    if (const auto *text = json.find("objective"); text != nullptr && text->is_string()) {
        objective.objective = *text->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        objective.provenance = event_ids_from_json(*provenance);
    }
    return objective;
}

[[nodiscard]] JsonValue issue_to_json(const UnresolvedIssue &issue) {
    JsonValue::Object object;
    object.emplace_back("issue", issue.issue);
    object.emplace_back("provenance", event_ids_to_json(issue.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] UnresolvedIssue issue_from(const JsonValue &json) {
    UnresolvedIssue issue;
    if (const auto *text = json.find("issue"); text != nullptr && text->is_string()) {
        issue.issue = *text->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        issue.provenance = event_ids_from_json(*provenance);
    }
    return issue;
}

[[nodiscard]] JsonValue side_effect_to_json(const UncertainSideEffect &effect) {
    JsonValue::Object object;
    object.emplace_back("action_id", effect.action_id.to_string());
    object.emplace_back("action_kind", effect.action_kind);
    object.emplace_back("reason", effect.reason);
    object.emplace_back("provenance", event_ids_to_json(effect.provenance));
    return JsonValue(std::move(object));
}

[[nodiscard]] UncertainSideEffect side_effect_from(const JsonValue &json) {
    UncertainSideEffect effect;
    if (const auto *id = json.find("action_id"); id != nullptr && id->is_string()) {
        if (auto parsed = ActionId::parse(*id->as_string()); parsed) {
            effect.action_id = *parsed;
        }
    }
    if (const auto *kind = json.find("action_kind"); kind != nullptr && kind->is_string()) {
        effect.action_kind = *kind->as_string();
    }
    if (const auto *reason = json.find("reason"); reason != nullptr && reason->is_string()) {
        effect.reason = *reason->as_string();
    }
    if (const auto *provenance = json.find("provenance"); provenance != nullptr) {
        effect.provenance = event_ids_from_json(*provenance);
    }
    return effect;
}

[[nodiscard]] JsonValue action_summary_to_json(const CheckpointActionSummary &summary) {
    JsonValue::Object object;
    object.emplace_back("action_id", summary.action_id.to_string());
    object.emplace_back("kind", summary.kind);
    object.emplace_back("outcome", summary.outcome);
    object.emplace_back("sequence", static_cast<std::int64_t>(summary.sequence));
    return JsonValue(std::move(object));
}

[[nodiscard]] CheckpointActionSummary action_summary_from(const JsonValue &json) {
    CheckpointActionSummary summary;
    if (const auto *id = json.find("action_id"); id != nullptr && id->is_string()) {
        if (auto parsed = ActionId::parse(*id->as_string()); parsed) {
            summary.action_id = *parsed;
        }
    }
    if (const auto *kind = json.find("kind"); kind != nullptr && kind->is_string()) {
        summary.kind = *kind->as_string();
    }
    if (const auto *outcome = json.find("outcome"); outcome != nullptr && outcome->is_string()) {
        summary.outcome = *outcome->as_string();
    }
    if (const auto *sequence = json.find("sequence");
        sequence != nullptr && sequence->is_integer()) {
        summary.sequence = static_cast<std::uint64_t>(sequence->as_integer().value());
    }
    return summary;
}

[[nodiscard]] JsonValue observation_ref_to_json(const CheckpointObservationRef &reference) {
    JsonValue::Object object;
    object.emplace_back("observation_id", reference.observation_id.to_string());
    object.emplace_back("digest", reference.digest.to_string());
    object.emplace_back("sequence", static_cast<std::int64_t>(reference.sequence));
    return JsonValue(std::move(object));
}

[[nodiscard]] CheckpointObservationRef observation_ref_from(const JsonValue &json) {
    CheckpointObservationRef reference;
    if (const auto *id = json.find("observation_id"); id != nullptr && id->is_string()) {
        if (auto parsed = ObservationId::parse(*id->as_string()); parsed) {
            reference.observation_id = *parsed;
        }
    }
    if (const auto *digest = json.find("digest"); digest != nullptr && digest->is_string()) {
        if (auto parsed = digest_from_hex(*digest->as_string()); parsed) {
            reference.digest = *parsed;
        }
    }
    if (const auto *sequence = json.find("sequence");
        sequence != nullptr && sequence->is_integer()) {
        reference.sequence = static_cast<std::uint64_t>(sequence->as_integer().value());
    }
    return reference;
}

// ---------------------------------------------------------------------------
// Reducer event application
// ---------------------------------------------------------------------------

struct PipeFields final {
    ActionId action_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::string target_type;
    std::string suffix;
};

[[nodiscard]] std::optional<std::uint64_t> parse_decimal(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit_value = static_cast<std::uint64_t>(digit) - '0';
        const std::uint64_t next = value * 10 + digit_value;
        if (next < value) {
            return std::nullopt; // overflow
        }
        value = next;
    }
    return value;
}

[[nodiscard]] std::optional<PipeFields> parse_pipe_payload(const std::string &data) {
    std::vector<std::string> fields;
    std::istringstream stream(data);
    std::string field;
    while (std::getline(stream, field, '|')) {
        fields.push_back(field);
    }
    if (fields.size() < 8) {
        return std::nullopt;
    }
    PipeFields parsed;
    if (auto id = ActionId::parse(fields[0]); id) {
        parsed.action_id = *id;
    } else {
        return std::nullopt;
    }
    if (auto task = TaskId::parse(fields[1]); task) {
        parsed.task_id = *task;
    } else {
        return std::nullopt;
    }
    const auto task_epoch = parse_decimal(fields[2]);
    const auto environment_epoch = parse_decimal(fields[3]);
    if (!task_epoch || !environment_epoch) {
        return std::nullopt;
    }
    parsed.task_epoch = *task_epoch;
    parsed.environment_epoch = *environment_epoch;
    parsed.target_type = fields[5];
    if (fields.size() > 8) {
        parsed.suffix = fields[8];
    }
    return parsed;
}

void apply_recent_action(TaskCheckpoint &checkpoint, const CheckpointBuilder::Config &config,
                         const ActionId &action_id, const std::string &kind,
                         const std::string &outcome, std::uint64_t sequence) {
    for (auto &summary : checkpoint.recent_actions) {
        if (summary.action_id == action_id && !action_id.is_nil()) {
            summary.outcome = outcome;
            return;
        }
    }
    checkpoint.recent_actions.push_back(
        CheckpointActionSummary{action_id, kind, outcome, sequence});
    if (checkpoint.recent_actions.size() > config.max_recent_actions) {
        checkpoint.recent_actions.erase(checkpoint.recent_actions.begin());
    }
}

void apply_json_event(TaskCheckpoint &checkpoint, const CheckpointBuilder::Config &config,
                      const std::string &type, const JsonValue &payload, const EventEnvelope &event,
                      ReducerStats &stats) {
    const auto require_string = [&payload](const char *key) -> const std::string * {
        const auto *value = payload.find(key);
        return value != nullptr && value->is_string() ? value->as_string() : nullptr;
    };
    if (type == "TaskGoalSet") {
        const auto *statement = require_string("statement");
        if (statement == nullptr) {
            ++stats.malformed;
            return;
        }
        checkpoint.goal_statement = *statement;
        const auto *criterion = require_string("success_criterion");
        checkpoint.success_criterion =
            criterion != nullptr ? std::optional<std::string>(*criterion) : std::nullopt;
    } else if (type == "TaskConstraintAdded") {
        const auto *key = require_string("key");
        const auto *requirement = require_string("requirement");
        if (key == nullptr || requirement == nullptr) {
            ++stats.malformed;
            return;
        }
        CheckpointConstraint constraint;
        constraint.key = *key;
        constraint.requirement = *requirement;
        if (const auto *safety = payload.find("safety");
            safety != nullptr && safety->is_boolean()) {
            constraint.safety = safety->as_boolean().value();
        }
        constraint.provenance = {event.event_id};
        auto existing = std::find_if(
            checkpoint.constraints.begin(), checkpoint.constraints.end(),
            [&key](const CheckpointConstraint &candidate) { return candidate.key == *key; });
        if (existing != checkpoint.constraints.end()) {
            *existing = std::move(constraint);
        } else {
            checkpoint.constraints.push_back(std::move(constraint));
        }
    } else if (type == "TaskConstraintRemoved") {
        const auto *key = require_string("key");
        if (key == nullptr) {
            ++stats.malformed;
            return;
        }
        std::erase_if(checkpoint.constraints, [&key](const CheckpointConstraint &candidate) {
            return candidate.key == *key;
        });
    } else if (type == "TaskFactVerified") {
        const auto *key = require_string("key");
        const auto *value = require_string("value");
        if (key == nullptr || value == nullptr) {
            ++stats.malformed;
            return;
        }
        VerifiedFact fact;
        fact.key = *key;
        fact.value = *value;
        fact.provenance = {event.event_id};
        auto existing =
            std::find_if(checkpoint.verified_facts.begin(), checkpoint.verified_facts.end(),
                         [&key](const VerifiedFact &candidate) { return candidate.key == *key; });
        if (existing != checkpoint.verified_facts.end()) {
            *existing = std::move(fact);
        } else {
            checkpoint.verified_facts.push_back(std::move(fact));
            if (checkpoint.verified_facts.size() > config.max_verified_facts) {
                checkpoint.verified_facts.erase(checkpoint.verified_facts.begin());
            }
        }
    } else if (type == "TaskStepCompleted") {
        const auto *summary = require_string("summary");
        const auto *step = require_string("step_id");
        if (summary == nullptr || step == nullptr) {
            ++stats.malformed;
            return;
        }
        CompletedStep completed;
        if (auto parsed = StepId::parse(*step); parsed) {
            completed.step_id = *parsed;
        } else {
            ++stats.malformed;
            return;
        }
        completed.summary = *summary;
        completed.provenance = {event.event_id};
        checkpoint.completed_steps.push_back(std::move(completed));
        if (checkpoint.completed_steps.size() > config.max_completed_steps) {
            checkpoint.completed_steps.erase(checkpoint.completed_steps.begin());
        }
    } else if (type == "TaskObjectiveAdded") {
        const auto *objective = require_string("objective");
        if (objective == nullptr) {
            ++stats.malformed;
            return;
        }
        const auto found =
            std::any_of(checkpoint.pending_objectives.begin(), checkpoint.pending_objectives.end(),
                        [&objective](const PendingObjective &candidate) {
                            return candidate.objective == *objective;
                        });
        if (!found) {
            checkpoint.pending_objectives.push_back(PendingObjective{*objective, {event.event_id}});
            if (checkpoint.pending_objectives.size() > config.max_pending_objectives) {
                checkpoint.pending_objectives.erase(checkpoint.pending_objectives.begin());
            }
        }
    } else if (type == "TaskObjectiveCompleted") {
        const auto *objective = require_string("objective");
        if (objective == nullptr) {
            ++stats.malformed;
            return;
        }
        std::erase_if(checkpoint.pending_objectives,
                      [&objective](const PendingObjective &candidate) {
                          return candidate.objective == *objective;
                      });
    } else if (type == "TaskIssueRaised") {
        const auto *issue = require_string("issue");
        if (issue == nullptr) {
            ++stats.malformed;
            return;
        }
        const auto found = std::any_of(
            checkpoint.unresolved_issues.begin(), checkpoint.unresolved_issues.end(),
            [&issue](const UnresolvedIssue &candidate) { return candidate.issue == *issue; });
        if (!found) {
            checkpoint.unresolved_issues.push_back(UnresolvedIssue{*issue, {event.event_id}});
            if (checkpoint.unresolved_issues.size() > config.max_unresolved_issues) {
                checkpoint.unresolved_issues.erase(checkpoint.unresolved_issues.begin());
            }
        }
    } else if (type == "TaskIssueResolved") {
        const auto *issue = require_string("issue");
        if (issue == nullptr) {
            ++stats.malformed;
            return;
        }
        std::erase_if(checkpoint.unresolved_issues, [&issue](const UnresolvedIssue &candidate) {
            return candidate.issue == *issue;
        });
    } else if (type == "TaskStateChanged") {
        const auto *to = require_string("to");
        if (to == nullptr) {
            ++stats.malformed;
            return;
        }
        const auto target = task_state_from_name(*to);
        if (!target) {
            ++stats.malformed;
            return;
        }
        // Terminal states are idempotent; a post-terminal revival attempt is a
        // malformed (rejected) transition rather than a silent resurrection.
        if (checkpoint.terminal_state.has_value()) {
            ++stats.malformed;
            return;
        }
        if (is_terminal(*target)) {
            checkpoint.terminal_state = *target;
        }
    } else if (type == "TaskEpochAdvanced") {
        const auto *epoch = payload.find("epoch");
        if (epoch == nullptr || !epoch->is_integer() || epoch->as_integer().value() < 0) {
            ++stats.malformed;
            return;
        }
        checkpoint.task_epoch = std::max<std::uint64_t>(
            checkpoint.task_epoch, static_cast<std::uint64_t>(epoch->as_integer().value()));
    } else if (type == "EnvironmentEpochChanged") {
        const auto *epoch = payload.find("epoch");
        if (epoch == nullptr || !epoch->is_integer() || epoch->as_integer().value() < 0) {
            ++stats.malformed;
            return;
        }
        checkpoint.environment_epoch = std::max<std::uint64_t>(
            checkpoint.environment_epoch, static_cast<std::uint64_t>(epoch->as_integer().value()));
    } else if (type == "ObservationRecorded") {
        const auto *observation = require_string("observation_id");
        if (observation == nullptr) {
            ++stats.malformed;
            return;
        }
        CheckpointObservationRef reference;
        if (auto parsed = ObservationId::parse(*observation); parsed) {
            reference.observation_id = *parsed;
        } else {
            ++stats.malformed;
            return;
        }
        if (const auto *digest = require_string("digest"); digest != nullptr) {
            if (auto parsed = digest_from_hex(*digest); parsed) {
                reference.digest = *parsed;
            }
        }
        reference.sequence = event.session_sequence;
        checkpoint.current_observation = std::move(reference);
    } else if (type == "ActionDispatched") {
        const auto *action = require_string("action");
        if (action == nullptr) {
            ++stats.malformed;
            return;
        }
        apply_recent_action(checkpoint, config, ActionId{}, *action, "dispatched",
                            event.session_sequence);
        checkpoint.verification_pending = true;
    } else if (type == "VerificationResult") {
        // Any verification observes the environment after dispatched actions:
        // pending verification and recorded uncertainty are settled.
        checkpoint.verification_pending = false;
        checkpoint.uncertain_side_effects.clear();
    } else if (type == "LoopSettled") {
        const auto *outcome = require_string("outcome");
        if (outcome == nullptr) {
            ++stats.malformed;
            return;
        }
        std::optional<TaskState> terminal;
        if (*outcome == "Completed") {
            terminal = TaskState::Completed;
        } else if (*outcome == "Failed" || *outcome == "MaxSteps") {
            terminal = TaskState::Failed;
        } else if (*outcome == "Cancelled") {
            terminal = TaskState::Cancelled;
        }
        if (terminal.has_value() && !checkpoint.terminal_state.has_value()) {
            checkpoint.terminal_state = *terminal;
        }
    } else {
        ++stats.ignored;
    }
}

void apply_event(TaskCheckpoint &checkpoint, const CheckpointBuilder::Config &config,
                 const EventEnvelope &event, ReducerStats &stats) {
    const auto &type = event.payload.type;
    if (type == "ActionDispatchStarted" || type == "ActionReceipt" ||
        type == "ActionExecutionUncertain") {
        const auto parsed = parse_pipe_payload(event.payload.data);
        if (!parsed) {
            ++stats.malformed;
            return;
        }
        checkpoint.task_epoch = std::max(checkpoint.task_epoch, parsed->task_epoch);
        checkpoint.environment_epoch =
            std::max(checkpoint.environment_epoch, parsed->environment_epoch);
        if (type == "ActionDispatchStarted") {
            apply_recent_action(checkpoint, config, parsed->action_id, parsed->target_type,
                                "dispatched", event.session_sequence);
            checkpoint.verification_pending = true;
        } else if (type == "ActionReceipt") {
            apply_recent_action(checkpoint, config, parsed->action_id, parsed->target_type,
                                "receipt", event.session_sequence);
        } else {
            UncertainSideEffect effect;
            effect.action_id = parsed->action_id;
            effect.action_kind = parsed->target_type;
            effect.reason = parsed->suffix.empty() ? "execution uncertain" : parsed->suffix;
            effect.provenance = {event.event_id};
            const auto existing = std::find_if(checkpoint.uncertain_side_effects.begin(),
                                               checkpoint.uncertain_side_effects.end(),
                                               [&effect](const UncertainSideEffect &candidate) {
                                                   return candidate.action_id == effect.action_id;
                                               });
            if (existing == checkpoint.uncertain_side_effects.end()) {
                checkpoint.uncertain_side_effects.push_back(std::move(effect));
            }
        }
        ++stats.applied;
        return;
    }
    const auto payload = parse_json(event.payload.data);
    if (!payload) {
        ++stats.malformed;
        return;
    }
    apply_json_event(checkpoint, config, type, payload.value(), event, stats);
    // JSON vocabulary events that reach this point and were not counted as
    // malformed or ignored are applied.
    const bool counted =
        type == "TaskGoalSet" || type == "TaskConstraintAdded" || type == "TaskConstraintRemoved" ||
        type == "TaskFactVerified" || type == "TaskStepCompleted" || type == "TaskObjectiveAdded" ||
        type == "TaskObjectiveCompleted" || type == "TaskIssueRaised" ||
        type == "TaskIssueResolved" || type == "TaskStateChanged" || type == "TaskEpochAdvanced" ||
        type == "EnvironmentEpochChanged" || type == "ObservationRecorded" ||
        type == "ActionDispatched" || type == "VerificationResult" || type == "LoopSettled";
    if (counted) {
        ++stats.applied;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// TaskCheckpoint
// ---------------------------------------------------------------------------

Result<void> TaskCheckpoint::validate() const {
    const auto schema = validate_schema_version(schema_version, checkpoint_schema_current());
    if (!schema) {
        return schema.error();
    }
    if (id.is_nil() || task_id.is_nil() || session_id.is_nil()) {
        return checkpoint_error(ErrorCode::InvalidArgument, "checkpoint ids must be non-nil");
    }
    if (constraints.size() > 1024 || verified_facts.size() > 4096 ||
        completed_steps.size() > 8192 || pending_objectives.size() > 2048 ||
        unresolved_issues.size() > 2048 || uncertain_side_effects.size() > 4096 ||
        recent_actions.size() > 128) {
        return checkpoint_error(ErrorCode::InvalidArgument, "checkpoint vector exceeds bounds");
    }
    if (terminal_state.has_value() && !is_terminal(*terminal_state)) {
        return checkpoint_error(ErrorCode::InvalidArgument,
                                "checkpoint terminal state must be terminal");
    }
    for (const auto &constraint : constraints) {
        if (constraint.key.empty() || constraint.requirement.empty()) {
            return checkpoint_error(ErrorCode::InvalidArgument,
                                    "checkpoint constraints must be keyed");
        }
    }
    return Result<void>{};
}

Hash TaskCheckpoint::projection_digest() const {
    JsonValue::Object object;
    object.emplace_back("schema_version",
                        JsonValue::Object{
                            {"major", static_cast<std::int64_t>(schema_version.major)},
                            {"minor", static_cast<std::int64_t>(schema_version.minor)},
                        });
    object.emplace_back("task_id", task_id.to_string());
    object.emplace_back("session_id", session_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(task_epoch));
    object.emplace_back("environment_epoch", static_cast<std::int64_t>(environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(through_event_sequence));
    object.emplace_back("goal_statement", goal_statement);
    if (success_criterion.has_value()) {
        object.emplace_back("success_criterion", *success_criterion);
    }
    JsonValue::Array constraint_entries;
    for (const auto &constraint : this->constraints) {
        constraint_entries.emplace_back(constraint_to_json(constraint));
    }
    object.emplace_back("constraints", JsonValue(std::move(constraint_entries)));
    JsonValue::Array facts;
    for (const auto &fact : verified_facts) {
        facts.emplace_back(fact_to_json(fact));
    }
    object.emplace_back("verified_facts", JsonValue(std::move(facts)));
    JsonValue::Array steps;
    for (const auto &step : completed_steps) {
        steps.emplace_back(step_to_json(step));
    }
    object.emplace_back("completed_steps", JsonValue(std::move(steps)));
    JsonValue::Array objectives;
    for (const auto &objective : pending_objectives) {
        objectives.emplace_back(objective_to_json(objective));
    }
    object.emplace_back("pending_objectives", JsonValue(std::move(objectives)));
    JsonValue::Array issues;
    for (const auto &issue : unresolved_issues) {
        issues.emplace_back(issue_to_json(issue));
    }
    object.emplace_back("unresolved_issues", JsonValue(std::move(issues)));
    JsonValue::Array effects;
    for (const auto &effect : uncertain_side_effects) {
        effects.emplace_back(side_effect_to_json(effect));
    }
    object.emplace_back("uncertain_side_effects", JsonValue(std::move(effects)));
    if (current_observation.has_value()) {
        object.emplace_back("current_observation", observation_ref_to_json(*current_observation));
    }
    JsonValue::Array actions;
    for (const auto &summary : recent_actions) {
        actions.emplace_back(action_summary_to_json(summary));
    }
    object.emplace_back("recent_actions", JsonValue(std::move(actions)));
    object.emplace_back("verification_pending", verification_pending);
    if (terminal_state.has_value()) {
        object.emplace_back(
            "terminal_state",
            terminal_state.has_value() && *terminal_state == TaskState::Completed
                ? "Completed"
                : (*terminal_state == TaskState::Cancelled ? "Cancelled" : "Failed"));
    }
    return canonical_json_digest(JsonValue(std::move(object)));
}

JsonValue checkpoint_to_json(const TaskCheckpoint &checkpoint) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(checkpoint.schema_version.major)},
                          {"minor", static_cast<std::int64_t>(checkpoint.schema_version.minor)}});
    object.emplace_back("id", checkpoint.id.to_string());
    object.emplace_back("task_id", checkpoint.task_id.to_string());
    object.emplace_back("session_id", checkpoint.session_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(checkpoint.task_epoch));
    object.emplace_back("environment_epoch",
                        static_cast<std::int64_t>(checkpoint.environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(checkpoint.through_event_sequence));
    object.emplace_back("created_at", wall_nanos(checkpoint.created_at));
    object.emplace_back("created_at_monotonic",
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            checkpoint.created_at.monotonic.time_since_epoch())
                            .count());
    object.emplace_back("goal_statement", checkpoint.goal_statement);
    if (checkpoint.success_criterion.has_value()) {
        object.emplace_back("success_criterion", *checkpoint.success_criterion);
    }
    JsonValue::Array constraints;
    for (const auto &constraint : checkpoint.constraints) {
        constraints.emplace_back(constraint_to_json(constraint));
    }
    object.emplace_back("constraints", JsonValue(std::move(constraints)));
    JsonValue::Array facts;
    for (const auto &fact : checkpoint.verified_facts) {
        facts.emplace_back(fact_to_json(fact));
    }
    object.emplace_back("verified_facts", JsonValue(std::move(facts)));
    JsonValue::Array steps;
    for (const auto &step : checkpoint.completed_steps) {
        steps.emplace_back(step_to_json(step));
    }
    object.emplace_back("completed_steps", JsonValue(std::move(steps)));
    JsonValue::Array objectives;
    for (const auto &objective : checkpoint.pending_objectives) {
        objectives.emplace_back(objective_to_json(objective));
    }
    object.emplace_back("pending_objectives", JsonValue(std::move(objectives)));
    JsonValue::Array issues;
    for (const auto &issue : checkpoint.unresolved_issues) {
        issues.emplace_back(issue_to_json(issue));
    }
    object.emplace_back("unresolved_issues", JsonValue(std::move(issues)));
    JsonValue::Array effects;
    for (const auto &effect : checkpoint.uncertain_side_effects) {
        effects.emplace_back(side_effect_to_json(effect));
    }
    object.emplace_back("uncertain_side_effects", JsonValue(std::move(effects)));
    if (checkpoint.current_observation.has_value()) {
        object.emplace_back("current_observation",
                            observation_ref_to_json(*checkpoint.current_observation));
    }
    JsonValue::Array actions;
    for (const auto &summary : checkpoint.recent_actions) {
        actions.emplace_back(action_summary_to_json(summary));
    }
    object.emplace_back("recent_actions", JsonValue(std::move(actions)));
    object.emplace_back("verification_pending", checkpoint.verification_pending);
    if (checkpoint.terminal_state.has_value()) {
        const char *name = "Failed";
        if (*checkpoint.terminal_state == TaskState::Completed) {
            name = "Completed";
        } else if (*checkpoint.terminal_state == TaskState::Cancelled) {
            name = "Cancelled";
        }
        object.emplace_back("terminal_state", name);
    }
    if (checkpoint.narrative_summary.has_value()) {
        object.emplace_back("narrative_summary", *checkpoint.narrative_summary);
    }
    return JsonValue(std::move(object));
}

Result<TaskCheckpoint> checkpoint_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return checkpoint_error(ErrorCode::InvalidArgument, "checkpoint must be an object");
    }
    TaskCheckpoint checkpoint;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto *major = schema->find("major");
        const auto *minor = schema->find("minor");
        if (major != nullptr && major->is_integer() && minor != nullptr && minor->is_integer()) {
            checkpoint.schema_version =
                SchemaVersion{static_cast<std::uint16_t>(major->as_integer().value()),
                              static_cast<std::uint16_t>(minor->as_integer().value())};
        }
        const auto supported =
            validate_schema_version(checkpoint.schema_version, checkpoint_schema_current());
        if (!supported) {
            return supported.error();
        }
    }
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return checkpoint_error(ErrorCode::InvalidArgument, "checkpoint requires an id");
    }
    if (auto parsed = CheckpointId::parse(*id->as_string()); parsed) {
        checkpoint.id = *parsed;
    } else {
        return checkpoint_error(ErrorCode::InvalidArgument, "checkpoint id is malformed");
    }
    const auto *task = json.find("task_id");
    if (task != nullptr && task->is_string()) {
        if (auto parsed = TaskId::parse(*task->as_string()); parsed) {
            checkpoint.task_id = *parsed;
        }
    }
    const auto *session = json.find("session_id");
    if (session != nullptr && session->is_string()) {
        if (auto parsed = SessionId::parse(*session->as_string()); parsed) {
            checkpoint.session_id = *parsed;
        }
    }
    const auto read_u64 = [&json](const char *key, std::uint64_t &field) {
        if (const auto *value = json.find(key); value != nullptr && value->is_integer()) {
            field = static_cast<std::uint64_t>(value->as_integer().value());
        }
    };
    read_u64("task_epoch", checkpoint.task_epoch);
    read_u64("environment_epoch", checkpoint.environment_epoch);
    read_u64("through_event_sequence", checkpoint.through_event_sequence);
    const auto *created = json.find("created_at");
    const auto *monotonic = json.find("created_at_monotonic");
    if (created != nullptr && created->is_integer()) {
        checkpoint.created_at = timestamp_from_nanos(
            created->as_integer().value(),
            monotonic != nullptr && monotonic->is_integer() ? monotonic->as_integer().value() : 0);
    }
    if (const auto *goal = json.find("goal_statement"); goal != nullptr && goal->is_string()) {
        checkpoint.goal_statement = *goal->as_string();
    }
    if (const auto *criterion = json.find("success_criterion");
        criterion != nullptr && criterion->is_string()) {
        checkpoint.success_criterion = *criterion->as_string();
    }
    const auto read_array = [&json](const char *key, auto &&decode, auto &target) {
        const auto *array = json.find(key);
        if (array == nullptr || !array->is_array()) {
            return;
        }
        for (const auto &entry : *array->as_array()) {
            if (entry.is_object()) {
                target.push_back(decode(entry));
            }
        }
    };
    read_array("constraints", constraint_from, checkpoint.constraints);
    read_array("verified_facts", fact_from, checkpoint.verified_facts);
    read_array("completed_steps", step_from, checkpoint.completed_steps);
    read_array("pending_objectives", objective_from, checkpoint.pending_objectives);
    read_array("unresolved_issues", issue_from, checkpoint.unresolved_issues);
    read_array("uncertain_side_effects", side_effect_from, checkpoint.uncertain_side_effects);
    read_array("recent_actions", action_summary_from, checkpoint.recent_actions);
    if (const auto *observation = json.find("current_observation");
        observation != nullptr && observation->is_object()) {
        checkpoint.current_observation = observation_ref_from(*observation);
    }
    if (const auto *pending = json.find("verification_pending");
        pending != nullptr && pending->is_boolean()) {
        checkpoint.verification_pending = pending->as_boolean().value();
    }
    if (const auto *terminal = json.find("terminal_state");
        terminal != nullptr && terminal->is_string()) {
        checkpoint.terminal_state = task_state_from_name(*terminal->as_string());
    }
    if (const auto *narrative = json.find("narrative_summary");
        narrative != nullptr && narrative->is_string()) {
        checkpoint.narrative_summary = *narrative->as_string();
    }
    return checkpoint;
}

// ---------------------------------------------------------------------------
// CheckpointBuilder
// ---------------------------------------------------------------------------

CheckpointBuilder::CheckpointBuilder(Config config) : config_(config) {}

CheckpointBuildResult CheckpointBuilder::build(const std::optional<TaskCheckpoint> &base,
                                               std::span<const EventEnvelope> events,
                                               const Timestamp &now) const {
    TaskCheckpoint checkpoint;
    if (base.has_value()) {
        checkpoint = *base;
    } else {
        // A full rebuild owns no task identity of its own; adopt it from the
        // first task-scoped event so foreign-task filtering stays correct.
        for (const auto &event : events) {
            if (event.task_id.has_value()) {
                checkpoint.task_id = *event.task_id;
                checkpoint.session_id = event.session_id;
                break;
            }
        }
    }
    ReducerStats stats;
    std::uint64_t last_sequence = checkpoint.through_event_sequence;
    for (const auto &event : events) {
        if (event.task_id.has_value() && *event.task_id != checkpoint.task_id) {
            ++stats.stale_skipped;
            continue;
        }
        if (event.session_sequence <= last_sequence) {
            ++stats.stale_skipped;
            continue;
        }
        last_sequence = event.session_sequence;
        checkpoint.through_event_sequence = event.session_sequence;
        apply_event(checkpoint, config_, event, stats);
    }
    checkpoint.id = CheckpointId::generate();
    checkpoint.created_at = now;
    return CheckpointBuildResult{std::move(checkpoint), stats};
}

// ---------------------------------------------------------------------------
// MemoryCheckpointStore
// ---------------------------------------------------------------------------

class MemoryCheckpointStore::Impl final {
  public:
    explicit Impl(std::size_t max_per_task) : max_per_task_(max_per_task) {}

    std::mutex mutex;
    std::map<TaskId, std::vector<TaskCheckpoint>> tasks;
    std::size_t max_per_task_;
    std::size_t erasures = 0;
};

MemoryCheckpointStore::MemoryCheckpointStore(std::size_t max_checkpoints_per_task)
    : impl_(std::make_unique<Impl>(max_checkpoints_per_task)) {}

MemoryCheckpointStore::~MemoryCheckpointStore() = default;

Result<void> MemoryCheckpointStore::put(const TaskCheckpoint &checkpoint) {
    const auto valid = checkpoint.validate();
    if (!valid) {
        return valid;
    }
    std::lock_guard lock(impl_->mutex);
    auto &entries = impl_->tasks[checkpoint.task_id];
    const auto existing = std::find_if(
        entries.begin(), entries.end(),
        [&checkpoint](const TaskCheckpoint &candidate) { return candidate.id == checkpoint.id; });
    if (existing != entries.end()) {
        *existing = checkpoint;
        return Result<void>{};
    }
    entries.push_back(checkpoint);
    std::sort(entries.begin(), entries.end(),
              [](const TaskCheckpoint &lhs, const TaskCheckpoint &rhs) {
                  return lhs.through_event_sequence < rhs.through_event_sequence;
              });
    if (entries.size() > impl_->max_per_task_) {
        entries.erase(entries.begin());
    }
    return Result<void>{};
}

Result<std::optional<TaskCheckpoint>> MemoryCheckpointStore::latest(TaskId task) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->tasks.find(task);
    if (found == impl_->tasks.end() || found->second.empty()) {
        return std::optional<TaskCheckpoint>{};
    }
    return std::optional<TaskCheckpoint>(found->second.back());
}

Result<std::optional<TaskCheckpoint>>
MemoryCheckpointStore::latest_at_or_before(TaskId task, std::uint64_t max_sequence) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->tasks.find(task);
    if (found == impl_->tasks.end() || found->second.empty()) {
        return std::optional<TaskCheckpoint>{};
    }
    const auto &entries = found->second;
    if (entries.front().through_event_sequence > max_sequence) {
        return std::optional<TaskCheckpoint>{};
    }
    auto upper = std::upper_bound(entries.begin(), entries.end(), max_sequence,
                                  [](std::uint64_t sequence, const TaskCheckpoint &candidate) {
                                      return sequence < candidate.through_event_sequence;
                                  });
    return std::optional<TaskCheckpoint>(*std::prev(upper));
}

Result<std::size_t> MemoryCheckpointStore::count(TaskId task) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->tasks.find(task);
    if (found == impl_->tasks.end()) {
        return std::size_t{0};
    }
    return found->second.size();
}

Result<std::size_t> MemoryCheckpointStore::erase_task(TaskId task, std::string reason) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->tasks.find(task);
    if (found == impl_->tasks.end()) {
        return std::size_t{0};
    }
    const auto removed = found->second.size();
    impl_->tasks.erase(found);
    impl_->erasures += 1;
    (void)reason;
    return removed;
}

std::size_t MemoryCheckpointStore::erasures() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->erasures;
}

// ---------------------------------------------------------------------------
// CheckpointCoordinator
// ---------------------------------------------------------------------------

std::string checkpoint_trigger_name(CheckpointTrigger trigger) {
    switch (trigger) {
    case CheckpointTrigger::Watermark:
        return "Watermark";
    case CheckpointTrigger::Pause:
        return "Pause";
    case CheckpointTrigger::Takeover:
        return "Takeover";
    case CheckpointTrigger::Shutdown:
        return "Shutdown";
    case CheckpointTrigger::ProjectionMissing:
        return "ProjectionMissing";
    case CheckpointTrigger::PeriodicRebuild:
        return "PeriodicRebuild";
    }
    return "Unknown";
}

CheckpointCoordinator::CheckpointCoordinator(IEventStore &events, ICheckpointStore &store,
                                             CheckpointSchedulePolicy policy)
    : events_(events), store_(store), policy_(policy), builder_(policy_.builder) {}

Result<std::vector<EventEnvelope>>
CheckpointCoordinator::read_task_events(TaskId task, SessionId session) const {
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
        for (const auto &event : page.value().events) {
            if (event.task_id.has_value() && *event.task_id == task) {
                collected.push_back(event);
            }
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        after = page.value().events.back().session_sequence;
        if (collected.size() > policy_.rebuild_interval_events * 16) {
            return checkpoint_error(ErrorCode::ResourceExhausted,
                                    "task event scan exceeded the scheduling bound");
        }
    }
    return collected;
}

Result<std::uint64_t> CheckpointCoordinator::latest_stored_sequence(TaskId task) const {
    auto stored = store_.latest(task);
    if (!stored) {
        return stored.error();
    }
    if (!stored.value().has_value()) {
        return std::uint64_t{0};
    }
    return stored.value()->through_event_sequence;
}

Result<std::optional<TaskCheckpoint>> CheckpointCoordinator::checkpoint(TaskId task,
                                                                        SessionId session,
                                                                        CheckpointTrigger trigger,
                                                                        const Timestamp &now) {
    auto events = read_task_events(task, session);
    if (!events) {
        return events.error();
    }
    auto stored = store_.latest(task);
    if (!stored) {
        return stored.error();
    }
    std::optional<TaskCheckpoint> base = std::move(stored).value();
    const std::uint64_t base_sequence = base.has_value() ? base->through_event_sequence : 0;
    std::size_t increment = 0;
    for (const auto &event : events.value()) {
        if (event.session_sequence > base_sequence) {
            ++increment;
        }
    }
    if (events.value().empty()) {
        return std::optional<TaskCheckpoint>{};
    }
    // Watermark and periodic triggers respect the increment policy; explicit
    // durability points (pause, takeover, shutdown, missing projection) do not.
    const bool policy_gated =
        trigger == CheckpointTrigger::Watermark || trigger == CheckpointTrigger::PeriodicRebuild;
    if (policy_gated && increment < policy_.min_event_increment) {
        return std::optional<TaskCheckpoint>{};
    }
    if (base.has_value() && base->terminal_state.has_value() &&
        trigger != CheckpointTrigger::PeriodicRebuild) {
        // Terminal tasks never gain new checkpoints from runtime triggers.
        return std::optional<TaskCheckpoint>{};
    }
    auto built = builder_.build(base, events.value(), now);
    const auto valid = built.checkpoint.validate();
    if (!valid) {
        return valid.error();
    }
    const auto put = store_.put(built.checkpoint);
    if (!put) {
        return put.error();
    }
    return std::optional<TaskCheckpoint>(built.checkpoint);
}

Result<ProjectionReport> CheckpointCoordinator::rebuild(TaskId task, SessionId session,
                                                        const Timestamp &now, bool repair) {
    auto events = read_task_events(task, session);
    if (!events) {
        return events.error();
    }
    auto built = builder_.build(std::nullopt, events.value(), now);
    const auto valid = built.checkpoint.validate();
    if (!valid) {
        return valid.error();
    }
    ProjectionReport report;
    report.rebuilt_sequence = built.checkpoint.through_event_sequence;
    report.rebuilt_digest = built.checkpoint.projection_digest();
    auto stored = store_.latest(task);
    if (!stored) {
        return stored.error();
    }
    if (stored.value().has_value()) {
        report.stored_sequence = stored.value()->through_event_sequence;
        report.stored_digest = stored.value()->projection_digest();
    }
    report.consistent = report.stored_digest == report.rebuilt_digest &&
                        report.stored_sequence == report.rebuilt_sequence;
    if (!report.consistent && repair) {
        const auto put = store_.put(built.checkpoint);
        if (!put) {
            return put.error();
        }
        report.repaired = true;
        report.note = "stored projection replaced by full rebuild";
    } else if (!report.consistent) {
        report.note = "stored projection diverges from the event log";
    }
    return report;
}

} // namespace mira
