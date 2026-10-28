#include "interaction_routing.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace mira::interaction_support {

[[nodiscard]] std::string bound_text(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, limit));
}

// The task state names of the frozen task state machine.
[[nodiscard]] std::string_view task_state_name(TaskState state) {
    switch (state) {
    case TaskState::Idle:
        return "Idle";
    case TaskState::Observing:
        return "Observing";
    case TaskState::Reasoning:
        return "Reasoning";
    case TaskState::Planning:
        return "Planning";
    case TaskState::Acting:
        return "Acting";
    case TaskState::Verifying:
        return "Verifying";
    case TaskState::Recovering:
        return "Recovering";
    case TaskState::Pausing:
        return "Pausing";
    case TaskState::Paused:
        return "Paused";
    case TaskState::TakeoverSettling:
        return "TakeoverSettling";
    case TaskState::SuspendedForTakeover:
        return "SuspendedForTakeover";
    case TaskState::Cancelling:
        return "Cancelling";
    case TaskState::Completed:
        return "Completed";
    case TaskState::Failed:
        return "Failed";
    case TaskState::Cancelled:
        return "Cancelled";
    }
    return "Idle";
}

// The fail-closed capability check of the coordinator (DEC-068 R9). A missing
// capability, an expired grant or a mismatched identity version rejects the
// input; the check never defaults to allow.
[[nodiscard]] bool permitted(const PrincipalContext &principal, const CoordinatorConfig &config) {
    if (principal.tenant_id.is_nil() || principal.user_id.is_nil()) {
        return false;
    }
    if (config.required_identity_version.has_value() &&
        principal.identity_version != *config.required_identity_version) {
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    return std::any_of(principal.grants.begin(), principal.grants.end(), [&](const auto &grant) {
        if (grant.capability != config.required_capability) {
            return false;
        }
        // The resource selector of a grant scopes environment effects, not the
        // admission of the principal's own conversation, so it does not
        // narrow this check.
        return grant.expires_at == std::chrono::system_clock::time_point{} ||
               grant.expires_at > now;
    });
}

[[nodiscard]] const PrincipalTaskEntry *find_task(const PrincipalState &state, TaskId task_id) {
    for (const auto &task : state.tasks) {
        if (task.task_id == task_id) {
            return &task;
        }
    }
    return nullptr;
}

[[nodiscard]] const PrincipalTaskEntry *task_of_input(const PrincipalState &state,
                                                      std::string_view input_id) {
    for (const auto &task : state.tasks) {
        for (const auto &candidate : task.input_ids) {
            if (candidate == input_id) {
                return &task;
            }
        }
    }
    return nullptr;
}

namespace {

[[nodiscard]] std::vector<const PrincipalTaskEntry *> live_tasks(const PrincipalState &state) {
    std::vector<const PrincipalTaskEntry *> live;
    for (const auto &task : state.tasks) {
        if (!is_terminal(task.state)) {
            live.push_back(&task);
        }
    }
    return live;
}

} // namespace

// The deterministic routing decision (DEC-068 §5). The inputs are the
// principal, the envelope and the principal state; a host suggestion is only
// adopted after it is validated against that state.
[[nodiscard]] RoutingDecision decide_route(const InputEnvelope &envelope,
                                           const PrincipalState &state,
                                           const std::optional<RouteSuggestion> &suggestion) {
    RoutingDecision decision;

    if (suggestion.has_value()) {
        const auto &proposal = *suggestion;
        switch (proposal.intent) {
        case RouteIntent::ContinueCurrent:
        case RouteIntent::SupplementTask:
        case RouteIntent::PauseTask:
        case RouteIntent::CancelTask: {
            if (!proposal.task_id.has_value()) {
                break;
            }
            const auto *target = find_task(state, *proposal.task_id);
            if (target == nullptr) {
                break;
            }
            if (is_terminal(target->state)) {
                // A terminal task is never revived. Continuing or correcting
                // it becomes a new task bound to its summary; pausing or
                // cancelling it is impossible (DEC-068 R2).
                if (proposal.intent == RouteIntent::ContinueCurrent ||
                    proposal.intent == RouteIntent::SupplementTask) {
                    decision.intent = RouteIntent::ResumeTask;
                    decision.session_id = target->session_id;
                    decision.task_id = target->task_id;
                    decision.rationale =
                        "the suggested task is terminal; the input resumes it through a new task";
                    return decision;
                }
                break;
            }
            decision.intent = proposal.intent;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                proposal.rationale.empty()
                    ? std::string("the suggested target is live and owned by the principal")
                    : proposal.rationale;
            return decision;
        }
        case RouteIntent::ResumeTask: {
            if (!proposal.task_id.has_value()) {
                break;
            }
            const auto *target = find_task(state, *proposal.task_id);
            if (target == nullptr || !is_terminal(target->state)) {
                break;
            }
            decision.intent = RouteIntent::ResumeTask;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                proposal.rationale.empty()
                    ? std::string("the suggested task is settled; the input resumes it "
                                  "through a new task")
                    : proposal.rationale;
            return decision;
        }
        case RouteIntent::NewTask:
            decision.intent = RouteIntent::NewTask;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input starts a new task")
                                     : proposal.rationale;
            return decision;
        case RouteIntent::Clarify:
            decision.intent = RouteIntent::Clarify;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input reference is ambiguous")
                                     : proposal.rationale;
            decision.clarification =
                proposal.clarification.value_or(std::string("Which item should this apply to?"));
            return decision;
        case RouteIntent::Reject:
            decision.intent = RouteIntent::Reject;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input was rejected by the host")
                                     : proposal.rationale;
            return decision;
        }
        // The suggestion did not survive validation: fall through to the
        // structural rules and say so in the rationale.
    }

    // Structural rule 1: an explicit reply reference names the work item.
    if (envelope.reply_to.has_value()) {
        const auto *target = task_of_input(state, *envelope.reply_to);
        if (target != nullptr && !is_terminal(target->state)) {
            decision.intent = RouteIntent::ContinueCurrent;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale = "the input replies to a known live task";
            return decision;
        }
        if (target != nullptr) {
            decision.intent = RouteIntent::ResumeTask;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                "the input replies to a settled task; it resumes it through a new task";
            return decision;
        }
    }

    // Structural rule 2: the live tasks of the principal.
    const auto live = live_tasks(state);
    if (live.empty()) {
        decision.intent = RouteIntent::NewTask;
        decision.rationale = "the principal has no live task; the input starts a new one";
        return decision;
    }
    if (live.size() == 1) {
        decision.intent = RouteIntent::ContinueCurrent;
        decision.session_id = live.front()->session_id;
        decision.task_id = live.front()->task_id;
        decision.rationale = "the principal has exactly one live task; the input continues it";
        return decision;
    }
    // Several live tasks: the reference is ambiguous. A clarification is a
    // normal reply, not an error code (DEC-068 R4).
    decision.intent = RouteIntent::Clarify;
    decision.rationale = "the principal has several live tasks; the reference is ambiguous";
    std::string question = "Which item should this apply to?";
    for (const auto *task : live) {
        question += "\n- ";
        question += bound_text(task->goal, 120);
    }
    decision.clarification = bound_text(question, kMaxClarificationBytes);
    return decision;
}

} // namespace mira::interaction_support
