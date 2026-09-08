// M9-04/05/06: the Strict and DryRun execution closed loops over the frozen
// workflow contracts — step dispatch through the BuiltIn boundary, W-02
// observation-before-verification, precondition skips, bounded loops,
// recovery hooks (Retry/FallbackStep/exhaustion), fail-closed admissions and
// the whole-run step budget.

#include "support/m9_support.hpp"

#include <algorithm>

namespace {

using namespace mira;
using namespace mira::testing;

int strict_run_completes_with_verification() {
    WorkflowFixture fixture;
    CountingTool plain;
    CountingTool effects; // side-effecting: exercises the W-02 observation
    effects.side_effects = true;
    MIRA_CHECK(register_registration(*fixture.registry_, plain.registration("counter", "sent")));
    MIRA_CHECK(register_registration(*fixture.registry_, effects.registration("actor", "sent")));

    auto counting_environment =
        std::make_shared<ObservationCountingEnvironment>(fixture.environment_);
    WorkflowRuntime workflow(fixture.executor_, *fixture.runtime_, fixture.session_id_,
                             counting_environment);
    workflow.set_event_store(fixture.events_);
    workflow.set_tool_registry(fixture.registry_);

    auto definition = base_definition("strict-happy");
    const auto first = tool_step("counter", std::nullopt);
    const auto second = tool_step(
        "counter", step_result_predicate(first.id, WorkflowPredicateOp::Eq, JsonValue{"sent"}));
    const auto third = tool_step(
        "actor", step_result_predicate(second.id, WorkflowPredicateOp::Eq, JsonValue{"sent"}));
    definition.steps = {first, second, third};

    const auto created =
        workflow.create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow.execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(plain.dispatches.load() == 2);
    MIRA_CHECK(effects.dispatches.load() == 1);
    MIRA_CHECK(result.value().steps.size() == 3);
    MIRA_CHECK(std::all_of(result.value().steps.begin(), result.value().steps.end(),
                           [](const WorkflowStepRecord &record) {
                               return record.disposition == WorkflowStepDisposition::Completed;
                           }));
    MIRA_CHECK(result.value().steps[1].verification == "satisfied");
    MIRA_CHECK(result.value().steps[2].verification == "satisfied");

    // W-02: the side-effecting step was verified against a fresh observation.
    MIRA_CHECK(counting_environment->observations() >= 1);

    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowRunStarted") == 1);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowStepStarted") == 3);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowStepSettled") == 3);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowRunSettled") == 1);
    return 0;
}

int precondition_skips_steps() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("skip-flow");
    auto conditional = tool_step("counter", std::nullopt);
    conditional.precondition =
        parameter_predicate("mode", WorkflowPredicateOp::Ne, JsonValue{"skip"});
    definition.steps = {conditional, tool_step("counter", std::nullopt)};

    const auto created = workflow->create_run(
        definition, JsonValue{JsonValue::Object{{"mode", JsonValue{"skip"}}}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 1);
    MIRA_CHECK(result.value().steps.front().disposition == WorkflowStepDisposition::Skipped);
    return 0;
}

int bounded_loop_exits_through_precondition() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration("counter", "count")));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("loop-exit");
    auto head = tool_step("counter", std::nullopt, {}, true);
    // Exit once the counter reports at least three dispatches.
    auto control =
        control_step(head.id, 8,
                     step_result_predicate(head.id, WorkflowPredicateOp::Lt,
                                          JsonValue{static_cast<std::int64_t>(3)}));
    definition.steps = {head, control};

    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 3);
    return 0;
}

int bounded_loop_budget_fails_closed() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration("counter", "count")));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("loop-forever");
    auto head = tool_step("counter", std::nullopt, {}, true);
    definition.steps = {head, control_step(head.id, 2)};

    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    // Two jumps executed, the third rejected by the iteration budget.
    MIRA_CHECK(counter.dispatches.load() == 3);
    MIRA_CHECK(result.value().safe_summary.find("loop iteration budget exceeded") !=
               std::string::npos);
    return 0;
}

int dry_run_plans_without_dispatch() {
    WorkflowFixture fixture;
    CountingTool effects;
    effects.side_effects = true;
    MIRA_CHECK(register_registration(*fixture.registry_, effects.registration("actor", "sent")));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("dry-plan");
    const auto probe = tool_step("actor", std::nullopt);
    definition.steps = {
        probe,
        tool_step("actor", step_result_predicate(probe.id, WorkflowPredicateOp::Eq,
                                                 JsonValue{"sent"})),
        verify_step(
            step_result_predicate(probe.id, WorkflowPredicateOp::Eq, JsonValue{"sent"})),
        navigate_step("ChatPage"),
    };

    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::DryRun);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(effects.dispatches.load() == 0); // Nothing dispatched in DryRun.
    MIRA_CHECK(result.value().steps.size() == 4);
    // step_result signals do not exist without execution: the predicates are
    // disclosed as not evaluable instead of claimed verified (RULE-10).
    MIRA_CHECK(result.value().steps[1].verification == "not_evaluable");
    MIRA_CHECK(result.value().steps[2].verification == "not_evaluable");
    MIRA_CHECK(result.value().unevaluable_verifications >= 2);
    return 0;
}

