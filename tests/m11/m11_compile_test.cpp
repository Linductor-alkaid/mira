// M11-01/M11-02: trajectory capture and the literal compiler (DEC-025 §1/§2).
// A completed side-effecting run captures its effective state (patched
// arguments, effective parameters, skipped steps dropped); compilation
// bakes observed defaults, keeps predicates and jump structure, and stays
// deterministic. DryRun-planned completions and non-terminal runs never
// compile.

#include "support/m11_support.hpp"

namespace {

using namespace mira;
using namespace mira::testing;

int capture_requires_completed_dispatching_run() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.side_effects = false;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = compilable_definition({tool_step("counter", std::nullopt)});

    // Non-terminal: a Created run was never driven.
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    const auto not_driven = workflow->capture_trajectory(created.value().run_id);
    MIRA_CHECK(!not_driven.has_value());

    // A DryRun completion planned only: capturing it must fail closed.
    const auto planned = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::DryRun);
    MIRA_CHECK(planned.has_value());
    const auto planned_drive = workflow->execute_run(planned.value().run_id, drive_context());
    MIRA_CHECK(planned_drive.has_value());
    MIRA_CHECK(planned_drive.value().state == WorkflowRunState::Completed);
    const auto planned_capture = workflow->capture_trajectory(planned.value().run_id);
    MIRA_CHECK(!planned_capture.has_value());

    // Unknown run.
    const auto unknown = workflow->capture_trajectory(WorkflowRunId::generate());
    MIRA_CHECK(!unknown.has_value());

    // A real completion captures with full provenance.
    const auto run = run_to_completion(*workflow, definition, JsonValue{JsonValue::Object{}});
    MIRA_CHECK(run.has_value());
    const WorkflowRunId run_id = run.value();
    const auto trajectory = workflow->capture_trajectory(run_id);
    MIRA_CHECK(trajectory.has_value());
    MIRA_CHECK(trajectory.value().source_run_id.has_value());
    MIRA_CHECK(*trajectory.value().source_run_id == run_id);
    MIRA_CHECK(trajectory.value().source_digest ==
               workflow_definition_digest(definition));
    MIRA_CHECK(trajectory.value().steps.size() == 1);
    MIRA_CHECK(trajectory.value().steps.front().kind == WorkflowStepKind::ToolCall);
    const auto *payload =
        trajectory.value().steps.front().arguments.find("payload");
    MIRA_CHECK(payload != nullptr && payload->is_string());
    return 0;
}

int capture_reflects_patch_effective_state() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.side_effects = true;
    counter.failures_first = 1; // The first dispatch escalates for repair.
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     counter.registration("counter", "sent")));
    auto workflow = fixture.make_workflow();
    const auto repaired = tool_step(
        "counter", parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"run"}));
    auto definition = compilable_definition({repaired}, "m11-patched", true);

    // WaitingAgent is a patch boundary (DEC-024 §1 agent-repair channel):
    // the agent repairs the arguments, resumes, and the run completes with
    // the patched effective state.
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                             WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());
    const auto waiting = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(waiting.has_value());
    MIRA_CHECK(waiting.value().state == WorkflowRunState::WaitingAgent);

    const auto patched = workflow->patch_run(
        created.value().run_id, WorkflowPatchId::generate(),
        {WorkflowPatchEntry{
            WorkflowPatchTarget::StepArguments, WorkflowPatchOp::Set, repaired.id.to_string(),
            JsonValue{JsonValue::Object{{"tool", JsonValue{"counter"}},
                                        {"payload", JsonValue{"patched"}}}}}});
    MIRA_CHECK(patched.has_value());
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);

    // Capture reflects the patched arguments; provenance keeps the source form.
    const auto trajectory = workflow->capture_trajectory(created.value().run_id);
    MIRA_CHECK(trajectory.has_value());
    const auto *payload = trajectory.value().steps.front().arguments.find("payload");
    MIRA_CHECK(payload != nullptr && payload->is_string() &&
               *payload->as_string() == "patched");
    const auto *raw = trajectory.value().steps.front().raw_arguments.find("payload");
    MIRA_CHECK(raw != nullptr && raw->is_string() && *raw->as_string() == "payload");
    return 0;
}

int compile_bakes_defaults_and_preserves_structure() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    auto first = tool_step("counter", std::nullopt);
    auto second = tool_step("counter", std::nullopt);
    auto head = tool_step("counter", std::nullopt, {}, true);
    // The precondition is not satisfied by the run's parameters, so the loop
    // settles as Skipped and the run completes linearly; the structure still
    // travels through capture and compilation.
    auto loop = control_step(head.id, 4,
                             parameter_predicate("mode", WorkflowPredicateOp::Eq,
                                                 JsonValue{"run"}));
    auto definition = compilable_definition({head, first, second, loop}, "m11-flow", true);
    MIRA_CHECK(!definition.parameters.empty());

    JsonValue::Object parameters;
    parameters.emplace_back("mode", JsonValue{"fast"});
    const auto run = run_to_completion(*workflow, definition,
                                       JsonValue{std::move(parameters)});
    MIRA_CHECK(run.has_value());
    const WorkflowRunId run_id = run.value();
    const auto trajectory = workflow->capture_trajectory(run_id);
    MIRA_CHECK(trajectory.has_value());

    WorkflowCompileOptions options;
    options.workflow_id = definition.workflow_id;
    options.name = "compiled-flow";
    options.summary = "baked from a successful run";
    const auto compiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(compiled.has_value());

    // Default baked from the observed value; required flipped to optional.
    MIRA_CHECK(compiled.value().parameters.size() == 1);
    MIRA_CHECK(compiled.value().parameters.front().name == "mode");
    MIRA_CHECK(!compiled.value().parameters.front().required);
    const auto *baked = compiled.value().parameters.front().default_value.as_string();
    MIRA_CHECK(baked != nullptr && *baked == "fast");

    // Step structure, ids (predicate and jump references) and loop budget
    // survive compilation.
    MIRA_CHECK(compiled.value().steps.size() == 4);
    MIRA_CHECK(compiled.value().steps.front().id == head.id);
    MIRA_CHECK(compiled.value().steps.front().loop_head);
    const auto &control = compiled.value().steps.back();
    MIRA_CHECK(control.kind == WorkflowStepKind::Control);
    MIRA_CHECK(control.jump_to.has_value() && *control.jump_to == head.id);
    MIRA_CHECK(control.max_iterations == 4);
    MIRA_CHECK(control.precondition.has_value() &&
               control.precondition->signal == "run_parameter:mode");

    // Policy set carried over; the compiled draft passes the same validation.
    MIRA_CHECK(compiled.value().allowed_policies.size() ==
               definition.allowed_policies.size());
    MIRA_CHECK(compiled.value().default_policy == definition.default_policy);
    MIRA_CHECK(validate_workflow_definition(compiled.value()).has_value());
    return 0;
}

