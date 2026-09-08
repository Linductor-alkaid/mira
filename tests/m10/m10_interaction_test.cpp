// M10-10..M10-12: decision-point interaction (DEC-024 §4-§6) and the tool
// closure: request_user_input through the BuiltIn registry, resolution by
// id + digest, AgentPrompt reject/accept flows, and the model-initiated
// patch and decision paths end to end.

#include "support/harness_support.hpp"
#include "support/m10_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/model_gateway.hpp>

#include <chrono>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] Result<ToolProposal> proposal_for(const BuiltinToolRegistry &registry,
                                                const std::string &wire_name,
                                                const JsonValue &arguments) {
    for (const auto &spec : registry.exposed_tools()) {
        if (spec.wire_name != wire_name) {
            continue;
        }
        ToolProposal proposal;
        proposal.provider_call_id = ProviderToolCallId{"m10-" + wire_name};
        proposal.tool_id = spec.tool_id;
        proposal.wire_name = spec.wire_name;
        proposal.tool_version = spec.version;
        proposal.arguments = arguments;
        proposal.arguments_digest = canonical_json_digest(arguments);
        proposal.operation_id = OperationId::generate();
        proposal.has_side_effects = spec.has_side_effects;
        return proposal;
    }
    Error error;
    error.code = ErrorCode::NotFound;
    error.safe_message = "tool is not exposed";
    return error;
}

[[nodiscard]] JsonValue patch_arguments(const WorkflowId &workflow_id,
                                        const WorkflowRunId &run_id,
                                        const WorkflowPatchId &patch_id,
                                        const std::vector<WorkflowPatchEntry> &entries) {
    JsonValue::Array entry_values;
    for (const auto &entry : entries) {
        JsonValue::Object object;
        object.emplace_back("target", workflow_patch_target_name(entry.target));
        object.emplace_back("op", workflow_patch_op_name(entry.op));
        object.emplace_back("path", entry.path);
        if (!entry.value.is_null()) {
            object.emplace_back("value", entry.value);
        }
        entry_values.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("workflow_id", workflow_id.to_string());
    root.emplace_back("run_id", run_id.to_string());
    root.emplace_back("patch_id", patch_id.to_string());
    root.emplace_back("patch_entries", JsonValue{std::move(entry_values)});
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue user_input_arguments(const WorkflowId &workflow_id,
                                             const WorkflowRunId &run_id,
                                             const std::string &prompt,
                                             const std::vector<WorkflowPatchEntry> &proposal) {
    JsonValue::Object root;
    root.emplace_back("workflow_id", workflow_id.to_string());
    root.emplace_back("run_id", run_id.to_string());
    root.emplace_back("prompt", prompt);
    if (!proposal.empty()) {
        JsonValue::Array entry_values;
        for (const auto &entry : proposal) {
            JsonValue::Object object;
            object.emplace_back("target", workflow_patch_target_name(entry.target));
            object.emplace_back("op", workflow_patch_op_name(entry.op));
            object.emplace_back("path", entry.path);
            if (!entry.value.is_null()) {
                object.emplace_back("value", entry.value);
            }
            entry_values.emplace_back(JsonValue{std::move(object)});
        }
        root.emplace_back("proposal", JsonValue{std::move(entry_values)});
    }
    return JsonValue{std::move(root)};
}

int request_user_input_tool_raises_and_resolves() {
    WorkflowFixture fixture;
    GatedTool gate;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    for (auto &registration : workflow->operation_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                    registration.handler));
    }
    for (auto &registration : workflow->decision_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                    registration.handler));
    }

    auto definition = full_policy_definition("user-input");
    definition.steps = {tool_step("gate", std::nullopt), tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto context = plain_loop_context();
    const auto asked = proposal_for(
        *fixture.registry_, "request_user_input",
        user_input_arguments(definition.workflow_id, created.value().run_id,
                             "attach the weekly summary?", {parameter_set("mode", JsonValue{"weekly"})}));
    MIRA_CHECK(asked.has_value());
    auto record = fixture.registry_->execute(asked.value(), context);
    MIRA_CHECK(record.has_value() && !record.value().failed);
    const auto decision_id =
        WorkflowDecisionId::parse(*record.value().result.find("decision_id")->as_string());
    MIRA_CHECK(decision_id.has_value());
    MIRA_CHECK(*record.value().result.find("state")->as_string() == "waiting_user");

    // The run parked in WaitingUser with the pending decision and the
    // in-flight step converged at its cooperative boundary.
    const auto parked = workflow->run_snapshot(created.value().run_id);
    MIRA_CHECK(parked.has_value());
    MIRA_CHECK(parked.value().state == WorkflowRunState::WaitingUser);
    MIRA_CHECK(parked.value().pending_decision.has_value());

    const auto decision = workflow->pending_decision_request(created.value().run_id);
    MIRA_CHECK(decision.has_value());
    MIRA_CHECK(decision.value().kind == WorkflowDecisionKind::AgentPrompt);
    MIRA_CHECK(decision.value().proposal.size() == 1);

    // A second decision on the same run is rejected (single pending point).
    const auto second =
        proposal_for(*fixture.registry_, "request_user_input",
                     user_input_arguments(definition.workflow_id, created.value().run_id,
                                          "another question?", {}));
    MIRA_CHECK(second.has_value());
    auto second_record = fixture.registry_->execute(second.value(), context);
    MIRA_CHECK(second_record.has_value() && second_record.value().failed);

    // Accept applies the proposed patch and continues to completion.
    const auto resolved =
        workflow->resolve_decision(created.value().run_id, decision.value().decision_id,
                                   decision.value().payload_digest,
                                   WorkflowDecisionResolution::Accept);
    MIRA_CHECK(resolved.has_value());
    gate.release.store(true);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().run_patch_epoch == 1);

    const auto raised =
        session_event_payloads(*fixture.events_, fixture.session_id_, "WorkflowDecisionRaised");
    MIRA_CHECK(raised.size() == 1);
    const auto resolved_events = session_event_payloads(*fixture.events_, fixture.session_id_,
                                                        "WorkflowDecisionResolved");
    MIRA_CHECK(resolved_events.size() == 1);
    MIRA_CHECK(*resolved_events.front().find("resolution")->as_string() == "accept");
    return 0;
}

