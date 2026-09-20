#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/tool_module.hpp>
#include <mira/tool_module_exposure.hpp>
#include <mira/tool_module_mcp.hpp>
#include <mira/tool_module_registry.hpp>
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
    return 0;
}
