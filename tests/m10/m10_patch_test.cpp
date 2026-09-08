// M10-07..M10-09: the conversation patch plane (DEC-024). Admission by
// policy and state, write-before idempotency, step-boundary application,
// parameter rebuilding, skips, policy switching with the navigate gate,
// audit events, epoch semantics and explicit rollback.

#include "support/m10_support.hpp"

#include <chrono>
#include <memory>

namespace {

using namespace mira;
using namespace mira::testing;

// Parks one Interactive run at Paused through a scenario-private gate so
// patch submission lands in a waiting boundary state (DEC-024 §1).
struct ParkedRun final {
    std::unique_ptr<GatedTool> gate = std::make_unique<GatedTool>();
    WorkflowDefinition definition;
    WorkflowRunView view;

    [[nodiscard]] bool park(WorkflowRuntime &workflow, BuiltinToolRegistry &registry,
                            std::string name, int serial,
                            std::optional<WorkflowPredicate> counter_precondition = {},
                            std::vector<WorkflowPolicy> allowed = {}) {
        const std::string wire = "gate-" + std::to_string(serial);
        if (!register_registration(registry, gate->registration(wire)).has_value()) {
            return false;
        }
        definition = full_policy_definition(std::move(name));
        if (!allowed.empty()) {
            definition.allowed_policies = std::move(allowed);
            definition.default_policy = WorkflowPolicy::Interactive;
        }
        auto counter_step = tool_step("counter", std::nullopt);
        counter_step.precondition = std::move(counter_precondition);
        definition.steps = {tool_step(wire, std::nullopt), counter_step};
        const auto created =
            workflow.create_run(definition, JsonValue{JsonValue::Object{}},
                                WorkflowPolicy::Interactive);
        if (!created.has_value()) {
            return false;
        }
        view = created.value();
        if (!workflow.start_run(view.run_id).has_value()) {
            return false;
        }
        while (gate->entries.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto paused = workflow.pause_run(view.run_id);
        if (!paused.has_value()) {
            return false;
        }
        view = paused.value();
        return view.state == WorkflowRunState::Paused;
    }

    [[nodiscard]] bool finish(WorkflowRuntime &workflow) {
        gate->release.store(true);
        if (!workflow.resume_run(view.run_id).has_value()) {
            return false;
        }
        const auto settled = workflow.wait_run(view.run_id, std::chrono::seconds(10));
        return settled.has_value() && settled.value().state == WorkflowRunState::Completed;
    }
};

int admission_matrix_gates_patches_by_policy_and_state() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // Strict rejects patches outright at a step boundary (DEC-024 §1).
    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "patch-strict", 1));
    {
        auto strict_definition = parked.definition;
        strict_definition.workflow_id = WorkflowId::generate();
        strict_definition.name = "patch-strict-strict";
        const auto strict =
            workflow->create_run(strict_definition, JsonValue{JsonValue::Object{}},
                                 WorkflowPolicy::Strict);
        MIRA_CHECK(strict.has_value());
        MIRA_CHECK(workflow->start_run(strict.value().run_id).has_value());
        while (parked.gate->entries.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        MIRA_CHECK(workflow->pause_run(strict.value().run_id).has_value());
        const auto rejected =
            workflow->patch_run(strict.value().run_id, WorkflowPatchId::generate(),
                                {parameter_set("mode", JsonValue{"quiet"})});
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::UnsupportedCapability);
        parked.gate->release.store(true);
        MIRA_CHECK(workflow->resume_run(strict.value().run_id).has_value());
        MIRA_CHECK(workflow->wait_run(strict.value().run_id, std::chrono::seconds(10))
                       .value()
                       .state == WorkflowRunState::Completed);
    }
    MIRA_CHECK(parked.finish(*workflow));

    // Terminal runs reject fresh patches.
    const auto terminal =
        workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                            {parameter_set("mode", JsonValue{"loud"})});
    MIRA_CHECK(!terminal.has_value());
    MIRA_CHECK(terminal.error().code == ErrorCode::InvalidState);
    return 0;
}

