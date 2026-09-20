#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/tool_module.hpp>
#include <mira/tool_module_exposure.hpp>
#include <mira/tool_module_mcp.hpp>
#include <mira/tool_module_registry.hpp>
#include <mira/tool_reference.hpp>
#include <mira/tool_skill.hpp>
#include <mira/version.hpp>

#include <chrono>
#include <vector>

int main() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::single_display()};

    mira::ObservationRequest request;
    request.required.screen = true;
    mira::OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    const auto observation = environment.observe(request, context);
    if (!observation.has_value() || !observation.value().screen.has_value()) {
        return 1;
    }

    mira::InputSequence sequence;
    sequence.events.push_back(mira::InputEvent{"tap", "0.5,0.5"});
    const auto receipt = environment.execute(sequence, context);
    if (!receipt.has_value() || receipt.value().status != mira::ExecutionStatus::Completed) {
        return 2;
    }

    mira::RuntimeBaseline runtime;
    if (!runtime.initialize()) {
        return 3;
    }
    const auto submission = runtime.submit({1, 1, 0, mira::BaselineCommandKind::Command});
    if (!submission.admitted) {
        return 4;
    }
    const auto result = runtime.wait(1, std::chrono::seconds(2));
    if (!runtime.request_shutdown()) {
        return 5;
    }
    runtime.finish_shutdown();
    const int baseline_status = result.code == mira::BaselineResultCode::Applied ? 0 : 6;
    if (baseline_status != 0) {
        return baseline_status;
    }

    // M7-TM0-G6 consumer closure: the new public tool module header must be
    // includable and linkable from a minimal external consumer.
    const mira::CapabilityCatalog &catalog = mira::CapabilityCatalog::core();
    const mira::CapabilityDescriptor *capability = catalog.find("env.screen.capture");
    if (capability == nullptr || capability->kind != mira::CapabilityKind::Boolean) {
        return 7;
    }
    const std::vector<mira::ModuleSnapshot> active;
    const mira::ModuleNegotiationResult negotiation =
        mira::negotiate_modules(active, mira::EnvironmentCapabilities{}, catalog);
    if (!negotiation.modules.empty()) {
        return 7;
    }

    // M7-TM1-G6 consumer closure: the registry lifecycle header must be
    // includable and linkable from the same minimal consumer, without the
    // Executor (the registry itself is a serial-plane component).
    mira::ModuleRegistry registry{mira::CapabilityCatalog::core()};
    if (registry.sealed() || registry.closed() || registry.active_snapshot().generation != 0) {
        return 8;
    }
    const mira::ModuleState parsed = mira::parse_module_state("quarantined").value();
    if (parsed != mira::ModuleState::Quarantined) {
        return 8;
    }
    const mira::ModuleTrustReport report =
        mira::verify_module_trust(mira::ToolModuleManifest{}, mira::ModuleTrustConfig{});
    if (report.trusted) {
        return 8;
    }
    mira::ModuleNegotiationCoordinator coordinator{mira::CapabilityCatalog::core()};
    if (coordinator.current() != nullptr) {
        return 8;
    }

    // M7-TM2-G6 consumer closure: the exposure projection header must be
    // includable and linkable from the same minimal consumer, and the empty
    // Active-set projection must yield the defined empty view (M3 empty
    // allowlist compatibility).
    const mira::ModuleNegotiationResult empty_negotiation =
        mira::negotiate_modules(active, mira::EnvironmentCapabilities{}, catalog);
    const auto exposure = mira::project_tool_exposure(1, active, empty_negotiation);
    if (!exposure.has_value() || !exposure.value().empty() ||
        exposure.value().snapshot_digest == mira::Hash{}) {
        return 9;
    }
    const auto reference = mira::make_simulator_reference_module();
    if (!reference.has_value() || reference.value().module_id != "builtin.simulator.env") {
        return 9;
    }

    // M7-MCP-G6 consumer closure: the MCP admission header must be includable
    // and linkable from the same minimal consumer. The conversion, session
    // policy and dispatcher factories are exercised without an Executor (the
    // dispatcher itself is only constructed, never driven, here).
    mira::McpToolDescriptor descriptor;
    descriptor.name = "files.read";
    descriptor.description = "Read one file through the admitted server.";
    descriptor.input_schema = mira::JsonSchema{
        mira::JsonValue{mira::JsonValue::Object{{"type", mira::JsonValue{std::string("object")}}}}};
    descriptor.read_only_hint = true;
    mira::McpServerListing listing;
    listing.tools.push_back(descriptor);
    mira::McpAdmissionOptions admission_options;
    admission_options.module_id = "host.mcp.sample";
    admission_options.signer = "sample-signer";
    admission_options.signature_algorithm = "sample-alg";
    admission_options.signature = "sample-signature";
    admission_options.resources.max_total_concurrent_invocations = 1;
    admission_options.resources.max_total_result_bytes = 4096;
    const auto converted = mira::convert_mcp_listing_to_module(listing, admission_options, catalog);
    if (!converted.has_value() ||
        converted.value().origin != mira::ToolModuleOrigin::OutOfProcess ||
        converted.value().tools.size() != 1 ||
        converted.value().tools.front().side_effect != mira::ActionRisk::R0ReadOnly) {
        return 10;
    }
    const mira::McpSessionPlan connect_plan = mira::plan_mcp_session_action(
        mira::McpSessionEvent::ServerConnected, nullptr, false, false);
    if (connect_plan.action != mira::McpAdmissionAction::AdmitModule) {
        return 10;
    }
    const mira::JsonValue admission_projection = mira::mcp_admission_to_json(converted.value());
    if (admission_projection.find("manifest_digest") == nullptr) {
        return 10;
    }
    mira::McpDispatchLimits dispatch_limits;
    dispatch_limits.max_concurrent_invocations = 1;
    const auto dispatcher = mira::McpToolDispatcher::make(mira::ToolExposure{}, dispatch_limits);
    if (!dispatcher.has_value()) {
        return 10;
    }
    const mira::McpDispatcherCloseReport close_report =
        dispatcher.value()->close(std::chrono::milliseconds{50});
    if (close_report.still_pending != 0) {
        return 10;
    }

    // M7-TR0-G6 consumer closure: the stable reference header must be
    // includable and linkable from the same minimal consumer. The reference
    // layer is a workflow-plane contract (DEC-040), so this closure links
    // Mira::workflow while still exercising only serial-plane pure functions:
    // a reference round trip, an extraction against a one-tool view and the
    // compatibility projection.
    const auto parsed_reference = mira::parse_tool_reference(
        "toolref:sample.read@1b1c90825e0d740692239b0bc408ed0d3ae12b525a8058d7f2b6f3d3f0f0f0f0");
    if (!parsed_reference.has_value() ||
        parsed_reference.value().mode != mira::ToolReferenceMode::PinnedDigest ||
        mira::tool_reference_to_string(parsed_reference.value()) !=
            "toolref:sample.read@"
            "1b1c90825e0d740692239b0bc408ed0d3ae12b525a8058d7f2b6f3d3f0f0f0f0") {
        return 11;
    }
    const auto follow_reference = mira::parse_tool_reference("toolref:sample.read");
    if (!follow_reference.has_value() ||
        follow_reference.value().mode != mira::ToolReferenceMode::FollowLatest) {
        return 11;
    }

    mira::WorkflowDefinition definition;
    definition.schema_version = mira::SchemaVersion{1, 0};
    definition.workflow_id = mira::WorkflowId::generate();
    definition.name = "consumer.sample";
    definition.summary = "Consumer sample skill.";
    definition.default_policy = mira::WorkflowPolicy::Strict;
    definition.allowed_policies = {mira::WorkflowPolicy::Strict};
    mira::WorkflowStep step;
    step.id = mira::StepId::generate();
    step.name = "read";
    step.kind = mira::WorkflowStepKind::ToolCall;
    step.arguments = mira::JsonValue{
        mira::JsonValue::Object{{"tool", mira::JsonValue{std::string("sample.read")}}}};
    definition.steps.push_back(step);

    mira::ExposedToolSpec spec;
    spec.tool_id = mira::ToolId::generate();
    spec.wire_name = "sample.read";
    spec.parameters_schema = mira::JsonSchema{
        mira::JsonValue{mira::JsonValue::Object{{"type", mira::JsonValue{std::string("object")}}}}};
    spec.spec_digest = mira::digest_string("sample.read/v1");
    const std::vector<mira::ExposedToolSpec> view{spec};
    const auto manifest = mira::extract_workflow_tool_references(definition, view);
    if (!manifest.has_value() || manifest.value().entries.size() != 1 ||
        manifest.value().entries.front().mode != mira::ToolReferenceMode::PinnedDigest) {
        return 11;
    }
    const auto projection =
        mira::project_workflow_tool_compatibility(definition, manifest.value(), view);
    if (!projection.has_value() ||
        projection.value().state != mira::WorkflowToolCompatState::Runnable) {
        return 11;
    }
    const mira::WorkflowToolCompatDecision decision =
        mira::admit_workflow_run_by_tool_compat(projection.value());
    if (!decision.admitted) {
        return 11;
    }

    // M7-TR1-G6 consumer closure: the skill publication header must be
    // includable and linkable from the same minimal consumer. The chain
    // derives a skill descriptor from the TR0 manifest, publishes it through
    // the registry against a runnable library version, exercises the
    // downgrade-only revocation and rebuilds the Procedure index.
    mira::WorkflowVersionHistory history;
    history.workflow_id = definition.workflow_id;
    mira::WorkflowVersionRecord version_record;
    version_record.content_digest = mira::workflow_definition_digest(definition);
    version_record.validation = mira::WorkflowValidationResult::DryRunPassed;
    version_record.validation_evidence = version_record.content_digest;
    if (!mira::append_workflow_version(history, version_record).has_value()) {
        return 12;
    }
    const auto skill_descriptor = mira::make_skill_descriptor(definition, manifest.value(), view,
                                                              "consumer.sample_skill", {1, 0, 0});
    if (!skill_descriptor.has_value() || skill_descriptor.value().surface.description.empty() ||
        !skill_descriptor.value().surface.parameters_schema.valid()) {
        return 12;
    }
    mira::SkillPublicationRegistry skill_registry;
    const auto published = skill_registry.publish_skill(skill_descriptor.value(), definition,
                                                        manifest.value(), view, history);
    if (!published.has_value() ||
        published.value().status != mira::SkillPublicationStatus::Published) {
        return 12;
    }
    const auto revoked = skill_registry.revoke_skill("consumer.sample_skill", "consumer closure");
    if (!revoked.has_value() || revoked.value().status != mira::SkillPublicationStatus::Revoked) {
        return 12;
    }
    const auto publications = skill_registry.publications();
    const auto index = mira::project_skill_procedure_index(publications);
    if (!index.has_value() || index.value().entries.size() != 1 ||
        index.value().entries.front().status != mira::SkillPublicationStatus::Revoked) {
        return 12;
    }
    const auto rebuilt =
        mira::skill_procedure_entry_from_statement(index.value().entries.front().statement);
    if (!rebuilt.has_value() || !(rebuilt.value() == index.value().entries.front())) {
        return 12;
    }
    return 0;
}
