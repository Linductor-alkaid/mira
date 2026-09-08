// M11-03/M11-04: the publish gate and its audit events (DEC-025 §3).
// Validated drafts enter the library as DryRunPassed versions with
// content-derived evidence; gate failures leave the library untouched and
// are audited through the three publish events; replaying the same content
// and evidence settles as an idempotent NoOp.

#include "support/m11_support.hpp"

#include <algorithm>

namespace {

using namespace mira;
using namespace mira::testing;

int gate_publishes_validated_draft_and_it_runs() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.side_effects = false;
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
    const auto compiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(compiled.has_value());

    const auto published = workflow->publish_validated(compiled.value(), "m11-test",
                                                       "first compilation", run_id);
    MIRA_CHECK(published.has_value());
    MIRA_CHECK(!published.value().idempotent);
    MIRA_CHECK(published.value().ir_digest == workflow_definition_digest(compiled.value()));

    // The new version is runnable and completes with the baked defaults
    // (no parameters supplied).
    const auto rerun = workflow->create_run(
        definition.workflow_id, published.value().ir_digest,
        JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(rerun.has_value());
    const auto driven = workflow->execute_run(rerun.value().run_id, drive_context());
    MIRA_CHECK(driven.has_value());
    MIRA_CHECK(driven.value().state == WorkflowRunState::Completed);
    return 0;
}

int gate_failure_leaves_library_untouched() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // A structurally valid draft whose DryRun cannot reach Completed: the
    // Verify step's predicate is not satisfiable with the baked default.
    auto definition = compilable_definition(
        {tool_step("counter", std::nullopt),
         verify_step(parameter_predicate("mode", WorkflowPredicateOp::Eq, JsonValue{"other"}))},
        "m11-flow", true);

    const auto published =
        workflow->publish_validated(definition, "m11-test", "doomed draft", std::nullopt);
    MIRA_CHECK(!published.has_value());

    // The library never learned the draft and a rejection was audited.
    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowPublishProposed") != types.end());
    MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowPublishRejected") != types.end());
    MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowPublishApplied") == types.end());

    // A later good publish still lands as the first version (1.0.1): the
    // failed attempt left no record behind.
    auto good = compilable_definition({tool_step("counter", std::nullopt)});
    const auto recovered =
        workflow->publish_validated(good, "m11-test", "recovered draft", std::nullopt);
    MIRA_CHECK(recovered.has_value());
    const auto replay = workflow->create_run(good.workflow_id, recovered.value().ir_digest,
                                             JsonValue{JsonValue::Object{}},
                                             WorkflowPolicy::DryRun);
    MIRA_CHECK(replay.has_value());
    return 0;
}

int gate_replay_is_idempotent() {
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
    const auto compiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(compiled.has_value());

    const auto first = workflow->publish_validated(compiled.value(), "m11-test", "compile",
                                                   run_id);
    MIRA_CHECK(first.has_value() && !first.value().idempotent);
    const auto second = workflow->publish_validated(compiled.value(), "m11-test", "compile",
                                                   run_id);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(second.value().idempotent);
    MIRA_CHECK(second.value().ir_digest == first.value().ir_digest);
    MIRA_CHECK(second.value().evidence == first.value().evidence);

    // Content-derived evidence: the same definition independently recompiled
    // (fresh digest computation, identical bytes) produces the same evidence.
    const auto recompiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(recompiled.has_value());
    MIRA_CHECK(workflow_definition_digest(recompiled.value()) == first.value().ir_digest);
    return 0;
}

int gate_rejects_invalid_drafts_and_required_parameters() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    // Structurally invalid: a Control step without a jump target.
    auto broken = compilable_definition({tool_step("counter", std::nullopt)});
    WorkflowStep dangling;
    dangling.id = StepId::generate();
    dangling.name = "dangling";
    dangling.kind = WorkflowStepKind::Control;
    broken.steps.push_back(dangling);
    MIRA_CHECK(!workflow->publish_validated(broken, "m11", "invalid", std::nullopt).has_value());

    // Structurally valid but not default-complete: a required parameter with
    // no default cannot pass the empty-parameter DryRun gate.
    auto demanding = compilable_definition({tool_step("counter", std::nullopt)});
    WorkflowParameterSpec spec;
    spec.name = "must";
    spec.type = WorkflowParameterType::String;
    spec.required = true;
    demanding.parameters.push_back(spec);
    MIRA_CHECK(
        !workflow->publish_validated(demanding, "m11", "required parameter", std::nullopt)
             .has_value());

    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    const auto rejections = std::count(types.begin(), types.end(), "WorkflowPublishRejected");
    MIRA_CHECK(rejections == 2);
    return 0;
}