int compile_is_deterministic_and_fail_closed() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = compilable_definition({tool_step("counter", std::nullopt)});
    const auto run = run_to_completion(*workflow, definition, JsonValue{JsonValue::Object{}});
    MIRA_CHECK(run.has_value());
    const WorkflowRunId run_id = run.value();
    const auto trajectory = workflow->capture_trajectory(run_id);
    MIRA_CHECK(trajectory.has_value());

    WorkflowCompileOptions options;
    options.workflow_id = definition.workflow_id;
    options.name = "compiled-flow";
    const auto left = compile_workflow(trajectory.value(), options);
    const auto right = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(left.has_value() && right.has_value());
    MIRA_CHECK(workflow_definition_digest(left.value()) ==
               workflow_definition_digest(right.value()));

    // Nil id and empty name reject; a policy outside the allowed set rejects.
    WorkflowCompileOptions bad = options;
    bad.workflow_id = WorkflowId{};
    MIRA_CHECK(!compile_workflow(trajectory.value(), bad).has_value());
    bad = options;
    bad.name.clear();
    MIRA_CHECK(!compile_workflow(trajectory.value(), bad).has_value());
    bad = options;
    bad.default_policy = WorkflowPolicy::AgentAssisted; // Not in the fixture allowed set.
    MIRA_CHECK(!compile_workflow(trajectory.value(), bad).has_value());

    // A control jump whose target survives compilation stays intact; one
    // whose target was dropped fails closed instead of being silently rewired.
    WorkflowTrajectory host_built = trajectory.value();
    host_built.steps.front().loop_head = true;
    WorkflowTrajectoryStep spinner;
    spinner.source_step_id = StepId::generate();
    spinner.name = "loop";
    spinner.kind = WorkflowStepKind::Control;
    spinner.jump_to = host_built.steps.front().source_step_id;
    spinner.max_iterations = 3;
    host_built.steps.push_back(std::move(spinner));
    const auto still_valid = compile_workflow(host_built, options);
    MIRA_CHECK(still_valid.has_value());
    MIRA_CHECK(still_valid.value().steps.back().jump_to.has_value());

    WorkflowTrajectory broken = trajectory.value();
    broken.steps.back().kind = WorkflowStepKind::Control;
    broken.steps.back().jump_to = StepId::generate(); // Dropped target.
    MIRA_CHECK(!compile_workflow(broken, options).has_value());

    // ToolCall steps must keep the reserved "tool" member.
    WorkflowTrajectory unbound = trajectory.value();
    unbound.steps.front().arguments = JsonValue{JsonValue::Object{{"payload", JsonValue{"x"}}}};
    MIRA_CHECK(!compile_workflow(unbound, options).has_value());
    return 0;
}

int host_built_trajectory_compiles_without_source() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // The agent path: the host fills the trajectory from successful tool
    // calls; no source definition exists.
    WorkflowTrajectory trajectory;
    trajectory.workflow_id = WorkflowId::generate();
    trajectory.effective_policy = WorkflowPolicy::Strict;
    WorkflowTrajectoryStep call;
    call.source_step_id = StepId::generate();
    call.name = "send-message";
    call.kind = WorkflowStepKind::ToolCall;
    call.max_attempts = 3;
    JsonValue::Object arguments;
    arguments.emplace_back("tool", JsonValue{"counter"});
    arguments.emplace_back("payload", JsonValue{"hello"}); // Constant leaf.
    arguments.emplace_back("recipient", JsonValue{"alice"}); // Varying leaf.
    call.arguments = JsonValue{std::move(arguments)};
    call.raw_arguments = call.arguments;
    trajectory.steps.push_back(std::move(call));

    WorkflowCompileOptions options;
    options.workflow_id = WorkflowId::generate();
    options.name = "agent-derived";
    const auto compiled = compile_workflow(trajectory, options);
    MIRA_CHECK(compiled.has_value());
    MIRA_CHECK(compiled.value().parameters.empty());
    MIRA_CHECK(compiled.value().steps.size() == 1);
    MIRA_CHECK(compiled.value().steps.front().max_attempts == 3);
    MIRA_CHECK(compiled.value().default_policy == WorkflowPolicy::Strict);
    MIRA_CHECK(compiled.value().allowed_policies.size() == 1);
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {capture_requires_completed_dispatching_run, capture_reflects_patch_effective_state,
         compile_bakes_defaults_and_preserves_structure, compile_is_deterministic_and_fail_closed,
         host_built_trajectory_compiles_without_source});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
