#include <mira/tool_module_mcp.hpp>

#include <executor/executor.hpp>

#include <mira/json.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>
#include <mira/tool_executor.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace mira {
namespace {

// Conversion, admission and session-policy errors share the module layer
// domain (aligned with TM0/TM1 so a single surface reports admission
// failures). Execution errors use the DEC-015 execution domain so the
// dispatcher's rejection surface reads like BuiltinToolRegistry's.
[[nodiscard]] Error make_admission_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_module";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] Error make_execution_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_executor";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::string truncate(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
        text += "...";
    }
    return text;
}

[[nodiscard]] std::string_view action_risk_name(ActionRisk risk) {
    switch (risk) {
    case ActionRisk::R0ReadOnly:
        return "read_only";
    case ActionRisk::R1ReversibleLow:
        return "reversible_low";
    case ActionRisk::R2UserVisible:
        return "user_visible";
    case ActionRisk::R3Sensitive:
        return "sensitive";
    case ActionRisk::R4Critical:
        return "critical";
    }
    return "critical";
}

[[nodiscard]] bool is_terminal_module_state(ModuleState state) {
    return state == ModuleState::Revoked || state == ModuleState::Quarantined;
}

[[nodiscard]] JsonValue object_result_schema() {
    return JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}};
}

} // namespace

// ---------------------------------------------------------------------------
// Conversion (design §5)
// ---------------------------------------------------------------------------

ActionRisk mcp_hint_risk(const McpToolDescriptor &descriptor) {
    if (descriptor.read_only_hint) {
        return ActionRisk::R0ReadOnly;
    }
    if (descriptor.destructive_hint) {
        return ActionRisk::R4Critical;
    }
    return ActionRisk::R2UserVisible;
}