int dry_run_evaluable_predicates_are_enforced() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("dry-assert");
    definition.steps = {
        tool_step("counter",
                  parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"run"})),
        tool_step("counter",
                  parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"other"})),
    };

    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::DryRun);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.front().verification == "satisfied");
    MIRA_CHECK(counter.dispatches.load() == 0);
    return 0;
}

int summaryless_definitions_round_trip() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // Definitions without a summary must validate and run: the canonical
    // form omits the optional member instead of emitting a rejected empty
    // one (regression: validate_workflow_definition round trip).
    auto definition = base_definition("no-summary");
    definition.summary.clear();
    definition.steps = {tool_step("counter", std::nullopt)};
    MIRA_CHECK(validate_workflow_definition(definition).has_value());
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    return 0;
}

int policy_admission_follows_allowed_set() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = base_definition("policy-scope");
    definition.steps = {tool_step("counter", std::nullopt)};

    // Stage C (DEC-023): every policy named in the allowed set is
    // executable; the stage-B Strict/DryRun-only gate is gone.
    const auto recoverable = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                                  WorkflowPolicy::Recoverable);
    MIRA_CHECK(recoverable.has_value());

    const auto interactive = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                                  WorkflowPolicy::Interactive);
    MIRA_CHECK(interactive.has_value());

    // Membership is still enforced: a policy outside the allowed set fails.
    definition.allowed_policies = {WorkflowPolicy::Strict, WorkflowPolicy::DryRun};
    const auto outside = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(!outside.has_value());
    MIRA_CHECK(outside.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int admission_fails_closed() {
    WorkflowFixture fixture;
    CountingTool plain;
    CountingTool effects;
    effects.side_effects = true;
    MIRA_CHECK(register_registration(*fixture.registry_, plain.registration("counter", "sent")));
    MIRA_CHECK(register_registration(*fixture.registry_, effects.registration("actor", "sent")));
    auto workflow = fixture.make_workflow();

    // Navigate steps cannot resolve before phase E: Strict rejects them at
    // admission.
    auto navigating = base_definition("nav");
    navigating.steps = {navigate_step("Home")};
    const auto strict_nav = workflow->create_run(navigating, JsonValue{JsonValue::Object{}},
                                                 WorkflowPolicy::Strict);
    MIRA_CHECK(!strict_nav.has_value());
    MIRA_CHECK(strict_nav.error().code == ErrorCode::UnsupportedCapability);

    // Side-effecting steps without a verification predicate are rejected
    // (W-02).
    auto unverified = base_definition("unverified");
    unverified.steps = {tool_step("actor", std::nullopt)};
    const auto rejected =
        workflow->create_run(unverified, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);

    // ToolCall arguments must name their tool through the reserved member.
    auto anonymous = base_definition("anonymous");
    auto step = tool_step("counter", std::nullopt);
    JsonValue::Object arguments;
    arguments.emplace_back("payload", "no tool named");
    step.arguments = JsonValue{std::move(arguments)};
    anonymous.steps = {step};
    const auto unbound =
        workflow->create_run(anonymous, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!unbound.has_value());
    MIRA_CHECK(unbound.error().code == ErrorCode::InvalidArgument);

    // Unknown tool wire names fail closed at admission.
    auto missing = base_definition("missing-tool");
    missing.steps = {tool_step("does_not_exist", std::nullopt)};
    const auto unknown =
        workflow->create_run(missing, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!unknown.has_value());
    MIRA_CHECK(unknown.error().code == ErrorCode::NotFound);

    // Parameter binding errors surface the deterministic codes.
    auto parameterized = base_definition("bind-error");
    parameterized.parameters.front().required = true;
    parameterized.parameters.front().default_value = JsonValue{}; // defaults are optional-only
    parameterized.steps = {tool_step("counter", std::nullopt)};
    const auto unbound_parameters =
        workflow->create_run(parameterized, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!unbound_parameters.has_value());
    MIRA_CHECK(unbound_parameters.error().domain_code ==
               static_cast<std::int32_t>(WorkflowBindError::MissingRequired));

    // Agent escalation hooks require an agent-capable policy for the run.
    auto escalating = base_definition("escalating");
    auto with_hook = tool_step("counter", std::nullopt);
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::AgentEscalation;
    with_hook.recovery = hook;
    escalating.steps = {with_hook};
    const auto escalated = workflow->create_run(escalating, JsonValue{JsonValue::Object{}},
                                                WorkflowPolicy::DryRun);
    MIRA_CHECK(!escalated.has_value());
    MIRA_CHECK(escalated.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int retry_hook_recovers_scripted_failure() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 1;
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::Retry;
    hook.max_retries = 2;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("retry-once");
    definition.steps = {tool_step("counter", std::nullopt, hook, false, 3)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    // The failed attempt and the successful retry are both settled records.
    MIRA_CHECK(result.value().steps.size() == 2);
    MIRA_CHECK(result.value().steps.front().attempt == 1);
    MIRA_CHECK(result.value().steps.back().attempt == 2);
    return 0;
}

int retry_hook_exhaustion_fails() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 10;
    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::Retry;
    hook.max_retries = 1;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("retry-exhausted");
    definition.steps = {tool_step("counter", std::nullopt, hook, false, 2)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    return 0;
}

int fallback_hook_advances_the_cursor() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 10; // The primary path always fails.
    CountingTool rescue;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, rescue.registration("rescue")));
    auto workflow = fixture.make_workflow();

    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::FallbackStep;
    auto fallback = tool_step("rescue", std::nullopt);
    hook.fallback_step = fallback.id;
    auto primary = tool_step("counter", std::nullopt, hook);

    auto definition = base_definition("fallback");
    definition.steps = {primary, fallback};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.value().ir_digest == workflow_definition_digest(definition));
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(result.value().steps.front().disposition == WorkflowStepDisposition::Failed);
    MIRA_CHECK(result.value().steps.back().disposition == WorkflowStepDisposition::Completed);
    MIRA_CHECK(counter.dispatches.load() == 1);
    MIRA_CHECK(rescue.dispatches.load() == 1);
    return 0;
}

