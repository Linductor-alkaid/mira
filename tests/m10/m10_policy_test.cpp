// M10-03..M10-06: the stage-C policy set (DEC-023). Recoverable and
// AgentAssisted escalate to WaitingAgent with the Recovering carrier,
// Interactive failures raise a WaitingUser decision, checkpoints hand off
// once per arrival, and the escalation budget bounds wait cycles.

#include "support/m10_support.hpp"

#include <chrono>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] WorkflowRecoveryHook escalation_hook() {
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::AgentEscalation;
    return hook;
}

int recoverable_failure_escalates_to_waiting_agent() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 1; // The first dispatch fails, no local hook.
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("recoverable");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());

    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::WaitingAgent);
    MIRA_CHECK(counter.dispatches.load() == 1);

    // The carrier task sits in Recovering (DEC-020 §2 mapping) and the
    // continuation context names the failed step.
    const auto continuation = workflow->agent_continuation(created.value().run_id);
    MIRA_CHECK(continuation.has_value());
    MIRA_CHECK(continuation.value().policy == WorkflowPolicy::Recoverable);
    MIRA_CHECK(continuation.value().current_step.has_value());
    MIRA_CHECK(!continuation.value().failure_reason.empty());
    MIRA_CHECK(!continuation.value().effective_parameters.is_null());

    // The agent repairs (the tool only failed once) and resumes: the step
    // re-executes and the run completes.
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    return 0;
}

int escalation_hook_escalates_on_failure() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 3; // Every in-run attempt fails.
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("hook-escalation");
    definition.default_policy = WorkflowPolicy::Recoverable;
    auto failing = tool_step("counter", std::nullopt, escalation_hook());
    definition.steps = {failing};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());

    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    // The AgentEscalation hook hands the failure to the agent directly:
    // no Retry branch ran, exactly one dispatch happened.
    MIRA_CHECK(result.value().state == WorkflowRunState::WaitingAgent);
    MIRA_CHECK(counter.dispatches.load() == 1);
    MIRA_CHECK(result.value().steps.back().disposition == WorkflowStepDisposition::Failed);
    return 0;
}

int agent_assisted_checkpoint_hands_off_before_execution() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("checkpoint");
    definition.default_policy = WorkflowPolicy::AgentAssisted;
    auto checkpoint = tool_step("counter", std::nullopt, escalation_hook());
    definition.steps = {checkpoint, tool_step("counter", std::nullopt)};

    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::AgentAssisted);
    MIRA_CHECK(created.has_value());

    const auto stopped = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(stopped.has_value());
    // Arrival-driven handoff: the checkpoint step did not execute and no
    // step settled; the drive stopped in WaitingAgent.
    MIRA_CHECK(stopped.value().state == WorkflowRunState::WaitingAgent);
    MIRA_CHECK(counter.dispatches.load() == 0);
    MIRA_CHECK(stopped.value().steps.empty());

    const auto continuation = workflow->agent_continuation(created.value().run_id);
    MIRA_CHECK(continuation.has_value());
    MIRA_CHECK(continuation.value().current_step.has_value());
    MIRA_CHECK(continuation.value().current_step.value() == checkpoint.id);

    // Resume consumes the handoff obligation: the checkpoint executes once
    // and the run finishes; a second drive would hand off only on a new
    // arrival (loop), which this linear flow does not produce.
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    return 0;
}

int checkpoint_revisits_hand_off_each_loop_arrival() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     counter.registration("counter", "count")));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("checkpoint-loop");
    definition.default_policy = WorkflowPolicy::AgentAssisted;
    auto head = tool_step("counter", std::nullopt, escalation_hook(), true);
    auto control =
        control_step(head.id, 2,
                     step_result_predicate(head.id, WorkflowPredicateOp::Lt,
                                           JsonValue{static_cast<std::int64_t>(2)}));
    definition.steps = {head, control};

    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::AgentAssisted);
    MIRA_CHECK(created.has_value());

    // First arrival: immediate handoff.
    const auto first = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(first.value().state == WorkflowRunState::WaitingAgent);

    // Resume: the checkpoint executes (count 1), the control jumps back.
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    const auto second = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(second.has_value());
    // Second arrival at the checkpoint hands off again before executing.
    MIRA_CHECK(second.value().state == WorkflowRunState::WaitingAgent);
    MIRA_CHECK(counter.dispatches.load() == 1);

    // Resume: executes (count 2), the control precondition fails now, the
    // control step is skipped and the run completes.
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    return 0;
}

int interactive_failure_raises_a_waiting_user_decision() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 1;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("interactive-failure");
    auto failing = tool_step("counter", std::nullopt);
    failing.max_attempts = 1;
    definition.steps = {failing, tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());

    const auto stopped = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(stopped.has_value());
    MIRA_CHECK(stopped.value().state == WorkflowRunState::WaitingUser);

    const auto decision = workflow->pending_decision_request(created.value().run_id);
    MIRA_CHECK(decision.has_value());
    MIRA_CHECK(decision.value().kind == WorkflowDecisionKind::StepFailure);
    MIRA_CHECK(decision.value().step_id.has_value());
    MIRA_CHECK(decision.value().step_id.value() == failing.id);
    MIRA_CHECK(decision.value().proposal.size() == 1);
    MIRA_CHECK(decision.value().proposal.front().op == WorkflowPatchOp::Skip);

    // A waiting-user run has exactly one exit: resolving the decision.
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(!resumed.has_value());
    MIRA_CHECK(resumed.error().code == ErrorCode::InvalidState);

    // Accept applies the skip proposal and continues past the failed step.
    const auto resolved =
        workflow->resolve_decision(created.value().run_id, decision.value().decision_id,
                                   decision.value().payload_digest,
                                   WorkflowDecisionResolution::Accept);
    MIRA_CHECK(resolved.has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    // One failed attempt (skipped thereafter) plus the second step.
    MIRA_CHECK(counter.dispatches.load() == 2);
    MIRA_CHECK(settled.value().steps.front().disposition == WorkflowStepDisposition::Failed);
    return 0;
}

