// M9-11/M9-12: the four stage-B operation handlers execute through the
// BuiltIn registry boundary (DEC-021 §5) with the library version gate
// (W-03/W-04), and one scripted model run proves the agent-initiated
// run_workflow path end to end.

#include "support/harness_support.hpp"
#include "support/m9_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/model_gateway.hpp>

#include <chrono>

namespace {

using namespace mira;
using namespace mira::testing;

// Builds one executable proposal for an exposed tool so the registry's
// identity checks pass.
[[nodiscard]] Result<ToolProposal> proposal_for(const BuiltinToolRegistry &registry,
                                                const std::string &wire_name,
                                                const JsonValue &arguments) {
    for (const auto &spec : registry.exposed_tools()) {
        if (spec.wire_name != wire_name) {
            continue;
        }
        ToolProposal proposal;
        proposal.provider_call_id = ProviderToolCallId{"m9-" + wire_name};
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

[[nodiscard]] JsonValue run_arguments(const WorkflowId &workflow_id, const Sha256Digest &digest,
                                      const JsonValue &parameters) {
    JsonValue::Object root;
    root.emplace_back("workflow_id", workflow_id.to_string());
    root.emplace_back("ir_digest", digest.to_string());
    root.emplace_back("parameters", parameters);
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue control_arguments(const WorkflowId &workflow_id,
                                          const WorkflowRunId &run_id) {
    JsonValue::Object root;
    root.emplace_back("workflow_id", workflow_id.to_string());
    root.emplace_back("run_id", run_id.to_string());
    return JsonValue{std::move(root)};
}

int four_operations_round_trip_through_the_registry() {
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
    // patch_workflow stays schema-only until stage C.
    MIRA_CHECK(workflow->operation_tool_registrations().size() == 5);

    // A runnable library version: DryRunPassed with evidence (W-04).
    auto definition = base_definition("tool-channel");
    definition.steps = {tool_step("counter", std::nullopt),
                        tool_step("gate", std::nullopt)};
    const auto digest = workflow->publish_workflow(
        definition, "m9-test", "stage B tool channel", WorkflowValidationResult::DryRunPassed,
        digest_string("m9-evidence"));
    MIRA_CHECK(digest.has_value());

    const auto context = plain_loop_context();
    const auto started = proposal_for(
        *fixture.registry_, "run_workflow",
        run_arguments(definition.workflow_id, digest.value(), JsonValue{JsonValue::Object{}}));
    MIRA_CHECK(started.has_value());
    auto start_record = fixture.registry_->execute(started.value(), context);
    MIRA_CHECK(start_record.has_value() && !start_record.value().failed);
    const auto run_id = WorkflowRunId::parse(*start_record.value().result.find("run_id")->as_string());
    MIRA_CHECK(run_id.has_value());
    const std::string start_state = *start_record.value().result.find("state")->as_string();
    MIRA_CHECK(start_state == "running" || start_state == "created");

    // Pause while the gated step is in flight.
    while (gate.entries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto pause = proposal_for(*fixture.registry_, "pause_workflow",
                                    control_arguments(definition.workflow_id, run_id.value()));
    MIRA_CHECK(pause.has_value());
    auto pause_record = fixture.registry_->execute(pause.value(), context);
    MIRA_CHECK(pause_record.has_value() && !pause_record.value().failed);
    MIRA_CHECK(*pause_record.value().result.find("state")->as_string() == "paused");

    gate.release.store(true);
    const auto resume = proposal_for(*fixture.registry_, "resume_workflow",
                                     control_arguments(definition.workflow_id, run_id.value()));
    MIRA_CHECK(resume.has_value());
    auto resume_record = fixture.registry_->execute(resume.value(), context);
    MIRA_CHECK(resume_record.has_value() && !resume_record.value().failed);
    MIRA_CHECK(*resume_record.value().result.find("state")->as_string() == "running");

    const auto settled = workflow->wait_run(run_id.value(), std::chrono::seconds(10));
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().state == WorkflowRunState::Completed);
    return 0;
}

int library_gate_and_error_envelopes() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    for (auto &registration : workflow->operation_tool_registrations()) {
        MIRA_CHECK(fixture.registry_->register_tool(std::move(registration.spec),
                                                     registration.handler));
    }

    auto definition = base_definition("gate-flow");
    definition.steps = {tool_step("counter", std::nullopt)};

    // Unvalidated versions never become runnable creation-time versions.
    const auto unvalidated = workflow->publish_workflow(definition, "m9-test", "not validated");
    MIRA_CHECK(unvalidated.has_value());
    const auto rejected = workflow->create_run(definition.workflow_id, unvalidated.value(),
                                               JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(!rejected.has_value());

    // run_workflow surfaces the same gate as a failed tool record.
    const auto context = plain_loop_context();
    const auto started = proposal_for(
        *fixture.registry_, "run_workflow",
        run_arguments(definition.workflow_id, unvalidated.value(), JsonValue{JsonValue::Object{}}));
    MIRA_CHECK(started.has_value());
    auto record = fixture.registry_->execute(started.value(), context);
    MIRA_CHECK(record.has_value() && record.value().failed);
    MIRA_CHECK(!record.value().safe_error_summary.empty());

    // Unknown digests and malformed arguments fail closed through the same
    // boundary.
    const auto unknown = proposal_for(
        *fixture.registry_, "run_workflow",
        run_arguments(definition.workflow_id, digest_string("m9-evidence"),
                      JsonValue{JsonValue::Object{}}));
    MIRA_CHECK(unknown.has_value());
    auto unknown_record = fixture.registry_->execute(unknown.value(), context);
    MIRA_CHECK(unknown_record.has_value() && unknown_record.value().failed);

    JsonValue::Object missing;
    missing.emplace_back("workflow_id", definition.workflow_id.to_string());
    const auto malformed =
        proposal_for(*fixture.registry_, "cancel_workflow", JsonValue{std::move(missing)});
    MIRA_CHECK(malformed.has_value());
    auto malformed_record = fixture.registry_->execute(malformed.value(), context);
    MIRA_CHECK(malformed_record.has_value() && malformed_record.value().failed);

    // A DryRunPassed version runs, and cancel is idempotent with the
    // already_terminal marker. The validated publish uses evolved content:
    // identical content would keep resolving to the earlier NotValidated
    // record (records are immutable; digests are content identities).
    definition.steps.push_back(tool_step("counter", std::nullopt));
    const auto validated = workflow->publish_workflow(
        definition, "m9-test", "dry run passed", WorkflowValidationResult::DryRunPassed,
        digest_string("m9-evidence"));
    MIRA_CHECK(validated.has_value());
    const auto created = workflow->create_run(definition.workflow_id, validated.value(),
                                              JsonValue{JsonValue::Object{}}, std::nullopt);
    MIRA_CHECK(created.has_value());
    const auto cancel = proposal_for(*fixture.registry_, "cancel_workflow",
                                     control_arguments(definition.workflow_id, created.value().run_id));
    MIRA_CHECK(cancel.has_value());
    auto first = fixture.registry_->execute(cancel.value(), context);
    MIRA_CHECK(first.has_value() && !first.value().failed);
    MIRA_CHECK(*first.value().result.find("state")->as_string() == "cancelled");
    MIRA_CHECK(first.value().result.find("already_terminal") != nullptr);
    MIRA_CHECK(first.value().result.find("already_terminal")->as_boolean().value_or(true) == false);
    auto second = fixture.registry_->execute(cancel.value(), context);
    // At-most-once per operation id: a second execution of the same proposal
    // is rejected by the registry boundary itself.
    MIRA_CHECK(!second.has_value() || second.value().failed);
    return 0;
}

int model_initiated_run_workflow_end_to_end() {
    WorkflowFixture fixture;
    CountingTool counter;
    MIRA_CHECK(register_registration(*fixture.registry_, counter.registration()));
    auto workflow = fixture.make_workflow();
    const auto registrations = workflow->operation_tool_registrations();
    BuiltinToolSpec run_spec;
    for (const auto &registration : registrations) {
        if (registration.spec.wire_name == "run_workflow") {
            run_spec = registration.spec;
            MIRA_CHECK(
                fixture.registry_->register_tool(registration.spec, registration.handler));
        }
    }
    MIRA_CHECK(!run_spec.wire_name.empty());

    // A runnable version the model can start.
    auto definition = base_definition("model-flow");
    definition.steps = {tool_step("counter", std::nullopt),
                        tool_step("counter", std::nullopt)};
    const auto digest = workflow->publish_workflow(
        definition, "m9-test", "model initiated", WorkflowValidationResult::DryRunPassed,
        digest_string("m9-evidence"));
    MIRA_CHECK(digest.has_value());

    // Agent harness assembly (M3 pattern): gateway + scripted provider.
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(fixture.executor_, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);
    const std::vector<ModelResponse> script = {
        tool_call_response(run_spec,
                           to_json_string(run_arguments(definition.workflow_id, digest.value(),
                                                        JsonValue{JsonValue::Object{}}))),
        text_response(R"json({"action":"done","reason":"workflow started"})json"),
    };
    gateway.register_provider(std::make_shared<RecordingProvider>(profile, script));

    AgentLoopSpec spec;
    spec.task_id = TaskId::generate();
    spec.session_id = fixture.session_id_;
    spec.task_epoch = 1;
    spec.profile_id = profile->id;
    spec.goal = "run the daily report workflow";
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

    // The model-initiated run really executed through the WorkflowRuntime.
    const auto settled_view = [&] {
        EventQuery query;
        query.session_id = fixture.session_id_;
        std::optional<WorkflowRunId> found;
        while (true) {
            const auto page = fixture.events_->read(query);
            if (!page.has_value()) {
                break;
            }
            for (const auto &envelope : page.value().events) {
                if (envelope.payload.type == "WorkflowRunStarted") {
                    auto started = parse_workflow_run_started(envelope.payload);
                    if (started.has_value()) {
                        found = started.value().run_id;
                    }
                }
            }
            if (!page.value().has_more || page.value().events.empty()) {
                break;
            }
            query.after_sequence = page.value().events.back().session_sequence;
        }
        return found;
    }();
    MIRA_CHECK(settled_view.has_value());
    const auto result = workflow->wait_run(settled_view.value(), std::chrono::seconds(10));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counter.dispatches.load() == 2);
    return 0;
}

} // namespace

int main() {
    int (*const scenarios[])() = {
        four_operations_round_trip_through_the_registry,
        library_gate_and_error_envelopes,
        model_initiated_run_workflow_end_to_end,
    };
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