int side_effect_retry_observes_first() {
    WorkflowFixture fixture;
    CountingTool effects;
    effects.failures_first = 1;
    effects.side_effects = true;
    MIRA_CHECK(register_registration(*fixture.registry_, effects.registration("actor", "count")));

    auto counting_environment =
        std::make_shared<ObservationCountingEnvironment>(fixture.environment_);
    WorkflowRuntime workflow(fixture.executor_, *fixture.runtime_, fixture.session_id_,
                             counting_environment);
    workflow.set_event_store(fixture.events_);
    workflow.set_tool_registry(fixture.registry_);

    WorkflowRecoveryHook hook;
    hook.mode = WorkflowRecoveryHook::Mode::Retry;
    hook.max_retries = 2;
    auto head = tool_step("actor", std::nullopt, hook, false, 3);
    head.verification =
        step_result_predicate(head.id, WorkflowPredicateOp::Ge, JsonValue{static_cast<std::int64_t>(1)});

    auto definition = base_definition("retry-observe");
    definition.steps = {head};
    const auto created =
        workflow.create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow.execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(effects.dispatches.load() == 2);
    // RULE-05: the redispatch was preceded by a fresh observation, and the
    // successful side-effect dispatch was verified against another one.
    MIRA_CHECK(counting_environment->observations() >= 2);
    return 0;
}

int verify_not_evaluable_fails_closed() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // The ghost step is skipped, so its step_result never exists: Strict
    // fails the referencing verification closed instead of passing it.
    auto ghost = tool_step("counter", std::nullopt);
    ghost.precondition =
        parameter_predicate("mode", WorkflowPredicateOp::Ne, JsonValue{"skip"});
    auto definition = base_definition("not-evaluable");
    definition.steps = {
        ghost,
        tool_step("counter", step_result_predicate(ghost.id, WorkflowPredicateOp::Eq,
                                                   JsonValue{"sent"})),
    };
    const auto created = workflow->create_run(
        definition, JsonValue{JsonValue::Object{{"mode", JsonValue{"skip"}}}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.front().disposition == WorkflowStepDisposition::Skipped);
    MIRA_CHECK(result.value().steps.back().disposition == WorkflowStepDisposition::Failed);
    return 0;
}

int step_budget_exhaustion_fails() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration("counter", "count")));
    WorkflowRuntimeConfig config;
    config.max_step_executions_per_run = 2;
    auto workflow = fixture.make_workflow(config);

    auto definition = base_definition("budget");
    auto head = tool_step("counter", std::nullopt, {}, true);
    definition.steps = {head, control_step(head.id, 8)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().safe_summary.find("run step budget exhausted") != std::string::npos);
    return 0;
}

int run_table_capacity_is_bounded() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    WorkflowRuntimeConfig config;
    config.max_active_runs = 1;
    auto workflow = fixture.make_workflow(config);

    auto definition = base_definition("capacity");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto first =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(first.has_value());
    const auto second =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!second.has_value());
    MIRA_CHECK(second.error().code == ErrorCode::ResourceExhausted);

    const auto result = workflow->execute_run(first.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    const auto third =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(third.has_value());
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        strict_run_completes_with_verification,
        precondition_skips_steps,
        bounded_loop_exits_through_precondition,
        bounded_loop_budget_fails_closed,
        dry_run_plans_without_dispatch,
        dry_run_evaluable_predicates_are_enforced,
        summaryless_definitions_round_trip,
        policy_admission_follows_allowed_set,
        admission_fails_closed,
        retry_hook_recovers_scripted_failure,
        retry_hook_exhaustion_fails,
        fallback_hook_advances_the_cursor,
        side_effect_retry_observes_first,
        verify_not_evaluable_fails_closed,
        step_budget_exhaustion_fails,
        run_table_capacity_is_bounded,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