Result<ToolModuleManifest> convert_mcp_listing_to_module(const McpServerListing &listing,
                                                         const McpAdmissionOptions &options,
                                                         const CapabilityCatalog &catalog,
                                                         const ToolModuleLimits &limits,
                                                         const McpAdmissionLimits &mcp_limits) {
    if (options.module_id.empty()) {
        return make_admission_error(ErrorCode::InvalidArgument,
                                    "admission options require a module_id");
    }
    if (options.signer.empty() || options.signature_algorithm.empty() ||
        options.signature.empty()) {
        return make_admission_error(
            ErrorCode::InvalidArgument,
            "out_of_process admission requires signer, signature_algorithm and signature");
    }
    if (listing.tools.size() > mcp_limits.max_descriptors) {
        return make_admission_error(ErrorCode::ResourceExhausted,
                                    "listing exceeds the descriptor limit");
    }
    if (options.risk_overrides.size() > mcp_limits.max_risk_overrides) {
        return make_admission_error(ErrorCode::ResourceExhausted,
                                    "admission options exceed the risk override limit");
    }

    // Pre-scan: MCP-specific checks the manifest parser cannot express
    // (non-empty descriptions for the model, object input schemas), plus the
    // override bookkeeping needed to apply the raise-only rule.
    std::map<std::string, ActionRisk> hint_risks;
    for (const auto &descriptor : listing.tools) {
        if (descriptor.name.empty()) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "descriptor name must not be empty");
        }
        if (descriptor.description.empty()) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "descriptor '" + descriptor.name +
                                            "' requires a non-empty description");
        }
        if (!descriptor.input_schema.valid()) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "descriptor '" + descriptor.name +
                                            "' input schema must be an object");
        }
        if (!hint_risks.emplace(descriptor.name, mcp_hint_risk(descriptor)).second) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "duplicate descriptor name: '" + descriptor.name + "'");
        }
    }
    std::map<std::string, ActionRisk> overrides;
    for (const auto &[name, risk] : options.risk_overrides) {
        const auto hint = hint_risks.find(name);
        if (hint == hint_risks.end()) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "risk override targets unknown member '" + name + "'");
        }
        if (risk < hint->second) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "risk override lowers the tier of member '" + name +
                                            "': forbidden");
        }
        if (!overrides.emplace(name, risk).second) {
            return make_admission_error(ErrorCode::InvalidArgument,
                                        "duplicate risk override for member '" + name + "'");
        }
    }

    // Build the manifest JSON and hand it to the real parser so every TM0
    // gate (charset, uniqueness, schema subset, catalog, resources, ABI)
    // applies unchanged to MCP-sourced modules.
    JsonValue::Array tools;
    tools.reserve(listing.tools.size());
    for (const auto &descriptor : listing.tools) {
        ActionRisk risk = hint_risks.at(descriptor.name);
        const auto override_entry = overrides.find(descriptor.name);
        if (override_entry != overrides.end()) {
            risk = override_entry->second;
        }
        JsonValue::Object tool;
        tool.emplace_back("name", descriptor.name);
        tool.emplace_back("version", semantic_version_to_json(options.module_version));
        tool.emplace_back("description", descriptor.description);
        tool.emplace_back("arguments_schema", descriptor.input_schema.root);
        tool.emplace_back("result_schema", object_result_schema());
        tool.emplace_back("side_effect", std::string(action_risk_name(risk)));
        tools.emplace_back(JsonValue(std::move(tool)));
    }

    JsonValue::Object origin;
    origin.emplace_back("isolation",
                        std::string(tool_module_origin_name(ToolModuleOrigin::OutOfProcess)));
    origin.emplace_back("signer", options.signer);
    origin.emplace_back("signature_algorithm", options.signature_algorithm);
    origin.emplace_back("signature", options.signature);

    JsonValue::Array required;
    required.reserve(options.required_capabilities.size());
    for (const auto &capability : options.required_capabilities) {
        required.emplace_back(capability);
    }

    JsonValue::Array conflicts;
    conflicts.reserve(options.conflicts_with.size());
    for (const auto &conflict : options.conflicts_with) {
        conflicts.emplace_back(conflict);
    }

    JsonValue::Object resources;
    resources.emplace_back(
        "max_total_concurrent_invocations",
        static_cast<std::int64_t>(options.resources.max_total_concurrent_invocations));
    resources.emplace_back("max_total_result_bytes",
                           static_cast<std::int64_t>(options.resources.max_total_result_bytes));

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolModuleManifestSchema));
    root.emplace_back("schema_version", std::string(kToolModuleManifestSchemaVersion));
    root.emplace_back("module_id", options.module_id);
    root.emplace_back("version", semantic_version_to_json(options.module_version));
    root.emplace_back("origin", JsonValue(std::move(origin)));
    root.emplace_back("min_mira_module_abi", static_cast<std::int64_t>(1));
    root.emplace_back("required_capabilities", JsonValue(std::move(required)));
    root.emplace_back("infra_capabilities", JsonValue{JsonValue::Array{}});
    root.emplace_back("tools", JsonValue(std::move(tools)));
    root.emplace_back("conflicts_with", JsonValue(std::move(conflicts)));
    root.emplace_back("resources", JsonValue(std::move(resources)));

    return parse_tool_module_manifest(JsonValue(std::move(root)), catalog, limits);
}

JsonValue mcp_admission_to_json(const ToolModuleManifest &manifest) {
    JsonValue::Array tools;
    tools.reserve(manifest.tools.size());
    for (const auto &tool : manifest.tools) {
        JsonValue::Object entry;
        entry.emplace_back("name", tool.name);
        entry.emplace_back("side_effect", std::string(action_risk_name(tool.side_effect)));
        entry.emplace_back("arguments_schema_digest",
                           canonical_json_digest(tool.arguments_schema.root).to_string());
        tools.emplace_back(JsonValue(std::move(entry)));
    }

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kMcpAdmissionSchema));
    root.emplace_back("module_id", manifest.module_id);
    root.emplace_back("version", semantic_version_to_json(manifest.version));
    root.emplace_back("manifest_digest", tool_module_manifest_digest(manifest).to_string());
    root.emplace_back("tools", JsonValue(std::move(tools)));
    return JsonValue(std::move(root));
}