int interactive_decision_reject_fails_and_cancel_cancels() {
    WorkflowFixture fixture;
    CountingTool counter;
    // Both scenarios consume one failing dispatch each.
    counter.failures_first = 2;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("interactive-reject");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context())
                   .value()
                   .state == WorkflowRunState::WaitingUser);

    // A wrong digest is rejected without settling the decision.
    const auto decision = workflow->pending_decision_request(created.value().run_id);
    MIRA_CHECK(decision.has_value());
    auto wrong = decision.value().payload_digest;
    wrong.bytes[0] ^= static_cast<std::uint8_t>(0x01);
    const auto mismatch = workflow->resolve_decision(
        created.value().run_id, decision.value().decision_id, wrong,
        WorkflowDecisionResolution::Accept);
    MIRA_CHECK(!mismatch.has_value());
    MIRA_CHECK(mismatch.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().state ==
               WorkflowRunState::WaitingUser);

    const auto rejected =
        workflow->resolve_decision(created.value().run_id, decision.value().decision_id,
                                   decision.value().payload_digest,
                                   WorkflowDecisionResolution::Reject);
    MIRA_CHECK(rejected.has_value());
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().state ==
               WorkflowRunState::Failed);

    // A second scenario exercises cancel_run through the decision API.
    const auto second = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                             WorkflowPolicy::Interactive);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(workflow->execute_run(second.value().run_id, drive_context())
                   .value()
                   .state == WorkflowRunState::WaitingUser);
    const auto second_decision = workflow->pending_decision_request(second.value().run_id);
    MIRA_CHECK(second_decision.has_value());
    const auto cancelled = workflow->resolve_decision(
        second.value().run_id, second_decision.value().decision_id,
        second_decision.value().payload_digest, WorkflowDecisionResolution::CancelRun);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(workflow->run_snapshot(second.value().run_id).value().state ==
               WorkflowRunState::Cancelled);
    return 0;
}

int escalation_budget_bounds_wait_cycles() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 64; // Every dispatch fails.
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    WorkflowRuntimeConfig config;
    config.max_escalations_per_run = 2;
    auto workflow = fixture.make_workflow(config);

    auto definition = full_policy_definition("escalation-budget");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());

    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context())
                   .value()
                   .state == WorkflowRunState::WaitingAgent);
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    MIRA_CHECK(workflow->wait_run(created.value().run_id, std::chrono::seconds(10))
                   .value()
                   .state == WorkflowRunState::WaitingAgent);
    // The third escalation exceeds the budget: the run settles Failed.
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(counter.dispatches.load() == 3);
    return 0;
}

int strict_and_dry_run_never_escalate() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 1;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("strict-no-agent");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto strict = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                             WorkflowPolicy::Strict);
    MIRA_CHECK(strict.has_value());
    MIRA_CHECK(workflow->execute_run(strict.value().run_id, drive_context())
                   .value()
                   .state == WorkflowRunState::Failed);

    // AgentEscalation hooks stay inadmissible under non-agent policies.
    auto hooked = full_policy_definition("hook-rejected");
    hooked.default_policy = WorkflowPolicy::Recoverable;
    hooked.steps = {tool_step("counter", std::nullopt, escalation_hook())};
    const auto rejected = workflow->create_run(hooked, JsonValue{JsonValue::Object{}},
                                               WorkflowPolicy::Strict);
    MIRA_CHECK(!rejected.has_value());

    // The agent continuation API answers fail closed outside WaitingAgent.
    const auto continuation = workflow->agent_continuation(strict.value().run_id);
    MIRA_CHECK(!continuation.has_value());
    return 0;
}

int wait_states_track_their_carrier_states() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 4;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("carrier-states");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context()).has_value());

    // WaitingAgent -> Recovering on the carrier (DEC-020 §2).
    const auto snapshot = workflow->run_snapshot(created.value().run_id);
    MIRA_CHECK(snapshot.has_value());
    MIRA_CHECK(snapshot.value().state == WorkflowRunState::WaitingAgent);

    const auto cancelled = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    MIRA_CHECK(cancelled.value().state == WorkflowRunState::Cancelled);
    // Cancel works from the wait state and is idempotent.
    const auto again = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value().state == WorkflowRunState::Cancelled);
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        recoverable_failure_escalates_to_waiting_agent,
        escalation_hook_escalates_on_failure,
        agent_assisted_checkpoint_hands_off_before_execution,
        checkpoint_revisits_hand_off_each_loop_arrival,
        interactive_failure_raises_a_waiting_user_decision,
        interactive_decision_reject_fails_and_cancel_cancels,
        escalation_budget_bounds_wait_cycles,
        strict_and_dry_run_never_escalate,
        wait_states_track_their_carrier_states,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