int patch_idempotency_noop_and_conflict() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "patch-idempotency", 1));

    const auto patch_id = WorkflowPatchId::generate();
    const auto first = workflow->patch_run(parked.view.run_id, patch_id,
                                           {parameter_set("mode", JsonValue{"quiet"})});
    MIRA_CHECK(first.has_value() && first.value().applied);
    MIRA_CHECK(first.value().view.run_patch_epoch == 1);

    // Same id, same content: the legal NoOp.
    const auto replay = workflow->patch_run(parked.view.run_id, patch_id,
                                            {parameter_set("mode", JsonValue{"quiet"})});
    MIRA_CHECK(replay.has_value());
    MIRA_CHECK(!replay.value().applied);
    MIRA_CHECK(replay.value().view.run_patch_epoch == 1);

    // Same id, different content: the conflict.
    const auto conflict = workflow->patch_run(parked.view.run_id, patch_id,
                                              {parameter_set("mode", JsonValue{"loud"})});
    MIRA_CHECK(!conflict.has_value());
    MIRA_CHECK(conflict.error().code == ErrorCode::AlreadyExists);

    // A corrected patch with a fresh id applies and advances the epoch.
    const auto second = workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                            {parameter_set("mode", JsonValue{"loud"})});
    MIRA_CHECK(second.has_value() && second.value().applied);
    MIRA_CHECK(second.value().view.run_patch_epoch == 2);

    // Unknown paths fail closed without occupying an id.
    const auto unknown = workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                             {parameter_set("missing", JsonValue{"x"})});
    MIRA_CHECK(!unknown.has_value());
    MIRA_CHECK(workflow->run_snapshot(parked.view.run_id).value().run_patch_epoch == 2);
    MIRA_CHECK(parked.finish(*workflow));
    return 0;
}

int running_runs_apply_queued_patches_at_the_boundary() {
    WorkflowFixture fixture;
    CountingTool counter;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("patch-boundary");
    definition.steps = {tool_step("counter", std::nullopt), tool_step("gate", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());

    // Wait for the gated step to be in flight, then submit a patch: it must
    // queue (the in-flight step is unaffected) and land at the boundary.
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto queued =
        workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(),
                            {parameter_set("mode", JsonValue{"boundary"})});
    MIRA_CHECK(queued.has_value());
    MIRA_CHECK(queued.value().applied);
    MIRA_CHECK(queued.value().queued);
    MIRA_CHECK(queued.value().view.run_patch_epoch == 0); // Not applied yet.

    gate.release.store(true);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().run_patch_epoch == 1);
    return 0;
}

int parameter_patches_rebuild_bindings_and_predicates() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     counter.registration("counter", "sent")));
    auto workflow = fixture.make_workflow();

    // The counter step of the parked flow runs only when the patched
    // parameter says so; the pre-patch value gates it out via the
    // precondition. The gate step always runs.
    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "param-rebuild", 1,
                           parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"go"})));
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                   {parameter_set("mode", JsonValue{"go"})})
                   .has_value());
    MIRA_CHECK(parked.finish(*workflow));
    // The precondition-armed counter step ran after the patch.
    MIRA_CHECK(counter.dispatches.load() == 1);

    // Unsetting an optional parameter falls back to its declared default
    // ("run"), which the precondition no longer matches.
    ParkedRun second_parked;
    MIRA_CHECK(second_parked.park(
        *workflow, *fixture.registry_, "param-rebuild", 2,
        parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"go"})));
    MIRA_CHECK(workflow->patch_run(second_parked.view.run_id, WorkflowPatchId::generate(),
                                   {parameter_set("mode", JsonValue{"go"}),
                                    parameter_unset("mode")})
                   .has_value());
    MIRA_CHECK(second_parked.finish(*workflow));
    MIRA_CHECK(counter.dispatches.load() == 1);
    const auto result = workflow->wait_run(second_parked.view.run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().steps.back().disposition == WorkflowStepDisposition::Skipped);
    return 0;
}

