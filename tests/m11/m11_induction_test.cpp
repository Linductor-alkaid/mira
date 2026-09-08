// M11-05..M11-07: task induction, parameterized compilation and the stage-D
// end-to-end loop (DEC-026). Structural diff across same-skeleton trajectories
// proposes candidates (provenance names win, constants stay literal),
// parameterized compilation rewrites leaves into $param references with
// inferred specs, and the compiled draft re-enters execution through the
// publish gate with a parameter the caller can vary.

#include "support/m11_support.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] WorkflowDefinition induction_definition() {
    auto definition = base_definition("m11-induct", true);
    definition.steps = {parameterized_tool_step("mode"),
                        tool_step("channeler", std::nullopt)};
    return definition;
}

int structural_diff_names_varying_leaves_only() {
    WorkflowFixture fixture;
    ChannelTool channeler;
    MIRA_CHECK(register_registration(*fixture.registry_, channeler.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = induction_definition();

    const auto fast = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(fast.has_value());
    const WorkflowRunId fast_id = fast.value();
    const auto slow = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"slow"}}}});
    MIRA_CHECK(slow.has_value());
    const WorkflowRunId slow_id = slow.value();
    const auto fast_trajectory = workflow->capture_trajectory(fast_id);
    const auto slow_trajectory = workflow->capture_trajectory(slow_id);
    MIRA_CHECK(fast_trajectory.has_value() && slow_trajectory.has_value());

    const auto candidates =
        induce_parameters({fast_trajectory.value(), slow_trajectory.value()});
    MIRA_CHECK(candidates.has_value());
    MIRA_CHECK(candidates.value().size() == 1);
    const auto &candidate = candidates.value().front();
    MIRA_CHECK(candidate.name == "mode"); // Provenance name from the $param reference.
    MIRA_CHECK(candidate.provenance == WorkflowCandidateProvenance::Provenance);
    MIRA_CHECK(candidate.step_index == 0);
    MIRA_CHECK(candidate.pointer == "/payload");
    MIRA_CHECK(candidate.observed_values.size() == 2);
    MIRA_CHECK(*candidate.observed_values.front().as_string() == "fast");

    // Identical runs induce nothing: every leaf is constant.
    const auto again = run_to_completion(*workflow, definition,
                                         JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(again.has_value());
    const WorkflowRunId again_id = again.value();
    const auto again_trajectory = workflow->capture_trajectory(again_id);
    MIRA_CHECK(again_trajectory.has_value());
    const auto constants =
        induce_parameters({fast_trajectory.value(), again_trajectory.value()});
    MIRA_CHECK(constants.has_value());
    MIRA_CHECK(constants.value().empty());
    return 0;
}

int parameterized_compilation_reopens_parameters() {
    WorkflowFixture fixture;
    ChannelTool channeler;
    MIRA_CHECK(register_registration(*fixture.registry_, channeler.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = induction_definition();

    const auto fast = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(fast.has_value());
    const WorkflowRunId fast_id = fast.value();
    const auto slow = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"slow"}}}});
    MIRA_CHECK(slow.has_value());
    const WorkflowRunId slow_id = slow.value();
    const auto fast_trajectory = workflow->capture_trajectory(fast_id);
    const auto slow_trajectory = workflow->capture_trajectory(slow_id);
    MIRA_CHECK(fast_trajectory.has_value() && slow_trajectory.has_value());
    const auto candidates =
        induce_parameters({fast_trajectory.value(), slow_trajectory.value()});
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);

    // Compile the anchor trajectory with the candidate: the leaf reopens as a
    // $param reference and the existing spec is reused with the baked default.
    WorkflowCompileOptions options;
    options.workflow_id = definition.workflow_id;
    options.name = "induced-flow";
    const auto compiled = compile_workflow(fast_trajectory.value(), options, candidates.value());
    MIRA_CHECK(compiled.has_value());
    MIRA_CHECK(compiled.value().parameters.size() == 1);
    MIRA_CHECK(compiled.value().parameters.front().name == "mode");
    MIRA_CHECK(!compiled.value().parameters.front().required);
    const auto *baked = compiled.value().parameters.front().default_value.as_string();
    MIRA_CHECK(baked != nullptr && *baked == "fast");

    const auto *payload = compiled.value().steps.front().arguments.find("payload");
    MIRA_CHECK(payload != nullptr && payload->is_object() && payload->as_object()->size() == 1);
    const auto *reference = payload->find("$param");
    MIRA_CHECK(reference != nullptr && reference->is_string() &&
               *reference->as_string() == "mode");
    // The constant channel leaf stays literal.
    const auto *channel = compiled.value().steps.front().arguments.find("channel");
    MIRA_CHECK(channel != nullptr && channel->is_string() &&
               *channel->as_string() == "default-channel");

    // Bind-time resolution: the default applies with no input, an explicit
    // value overrides it, and the binding pure functions do the work.
    const auto defaulted = bind_workflow_parameters(compiled.value(),
                                                    JsonValue{JsonValue::Object{}});
    MIRA_CHECK(defaulted.has_value());
    const auto resolved_default = resolve_step_arguments(defaulted.value(),
                                                         compiled.value().steps.front().arguments);
    MIRA_CHECK(resolved_default.has_value());
    MIRA_CHECK(*resolved_default.value().find("payload")->as_string() == "fast");
    const auto overridden = bind_workflow_parameters(
        compiled.value(), JsonValue{JsonValue::Object{{"mode", JsonValue{"turbo"}}}});
    MIRA_CHECK(overridden.has_value());
    const auto resolved_override = resolve_step_arguments(overridden.value(),
                                                          compiled.value().steps.front().arguments);
    MIRA_CHECK(resolved_override.has_value());
    MIRA_CHECK(*resolved_override.value().find("payload")->as_string() == "turbo");

    // Type mismatches surface as the existing deterministic bind errors.
    const auto bad_type = bind_workflow_parameters(
        compiled.value(), JsonValue{JsonValue::Object{{"mode", JsonValue{7}}}});
    MIRA_CHECK(!bad_type.has_value());
    return 0;
}