int agent_prompt_reject_continues_without_the_patch() {
    WorkflowFixture fixture;
    GatedTool gate;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    for (auto &registration : workflow->decision_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                    registration.handler));
    }

    auto definition = full_policy_definition("prompt-reject");
    definition.steps = {tool_step("gate", std::nullopt), tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto context = plain_loop_context();
    const auto asked = proposal_for(
        *fixture.registry_, "request_user_input",
        user_input_arguments(definition.workflow_id, created.value().run_id, "change the mode?",
                             {parameter_set("mode", JsonValue{"other"})}));
    MIRA_CHECK(asked.has_value());
    auto record = fixture.registry_->execute(asked.value(), context);
    MIRA_CHECK(record.has_value() && !record.value().failed);

    const auto decision = workflow->pending_decision_request(created.value().run_id);
    MIRA_CHECK(decision.has_value());
    const auto rejected =
        workflow->resolve_decision(created.value().run_id, decision.value().decision_id,
                                   decision.value().payload_digest,
                                   WorkflowDecisionResolution::Reject);
    MIRA_CHECK(rejected.has_value());

    gate.release.store(true);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    // Reject applied nothing.
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().run_patch_epoch == 0);
    return 0;
}

int request_user_input_gates_admission() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    for (auto &registration : workflow->decision_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                    registration.handler));
    }

    auto definition = full_policy_definition("prompt-admission");
    definition.steps = {tool_step("counter", std::nullopt)};

    // Non-interactive policies cannot raise decision points.
    const auto recoverable = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                                  WorkflowPolicy::Recoverable);
    MIRA_CHECK(recoverable.has_value());
    const auto context = plain_loop_context();
    const auto asked = proposal_for(
        *fixture.registry_, "request_user_input",
        user_input_arguments(definition.workflow_id, recoverable.value().run_id, "question?", {}));
    MIRA_CHECK(asked.has_value());
    auto record = fixture.registry_->execute(asked.value(), context);
    MIRA_CHECK(record.has_value() && record.value().failed);

    // Malformed arguments (missing prompt) fail closed at the schema layer.
    JsonValue::Object missing;
    missing.emplace_back("workflow_id", definition.workflow_id.to_string());
    missing.emplace_back("run_id", recoverable.value().run_id.to_string());
    const auto malformed =
        proposal_for(*fixture.registry_, "request_user_input", JsonValue{std::move(missing)});
    MIRA_CHECK(malformed.has_value());
    auto malformed_record = fixture.registry_->execute(malformed.value(), context);
    MIRA_CHECK(malformed_record.has_value() && malformed_record.value().failed);
    return 0;
}