// ---------------------------------------------------------------------------
// Session lifecycle policy (design §6)
// ---------------------------------------------------------------------------

std::string_view mcp_session_event_name(McpSessionEvent event) {
    switch (event) {
    case McpSessionEvent::ServerConnected:
        return "server_connected";
    case McpSessionEvent::ServerDisconnected:
        return "server_disconnected";
    case McpSessionEvent::ToolListChanged:
        return "tool_list_changed";
    }
    return "server_connected";
}

std::string_view mcp_admission_action_name(McpAdmissionAction action) {
    switch (action) {
    case McpAdmissionAction::AdmitModule:
        return "admit_module";
    case McpAdmissionAction::RevokeModule:
        return "revoke_module";
    case McpAdmissionAction::RejectEvent:
        return "reject_event";
    case McpAdmissionAction::NoAction:
        return "no_action";
    }
    return "no_action";
}

McpSessionPlan plan_mcp_session_action(McpSessionEvent event, const ModuleRecord *record,
                                       bool registry_sealed, bool registry_closed) {
    if (registry_closed) {
        return {McpAdmissionAction::RejectEvent, "registry is closed: session event rejected"};
    }
    switch (event) {
    case McpSessionEvent::ServerConnected:
        if (record == nullptr) {
            if (registry_sealed) {
                return {McpAdmissionAction::RejectEvent,
                        "registration window is sealed: runtime admission rejected"};
            }
            return {McpAdmissionAction::AdmitModule, "deploy-window admission"};
        }
        return {McpAdmissionAction::RejectEvent,
                "module already admitted: runtime re-admission forbidden"};
    case McpSessionEvent::ServerDisconnected:
        if (record == nullptr) {
            return {McpAdmissionAction::NoAction, "module not registered"};
        }
        if (is_terminal_module_state(record->state)) {
            return {McpAdmissionAction::NoAction, "module already terminal"};
        }
        return {McpAdmissionAction::RevokeModule, "server disconnected: downgrade-only revocation"};
    case McpSessionEvent::ToolListChanged:
        if (record == nullptr) {
            return {McpAdmissionAction::NoAction, "module not registered"};
        }
        if (is_terminal_module_state(record->state)) {
            return {McpAdmissionAction::NoAction, "module already terminal"};
        }
        return {McpAdmissionAction::RevokeModule,
                "tool list changed: changed digest requires a new registration cycle"};
    }
    return {McpAdmissionAction::NoAction, "unknown session event"};
}

McpModuleAdmission::McpModuleAdmission(CapabilityCatalog catalog, ModuleRegistry &registry)
    : catalog_(std::move(catalog)), registry_(registry) {}

Result<Hash> McpModuleAdmission::admit_server(const McpServerListing &listing,
                                              const McpAdmissionOptions &options,
                                              const ToolModuleLimits &limits,
                                              const McpAdmissionLimits &mcp_limits) {
    if (!module_id_.empty() && options.module_id != module_id_) {
        return make_admission_error(ErrorCode::InvalidState, "admission is bound to module '" +
                                                                 module_id_ +
                                                                 "': refusing a second module");
    }
    const ModuleRecord *record = registry_.find(options.module_id);
    const McpSessionPlan plan = plan_mcp_session_action(McpSessionEvent::ServerConnected, record,
                                                        registry_.sealed(), registry_.closed());
    if (plan.action != McpAdmissionAction::AdmitModule) {
        return make_admission_error(ErrorCode::InvalidState,
                                    "server admission rejected: " + plan.reason);
    }
    auto manifest = convert_mcp_listing_to_module(listing, options, catalog_, limits, mcp_limits);
    if (!manifest.has_value()) {
        return manifest.error();
    }
    const Hash digest = tool_module_manifest_digest(manifest.value());
    if (const auto registered = registry_.register_module(std::move(manifest).value());
        !registered) {
        return registered.error();
    }
    module_id_ = options.module_id;
    return digest;
}