int explicit_candidates_validate_fail_closed() {
    WorkflowFixture fixture;
    ChannelTool channeler;
    MIRA_CHECK(register_registration(*fixture.registry_, channeler.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = induction_definition();
    const auto run = run_to_completion(*workflow, definition,
                                       JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(run.has_value());
    const WorkflowRunId run_id = run.value();
    const auto trajectory = workflow->capture_trajectory(run_id);
    MIRA_CHECK(trajectory.has_value());
    WorkflowCompileOptions options;
    options.workflow_id = WorkflowId::generate();
    options.name = "explicit";

    // A legal explicit candidate lifts the constant channel leaf.
    WorkflowParameterCandidate candidate;
    candidate.name = "channel";
    candidate.step_index = 0;
    candidate.pointer = "/channel";
    candidate.provenance = WorkflowCandidateProvenance::Explicit;
    candidate.observed_values = {JsonValue{"default-channel"}, JsonValue{"beta-channel"}};
    const auto compiled = compile_workflow(trajectory.value(), options, {candidate});
    MIRA_CHECK(compiled.has_value());
    MIRA_CHECK(compiled.value().parameters.size() == 2); // mode (source) + channel (induced)
    const auto &parameters = compiled.value().parameters;
    const auto channel_spec = std::find_if(
        parameters.begin(), parameters.end(),
        [](const WorkflowParameterSpec &spec) { return spec.name == "channel"; });
    MIRA_CHECK(channel_spec != parameters.end());
    MIRA_CHECK(channel_spec->type == WorkflowParameterType::String);
    MIRA_CHECK(!channel_spec->required);
    MIRA_CHECK(*channel_spec->default_value.as_string() == "default-channel");

    // Every shape violation fails closed with a deterministic code.
    const auto expect_failure = [&](WorkflowParameterCandidate bad,
                                    WorkflowCompileError code) {
        const auto outcome = compile_workflow(trajectory.value(), options, {bad});
        if (outcome.has_value() ||
            outcome.error().domain_code != static_cast<std::int32_t>(code)) {
            std::cerr << "expected compile failure with code " << static_cast<int>(code)
                      << '\n';
            return 1;
        }
        return 0;
    };
    WorkflowParameterCandidate invalid = candidate;
    invalid.name = "9lives";
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductCandidateInvalid);
        code != 0) {
        return code;
    }
    invalid = candidate;
    invalid.name = "mode"; // Collides with the declared parameter, not provenance.
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductNameConflict);
        code != 0) {
        return code;
    }
    invalid = candidate;
    invalid.pointer = "/tool"; // Reserved member.
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductReservedMember);
        code != 0) {
        return code;
    }
    invalid = candidate;
    invalid.pointer = "/missing";
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductLeafNotScalar);
        code != 0) {
        return code;
    }
    invalid = candidate;
    invalid.step_index = 9;
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductCandidateInvalid);
        code != 0) {
        return code;
    }
    invalid = candidate;
    invalid.provenance = WorkflowCandidateProvenance::Provenance; // Not a real ref of "channel".
    if (const int code = expect_failure(invalid, WorkflowCompileError::InductCandidateInvalid);
        code != 0) {
        return code;
    }
    return 0;
}