int patch_workflow_tool_executes_through_the_registry() {
    WorkflowFixture fixture;
    CountingTool counter;
    GatedTool gate;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    auto workflow = fixture.make_workflow();
    for (auto &registration : workflow->operation_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                    registration.handler));
    }

    auto definition = full_policy_definition("patch-tool");
    definition.steps = {tool_step("gate", std::nullopt), tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIRA_CHECK(workflow->pause_run(created.value().run_id).has_value());

    const auto context = plain_loop_context();
    const auto patch_id = WorkflowPatchId::generate();
    const auto submitted = proposal_for(
        *fixture.registry_, "patch_workflow",
        patch_arguments(definition.workflow_id, created.value().run_id, patch_id,
                        {parameter_set("mode", JsonValue{"patched"})}));
    MIRA_CHECK(submitted.has_value());
    auto record = fixture.registry_->execute(submitted.value(), context);
    MIRA_CHECK(record.has_value() && !record.value().failed);
    MIRA_CHECK(record.value().result.find("applied")->as_boolean().value() == true);
    MIRA_CHECK(record.value().result.find("run_patch_epoch")->as_integer().value() == 1);

    // The replay through the tool is the idempotent NoOp.
    const auto replayed = proposal_for(
        *fixture.registry_, "patch_workflow",
        patch_arguments(definition.workflow_id, created.value().run_id, patch_id,
                        {parameter_set("mode", JsonValue{"patched"})}));
    MIRA_CHECK(replayed.has_value());
    auto replay = fixture.registry_->execute(replayed.value(), context);
    MIRA_CHECK(replay.has_value() && !replay.value().failed);
    MIRA_CHECK(replay.value().result.find("applied")->as_boolean().value() == false);

    // Finish the parked run after the tool-path assertions.
    gate.release.store(true);
    MIRA_CHECK(workflow->resume_run(created.value().run_id).has_value());
    MIRA_CHECK(workflow->wait_run(created.value().run_id, std::chrono::seconds(10))
                   .value()
                   .state == WorkflowRunState::Completed);

    // Strict runs surface the admission rejection as a failed record: a
    // paused Strict boundary rejects the patch.
    GatedTool strict_gate;
    MIRA_CHECK(register_registration(*fixture.registry_, strict_gate.registration("strict-gate")));
    auto strict_definition = full_policy_definition("patch-tool-strict");
    strict_definition.steps = {tool_step("strict-gate", std::nullopt)};
    const auto strict = workflow->create_run(strict_definition, JsonValue{JsonValue::Object{}},
                                             WorkflowPolicy::Strict);
    MIRA_CHECK(strict.has_value());
    MIRA_CHECK(workflow->start_run(strict.value().run_id).has_value());
    while (strict_gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIRA_CHECK(workflow->pause_run(strict.value().run_id).has_value());
    const auto denied = proposal_for(
        *fixture.registry_, "patch_workflow",
        patch_arguments(strict_definition.workflow_id, strict.value().run_id,
                        WorkflowPatchId::generate(), {parameter_set("mode", JsonValue{"x"})}));
    MIRA_CHECK(denied.has_value());
    auto denied_record = fixture.registry_->execute(denied.value(), context);
    MIRA_CHECK(denied_record.has_value() && denied_record.value().failed);
    strict_gate.release.store(true);
    MIRA_CHECK(workflow->resume_run(strict.value().run_id).has_value());
    MIRA_CHECK(workflow->wait_run(strict.value().run_id, std::chrono::seconds(10))
                   .value()
                   .state == WorkflowRunState::Completed);
    return 0;
}

int model_initiated_patch_and_decision_end_to_end() {
    WorkflowFixture fixture;
    GatedTool gate;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, gate.registration()));
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();

    BuiltinToolSpec patch_spec;
    BuiltinToolSpec input_spec;
    for (const auto &registration : workflow->operation_tool_registrations()) {
        if (registration.spec.wire_name == "patch_workflow") {
            patch_spec = registration.spec;
            MIRA_CHECK(
                fixture.registry_->register_tool(registration.spec, registration.handler));
        }
    }
    for (const auto &registration : workflow->decision_tool_registrations()) {
        input_spec = registration.spec;
        MIRA_CHECK(fixture.registry_->register_tool(registration.spec, registration.handler));
    }
    MIRA_CHECK(!patch_spec.wire_name.empty());
    MIRA_CHECK(!input_spec.wire_name.empty());

    auto definition = full_policy_definition("model-intervention");
    definition.steps = {tool_step("gate", std::nullopt), tool_step("counter", std::nullopt)};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Scripted model turn: patch the run parameter, then ask the user.
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(fixture.executor_, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);
    const std::vector<ModelResponse> script = {
        tool_call_response(patch_spec,
                           to_json_string(patch_arguments(
                               definition.workflow_id, created.value().run_id,
                               WorkflowPatchId::generate(),
                               {parameter_set("mode", JsonValue{"model-patch"})}))),
        tool_call_response(input_spec,
                           to_json_string(user_input_arguments(
                               definition.workflow_id, created.value().run_id,
                               "run in quiet mode this time?", {}))),
        text_response(R"json({"action":"done","reason":"asked the user"})json"),
    };
    gateway.register_provider(std::make_shared<RecordingProvider>(profile, script));

    AgentLoopSpec spec;
    spec.task_id = TaskId::generate();
    spec.session_id = fixture.session_id_;
    spec.task_epoch = 1;
    spec.profile_id = profile->id;
    spec.goal = "adjust the running workflow";
    AgentLoop loop(fixture.environment_, gateway);
    loop.set_event_store(fixture.events_, RuntimeId::generate(), fixture.session_id_);
    loop.set_tool_registry(fixture.registry_);
    admission->activate(spec.task_id, spec.task_epoch);

    OperationContext loop_context;
    loop_context.session = fixture.session_id_;
    loop_context.task = spec.task_id;
    loop_context.started_at = Timestamp::now();
    ModelDoneVerifier verifier;
    auto future = fixture.executor_.submit_auto(
        [&loop, &spec, &loop_context, &verifier] { return loop.run(spec, loop_context, verifier); });
    const auto loop_result = future.get();
    MIRA_CHECK(loop_result.has_value());
    MIRA_CHECK(loop_result.value().outcome == LoopOutcome::Completed);

    // The model-initiated patch landed at the boundary and the decision
    // point parked the run; resolve and finish.
    const auto decision = workflow->pending_decision_request(created.value().run_id);
    MIRA_CHECK(decision.has_value());
    MIRA_CHECK(decision.value().kind == WorkflowDecisionKind::AgentPrompt);
    const auto resolved =
        workflow->resolve_decision(created.value().run_id, decision.value().decision_id,
                                   decision.value().payload_digest,
                                   WorkflowDecisionResolution::Accept);
    MIRA_CHECK(resolved.has_value());
    gate.release.store(true);
    const auto settled = workflow->wait_run(created.value().run_id, std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(workflow->run_snapshot(created.value().run_id).value().run_patch_epoch == 1);
    MIRA_CHECK(counter.dispatches.load() == 1);
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        request_user_input_tool_raises_and_resolves,
        agent_prompt_reject_continues_without_the_patch,
        request_user_input_gates_admission,
        patch_workflow_tool_executes_through_the_registry,
        model_initiated_patch_and_decision_end_to_end,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