Result<McpSessionPlan> McpModuleAdmission::on_session_event(McpSessionEvent event) {
    if (module_id_.empty()) {
        return make_admission_error(ErrorCode::InvalidState,
                                    "no module admitted: session event has no bound module");
    }
    const ModuleRecord *record = registry_.find(module_id_);
    McpSessionPlan plan =
        plan_mcp_session_action(event, record, registry_.sealed(), registry_.closed());
    if (plan.action == McpAdmissionAction::AdmitModule) {
        return make_admission_error(ErrorCode::InvalidState,
                                    "server reconnection requires an explicit admit_server with "
                                    "the listing in an open registration window");
    }
    if (plan.action == McpAdmissionAction::RevokeModule) {
        if (const auto revoked = registry_.revoke_module(module_id_, plan.reason); !revoked) {
            return revoked.error();
        }
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Executor-routed invocation (design §7.3)
// ---------------------------------------------------------------------------

Result<std::future<Result<JsonValue>>>
submit_mcp_invocation(executor::Executor &executor, IMcpToolTransport &transport,
                      std::string wire_name, JsonValue arguments, McpInvocationProbe probe) {
    auto task = [&transport, wire_name = std::move(wire_name), arguments = std::move(arguments),
                 probe = std::move(probe)]() mutable -> Result<JsonValue> {
        try {
            return transport.invoke(wire_name, arguments, probe);
        } catch (const std::exception &) {
            return make_execution_error(ErrorCode::Internal, "mcp transport raised an exception");
        } catch (...) {
            return make_execution_error(ErrorCode::Internal, "mcp transport raised an exception");
        }
    };

    try {
        return executor.submit_auto(std::move(task));
    } catch (const executor::ExecutorStopping &) {
        return make_execution_error(ErrorCode::Unavailable,
                                    "executor is stopping: invocation not submitted");
    } catch (const executor::CapacityExhaustedException &) {
        return make_execution_error(ErrorCode::ResourceExhausted,
                                    "executor capacity exhausted: invocation not submitted");
    } catch (const std::runtime_error &) {
        // A stopped or uninitialized submission path surfaces as a plain
        // runtime_error; the specific executor types were matched above.
        return make_execution_error(
            ErrorCode::Unavailable,
            "executor is not accepting submissions (stopped or not initialized)");
    } catch (...) {
        return make_execution_error(ErrorCode::Internal, "invocation submission failed");
    }
}

Result<JsonValue> consume_mcp_invocation(std::future<Result<JsonValue>> future) {
    try {
        return future.get();
    } catch (const executor::CapacityExhaustedException &) {
        return make_execution_error(ErrorCode::ResourceExhausted,
                                    "executor capacity exhausted: invocation not admitted");
    } catch (const executor::ExecutorStopping &) {
        return make_execution_error(ErrorCode::Unavailable,
                                    "executor is stopping: invocation not admitted");
    } catch (const std::exception &) {
        return make_execution_error(ErrorCode::Unavailable,
                                    "executor rejected the invocation submission");
    } catch (...) {
        return make_execution_error(ErrorCode::Internal, "invocation consumption failed");
    }
}

// ---------------------------------------------------------------------------
// McpToolDispatcher
// ---------------------------------------------------------------------------

struct McpToolDispatcher::Impl final {
    // One in-flight invocation. `owner_waiting` (guarded by the dispatcher
    // mutex) marks whether an execute() call is still waiting on the future:
    // only that owner consumes it. close() drains solely the slots whose
    // owner has already given up (or never reached the wait), so a future is
    // consumed exactly once.
    struct Slot final {
        std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::future<Result<JsonValue>>> future;
        bool owner_waiting = false;
    };

    Impl(ToolExposure exposure_value, McpDispatchLimits limits_value)
        : exposure(std::move(exposure_value)), limits(limits_value) {}

    ToolExposure exposure;
    McpDispatchLimits limits;
    mutable std::mutex mutex;
    std::unordered_set<OperationId, OperationIdHash> dispatched;
    std::vector<std::shared_ptr<Slot>> active; // reserved, unsettled invocations
    std::size_t abandoned = 0;                 // active slots whose wait was given up
    bool closed = false;
    McpDispatcherStats stats;

    [[nodiscard]] std::size_t active_size_locked() const { return active.size(); }

    void drop_slot_locked(const std::shared_ptr<Slot> &slot) {
        const auto found = std::find(active.begin(), active.end(), slot);
        if (found != active.end()) {
            active.erase(found);
        }
    }
};

Result<std::unique_ptr<McpToolDispatcher>> McpToolDispatcher::make(ToolExposure exposure,
                                                                   McpDispatchLimits limits) {
    if (limits.max_concurrent_invocations == 0 || limits.max_result_bytes == 0 ||
        limits.max_pending_invocations == 0) {
        return make_execution_error(ErrorCode::InvalidArgument,
                                    "dispatch limits require non-zero concurrency, result and "
                                    "pending budgets");
    }
    if (limits.max_invocation_duration.count() <= 0 || limits.cancellation_grace.count() <= 0 ||
        limits.default_close_drain_budget.count() <= 0) {
        return make_execution_error(ErrorCode::InvalidArgument,
                                    "dispatch limits require positive durations");
    }
    std::unique_ptr<McpToolDispatcher> dispatcher(
        new McpToolDispatcher(std::move(exposure), limits));
    return dispatcher;
}

McpToolDispatcher::McpToolDispatcher(ToolExposure exposure, McpDispatchLimits limits)
    : impl_(new Impl(std::move(exposure), limits)) {}

McpToolDispatcher::~McpToolDispatcher() {
    if (impl_ == nullptr) {
        return;
    }
    if (!closed()) {
        close(impl_->limits.default_close_drain_budget);
    }
}

namespace {

[[nodiscard]] ToolExecutionRecord failed_record(const ToolProposal &proposal, std::string summary) {
    ToolExecutionRecord record;
    record.provider_call_id = proposal.provider_call_id;
    record.tool_id = proposal.tool_id;
    record.failed = true;
    record.safe_error_summary = std::move(summary);
    return record;
}

} // namespace

Result<ToolExecutionRecord> McpToolDispatcher::execute(const ToolProposal &proposal,
                                                       const OperationContext &context,
                                                       executor::Executor &executor,
                                                       IMcpToolTransport &transport) {
    const auto started = std::chrono::steady_clock::now();
    const auto hard_deadline = started + impl_->limits.max_invocation_duration;
    std::optional<std::chrono::steady_clock::time_point> effective_deadline = hard_deadline;
    if (context.deadline.has_value() && *context.deadline < hard_deadline) {
        effective_deadline = context.deadline;
    }

    const ExposedToolSpec *spec = nullptr;
    auto slot = std::make_shared<Impl::Slot>();
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closed) {
            ++impl_->stats.closed_rejections;
            return make_execution_error(ErrorCode::InvalidState,
                                        "dispatcher is closed: invocation rejected");
        }
        const auto found = std::find_if(
            impl_->exposure.tools.begin(), impl_->exposure.tools.end(),
            [&proposal](const ExposedToolSpec &tool) { return tool.tool_id == proposal.tool_id; });
        if (found == impl_->exposure.tools.end()) {
            ++impl_->stats.identity_rejections;
            return make_execution_error(ErrorCode::NotFound,
                                        "tool is not part of the pinned exposure");
        }
        if (found->wire_name != proposal.wire_name || found->version != proposal.tool_version ||
            found->has_side_effects != proposal.has_side_effects) {
            ++impl_->stats.identity_rejections;
            return make_execution_error(ErrorCode::InvalidState,
                                        "tool identity no longer matches the pinned exposure");
        }
        if (impl_->dispatched.find(proposal.operation_id) != impl_->dispatched.end()) {
            ++impl_->stats.identity_rejections;
            return make_execution_error(ErrorCode::AlreadyExists,
                                        "operation was already dispatched; at most once per "
                                        "operation");
        }
        if (impl_->active_size_locked() >= impl_->limits.max_concurrent_invocations) {
            ++impl_->stats.concurrency_rejections;
            return make_execution_error(ErrorCode::ResourceExhausted,
                                        "module concurrency budget exhausted");
        }
        if (impl_->abandoned >= impl_->limits.max_pending_invocations) {
            ++impl_->stats.concurrency_rejections;
            return make_execution_error(ErrorCode::ResourceExhausted,
                                        "abandoned invocation backlog limit reached");
        }
        impl_->dispatched.insert(proposal.operation_id);
        impl_->active.push_back(slot);
        spec = &*found;
    }

    // Model-attributable argument validation, same position in the pipeline
    // as BuiltinToolRegistry::execute (before anything is dispatched).
    const auto violations =
        validate_instance_against_schema(proposal.arguments, spec->parameters_schema);
    if (!violations.empty()) {
        std::ostringstream summary;
        summary << "arguments failed schema validation at " << violations.front().path << " ("
                << violations.front().keyword << "): " << violations.front().message;
        const std::lock_guard lock(impl_->mutex);
        impl_->dispatched.erase(proposal.operation_id);
        impl_->drop_slot_locked(slot);
        ++impl_->stats.failed_records;
        return failed_record(proposal, truncate(summary.str(), 512));
    }

    auto submission =
        submit_mcp_invocation(executor, transport, proposal.wire_name, proposal.arguments,
                              McpInvocationProbe(slot->stop, effective_deadline));
    if (!submission.has_value()) {
        const std::lock_guard lock(impl_->mutex);
        impl_->dispatched.erase(proposal.operation_id);
        impl_->drop_slot_locked(slot);
        ++impl_->stats.submission_rejections;
        return submission.error();
    }
    {
        const std::lock_guard lock(impl_->mutex);
        slot->future =
            std::make_shared<std::future<Result<JsonValue>>>(std::move(submission.value()));
        slot->owner_waiting = true;
        ++impl_->stats.dispatched;
    }

    // Bounded cooperative wait: propagate the caller's cancellation into the
    // probe, let the deadline fire through the probe, and abandon the wait
    // (retaining the future for the close drain) once the grace elapses.
    bool abandoning = false;
    bool abandoned_for_deadline = false;
    auto grace_until = std::chrono::steady_clock::time_point::max();
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (!abandoning) {
            if (context.cancelled()) {
                slot->stop->store(true, std::memory_order_release);
                abandoning = true;
                grace_until = now + impl_->limits.cancellation_grace;
            } else if (effective_deadline.has_value() && now >= *effective_deadline) {
                slot->stop->store(true, std::memory_order_release);
                abandoning = true;
                abandoned_for_deadline = true;
                grace_until = now + impl_->limits.cancellation_grace;
            }
        }
        if (slot->future->wait_for(std::chrono::milliseconds(2)) == std::future_status::ready) {
            break;
        }
        if (abandoning && now >= grace_until) {
            const std::lock_guard lock(impl_->mutex);
            slot->owner_waiting = false;
            ++impl_->abandoned;
            if (abandoned_for_deadline) {
                ++impl_->stats.deadline_exceeded;
                return failed_record(proposal, "invocation exceeded its deadline");
            }
            ++impl_->stats.cancelled;
            return make_execution_error(ErrorCode::Cancelled,
                                        "invocation was cancelled and the transport did not "
                                        "return within the grace period");
        }
    }

    // The wait is over and this thread owns the future (owner_waiting is
    // still set), so it is safe to consume it here; get() returns
    // immediately because wait_for() reported readiness.
    auto outcome = consume_mcp_invocation(std::move(*slot->future));
    {
        const std::lock_guard lock(impl_->mutex);
        slot->future.reset();
        slot->owner_waiting = false;
        impl_->drop_slot_locked(slot);
    }
    if (!outcome.has_value()) {
        const std::lock_guard lock(impl_->mutex);
        if (outcome.error().code == ErrorCode::Cancelled) {
            ++impl_->stats.cancelled;
            return make_execution_error(ErrorCode::Cancelled, "invocation was cancelled");
        }
        if (outcome.error().code == ErrorCode::DeadlineExceeded) {
            ++impl_->stats.deadline_exceeded;
            return failed_record(proposal, "invocation exceeded its deadline");
        }
        ++impl_->stats.failed_records;
        return failed_record(proposal, truncate(outcome.error().safe_message, 512));
    }

    const auto serialized = to_json_string(outcome.value());
    const auto result_cap = std::min<std::uint64_t>(impl_->limits.max_result_bytes,
                                                    kDefaultToolBridgeLimits.max_result_bytes);
    if (serialized.size() > result_cap) {
        const std::lock_guard lock(impl_->mutex);
        ++impl_->stats.oversize_results;
        ++impl_->stats.failed_records;
        return failed_record(proposal, "tool result exceeds the size limit");
    }

    ToolExecutionRecord record;
    record.provider_call_id = proposal.provider_call_id;
    record.tool_id = proposal.tool_id;
    record.result = std::move(outcome).value();
    {
        const std::lock_guard lock(impl_->mutex);
        ++impl_->stats.completed;
    }
    return record;
}