int skip_and_policy_switch_entries_execute() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "skip-and-switch", 1));
    // Skip the gate step entirely and switch the policy to Recoverable.
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                   {step_skip(parked.definition.steps.front()),
                                    policy_set(WorkflowPolicy::Recoverable)})
                   .has_value());
    MIRA_CHECK(workflow->run_snapshot(parked.view.run_id).value().policy ==
               WorkflowPolicy::Recoverable);
    MIRA_CHECK(parked.finish(*workflow));
    const auto result = workflow->wait_run(parked.view.run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    // The pause cut the gate step short (Stale); the patched skip then
    // settled it instead of re-executing, and only the counter ran.
    MIRA_CHECK(result.value().steps.size() == 3);
    MIRA_CHECK(result.value().steps.front().disposition == WorkflowStepDisposition::Stale);
    MIRA_CHECK(result.value().steps.at(1).disposition == WorkflowStepDisposition::Skipped);
    MIRA_CHECK(counter.dispatches.load() == 1);
    MIRA_CHECK(parked.gate->entries.load() == 1);

    // A policy outside the run's pinned allowed set is rejected.
    ParkedRun narrow;
    MIRA_CHECK(narrow.park(*workflow, *fixture.registry_, "narrow-switch", 2, std::nullopt,
                           {WorkflowPolicy::Interactive}));
    const auto outside =
        workflow->patch_run(narrow.view.run_id, WorkflowPatchId::generate(),
                            {policy_set(WorkflowPolicy::Recoverable)});
    MIRA_CHECK(!outside.has_value());
    MIRA_CHECK(narrow.finish(*workflow));

    // Switching a run with navigate steps to a dispatching policy fails
    // closed (the resolver arrives with phase E).
    auto navigating = full_policy_definition("navigate-switch");
    navigating.steps = {navigate_step("home")};
    const auto dry = workflow->create_run(navigating, JsonValue{JsonValue::Object{}},
                                          WorkflowPolicy::DryRun);
    MIRA_CHECK(dry.has_value());
    const auto switched = workflow->patch_run(dry.value().run_id, WorkflowPatchId::generate(),
                                              {policy_set(WorkflowPolicy::Interactive)});
    MIRA_CHECK(!switched.has_value());
    return 0;
}

int waiting_agent_runs_accept_repair_patches() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.failures_first = 1;
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     counter.registration("counter", "count")));
    auto workflow = fixture.make_workflow();

    auto definition = full_policy_definition("repair-patch");
    definition.steps = {tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context())
                   .value()
                   .state == WorkflowRunState::WaitingAgent);

    // The repairing agent patches run parameters in the wait state
    // (DEC-024 §1), then resumes.
    const auto patched = workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(),
                                             {parameter_set("mode", JsonValue{"repaired"})});
    MIRA_CHECK(patched.has_value() && patched.value().applied);
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);

    // Recoverable runs outside the wait state still reject patches (a
    // paused boundary is not an agent-repair wait).
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration("gate")));
    auto gated = definition;
    gated.workflow_id = WorkflowId::generate();
    gated.steps = {tool_step("gate", std::nullopt)};
    const auto busy = workflow->create_run(gated, JsonValue{JsonValue::Object{}},
                                           WorkflowPolicy::Recoverable);
    MIRA_CHECK(busy.has_value());
    MIRA_CHECK(workflow->start_run(busy.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIRA_CHECK(workflow->pause_run(busy.value().run_id).has_value());
    const auto rejected =
        workflow->patch_run(busy.value().run_id, WorkflowPatchId::generate(),
                            {parameter_set("mode", JsonValue{"x"})});
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().code == ErrorCode::UnsupportedCapability);
    gate.release.store(true);
    MIRA_CHECK(workflow->resume_run(busy.value().run_id).has_value());
    MIRA_CHECK(workflow->wait_run(busy.value().run_id, std::chrono::seconds(10))
                   .value()
                   .state == WorkflowRunState::Completed);
    return 0;
}

