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
    auto end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
        --end;
    return std::string(text.substr(0, end));
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
        if (!is_terminal(task.state) && !task.historical) {
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

    // Explicit host references outrank semantic suggestions (DEC-102). An
    // unknown reference must never fall back to an unrelated live task.
    const PrincipalTaskEntry *explicit_task = nullptr;
    if (envelope.target_task) {
        explicit_task = find_task(state, *envelope.target_task);
        if (explicit_task == nullptr) {
            decision.rationale = "the explicit task is not owned by this principal";
            return decision;
        }
    }
    if (envelope.reply_to) {
        const auto *reply_task = task_of_input(state, *envelope.reply_to);
        if (reply_task == nullptr || (explicit_task && explicit_task != reply_task)) {
            decision.rationale = "the explicit reply is unknown or conflicts with the task";
            return decision;
        }
        explicit_task = reply_task;
    }
    if (envelope.requested_intent == RouteIntent::NewTask) {
        if (explicit_task) {
            decision.rationale = "a new-task request cannot also name existing work";
            return decision;
        }
        decision.intent = RouteIntent::NewTask;
        decision.rationale = "the host explicitly requested a new task";
        return decision;
    }
    const auto select = [&](const PrincipalTaskEntry &target, RouteIntent intent,
                            std::string rationale) {
        RoutingDecision selected;
        selected.session_id = target.session_id;
        selected.task_id = target.task_id;
        selected.rationale = std::move(rationale);
        if (target.historical)
            selected.session_id.reset(); // A new Runtime must open a fresh session.
        switch (intent) {
        case RouteIntent::ContinueCurrent:
        case RouteIntent::SupplementTask:
            selected.intent =
                (is_terminal(target.state) || target.historical) ? RouteIntent::ResumeTask : intent;
            break;
        case RouteIntent::ResumeTask:
            selected.intent =
                (is_terminal(target.state) || target.historical) ? intent : RouteIntent::Reject;
            break;
        case RouteIntent::CancelTask:
            selected.intent = intent; // Runtime provides terminal NoOp semantics.
            break;
        case RouteIntent::PauseTask:
            selected.intent = is_terminal(target.state) ? RouteIntent::Reject : intent;
            break;
        default:
            selected.intent = RouteIntent::Reject;
            break;
        }
        return selected;
    };
    if (explicit_task) {
        return select(*explicit_task,
                      envelope.requested_intent.value_or(RouteIntent::ContinueCurrent),
                      "explicit principal-owned reference takes precedence over suggestions");
    }
    if (envelope.requested_intent) {
        decision.rationale = "the explicit intent requires a principal-owned target";
        return decision;
    }

    if (suggestion) {
        const auto &proposal = *suggestion;
        if (proposal.task_id) {
            const auto *target = find_task(state, *proposal.task_id);
            if (target && (!proposal.session_id || proposal.session_id == target->session_id)) {
                auto selected =
                    select(*target, proposal.intent,
                           proposal.rationale.empty() ? "validated principal-owned route suggestion"
                                                      : bound_text(proposal.rationale, 512));
                if (selected.intent != RouteIntent::Reject)
                    return selected;
            }
        } else if (proposal.intent == RouteIntent::NewTask && !proposal.session_id) {
            decision.intent = RouteIntent::NewTask;
            decision.rationale = "validated new-task suggestion";
            return decision;
        }
        if (proposal.intent == RouteIntent::Reject) {
            decision.rationale = bound_text(proposal.rationale, 512);
            return decision;
        }
        if (proposal.intent == RouteIntent::Clarify) {
            decision.intent = RouteIntent::Clarify;
            decision.rationale = "the suggestion requests disambiguation";
            decision.clarification =
                bound_text(proposal.clarification.value_or("Which item should this apply to?"),
                           kMaxClarificationBytes);
            for (const auto *task : live_tasks(state))
                decision.candidates.push_back(task->task_id);
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
        decision.candidates.push_back(task->task_id);
        question += "\n- ";
        question += bound_text(task->goal, 120);
    }
    decision.clarification = bound_text(question, kMaxClarificationBytes);
    return decision;
}

} // namespace mira::interaction_support