int publish_events_round_trip_through_payloads() {
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
    const auto compiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(compiled.has_value());
    const auto published =
        workflow->publish_validated(compiled.value(), "m11", "compile", run_id);
    MIRA_CHECK(published.has_value());

    // Every emitted publish payload parses back with its identity intact and
    // fails closed on mutation (DEC-022 §5 conventions).
    const auto proposed = session_event_payloads(*fixture.events_, fixture.session_id_,
                                                  "WorkflowPublishProposed");
    MIRA_CHECK(proposed.size() == 1);
    const auto parsed_proposed = parse_workflow_publish_proposed(
        EventPayload{"WorkflowPublishProposed",
                     to_json_string(proposed.front()), EventClass::State});
    MIRA_CHECK(parsed_proposed.has_value());
    MIRA_CHECK(parsed_proposed.value().source_run_id.has_value());
    MIRA_CHECK(*parsed_proposed.value().source_run_id == run_id);
    MIRA_CHECK(parsed_proposed.value().ir_digest == published.value().ir_digest);

    const auto applied = session_event_payloads(*fixture.events_, fixture.session_id_,
                                                "WorkflowPublishApplied");
    MIRA_CHECK(applied.size() == 1);
    const auto parsed_applied = parse_workflow_publish_applied(
        EventPayload{"WorkflowPublishApplied", to_json_string(applied.front()),
                     EventClass::State});
    MIRA_CHECK(parsed_applied.has_value());
    MIRA_CHECK(parsed_applied.value().evidence == published.value().evidence);
    MIRA_CHECK(parsed_applied.value().dry_run_id == published.value().dry_run_id);

    // Unknown fields fail closed.
    auto mutated = applied.front();
    mutated.set("extra", JsonValue{"field"});
    MIRA_CHECK(!parse_workflow_publish_applied(
                    EventPayload{"WorkflowPublishApplied", to_json_string(mutated),
                                 EventClass::State})
                 .has_value());

    MIRA_CHECK(is_workflow_event_type("WorkflowPublishProposed"));
    MIRA_CHECK(is_workflow_event_type("WorkflowPublishApplied"));
    MIRA_CHECK(is_workflow_event_type("WorkflowPublishRejected"));
    MIRA_CHECK(!is_workflow_event_type("WorkflowCompiled"));
    return 0;
}

int publish_workflow_raw_path_stays_unaudited() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = compilable_definition({tool_step("counter", std::nullopt)});

    // The M8 host-trust-boundary path still appends NotValidated records and
    // emits no publish events.
    const auto digest = workflow->publish_workflow(definition, "m11", "raw publish");
    MIRA_CHECK(digest.has_value());
    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowPublishProposed") ==
               types.end());

    // The gated path on other content appends a DryRunPassed record that is
    // runnable through library-resolved creation.
    auto gated = compilable_definition({tool_step("counter", std::nullopt)}, "m11-gated");
    const auto published =
        workflow->publish_validated(gated, "m11", "gated publish", std::nullopt);
    MIRA_CHECK(published.has_value());
    const auto runnable = workflow->create_run(gated.workflow_id, published.value().ir_digest,
                                               JsonValue{JsonValue::Object{}},
                                               WorkflowPolicy::DryRun);
    MIRA_CHECK(runnable.has_value());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {gate_publishes_validated_draft_and_it_runs, gate_failure_leaves_library_untouched,
         gate_replay_is_idempotent, gate_rejects_invalid_drafts_and_required_parameters,
         publish_events_round_trip_through_payloads, publish_workflow_raw_path_stays_unaudited});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