int induction_rejects_mismatched_skeletons_and_types() {
    WorkflowFixture fixture;
    ChannelTool channeler;
    MIRA_CHECK(register_registration(*fixture.registry_, channeler.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = induction_definition();
    const auto run = run_to_completion(*workflow, definition,
                                       JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(run.has_value());
    const WorkflowRunId run_id = run.value();
    const auto trajectory = workflow->capture_trajectory(run_id);
    MIRA_CHECK(trajectory.has_value());

    // Different step counts: skeleton mismatch.
    auto shorter = trajectory.value();
    shorter.steps.pop_back();
    auto outcome = induce_parameters({trajectory.value(), shorter});
    MIRA_CHECK(!outcome.has_value());
    MIRA_CHECK(outcome.error().domain_code ==
               static_cast<std::int32_t>(WorkflowCompileError::InductSkeletonMismatch));

    // Same scalar position with a different JSON type: type mismatch.
    auto retyped = trajectory.value();
    retyped.steps.front().arguments.set("payload", JsonValue{std::int64_t{7}});
    outcome = induce_parameters({trajectory.value(), retyped});
    MIRA_CHECK(!outcome.has_value());
    MIRA_CHECK(outcome.error().domain_code ==
               static_cast<std::int32_t>(WorkflowCompileError::InductTypeMismatch));

    // Object-vs-scalar at the same position: skeleton mismatch.
    auto reshaped = trajectory.value();
    reshaped.steps.front().arguments.set(
        "payload", JsonValue{JsonValue::Object{{"nested", JsonValue{"x"}}}});
    outcome = induce_parameters({trajectory.value(), reshaped});
    MIRA_CHECK(!outcome.has_value());
    MIRA_CHECK(outcome.error().domain_code ==
               static_cast<std::int32_t>(WorkflowCompileError::InductSkeletonMismatch));

    // One trajectory is not an induction.
    MIRA_CHECK(!induce_parameters({trajectory.value()}).has_value());
    // Structural naming is stable and avoids declared names. These simulate
    // host-built trajectories: no source parameters, no $param provenance.
    WorkflowTrajectory left = trajectory.value();
    left.source_parameters.clear();
    left.steps.front().raw_arguments = left.steps.front().arguments;
    left.steps.front().arguments.set("payload", JsonValue{"left"});
    left.steps.front().raw_arguments = left.steps.front().arguments;
    WorkflowTrajectory right = trajectory.value();
    right.source_parameters.clear();
    right.steps.front().raw_arguments = right.steps.front().arguments;
    right.steps.front().arguments.set("payload", JsonValue{"right"});
    right.steps.front().raw_arguments = right.steps.front().arguments;
    const auto generated = induce_parameters({left, right});
    MIRA_CHECK(generated.has_value());
    MIRA_CHECK(generated.value().size() == 1);
    MIRA_CHECK(generated.value().front().name == "param_1");
    MIRA_CHECK(generated.value().front().provenance == WorkflowCandidateProvenance::Structural);
    return 0;
}

int end_to_end_patch_repair_compiles_into_new_version() {
    WorkflowFixture fixture;
    CountingTool counter;
    counter.side_effects = true;
    counter.failures_first = 1; // The first dispatch escalates for repair.
    MIRA_CHECK(register_registration(*fixture.registry_,
                                     counter.registration("counter", "sent")));
    auto workflow = fixture.make_workflow();

    // The scenario: a flow whose verification expects a repaired parameter
    // escalates to the agent, the agent repairs through a run patch, the run
    // completes, and compiling the successful trajectory bakes the repair as
    // the new version's default (DEC-022's run -> default target).
    auto step = tool_step("counter",
                          parameter_predicate("mode", WorkflowPredicateOp::Eq,
                                              JsonValue{"repaired"}));
    auto definition = compilable_definition({step}, "m11-e2e", true);

    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                             WorkflowPolicy::Recoverable);
    MIRA_CHECK(created.has_value());
    const auto waiting = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(waiting.has_value());
    MIRA_CHECK(waiting.value().state == WorkflowRunState::WaitingAgent);

    const auto repaired = workflow->patch_run(
        created.value().run_id, WorkflowPatchId::generate(),
        {parameter_set("mode", JsonValue{"repaired"})});
    MIRA_CHECK(repaired.has_value());
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);

    const auto trajectory = workflow->capture_trajectory(created.value().run_id);
    MIRA_CHECK(trajectory.has_value());
    WorkflowCompileOptions options;
    options.workflow_id = definition.workflow_id;
    options.name = definition.name;
    options.summary = "repair baked as the new default";
    const auto compiled = compile_workflow(trajectory.value(), options);
    MIRA_CHECK(compiled.has_value());
    MIRA_CHECK(*compiled.value().parameters.front().default_value.as_string() == "repaired");

    // Gated publish chains onto the existing history and the new version
    // runs with the repaired default, no parameters supplied.
    const auto published = workflow->publish_validated(
        compiled.value(), "m11-test", "bake the repair", created.value().run_id);
    MIRA_CHECK(published.has_value());
    const auto rerun = workflow->create_run(definition.workflow_id,
                                            published.value().ir_digest,
                                            JsonValue{JsonValue::Object{}},
                                            WorkflowPolicy::Strict);
    MIRA_CHECK(rerun.has_value());
    const auto rerun_driven = workflow->execute_run(rerun.value().run_id, drive_context());
    MIRA_CHECK(rerun_driven.has_value());
    MIRA_CHECK(rerun_driven.value().state == WorkflowRunState::Completed);

    // Audit: the proposed event carries the source run, the applied event
    // the gate run and evidence.
    const auto proposed = session_event_payloads(*fixture.events_, fixture.session_id_,
                                                  "WorkflowPublishProposed");
    MIRA_CHECK(proposed.size() == 1);
    MIRA_CHECK(*proposed.front().find("source_run_id")->as_string() ==
               created.value().run_id.to_string());
    const auto applied = session_event_payloads(*fixture.events_, fixture.session_id_,
                                                "WorkflowPublishApplied");
    MIRA_CHECK(applied.size() == 1);
    MIRA_CHECK(*applied.front().find("dry_run_id")->as_string() ==
               published.value().dry_run_id.to_string());
    return 0;
}

int end_to_end_induced_version_reuses_parameters() {
    WorkflowFixture fixture;
    ChannelTool channeler;
    MIRA_CHECK(register_registration(*fixture.registry_, channeler.registration()));
    auto workflow = fixture.make_workflow();
    auto definition = induction_definition();

    const auto fast = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"fast"}}}});
    MIRA_CHECK(fast.has_value());
    const WorkflowRunId fast_id = fast.value();
    const auto slow = run_to_completion(*workflow, definition,
                                        JsonValue{JsonValue::Object{{"mode", JsonValue{"slow"}}}});
    MIRA_CHECK(slow.has_value());
    const WorkflowRunId slow_id = slow.value();
    const auto fast_trajectory = workflow->capture_trajectory(fast_id);
    const auto slow_trajectory = workflow->capture_trajectory(slow_id);
    MIRA_CHECK(fast_trajectory.has_value() && slow_trajectory.has_value());
    const auto candidates =
        induce_parameters({fast_trajectory.value(), slow_trajectory.value()});
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);

    WorkflowCompileOptions options;
    options.workflow_id = WorkflowId::generate(); // A derived workflow.
    options.name = "induced-flow";
    const auto compiled =
        compile_workflow(fast_trajectory.value(), options, candidates.value());
    MIRA_CHECK(compiled.has_value());

    const auto published =
        workflow->publish_validated(compiled.value(), "m11-test", "induction", fast_id);
    MIRA_CHECK(published.has_value());

    // The induced version runs with its anchor default and with an explicit
    // override of the reopened parameter.
    const auto default_run = workflow->create_run(
        compiled.value().workflow_id, published.value().ir_digest,
        JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(default_run.has_value());
    const auto default_driven =
        workflow->execute_run(default_run.value().run_id, drive_context());
    MIRA_CHECK(default_driven.has_value());
    MIRA_CHECK(default_driven.value().state == WorkflowRunState::Completed);

    const auto override_run = workflow->create_run(
        compiled.value().workflow_id, published.value().ir_digest,
        JsonValue{JsonValue::Object{{"mode", JsonValue{"turbo"}}}},
        WorkflowPolicy::Strict);
    MIRA_CHECK(override_run.has_value());
    const auto override_driven =
        workflow->execute_run(override_run.value().run_id, drive_context());
    MIRA_CHECK(override_driven.has_value());
    MIRA_CHECK(override_driven.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(channeler.dispatches.load() == 8); // 2 + 2 + 2 + 2 dispatches, gates dispatch nothing
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {structural_diff_names_varying_leaves_only,
         parameterized_compilation_reopens_parameters, explicit_candidates_validate_fail_closed,
         induction_rejects_mismatched_skeletons_and_types,
         end_to_end_patch_repair_compiles_into_new_version,
         end_to_end_induced_version_reuses_parameters});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