McpDispatcherCloseReport McpToolDispatcher::close(std::chrono::milliseconds drain_budget) {
    std::vector<std::shared_ptr<Impl::Slot>> snapshot;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
        impl_->stats.closed = true;
        snapshot = impl_->active;
    }

    McpDispatcherCloseReport report;
    const auto budget_end = std::chrono::steady_clock::now() + drain_budget;
    for (const auto &slot : snapshot) {
        slot->stop->store(true, std::memory_order_release);
        bool drainable = false;
        {
            const std::lock_guard lock(impl_->mutex);
            // Only slots whose owner has already given up (or never reached
            // the wait) are close's to consume; slots with an active waiter
            // settle through their owner, which observes the stop flag.
            drainable = slot->future != nullptr && !slot->owner_waiting;
        }
        if (!drainable) {
            continue;
        }
        while (std::chrono::steady_clock::now() < budget_end) {
            if (slot->future->wait_for(std::chrono::milliseconds(2)) == std::future_status::ready) {
                break;
            }
        }
        const std::lock_guard lock(impl_->mutex);
        if (slot->future != nullptr && !slot->owner_waiting &&
            slot->future->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            // Consume the settled future so no outcome is silently dropped;
            // the drained result is discarded by design (the caller already
            // returned) and only counted.
            (void)consume_mcp_invocation(std::move(*slot->future));
            slot->future.reset();
            impl_->drop_slot_locked(slot);
            if (impl_->abandoned > 0) {
                --impl_->abandoned;
            }
            ++impl_->stats.pending_drained;
            ++report.drained;
        } else {
            ++impl_->stats.pending_abandoned;
            ++report.abandoned;
        }
    }
    {
        const std::lock_guard lock(impl_->mutex);
        report.still_pending = impl_->active.size();
    }
    return report;
}

bool McpToolDispatcher::closed() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->closed;
}

McpDispatcherStats McpToolDispatcher::stats() const {
    const std::lock_guard lock(impl_->mutex);
    McpDispatcherStats snapshot = impl_->stats;
    snapshot.in_flight = impl_->active.size();
    snapshot.pending = impl_->abandoned;
    return snapshot;
}

} // namespace mira
