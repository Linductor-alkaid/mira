#pragma once

// Internal to the interaction layer (core module, src/interaction): the
// deterministic routing decision and the small shared bounds of the
// coordinator. The public surface of the layer lives in
// include/mira/interaction_coordinator.hpp; this header is private to the
// module and is not installed.
//
// The routing rules live in their own translation unit so the coordinator
// stays well inside the architecture file-size budget (tools/architecture-
// policy.json) and so M29 can extend the coordinator's lifecycle without
// pushing either file over it.

#include <mira/interaction_coordinator.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira::interaction_support {

// The bounds of the per-task user constraint list. The list is the "latest
// user constraints" of one work item; it is bounded so a long conversation
// cannot grow one task's context without limit (RULE-08).
inline constexpr std::size_t kMaxConstraintsPerTask = 8;
inline constexpr std::size_t kMaxConstraintBytes = 1024;
inline constexpr std::size_t kMaxGoalBytes = 4096;
inline constexpr std::size_t kMaxClarificationBytes = 1024;

// Truncates a text to a byte bound. Host-provided text reaches the error
// messages and the durable event stream through this bound.
[[nodiscard]] std::string bound_text(std::string_view text, std::size_t limit);

// The task state names of the frozen task state machine.
[[nodiscard]] std::string_view task_state_name(TaskState state);

// The fail-closed capability check of the coordinator (DEC-068 R9): a missing
// capability, an expired grant or a mismatched identity version rejects the
// input; the check never defaults to allow.
[[nodiscard]] bool permitted(const PrincipalContext &principal, const CoordinatorConfig &config);

// The registry lookup of one task of a principal.
[[nodiscard]] const PrincipalTaskEntry *find_task(const PrincipalState &state, TaskId task_id);

// The registry lookup of the task an earlier input was routed to.
[[nodiscard]] const PrincipalTaskEntry *task_of_input(const PrincipalState &state,
                                                      std::string_view input_id);

// The deterministic routing decision (DEC-068 §5). The inputs are the
// principal, the envelope and the principal state; a host suggestion is only
// adopted after it is validated against that state.
[[nodiscard]] RoutingDecision decide_route(const InputEnvelope &envelope,
                                           const PrincipalState &state,
                                           const std::optional<RouteSuggestion> &suggestion);

} // namespace mira::interaction_support