int patch_audit_events_and_epoch_semantics() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "patch-audit", 1));

    const auto patch_id = WorkflowPatchId::generate();
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, patch_id,
                                   {parameter_set("mode", JsonValue{"one"})})
                   .has_value());
    // A rejected patch (unknown parameter) emits Proposed then Rejected and
    // does not advance the epoch.
    MIRA_CHECK(!workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                    {parameter_set("missing", JsonValue{"x"})})
                    .has_value());
    MIRA_CHECK(workflow->run_snapshot(parked.view.run_id).value().run_patch_epoch == 1);

    const auto proposed =
        session_event_payloads(*fixture.events_, fixture.session_id_, "WorkflowPatchProposed");
    MIRA_CHECK(proposed.size() == 2);
    MIRA_CHECK(*proposed.front().find("patch_id")->as_string() == patch_id.to_string());
    MIRA_CHECK(*proposed.back().find("target")->as_string() == "run_parameters");

    const auto applied =
        session_event_payloads(*fixture.events_, fixture.session_id_, "WorkflowPatchApplied");
    MIRA_CHECK(applied.size() == 1);
    MIRA_CHECK(applied.front().find("run_patch_epoch")->as_integer().value() == 1);

    const auto rejected =
        session_event_payloads(*fixture.events_, fixture.session_id_, "WorkflowPatchRejected");
    MIRA_CHECK(rejected.size() == 1);
    MIRA_CHECK(rejected.front().find("reason_code")->is_string());

    // A policy switch emits WorkflowPolicySwitched.
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                   {policy_set(WorkflowPolicy::Recoverable)})
                   .has_value());
    const auto switched =
        session_event_payloads(*fixture.events_, fixture.session_id_, "WorkflowPolicySwitched");
    MIRA_CHECK(switched.size() == 1);
    MIRA_CHECK(*switched.front().find("from")->as_string() == "interactive");
    MIRA_CHECK(*switched.front().find("to")->as_string() == "recoverable");
    MIRA_CHECK(parked.finish(*workflow));
    return 0;
}

int rollback_restores_the_boundary_state() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    ParkedRun parked;
    MIRA_CHECK(parked.park(*workflow, *fixture.registry_, "patch-rollback", 1));

    const auto boundary = WorkflowPatchId::generate();
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, boundary,
                                   {parameter_set("mode", JsonValue{"keep"})})
                   .has_value());
    // A later patch drifts the effective state away.
    MIRA_CHECK(workflow->patch_run(parked.view.run_id, WorkflowPatchId::generate(),
                                   {parameter_set("mode", JsonValue{"drifted"}),
                                    step_skip(parked.definition.steps.back())})
                   .has_value());
    MIRA_CHECK(workflow->run_snapshot(parked.view.run_id).value().run_patch_epoch == 2);

    const auto rollback = workflow->rollback_run_patch(parked.view.run_id, boundary);
    MIRA_CHECK(rollback.has_value() && rollback.value().applied);
    MIRA_CHECK(rollback.value().view.run_patch_epoch == 3);

    // The effective state matches the boundary again: the skip is gone, so
    // the drifted-patch skip never settles and both steps execute (the
    // pause-cut gate step keeps its Stale record from before the boundary).
    MIRA_CHECK(parked.finish(*workflow));
    const auto result = workflow->wait_run(parked.view.run_id, std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    for (const auto &record : result.value().steps) {
        MIRA_CHECK(record.disposition != WorkflowStepDisposition::Skipped);
    }
    MIRA_CHECK(counter.dispatches.load() == 1);
    MIRA_CHECK(parked.gate->entries.load() == 2);

    // Rolling back an unknown patch fails closed.
    const auto unknown =
        workflow->rollback_run_patch(parked.view.run_id, WorkflowPatchId::generate());
    MIRA_CHECK(!unknown.has_value());
    MIRA_CHECK(unknown.error().code == ErrorCode::NotFound);
    return 0;
}

int pending_patch_queue_is_bounded() {
    WorkflowFixture fixture;
    CountingTool counter;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    WorkflowRuntimeConfig config;
    config.max_pending_patches_per_run = 2;
    auto workflow = fixture.make_workflow(config);

    auto definition = full_policy_definition("patch-capacity");
    definition.steps = {tool_step("gate", std::nullopt), tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    MIRA_CHECK(workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(),
                                   {parameter_set("mode", JsonValue{"a"})})
                   .has_value());
    MIRA_CHECK(workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(),
                                   {parameter_set("mode", JsonValue{"b"})})
                   .has_value());
    const auto overflow = workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(),
                                              {parameter_set("mode", JsonValue{"c"})});
    MIRA_CHECK(!overflow.has_value());
    MIRA_CHECK(overflow.error().code == ErrorCode::ResourceExhausted);

    gate.release.store(true);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().run_patch_epoch == 2);
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        admission_matrix_gates_patches_by_policy_and_state,
        patch_idempotency_noop_and_conflict,
        running_runs_apply_queued_patches_at_the_boundary,
        parameter_patches_rebuild_bindings_and_predicates,
        skip_and_policy_switch_entries_execute,
        waiting_agent_runs_accept_repair_patches,
        patch_audit_events_and_epoch_semantics,
        rollback_restores_the_boundary_state,
        pending_patch_queue_is_bounded,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
