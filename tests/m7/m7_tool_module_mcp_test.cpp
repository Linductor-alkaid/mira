// M7 MCP admission verification: gates M7-MCP-G1..G6 (frozen in
// docs/plans/m7-tools-evaluation-platform-v1.md section 5.4; contract source
// docs/design/mcp_tool_admission_design.md).
//
// Deterministic by construction: no clock value and no randomness reach any
// asserted value or the --report output. The gates that exercise cancellation,
// deadlines and concurrency use real wall-clock waits inside transports, but
// every wait is handshake- or budget-driven and only outcome kinds, counters
// and digests are asserted (never durations). The --report mode writes a
// canonical JSON report of fixed conversion/admission/policy scenarios for
// cross-process byte comparison.

#include "../support/test.hpp"

#include <mira/tool_executor.hpp>
#include <mira/tool_module_mcp.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Shared fixtures
// ---------------------------------------------------------------------------

[[noreturn]] void fixture_failed(const std::string &what) {
    std::cerr << "fixture failure: " << what << '\n';
    std::abort();
}

JsonValue json_or_abort(std::string_view text) {
    auto parsed = parse_json(text);
    if (!parsed.has_value()) {
        fixture_failed("fixture JSON does not parse");
    }
    return parsed.value();
}

McpToolDescriptor descriptor(const std::string &name, const std::string &description,
                             std::string_view schema_text = R"json({"type":"object"})json",
                             bool read_only = false, bool destructive = false) {
    auto schema = parse_json(schema_text);
    if (!schema.has_value()) {
        fixture_failed("descriptor schema does not parse");
    }
    McpToolDescriptor value;
    value.name = name;
    value.description = description;
    value.input_schema = JsonSchema{schema.value()};
    value.read_only_hint = read_only;
    value.destructive_hint = destructive;
    return value;
}

McpAdmissionOptions admission_options(const std::string &module_id = "acme.mcp.fixture") {
    McpAdmissionOptions options;
    options.module_id = module_id;
    options.module_version = SemanticVersion{2, 1, 0};
    options.signer = "acme-mcp-signer";
    options.signature_algorithm = "ed25519-ref";
    options.signature = "bWNwLWZpeHR1cmUtc2lnbmF0dXJlLW1hdGVyaWFs";
    options.resources.max_total_concurrent_invocations = 2;
    options.resources.max_total_result_bytes = 8192;
    return options;
}

// Four descriptors covering the three hint branches plus one raise-only
// override target; names deliberately out of wire order.
McpServerListing sample_listing() {
    McpServerListing listing;
    listing.tools.push_back(descriptor("mcp.rename", "renames a user-visible object"));
    listing.tools.push_back(descriptor("mcp.lookup", "read-only lookup", R"json({
            "type": "object",
            "properties": {"query": {"type": "string", "minLength": 1}},
            "required": ["query"],
            "additionalProperties": false
        })json",
                                       /*read_only=*/true));
    listing.tools.push_back(descriptor("mcp.purge", "destructive purge",
                                       R"json({"type":"object"})json",
                                       /*read_only=*/false, /*destructive=*/true));
    listing.tools.push_back(descriptor("mcp.audit_sync", "sync audit log"));
    return listing;
}

std::vector<std::pair<std::string, ActionRisk>> sample_overrides() {
    return {{"mcp.audit_sync", ActionRisk::R3Sensitive}};
}

std::string_view risk_name(ActionRisk risk) {
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

// Deterministic OutOfProcess trust verifier: accepts the binding and records
// the exact unsigned digest it was asked to authenticate.
class RecordingAcceptor final : public IModuleSignatureVerifier {
  public:
    bool verify(std::string_view signer, std::string_view algorithm, std::string_view signature,
                const Hash &signed_digest) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        last_signer = std::string(signer);
        last_algorithm = std::string(algorithm);
        last_signature = std::string(signature);
        last_digest = signed_digest;
        return true;
    }

    std::atomic<int> calls_{0};
    std::string last_signer;
    std::string last_algorithm;
    std::string last_signature;
    Hash last_digest{};
};

ModuleTrustConfig mcp_trust(IModuleSignatureVerifier *verifier,
                            const std::vector<std::string> &signers = {"acme-mcp-signer"}) {
    ModuleTrustConfig trust;
    trust.trusted_out_of_process_signers = signers;
    trust.verifier = verifier;
    return trust;
}

RuntimeId test_runtime_id() {
    const auto id = RuntimeId::parse("0102030405060708090a0b0c0d0e0f10");
    return id.has_value() ? *id : RuntimeId{};
}

SessionId test_session_id() {
    const auto id = SessionId::parse("1112131415161718191a1b1c1d1e1f20");
    return id.has_value() ? *id : SessionId{};
}

template <typename T> bool failed_with(const Result<T> &result, ErrorCode code) {
    return !result.has_value() && result.error().code == code;
}

template <typename T> bool failed_in_tool_module_domain(const Result<T> &result) {
    return !result.has_value() && result.error().domain == "mira.tool_module" &&
           !result.error().safe_message.empty();
}

template <typename T> bool failed_in_tool_executor_domain(const Result<T> &result) {
    return !result.has_value() && result.error().domain == "mira.tool_executor" &&
           !result.error().safe_message.empty();
}

// ---------------------------------------------------------------------------
// Execution fixtures (transports, proposals, executors)
// ---------------------------------------------------------------------------

class EchoTransport final : public IMcpToolTransport {
  public:
    enum class Mode { Value, Error, Throw };

    explicit EchoTransport(Mode mode = Mode::Value, std::string message = "transport failed",
                           JsonValue payload = JsonValue{})
        : mode_(mode), message_(std::move(message)), payload_(std::move(payload)) {}

    Result<JsonValue> invoke(const std::string &wire_name, const JsonValue &arguments,
                             const McpInvocationProbe &) override {
        if (mode_ == Mode::Throw) {
            throw std::runtime_error("mcp transport defect");
        }
        if (mode_ == Mode::Error) {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "test.transport";
            error.safe_message = message_;
            return error;
        }
        if (!payload_.is_null()) {
            return payload_;
        }
        JsonValue::Object result;
        result.emplace_back("echo", arguments);
        result.emplace_back("wire", wire_name);
        return JsonValue(std::move(result));
    }

  private:
    Mode mode_;
    std::string message_;
    JsonValue payload_;
};

// Polls the probe between short slices; optionally raises the caller's cancel
// flag after a few slices to drive the three-layer propagation loop. Returns
// Cancelled as soon as the probe fires (the host obligation).
class ProbePollingTransport final : public IMcpToolTransport {
  public:
    explicit ProbePollingTransport(std::atomic<bool> *cancel_flag = nullptr)
        : cancel_flag_(cancel_flag) {}

    Result<JsonValue> invoke(const std::string &, const JsonValue &,
                             const McpInvocationProbe &probe) override {
        for (int slice = 0; slice < 5000; ++slice) {
            if (slice == 5 && cancel_flag_ != nullptr) {
                cancel_flag_->store(true, std::memory_order_release);
            }
            if (probe.stop_requested() || probe.deadline_expired()) {
                Error error;
                error.code = ErrorCode::Cancelled;
                error.domain = "test.transport";
                error.safe_message = "cancelled by probe";
                return error;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return JsonValue{JsonValue::Object{{"polled", JsonValue{std::int64_t{1}}}}};
    }

  private:
    std::atomic<bool> *cancel_flag_;
};

// Ignores the probe entirely (defective host transport): sleeps a fixed
// duration, then succeeds. Used to force the deadline / grace-exhaustion
// abandon paths.
class SleepTransport final : public IMcpToolTransport {
  public:
    explicit SleepTransport(std::chrono::milliseconds duration) : duration_(duration) {}

    Result<JsonValue> invoke(const std::string &, const JsonValue &,
                             const McpInvocationProbe &) override {
        std::this_thread::sleep_for(duration_);
        return JsonValue{JsonValue::Object{{"slept", JsonValue{std::int64_t{1}}}}};
    }

  private:
    std::chrono::milliseconds duration_;
};

// Blocks until an external flag is set (bounded), then succeeds. Used to hold
// the concurrency slot deterministically.
class GateTransport final : public IMcpToolTransport {
  public:
    explicit GateTransport(const std::atomic<bool> &release) : release_(release) {}

    Result<JsonValue> invoke(const std::string &, const JsonValue &,
                             const McpInvocationProbe &) override {
        for (int slice = 0; slice < 30000 && !release_.load(std::memory_order_acquire); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return JsonValue{JsonValue::Object{{"gated", JsonValue{std::int64_t{1}}}}};
    }

  private:
    const std::atomic<bool> &release_;
};

const JsonSchema &echo_schema() {
    static const JsonSchema schema{json_or_abort(R"json({
        "type": "object",
        "properties": {"x": {"type": "integer"}},
        "required": ["x"],
        "additionalProperties": false
    })json")};
    return schema;
}

ExposedToolSpec echo_spec(const ToolId &tool_id) {
    ExposedToolSpec spec;
    spec.tool_id = tool_id;
    spec.version = SemanticVersion{1, 2, 3};
    spec.wire_name = "cmp.echo";
    spec.description = "comparison fixture tool";
    spec.parameters_schema = echo_schema();
    spec.spec_digest = digest_string("mcp-test-echo-fixture");
    spec.has_side_effects = true;
    return spec;
}

ToolExposure single_exposure(const ExposedToolSpec &spec) {
    ToolExposure exposure;
    exposure.negotiation_generation = 7;
    exposure.tools.push_back(spec);
    exposure.snapshot_digest = digest_string("mcp-test-single-exposure");
    return exposure;
}

ToolProposal make_proposal(const ExposedToolSpec &spec, JsonValue arguments,
                           const std::string &call_id = "call_1") {
    ToolProposal proposal;
    proposal.provider_call_id = ProviderToolCallId{call_id};
    proposal.tool_id = spec.tool_id;
    proposal.wire_name = spec.wire_name;
    proposal.tool_version = spec.version;
    proposal.arguments = std::move(arguments);
    proposal.arguments_digest = digest_string(to_json_string(proposal.arguments));
    proposal.operation_id = OperationId::generate();
    proposal.has_side_effects = spec.has_side_effects;
    return proposal;
}

OperationContext plain_context() {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

OperationContext cancelled_context(std::atomic<bool> *flag = nullptr) {
    OperationContext context = plain_context();
    if (flag != nullptr) {
        context.cancellation_requested = [flag] { return flag->load(std::memory_order_acquire); };
    } else {
        context.cancellation_requested = [] { return true; };
    }
    return context;
}

McpDispatchLimits fast_limits() {
    McpDispatchLimits limits;
    limits.max_concurrent_invocations = 1;
    limits.max_result_bytes = 64 * 1024;
    limits.max_invocation_duration = std::chrono::milliseconds{2'000};
    limits.cancellation_grace = std::chrono::milliseconds{150};
    limits.max_pending_invocations = 8;
    limits.default_close_drain_budget = std::chrono::milliseconds{1'000};
    return limits;
}

bool make_executor(executor::Executor &executor, std::size_t min_threads, std::size_t max_threads) {
    executor::ExecutorConfig config;
    config.min_threads = min_threads;
    config.max_threads = max_threads;
    config.queue_capacity = 64;
    return executor.initialize(config);
}

// ---------------------------------------------------------------------------
// --report scenario (pure content-derived values only)
// ---------------------------------------------------------------------------

McpServerListing report_listing() {
    McpServerListing listing;
    listing.tools.push_back(descriptor("rep.lookup", "report read-only member",
                                       R"json({"type":"object"})json",
                                       /*read_only=*/true));
    listing.tools.push_back(descriptor("rep.erase", "report destructive member",
                                       R"json({"type":"object"})json",
                                       /*read_only=*/false, /*destructive=*/true));
    listing.tools.push_back(descriptor("rep.plain", "report default member"));
    return listing;
}

std::string build_mcp_report() {
    const auto must_void = [](const Result<void> &result, const char *what) {
        if (!result.has_value()) {
            std::cerr << "report scenario failed: " << what << '\n';
            std::abort();
        }
    };
    const auto must = [](const auto &result, const char *what) {
        if (!result.has_value()) {
            std::cerr << "report scenario failed: " << what << '\n';
            std::abort();
        }
        return result.value();
    };

    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.tool_module.mcp.report.v1"));

    // Hint mapping for the frozen branches.
    JsonValue::Array hint_risks;
    const std::pair<const char *, ActionRisk> kHintCases[] = {
        {"read_only",
         mcp_hint_risk(descriptor("h", "d", R"json({"type":"object"})json", true, false))},
        {"destructive",
         mcp_hint_risk(descriptor("h", "d", R"json({"type":"object"})json", false, true))},
        {"default",
         mcp_hint_risk(descriptor("h", "d", R"json({"type":"object"})json", false, false))},
        {"both_hints",
         mcp_hint_risk(descriptor("h", "d", R"json({"type":"object"})json", true, true))},
    };
    for (const auto &hint_case : kHintCases) {
        JsonValue::Object entry;
        entry.emplace_back("case", hint_case.first);
        entry.emplace_back("risk", std::string(risk_name(hint_case.second)));
        hint_risks.emplace_back(JsonValue(std::move(entry)));
    }
    report.emplace_back("hint_risks", JsonValue(std::move(hint_risks)));

    // Conversion determinism anchors: manifest digest + admission projection.
    McpAdmissionOptions options = admission_options("acme.mcp.report");
    options.risk_overrides.push_back({"rep.plain", ActionRisk::R3Sensitive});
    const ToolModuleManifest manifest =
        must(convert_mcp_listing_to_module(report_listing(), options, CapabilityCatalog::core()),
             "convert");
    report.emplace_back("manifest_digest", tool_module_manifest_digest(manifest).to_string());
    report.emplace_back("unsigned_manifest_digest",
                        tool_module_unsigned_manifest_digest(manifest).to_string());
    report.emplace_back("admission_json", canonical_json_string(mcp_admission_to_json(manifest)));

    // Pure policy matrix over the frozen design section 6 table.
    JsonValue::Array matrix;
    const McpSessionEvent kEvents[] = {McpSessionEvent::ServerConnected,
                                       McpSessionEvent::ServerDisconnected,
                                       McpSessionEvent::ToolListChanged};
    const ModuleState kStates[] = {ModuleState::Verified, ModuleState::Staged,
                                   ModuleState::Active,   ModuleState::Deprecated,
                                   ModuleState::Revoked,  ModuleState::Quarantined};
    for (const McpSessionEvent event : kEvents) {
        // record == null branch.
        for (const bool sealed : {false, true}) {
            for (const bool closed : {false, true}) {
                const McpSessionPlan plan = plan_mcp_session_action(event, nullptr, sealed, closed);
                JsonValue::Object entry;
                entry.emplace_back("event", std::string(mcp_session_event_name(event)));
                entry.emplace_back("record", "null");
                entry.emplace_back("sealed", sealed);
                entry.emplace_back("closed", closed);
                entry.emplace_back("action", std::string(mcp_admission_action_name(plan.action)));
                entry.emplace_back("reason", plan.reason);
                matrix.emplace_back(JsonValue(std::move(entry)));
            }
        }
        // record != null branches.
        for (const ModuleState state : kStates) {
            for (const bool sealed : {false, true}) {
                for (const bool closed : {false, true}) {
                    ModuleRecord record;
                    record.state = state;
                    const McpSessionPlan plan =
                        plan_mcp_session_action(event, &record, sealed, closed);
                    JsonValue::Object entry;
                    entry.emplace_back("event", std::string(mcp_session_event_name(event)));
                    entry.emplace_back("record", std::string(module_state_name(state)));
                    entry.emplace_back("sealed", sealed);
                    entry.emplace_back("closed", closed);
                    entry.emplace_back("action",
                                       std::string(mcp_admission_action_name(plan.action)));
                    entry.emplace_back("reason", plan.reason);
                    matrix.emplace_back(JsonValue(std::move(entry)));
                }
            }
        }
    }
    report.emplace_back("policy_matrix", JsonValue(std::move(matrix)));

    // Deploy-window admission chain digests: admit -> activate -> negotiate ->
    // project, then a runtime downgrade and a shrunk re-projection.
    MemoryEventStore store;
    RecordingAcceptor verifier;
    ModuleRegistry registry{CapabilityCatalog::core(), mcp_trust(&verifier), &store,
                            test_runtime_id(), test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};
    McpModuleAdmission admission{CapabilityCatalog::core(), registry};

    const std::string admitted_digest =
        must(admission.admit_server(report_listing(), options), "admit").to_string();
    must_void(registry.stage_module("acme.mcp.report"), "stage");
    must_void(registry.activate_module("acme.mcp.report"), "activate");
    const NegotiationView first =
        must(coordinator.on_session_established(5, EnvironmentCapabilities{}, registry), "session");
    const ToolExposure before = must(
        project_tool_exposure(1, registry.active_snapshot().modules, first.negotiation), "project");

    JsonValue::Array exposures;
    {
        JsonValue::Object entry;
        entry.emplace_back("phase", "active");
        entry.emplace_back("admitted_digest", admitted_digest);
        entry.emplace_back("view_digest", first.view_digest.to_string());
        entry.emplace_back("exposure_snapshot_digest", before.snapshot_digest.to_string());
        JsonValue::Array tools;
        for (const auto &tool : before.tools) {
            JsonValue::Object member;
            member.emplace_back("wire_name", tool.wire_name);
            member.emplace_back("tool_id", tool.tool_id.to_string());
            member.emplace_back(
                "side_effect", std::string(tool.has_side_effects ? "side_effecting" : "read_only"));
            tools.emplace_back(JsonValue(std::move(member)));
        }
        entry.emplace_back("tools", JsonValue(std::move(tools)));
        exposures.emplace_back(JsonValue(std::move(entry)));
    }

    const McpSessionPlan downgrade =
        must(admission.on_session_event(McpSessionEvent::ToolListChanged), "downgrade");
    const NegotiationView second = must(coordinator.on_modules_changed(registry), "renegotiate");
    const ToolExposure after =
        must(project_tool_exposure(2, registry.active_snapshot().modules, second.negotiation),
             "reproject");
    {
        JsonValue::Object entry;
        entry.emplace_back("phase", "revoked");
        entry.emplace_back("action", std::string(mcp_admission_action_name(downgrade.action)));
        entry.emplace_back("view_digest", second.view_digest.to_string());
        entry.emplace_back("exposure_snapshot_digest", after.snapshot_digest.to_string());
        entry.emplace_back("tool_count", static_cast<std::int64_t>(after.tools.size()));
        entry.emplace_back("tombstoned",
                           registry.is_tombstoned(tool_module_manifest_digest(manifest)));
        exposures.emplace_back(JsonValue(std::move(entry)));
    }
    report.emplace_back("lifecycle_chain", JsonValue(std::move(exposures)));

    return canonical_json_string(JsonValue(std::move(report)));
}

// ---------------------------------------------------------------------------
// G1: conversion matrix and determinism (M7-MCP-G1)
// ---------------------------------------------------------------------------

int g1_hint_risk_mapping() {
    const auto read_only = descriptor("t.one", "d", R"json({"type":"object"})json", true, false);
    const auto destructive = descriptor("t.two", "d", R"json({"type":"object"})json", false, true);
    const auto plain = descriptor("t.three", "d", R"json({"type":"object"})json", false, false);
    const auto both = descriptor("t.four", "d", R"json({"type":"object"})json", true, true);
    // Frozen section 5.2: readOnlyHint wins, then destructiveHint, default is
    // user_visible.
    MIRA_CHECK(mcp_hint_risk(read_only) == ActionRisk::R0ReadOnly);
    MIRA_CHECK(mcp_hint_risk(destructive) == ActionRisk::R4Critical);
    MIRA_CHECK(mcp_hint_risk(plain) == ActionRisk::R2UserVisible);
    MIRA_CHECK(mcp_hint_risk(both) == ActionRisk::R0ReadOnly);
    MIRA_CHECK(mcp_hint_risk(read_only) == mcp_hint_risk(read_only));
    return 0;
}

int g1_conversion_positive_matrix() {
    McpAdmissionOptions options = admission_options();
    options.required_capabilities = {"env.screen.capture"};
    options.conflicts_with = {"acme.mcp.rival"};
    options.risk_overrides = sample_overrides();

    auto converted =
        convert_mcp_listing_to_module(sample_listing(), options, CapabilityCatalog::core());
    MIRA_CHECK(converted.has_value());
    const ToolModuleManifest &manifest = converted.value();

    // Module-level mapping.
    MIRA_CHECK(manifest.module_id == "acme.mcp.fixture");
    MIRA_CHECK(manifest.version == (SemanticVersion{2, 1, 0}));
    MIRA_CHECK(manifest.origin == ToolModuleOrigin::OutOfProcess);
    MIRA_CHECK(manifest.signer == "acme-mcp-signer");
    MIRA_CHECK(manifest.signature_algorithm == "ed25519-ref");
    MIRA_CHECK(manifest.resources.max_total_concurrent_invocations == 2);
    MIRA_CHECK(manifest.resources.max_total_result_bytes == 8192);
    MIRA_CHECK(manifest.required_capabilities == (std::vector<CapabilityId>{"env.screen.capture"}));
    MIRA_CHECK(manifest.conflicts_with == (std::vector<std::string>{"acme.mcp.rival"}));
    MIRA_CHECK(manifest.min_mira_module_abi <= kCurrentToolModuleAbi);

    // Member mapping (section 5.1): name verbatim, description verbatim,
    // version = module_version, result_schema pinned to {"type":"object"},
    // hint->risk mapping, raise-only override applied, member capabilities
    // and data access left empty (host maps module-level capabilities).
    MIRA_CHECK(manifest.tools.size() == 4);
    const auto member = [&manifest](const std::string &name) -> const ModuleToolSpec * {
        for (const auto &tool : manifest.tools) {
            if (tool.name == name) {
                return &tool;
            }
        }
        return nullptr;
    };

    const ModuleToolSpec *const lookup = member("mcp.lookup");
    MIRA_CHECK(lookup != nullptr);
    MIRA_CHECK(lookup->description == "read-only lookup");
    MIRA_CHECK(lookup->version == options.module_version);
    MIRA_CHECK(lookup->side_effect == ActionRisk::R0ReadOnly);
    MIRA_CHECK(to_json_string(lookup->result_schema.root) == R"json({"type":"object"})json");
    MIRA_CHECK(lookup->required_capabilities.empty());
    MIRA_CHECK(lookup->data_access.reads.empty() && lookup->data_access.writes.empty());
    MIRA_CHECK(to_json_string(lookup->arguments_schema.root).find("query") != std::string::npos);

    const ModuleToolSpec *const purge = member("mcp.purge");
    MIRA_CHECK(purge != nullptr);
    MIRA_CHECK(purge->side_effect == ActionRisk::R4Critical);
    MIRA_CHECK(purge->description == "destructive purge");

    const ModuleToolSpec *const rename = member("mcp.rename");
    MIRA_CHECK(rename != nullptr);
    MIRA_CHECK(rename->side_effect == ActionRisk::R2UserVisible);
    MIRA_CHECK(rename->description == "renames a user-visible object");
    MIRA_CHECK(rename->version == options.module_version);
    MIRA_CHECK(to_json_string(rename->result_schema.root) == R"json({"type":"object"})json");

    const ModuleToolSpec *const audit = member("mcp.audit_sync");
    MIRA_CHECK(audit != nullptr);
    MIRA_CHECK(audit->side_effect == ActionRisk::R3Sensitive); // raised from R2

    // Raise-only overrides cover every non-lowering direction, including an
    // equal-tier entry (equal is not a lowering).
    {
        McpAdmissionOptions raised = admission_options();
        raised.risk_overrides = {
            {"mcp.lookup", ActionRisk::R1ReversibleLow},   // R0 -> R1
            {"mcp.rename", ActionRisk::R4Critical},        // R2 -> R4
            {"mcp.audit_sync", ActionRisk::R2UserVisible}, // R2 -> R2
        };
        auto result =
            convert_mcp_listing_to_module(sample_listing(), raised, CapabilityCatalog::core());
        MIRA_CHECK(result.has_value());
        const auto raised_member = [&result](const std::string &name) -> const ModuleToolSpec * {
            for (const auto &tool : result.value().tools) {
                if (tool.name == name) {
                    return &tool;
                }
            }
            return nullptr;
        };
        MIRA_CHECK(raised_member("mcp.lookup")->side_effect == ActionRisk::R1ReversibleLow);
        MIRA_CHECK(raised_member("mcp.rename")->side_effect == ActionRisk::R4Critical);
        MIRA_CHECK(raised_member("mcp.audit_sync")->side_effect == ActionRisk::R2UserVisible);
    }
    return 0;
}

int g1_conversion_negative_matrix() {
    // Every case must reject the whole group with one mira.tool_module error;
    // no partial manifest escapes (the Result carries either value or error).
    const auto rejected = [](const McpServerListing &listing, const McpAdmissionOptions &options,
                             const ToolModuleLimits &limits = kDefaultToolModuleLimits,
                             const McpAdmissionLimits &mcp_limits = kDefaultMcpAdmissionLimits) {
        auto result = convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core(),
                                                    limits, mcp_limits);
        return failed_in_tool_module_domain(result) &&
               (result.error().code == ErrorCode::InvalidArgument ||
                result.error().code == ErrorCode::ResourceExhausted);
    };

    const McpAdmissionOptions base = admission_options();

    // Descriptor names: empty and charset violations (uppercase, whitespace,
    // empty dot segment, edges, non-ASCII).
    for (const std::string &bad_name :
         {std::string(""), std::string("Bad-Name"), std::string("a b"), std::string("dot..double"),
          std::string(".leading"), std::string("trailing."), std::string("t\xff)")}) {
        McpServerListing listing;
        listing.tools.push_back(descriptor(bad_name, "d"));
        MIRA_CHECK(rejected(listing, base));
    }
    // Duplicate descriptor names reject the whole listing (co-member too).
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.dup", "first"));
        listing.tools.push_back(descriptor("mcp.other", "fine"));
        listing.tools.push_back(descriptor("mcp.dup", "second"));
        MIRA_CHECK(rejected(listing, base));
    }
    // Empty description.
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.silent", ""));
        MIRA_CHECK(rejected(listing, base));
    }
    // Non-object input schema (root is a string / array / boolean).
    for (const JsonValue &root :
         {JsonValue{std::string("object")}, JsonValue{JsonValue::Array{}}, JsonValue{true}}) {
        McpServerListing listing;
        McpToolDescriptor bad;
        bad.name = "mcp.schema";
        bad.description = "d";
        bad.input_schema = JsonSchema{root};
        listing.tools.push_back(bad);
        MIRA_CHECK(rejected(listing, base));
    }
    // Schemas outside the shared subset: oneOf, $ref, allOf.
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor(
            "mcp.oneof", "d", R"json({"oneOf":[{"type":"object"},{"type":"array"}]})json"));
        MIRA_CHECK(rejected(listing, base));
    }
    {
        McpServerListing listing;
        listing.tools.push_back(
            descriptor("mcp.ref", "d", R"json({"$ref":"#/definitions/x"})json"));
        MIRA_CHECK(rejected(listing, base));
    }
    {
        McpServerListing listing;
        listing.tools.push_back(
            descriptor("mcp.allof", "d", R"json({"allOf":[{"type":"object"}]})json"));
        MIRA_CHECK(rejected(listing, base));
    }
    // Descriptor count over the MCP limit (whole group, ResourceExhausted).
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.a", "d"));
        listing.tools.push_back(descriptor("mcp.b", "d"));
        listing.tools.push_back(descriptor("mcp.c", "d"));
        McpAdmissionLimits mcp_limits;
        mcp_limits.max_descriptors = 2;
        auto result =
            convert_mcp_listing_to_module(listing, base, CapabilityCatalog::core(), {}, mcp_limits);
        MIRA_CHECK(failed_with(result, ErrorCode::ResourceExhausted));
        MIRA_CHECK(failed_in_tool_module_domain(result));
    }
    // Override count over the MCP limit.
    {
        McpAdmissionOptions options = base;
        options.risk_overrides = {{"mcp.rename", ActionRisk::R3Sensitive},
                                  {"mcp.audit_sync", ActionRisk::R4Critical}};
        McpAdmissionLimits mcp_limits;
        mcp_limits.max_risk_overrides = 1;
        auto result = convert_mcp_listing_to_module(sample_listing(), options,
                                                    CapabilityCatalog::core(), {}, mcp_limits);
        MIRA_CHECK(failed_with(result, ErrorCode::ResourceExhausted));
        MIRA_CHECK(failed_in_tool_module_domain(result));
    }
    // Lowering overrides (R2 -> R1 alone; R0 ok + R4 -> R2 together).
    {
        McpAdmissionOptions options = base;
        options.risk_overrides = {{"mcp.rename", ActionRisk::R1ReversibleLow}};
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    {
        McpAdmissionOptions options = base;
        options.risk_overrides = {{"mcp.lookup", ActionRisk::R0ReadOnly},
                                  {"mcp.purge", ActionRisk::R2UserVisible}};
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Override targeting an unknown member.
    {
        McpAdmissionOptions options = base;
        options.risk_overrides = {{"mcp.ghost", ActionRisk::R4Critical}};
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Duplicate override for one member.
    {
        McpAdmissionOptions options = base;
        options.risk_overrides = {{"mcp.rename", ActionRisk::R3Sensitive},
                                  {"mcp.rename", ActionRisk::R4Critical}};
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Incomplete signer triple: each field missing alone rejects.
    {
        McpAdmissionOptions options = base;
        options.signer.clear();
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    {
        McpAdmissionOptions options = base;
        options.signature_algorithm.clear();
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    {
        McpAdmissionOptions options = base;
        options.signature.clear();
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Empty module id and charset-invalid module ids.
    {
        McpAdmissionOptions options = admission_options();
        options.module_id.clear();
        auto result =
            convert_mcp_listing_to_module(sample_listing(), options, CapabilityCatalog::core());
        MIRA_CHECK(failed_with(result, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_module_domain(result));
    }
    for (const std::string &bad_id : {std::string("Acme.MCP"), std::string("acme mcp")}) {
        McpAdmissionOptions options = admission_options(bad_id);
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Zero-valued resource budgets (either field alone).
    {
        McpAdmissionOptions options = base;
        options.resources.max_total_concurrent_invocations = 0;
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    {
        McpAdmissionOptions options = base;
        options.resources.max_total_result_bytes = 0;
        MIRA_CHECK(rejected(sample_listing(), options));
    }
    // Over-length trust material through the manifest limits.
    {
        ToolModuleLimits limits;
        limits.max_signer_bytes = 8;
        MIRA_CHECK(rejected(sample_listing(), base, limits));
    }
    {
        ToolModuleLimits limits;
        limits.max_signature_bytes = 8;
        MIRA_CHECK(rejected(sample_listing(), base, limits));
    }
    // Boundary pin: a lowercase dashed/underscored name is inside the charset.
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.dashed_name2", "d"));
        auto result = convert_mcp_listing_to_module(listing, base, CapabilityCatalog::core());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().tools[0].name == "mcp.dashed_name2");
    }
    return 0;
}

int g1_empty_listing_and_determinism() {
    const McpAdmissionOptions options = admission_options();

    // Empty listing: whole-group rejection through the real parser. The frozen
    // TM0 manifest contract (M7-TM0-G5) requires 1..256 member tools and is
    // NOT relaxed for the MCP stage (design section 5.3 rule 5, revised
    // 2026-09-20: the upper-tier contract wins; a zero-member module has no
    // exposure). Deterministic: same verdict, same error, twice.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto empty =
            convert_mcp_listing_to_module(McpServerListing{}, options, CapabilityCatalog::core());
        MIRA_CHECK(!empty.has_value()); // no partial manifest escapes
        MIRA_CHECK(failed_in_tool_module_domain(empty));
        MIRA_CHECK(empty.error().code == ErrorCode::InvalidArgument);
        // The rejection reason carries the parser's frozen member bound.
        const std::string &message = empty.error().safe_message;
        MIRA_CHECK(message.find("1..256") != std::string::npos);
        MIRA_CHECK(message.find("member tools") != std::string::npos);
    }

    // Host contrast: a host that skips admission for an empty listing simply
    // never reaches the registry — admit_server with an empty listing fails
    // closed and leaves no record, no binding and no state movement.
    {
        MemoryEventStore store;
        RecordingAcceptor verifier;
        ModuleRegistry registry{CapabilityCatalog::core(), mcp_trust(&verifier), &store,
                                test_runtime_id(), test_session_id()};
        McpModuleAdmission admission{CapabilityCatalog::core(), registry};
        const McpAdmissionOptions skip_options = admission_options("acme.mcp.skipempty");
        auto rejected = admission.admit_server(McpServerListing{}, skip_options);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
        MIRA_CHECK(rejected.error().safe_message.find("1..256") != std::string::npos);
        MIRA_CHECK(registry.find("acme.mcp.skipempty") == nullptr); // no record
        MIRA_CHECK(registry.active_snapshot().modules.empty());     // no state movement
        MIRA_CHECK(admission.module_id().empty());                  // component unbound
        MIRA_CHECK(verifier.calls_.load() == 0);                    // trust never engaged
        // The same component stays usable for a real listing afterwards.
        auto admitted = admission.admit_server(sample_listing(), skip_options);
        MIRA_CHECK(admitted.has_value());
        MIRA_CHECK(admission.module_id() == "acme.mcp.skipempty");
    }

    // Same inputs twice: identical manifest digest, identical admission JSON.
    auto first =
        convert_mcp_listing_to_module(sample_listing(), options, CapabilityCatalog::core());
    auto second =
        convert_mcp_listing_to_module(sample_listing(), options, CapabilityCatalog::core());
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(tool_module_manifest_digest(first.value()) ==
               tool_module_manifest_digest(second.value()));
    MIRA_CHECK(canonical_json_string(mcp_admission_to_json(first.value())) ==
               canonical_json_string(mcp_admission_to_json(second.value())));

    // Different content moves the digest (deterministic and discriminating).
    McpAdmissionOptions bumped = options;
    bumped.module_version = SemanticVersion{2, 2, 0};
    auto moved = convert_mcp_listing_to_module(sample_listing(), bumped, CapabilityCatalog::core());
    MIRA_CHECK(moved.has_value());
    MIRA_CHECK(tool_module_manifest_digest(first.value()) !=
               tool_module_manifest_digest(moved.value()));
    return 0;
}

int g1_admission_projection_shape_and_redaction() {
    McpAdmissionOptions options = admission_options();
    options.risk_overrides = sample_overrides();
    auto converted =
        convert_mcp_listing_to_module(sample_listing(), options, CapabilityCatalog::core());
    MIRA_CHECK(converted.has_value());

    const JsonValue projection = mcp_admission_to_json(converted.value());
    MIRA_CHECK(projection.is_object());
    MIRA_CHECK(kMcpAdmissionSchema == "mira.tool_module.mcp.admission.v1");
    const auto *schema = projection.find("schema");
    MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
               *schema->as_string() == kMcpAdmissionSchema);
    const auto *module_id = projection.find("module_id");
    MIRA_CHECK(module_id != nullptr && module_id->as_string() != nullptr &&
               *module_id->as_string() == "acme.mcp.fixture");
    const auto *digest = projection.find("manifest_digest");
    MIRA_CHECK(digest != nullptr && digest->as_string() != nullptr &&
               *digest->as_string() == tool_module_manifest_digest(converted.value()).to_string());
    const auto *version = projection.find("version");
    MIRA_CHECK(version != nullptr && version->is_object());
    const auto *major = version->find("major");
    MIRA_CHECK(major != nullptr && major->is_integer() && *major->as_integer() == 2);

    // Per-member entries: name, side_effect, arguments_schema_digest.
    const auto *tools = projection.find("tools");
    MIRA_CHECK(tools != nullptr && tools->is_array() && tools->as_array()->size() == 4);
    std::size_t seen_sensitive = 0;
    for (const auto &entry : *tools->as_array()) {
        const auto *name = entry.find("name");
        MIRA_CHECK(name != nullptr && name->as_string() != nullptr);
        const auto *side_effect = entry.find("side_effect");
        MIRA_CHECK(side_effect != nullptr && side_effect->as_string() != nullptr);
        const auto *schema_digest = entry.find("arguments_schema_digest");
        MIRA_CHECK(schema_digest != nullptr && schema_digest->as_string() != nullptr &&
                   schema_digest->as_string()->size() == 64);
        // Cross-check the digest against an independent recomputation.
        const ModuleToolSpec *member = nullptr;
        for (const auto &tool : converted.value().tools) {
            if (tool.name == *name->as_string()) {
                member = &tool;
            }
        }
        MIRA_CHECK(member != nullptr);
        MIRA_CHECK(*schema_digest->as_string() ==
                   canonical_json_digest(member->arguments_schema.root).to_string());
        if (*side_effect->as_string() == "sensitive") {
            ++seen_sensitive;
        }
    }
    MIRA_CHECK(seen_sensitive == 1); // exactly the raised override member

    // Redaction: no description text, no signature material, no such keys.
    const std::string text = canonical_json_string(projection);
    MIRA_CHECK(text.find("read-only lookup") == std::string::npos);
    MIRA_CHECK(text.find("renames a user-visible object") == std::string::npos);
    MIRA_CHECK(text.find("acme-mcp-signer") == std::string::npos);
    MIRA_CHECK(text.find("bWNwLWZpeHR1cmUtc2lnbmF0dXJlLW1hdGVyaWFs") == std::string::npos);
    MIRA_CHECK(projection.find("description") == nullptr);
    MIRA_CHECK(projection.find("signature") == nullptr);
    MIRA_CHECK(projection.find("signer") == nullptr);
    MIRA_CHECK(projection.find("signature_algorithm") == nullptr);
    return 0;
}

int g1_report_is_deterministic() {
    // In-process determinism: byte-identical rebuild. Cross-process
    // determinism is asserted by running --report twice and comparing bytes.
    const std::string first = build_mcp_report();
    const std::string second = build_mcp_report();
    MIRA_CHECK(first == second);
    MIRA_CHECK(!first.empty());

    auto parsed = parse_json(first);
    MIRA_CHECK(parsed.has_value());
    const auto *schema = parsed.value().find("schema");
    MIRA_CHECK(schema != nullptr && schema->as_string() != nullptr &&
               *schema->as_string() == "mira.tool_module.mcp.report.v1");
    const auto *matrix = parsed.value().find("policy_matrix");
    MIRA_CHECK(matrix != nullptr && matrix->is_array());
    // 3 events x (4 null-record window combinations + 6 states x 4 window
    // combinations) = 3 x 28 = 84 rows.
    MIRA_CHECK(matrix->as_array()->size() == 84);
    const auto *chain = parsed.value().find("lifecycle_chain");
    MIRA_CHECK(chain != nullptr && chain->is_array() && chain->as_array()->size() == 2);
    // No clock or randomness source leaks into the report text.
    MIRA_CHECK(first.find("generated_at") == std::string::npos);
    MIRA_CHECK(first.find("timestamp") == std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// G2: session lifecycle downgrade-only (M7-MCP-G2)
// ---------------------------------------------------------------------------

int g2_policy_matrix_pure() {
    // Closed registry: every event is rejected regardless of record.
    for (const McpSessionEvent event :
         {McpSessionEvent::ServerConnected, McpSessionEvent::ServerDisconnected,
          McpSessionEvent::ToolListChanged}) {
        ModuleRecord record;
        record.state = ModuleState::Active;
        const auto closed_null = plan_mcp_session_action(event, nullptr, false, true);
        MIRA_CHECK(closed_null.action == McpAdmissionAction::RejectEvent);
        const auto closed_record = plan_mcp_session_action(event, &record, true, true);
        MIRA_CHECK(closed_record.action == McpAdmissionAction::RejectEvent);
    }
    // ServerConnected: admit only in the open window for an unknown module.
    MIRA_CHECK(
        plan_mcp_session_action(McpSessionEvent::ServerConnected, nullptr, false, false).action ==
        McpAdmissionAction::AdmitModule);
    const auto sealed =
        plan_mcp_session_action(McpSessionEvent::ServerConnected, nullptr, true, false);
    MIRA_CHECK(sealed.action == McpAdmissionAction::RejectEvent);
    for (const ModuleState state : {ModuleState::Verified, ModuleState::Active,
                                    ModuleState::Revoked, ModuleState::Quarantined}) {
        ModuleRecord record;
        record.state = state;
        const auto plan =
            plan_mcp_session_action(McpSessionEvent::ServerConnected, &record, false, false);
        MIRA_CHECK(plan.action == McpAdmissionAction::RejectEvent);
    }
    // Disconnected / ToolListChanged: revoke every non-terminal registered
    // state, NoAction on terminal or unregistered modules.
    for (const McpSessionEvent event :
         {McpSessionEvent::ServerDisconnected, McpSessionEvent::ToolListChanged}) {
        MIRA_CHECK(plan_mcp_session_action(event, nullptr, false, false).action ==
                   McpAdmissionAction::NoAction);
        for (const ModuleState state :
             {ModuleState::Discovered, ModuleState::Verified, ModuleState::Staged,
              ModuleState::Active, ModuleState::Deprecated}) {
            ModuleRecord record;
            record.state = state;
            const auto plan = plan_mcp_session_action(event, &record, false, false);
            MIRA_CHECK(plan.action == McpAdmissionAction::RevokeModule);
            MIRA_CHECK(!plan.reason.empty());
        }
        for (const ModuleState state : {ModuleState::Revoked, ModuleState::Quarantined}) {
            ModuleRecord record;
            record.state = state;
            MIRA_CHECK(plan_mcp_session_action(event, &record, false, false).action ==
                       McpAdmissionAction::NoAction);
        }
        // Sealed but open: downgrades remain available.
        ModuleRecord active;
        active.state = ModuleState::Active;
        MIRA_CHECK(plan_mcp_session_action(event, &active, true, false).action ==
                   McpAdmissionAction::RevokeModule);
    }
    // Every plan carries a bounded non-empty reason.
    ModuleRecord record;
    record.state = ModuleState::Active;
    for (const McpSessionEvent event :
         {McpSessionEvent::ServerConnected, McpSessionEvent::ServerDisconnected,
          McpSessionEvent::ToolListChanged}) {
        const McpSessionPlan plan = plan_mcp_session_action(event, &record, false, false);
        MIRA_CHECK(!plan.reason.empty());
        MIRA_CHECK(plan.reason.size() <= 512);
    }
    // Name helpers round-trip the whole vocabulary.
    MIRA_CHECK(mcp_session_event_name(McpSessionEvent::ServerConnected) == "server_connected");
    MIRA_CHECK(mcp_session_event_name(McpSessionEvent::ServerDisconnected) ==
               "server_disconnected");
    MIRA_CHECK(mcp_session_event_name(McpSessionEvent::ToolListChanged) == "tool_list_changed");
    MIRA_CHECK(mcp_admission_action_name(McpAdmissionAction::AdmitModule) == "admit_module");
    MIRA_CHECK(mcp_admission_action_name(McpAdmissionAction::RevokeModule) == "revoke_module");
    MIRA_CHECK(mcp_admission_action_name(McpAdmissionAction::RejectEvent) == "reject_event");
    MIRA_CHECK(mcp_admission_action_name(McpAdmissionAction::NoAction) == "no_action");
    return 0;
}

// Full deploy-window chain shared by several G2 gates: registry with
// OutOfProcess trust, admission of `listing` under `options`, host promotion,
// session negotiation and exposure projection.
struct AdmittedChain {
    MemoryEventStore store;
    RecordingAcceptor verifier;
    std::unique_ptr<ModuleRegistry> registry;
    std::unique_ptr<ModuleNegotiationCoordinator> coordinator;
    std::unique_ptr<McpModuleAdmission> admission;
    ToolModuleManifest manifest;
    Hash digest{};
    NegotiationView view{};
    ToolExposure exposure{};
};

bool build_admitted_chain(AdmittedChain &chain, const McpServerListing &listing,
                          const McpAdmissionOptions &options) {
    chain.registry =
        std::make_unique<ModuleRegistry>(CapabilityCatalog::core(), mcp_trust(&chain.verifier),
                                         &chain.store, test_runtime_id(), test_session_id());
    chain.coordinator = std::make_unique<ModuleNegotiationCoordinator>(
        CapabilityCatalog::core(), &chain.store, test_runtime_id(), test_session_id());
    chain.admission =
        std::make_unique<McpModuleAdmission>(CapabilityCatalog::core(), *chain.registry);

    auto expected = convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core());
    if (!expected.has_value()) {
        return false;
    }
    chain.manifest = expected.value();
    chain.digest = tool_module_manifest_digest(chain.manifest);

    auto admitted = chain.admission->admit_server(listing, options);
    if (!admitted.has_value() || admitted.value() != chain.digest) {
        return false;
    }
    const ModuleRecord *record = chain.registry->find(options.module_id);
    if (record == nullptr || record->state != ModuleState::Verified) {
        return false;
    }
    if (!chain.registry->stage_module(options.module_id).has_value() ||
        !chain.registry->activate_module(options.module_id).has_value()) {
        return false;
    }
    auto session =
        chain.coordinator->on_session_established(5, EnvironmentCapabilities{}, *chain.registry);
    if (!session.has_value()) {
        return false;
    }
    chain.view = session.value();
    auto projected =
        project_tool_exposure(1, chain.registry->active_snapshot().modules, chain.view.negotiation);
    if (!projected.has_value()) {
        return false;
    }
    chain.exposure = projected.value();
    return chain.verifier.calls_.load() >= 1;
}

int g2_admit_full_chain() {
    AdmittedChain chain;
    MIRA_CHECK(build_admitted_chain(chain, sample_listing(), admission_options()));

    // Registration reached Verified through the real TM1 trust path and the
    // verifier authenticated the unsigned digest of exactly this manifest.
    MIRA_CHECK(chain.verifier.calls_.load() >= 1);
    MIRA_CHECK(chain.verifier.last_signer == "acme-mcp-signer");
    MIRA_CHECK(chain.verifier.last_algorithm == "ed25519-ref");
    MIRA_CHECK(chain.verifier.last_digest == tool_module_unsigned_manifest_digest(chain.manifest));
    MIRA_CHECK(chain.verifier.last_digest != chain.digest);
    MIRA_CHECK(chain.admission->module_id() == "acme.mcp.fixture");
    MIRA_CHECK(chain.registry->find("acme.mcp.fixture")->state == ModuleState::Active);

    // Negotiation verdict: Available (module requires no capabilities).
    MIRA_CHECK(chain.view.negotiation.modules.size() == 1);
    MIRA_CHECK(chain.view.negotiation.modules[0].status == ModuleStatus::Available);
    MIRA_CHECK(chain.view.negotiation.modules[0].module_digest == chain.digest);

    // Projection contains the MCP members with derived identity and the
    // side-effect mapping (read-only -> false, everything else -> true).
    MIRA_CHECK(chain.exposure.tools.size() == 4);
    MIRA_CHECK(chain.exposure.modules.size() == 1);
    MIRA_CHECK(chain.exposure.modules[0].module_id == "acme.mcp.fixture");
    for (const auto &tool : chain.exposure.tools) {
        const ModuleToolSpec *member = nullptr;
        for (const auto &spec : chain.manifest.tools) {
            if (spec.name == tool.wire_name) {
                member = &spec;
            }
        }
        MIRA_CHECK(member != nullptr);
        MIRA_CHECK(tool.tool_id ==
                   derive_module_tool_id("acme.mcp.fixture", chain.digest, tool.wire_name));
        MIRA_CHECK(tool.has_side_effects == (member->side_effect != ActionRisk::R0ReadOnly));
        MIRA_CHECK(tool.version == admission_options().module_version);
    }
    // The read-only member maps false; the raised member stays side-effecting.
    const ExposedToolSpec *lookup = nullptr;
    for (const auto &tool : chain.exposure.tools) {
        if (tool.wire_name == "mcp.lookup") {
            lookup = &tool;
        }
    }
    MIRA_CHECK(lookup != nullptr && !lookup->has_side_effects);
    return 0;
}

int g2_runtime_downgrade_and_pinned_settlement() {
    AdmittedChain chain;
    MIRA_CHECK(build_admitted_chain(chain, sample_listing(), admission_options()));
    const ToolExposure pinned = chain.exposure; // in-flight consumer pins view 1

    // ToolListChanged on the Active module: downgrade-only revoke.
    auto changed = chain.admission->on_session_event(McpSessionEvent::ToolListChanged);
    MIRA_CHECK(changed.has_value());
    MIRA_CHECK(changed.value().action == McpAdmissionAction::RevokeModule);
    MIRA_CHECK(!changed.value().reason.empty());
    const ModuleRecord *record = chain.registry->find("acme.mcp.fixture");
    MIRA_CHECK(record != nullptr && record->state == ModuleState::Revoked);
    MIRA_CHECK(chain.registry->is_tombstoned(chain.digest));

    // Renegotiation converges: the shrunk projection no longer contains any
    // member and the exposure surface shrank (digest moved, tools empty).
    auto renegotiated = chain.coordinator->on_modules_changed(*chain.registry);
    MIRA_CHECK(renegotiated.has_value());
    auto after = project_tool_exposure(2, chain.registry->active_snapshot().modules,
                                       renegotiated.value().negotiation);
    MIRA_CHECK(after.has_value());
    MIRA_CHECK(after.value().tools.empty());
    MIRA_CHECK(after.value().modules.empty());
    MIRA_CHECK(after.value().snapshot_digest != pinned.snapshot_digest);

    // In-flight settlement: a dispatcher bound to the pinned (pre-revoke)
    // exposure still executes its members normally (design section 7.4).
    {
        executor::Executor executor;
        MIRA_CHECK(make_executor(executor, 1, 2));
        auto dispatcher = McpToolDispatcher::make(pinned, fast_limits());
        MIRA_CHECK(dispatcher.has_value());
        EchoTransport transport;
        const ExposedToolSpec &spec = pinned.tools[0]; // mcp.audit_sync, plain object schema
        ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({})json"));
        auto settled = dispatcher.value()->execute(proposal, plain_context(), executor, transport);
        MIRA_CHECK(settled.has_value());
        MIRA_CHECK(!settled.value().failed);
        MIRA_CHECK(settled.value().tool_id == spec.tool_id);
        dispatcher.value()->close(std::chrono::milliseconds{0});
        MIRA_CHECK(dispatcher.value()->stats().in_flight == 0);
        MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    // ServerDisconnected revokes a second module through the same matrix.
    {
        MemoryEventStore store;
        RecordingAcceptor verifier;
        ModuleRegistry registry{CapabilityCatalog::core(), mcp_trust(&verifier), &store,
                                test_runtime_id(), test_session_id()};
        McpModuleAdmission admission{CapabilityCatalog::core(), registry};
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.other_probe", "second module member"));
        const McpAdmissionOptions options = admission_options("acme.mcp.other");
        MIRA_CHECK(admission.admit_server(listing, options).has_value());
        MIRA_CHECK(registry.stage_module("acme.mcp.other").has_value());
        MIRA_CHECK(registry.activate_module("acme.mcp.other").has_value());

        auto disconnected = admission.on_session_event(McpSessionEvent::ServerDisconnected);
        MIRA_CHECK(disconnected.has_value());
        MIRA_CHECK(disconnected.value().action == McpAdmissionAction::RevokeModule);
        MIRA_CHECK(registry.find("acme.mcp.other")->state == ModuleState::Revoked);

        // Terminal module: events are idempotent NoAction, no second revoke.
        const std::uint64_t revoked_before = registry.stats().revoked;
        for (const McpSessionEvent event :
             {McpSessionEvent::ServerDisconnected, McpSessionEvent::ToolListChanged}) {
            auto plan = admission.on_session_event(event);
            MIRA_CHECK(plan.has_value());
            MIRA_CHECK(plan.value().action == McpAdmissionAction::NoAction);
        }
        MIRA_CHECK(registry.stats().revoked == revoked_before);
        MIRA_CHECK(registry.find("acme.mcp.other")->state == ModuleState::Revoked);
    }

    // Re-admission of the same module_id in the same registry lifetime is
    // forbidden (record exists), and a fresh registry seeded with the
    // tombstone rejects the identical content digest.
    {
        McpModuleAdmission second{CapabilityCatalog::core(), *chain.registry};
        auto repeat = second.admit_server(sample_listing(), admission_options());
        MIRA_CHECK(failed_with(repeat, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(repeat));

        MemoryEventStore seeded_store;
        RecordingAcceptor seeded_verifier;
        ModuleTrustConfig seeded_trust = mcp_trust(&seeded_verifier);
        seeded_trust.revoked_digests.push_back(chain.digest);
        ModuleRegistry seeded{CapabilityCatalog::core(), seeded_trust, &seeded_store,
                              test_runtime_id(), test_session_id()};
        McpModuleAdmission third{CapabilityCatalog::core(), seeded};
        auto blocked = third.admit_server(sample_listing(), admission_options());
        MIRA_CHECK(failed_with(blocked, ErrorCode::InvalidState)); // tombstone hit
        MIRA_CHECK(failed_in_tool_module_domain(blocked));
    }
    return 0;
}

int g2_window_duplicate_and_close_semantics() {
    AdmittedChain chain;
    MIRA_CHECK(build_admitted_chain(chain, sample_listing(), admission_options()));

    // Duplicate admission of the bound module: rejected, state unchanged.
    auto duplicate = chain.admission->admit_server(sample_listing(), admission_options());
    MIRA_CHECK(failed_with(duplicate, ErrorCode::InvalidState));
    MIRA_CHECK(failed_in_tool_module_domain(duplicate));
    MIRA_CHECK(chain.registry->find("acme.mcp.fixture")->state == ModuleState::Active);

    // The component refuses a second, different module once bound.
    McpAdmissionOptions other = admission_options("acme.mcp.other");
    auto stranger = chain.admission->admit_server(sample_listing(), other);
    MIRA_CHECK(failed_with(stranger, ErrorCode::InvalidState));
    MIRA_CHECK(failed_in_tool_module_domain(stranger));
    MIRA_CHECK(chain.registry->find("acme.mcp.other") == nullptr);

    // Events without an admitted module are an explicit error.
    {
        RecordingAcceptor verifier;
        ModuleRegistry registry{CapabilityCatalog::core(), mcp_trust(&verifier)};
        McpModuleAdmission unbound{CapabilityCatalog::core(), registry};
        auto plan = unbound.on_session_event(McpSessionEvent::ServerConnected);
        MIRA_CHECK(failed_with(plan, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(plan));
    }

    // Seal ends the registration window: a fresh admission is rejected and
    // leaves no record, while downgrades keep working.
    MIRA_CHECK(chain.registry->seal().has_value());
    MIRA_CHECK(chain.registry->sealed());
    {
        McpModuleAdmission late{CapabilityCatalog::core(), *chain.registry};
        McpAdmissionOptions sealed_options = admission_options("acme.mcp.late");
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.late_probe", "late member"));
        auto rejected = late.admit_server(listing, sealed_options);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(failed_in_tool_module_domain(rejected));
        MIRA_CHECK(chain.registry->find("acme.mcp.late") == nullptr);
    }
    {
        auto downgrade = chain.admission->on_session_event(McpSessionEvent::ServerDisconnected);
        MIRA_CHECK(downgrade.has_value());
        MIRA_CHECK(downgrade.value().action == McpAdmissionAction::RevokeModule);
        MIRA_CHECK(chain.registry->find("acme.mcp.fixture")->state == ModuleState::Revoked);
    }

    // Close: every session event is rejected and admission fails closed.
    const auto close_report = chain.registry->close();
    MIRA_CHECK(chain.registry->closed());
    MIRA_CHECK(close_report.snapshot_generation == chain.registry->active_snapshot().generation);
    for (const McpSessionEvent event :
         {McpSessionEvent::ServerConnected, McpSessionEvent::ServerDisconnected,
          McpSessionEvent::ToolListChanged}) {
        auto plan = chain.admission->on_session_event(event);
        MIRA_CHECK(plan.has_value());
        MIRA_CHECK(plan.value().action == McpAdmissionAction::RejectEvent);
        MIRA_CHECK(!plan.value().reason.empty());
    }
    {
        McpModuleAdmission closed_admission{CapabilityCatalog::core(), *chain.registry};
        McpAdmissionOptions closed_options = admission_options("acme.mcp.closed");
        auto rejected = closed_admission.admit_server(McpServerListing{}, closed_options);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
        MIRA_CHECK(chain.registry->find("acme.mcp.closed") == nullptr);
    }
    return 0;
}

int g2_lifecycle_events_redacted() {
    const std::string secret_description = "UNIQUE-MCP-DESC-7f3a91";
    const std::string secret_signature = "UNIQUE-MCP-SIG-0c52d8-material";

    McpServerListing listing;
    listing.tools.push_back(descriptor("mcp.secret_probe", secret_description));
    McpAdmissionOptions options = admission_options("acme.mcp.secret");
    options.signature = secret_signature;

    MemoryEventStore store;
    RecordingAcceptor verifier;
    ModuleRegistry registry{CapabilityCatalog::core(), mcp_trust(&verifier), &store,
                            test_runtime_id(), test_session_id()};
    ModuleNegotiationCoordinator coordinator{CapabilityCatalog::core(), &store, test_runtime_id(),
                                             test_session_id()};
    McpModuleAdmission admission{CapabilityCatalog::core(), registry};

    MIRA_CHECK(admission.admit_server(listing, options).has_value());
    MIRA_CHECK(registry.stage_module("acme.mcp.secret").has_value());
    MIRA_CHECK(registry.activate_module("acme.mcp.secret").has_value());
    MIRA_CHECK(
        coordinator.on_session_established(3, EnvironmentCapabilities{}, registry).has_value());
    auto disconnected = admission.on_session_event(McpSessionEvent::ServerDisconnected);
    MIRA_CHECK(disconnected.has_value());
    MIRA_CHECK(disconnected.value().action == McpAdmissionAction::RevokeModule);

    EventQuery query;
    query.session_id = test_session_id();
    query.limit = 8192;
    auto page = store.read(query);
    MIRA_CHECK(page.has_value());
    MIRA_CHECK(page.value().events.size() >= 6); // discovered..revoked + negotiation
    for (const auto &envelope : page.value().events) {
        MIRA_CHECK(envelope.payload.type == kModuleLifecycleEventSchema ||
                   envelope.payload.type == kModuleNegotiationEventSchema);
        // No description text, no signature material, no signature keys.
        MIRA_CHECK(envelope.payload.data.find(secret_description) == std::string::npos);
        MIRA_CHECK(envelope.payload.data.find(secret_signature) == std::string::npos);
        auto data = parse_json(envelope.payload.data);
        MIRA_CHECK(data.has_value());
        MIRA_CHECK(data.value().find("signature") == nullptr);
        MIRA_CHECK(data.value().find("signer") == nullptr);
        MIRA_CHECK(data.value().find("description") == nullptr);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// G3: single gate parity with BuiltinToolRegistry (M7-MCP-G3, DEC-015)
// ---------------------------------------------------------------------------

enum class OutcomeKind { OkRecord, FailedRecord, Error };

struct Verdict final {
    OutcomeKind kind = OutcomeKind::Error;
    ErrorCode code = ErrorCode::Internal;
    bool failed = false;
    std::string summary;
    ProviderToolCallId call;
    ToolId tool{};
    JsonValue result{JsonValue::Object{}};
};

Verdict classify(const Result<ToolExecutionRecord> &outcome) {
    Verdict verdict;
    if (outcome.has_value()) {
        verdict.kind = outcome.value().failed ? OutcomeKind::FailedRecord : OutcomeKind::OkRecord;
        verdict.failed = outcome.value().failed;
        verdict.summary = outcome.value().safe_error_summary;
        verdict.call = outcome.value().provider_call_id;
        verdict.tool = outcome.value().tool_id;
        verdict.result = outcome.value().result;
        return verdict;
    }
    verdict.kind = OutcomeKind::Error;
    verdict.code = outcome.error().code;
    return verdict;
}

// Drives one proposal through BuiltinToolRegistry. The registry is loaded
// with the PINNED truth (`spec`); the proposal (possibly tampered) is checked
// against that truth, exactly like the dispatcher's pinned exposure.
Verdict run_builtin(const ExposedToolSpec &spec, const ToolProposal &proposal,
                    const OperationContext &context, EchoTransport &transport) {
    BuiltinToolRegistry registry;
    BuiltinToolSpec registered;
    registered.tool_id = spec.tool_id;
    registered.version = spec.version;
    registered.wire_name = spec.wire_name;
    registered.description = spec.description;
    registered.parameters_schema = spec.parameters_schema;
    registered.has_side_effects = spec.has_side_effects;
    BuiltinToolHandler handler = [&transport](const JsonValue &arguments,
                                              const OperationContext &ctx) -> Result<JsonValue> {
        if (ctx.cancelled()) {
            Error error;
            error.code = ErrorCode::Cancelled;
            error.domain = "test.handler";
            error.safe_message = "cancelled by context";
            return error;
        }
        return transport.invoke("cmp.echo", arguments, McpInvocationProbe{});
    };
    if (!registry.register_tool(registered, handler).has_value()) {
        fixture_failed("builtin comparison fixture failed to register");
    }
    return classify(registry.execute(proposal, context));
}

// Drives one proposal through the MCP dispatcher bound to the same pinned
// identity and a live executor.
Verdict run_mcp(const ExposedToolSpec &spec, const ToolProposal &proposal,
                const OperationContext &context, executor::Executor &executor,
                IMcpToolTransport &transport, const McpDispatchLimits &limits) {
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), limits);
    if (!dispatcher.has_value()) {
        fixture_failed("mcp comparison fixture failed to construct the dispatcher");
    }
    Verdict verdict = classify(dispatcher.value()->execute(proposal, context, executor, transport));
    dispatcher.value()->close(std::chrono::milliseconds{100});
    return verdict;
}

bool verdicts_match(const Verdict &builtin, const Verdict &mcp, const char *&why) {
    if (builtin.kind != mcp.kind) {
        why = "outcome kind differs";
        return false;
    }
    if (builtin.kind == OutcomeKind::Error && builtin.code != mcp.code) {
        why = "error code differs";
        return false;
    }
    if (builtin.kind != OutcomeKind::Error) {
        if (builtin.failed != mcp.failed) {
            why = "failed flag differs";
            return false;
        }
        if (builtin.call.value != mcp.call.value) {
            why = "provider call id backfill differs";
            return false;
        }
        if (!(builtin.tool == mcp.tool)) {
            why = "tool id backfill differs";
            return false;
        }
        if (builtin.summary != mcp.summary) {
            why = "safe_error_summary differs";
            return false;
        }
        if (to_json_string(builtin.result) != to_json_string(mcp.result)) {
            why = "result payload differs";
            return false;
        }
    }
    return true;
}

int g3_gate_parity_with_builtin() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const McpDispatchLimits limits = fast_limits();
    const ToolId tool_id = ToolId::generate();
    const ExposedToolSpec spec = echo_spec(tool_id);
    std::size_t rows = 0;
    std::size_t mismatches = 0;

    const auto record_row = [&rows](const char *name, const Verdict &builtin, const Verdict &mcp,
                                    const char *why) {
        ++rows;
        const auto kind_name = [](OutcomeKind kind) {
            switch (kind) {
            case OutcomeKind::OkRecord:
                return "ok";
            case OutcomeKind::FailedRecord:
                return "failed_record";
            case OutcomeKind::Error:
                return "error";
            }
            return "error";
        };
        std::cout << "  [parity] " << name << ": builtin=" << kind_name(builtin.kind)
                  << (builtin.kind == OutcomeKind::Error
                          ? "/" + std::to_string(static_cast<int>(builtin.code))
                          : std::string())
                  << " mcp=" << kind_name(mcp.kind)
                  << (mcp.kind == OutcomeKind::Error
                          ? "/" + std::to_string(static_cast<int>(mcp.code))
                          : std::string())
                  << (why != nullptr ? std::string(" MISMATCH: ") + why : std::string()) << '\n';
    };

    // Same-transport parity runner (value/error/throw modes).
    const auto parity = [&](const char *name, const ToolProposal &proposal,
                            const OperationContext &context, EchoTransport &transport) {
        const Verdict builtin = run_builtin(spec, proposal, context, transport);
        const Verdict mcp = run_mcp(spec, proposal, context, executor, transport, limits);
        const char *why = nullptr;
        const bool match = verdicts_match(builtin, mcp, why);
        if (!match) {
            ++mismatches;
        }
        record_row(name, builtin, mcp, match ? nullptr : why);
        return match;
    };

    const ToolProposal valid = make_proposal(spec, json_or_abort(R"json({"x":5})json"), "call_ok");
    const OperationContext plain = plain_context();
    EchoTransport ok_transport;

    // 1. Valid call: both succeed with the same payload and backfill.
    MIRA_CHECK(parity("valid", valid, plain, ok_transport));

    // 2. Unknown tool id.
    {
        ToolProposal unknown = valid;
        unknown.tool_id = ToolId::generate();
        unknown.operation_id = OperationId::generate();
        MIRA_CHECK(parity("unknown tool id", unknown, plain, ok_transport));
    }
    // 3. Wire name tampered.
    {
        ToolProposal tampered = valid;
        tampered.wire_name = "cmp.echo.evil";
        tampered.operation_id = OperationId::generate();
        MIRA_CHECK(parity("wire name mismatch", tampered, plain, ok_transport));
    }
    // 4. Version tampered.
    {
        ToolProposal tampered = valid;
        tampered.tool_version = SemanticVersion{9, 9, 9};
        tampered.operation_id = OperationId::generate();
        MIRA_CHECK(parity("version mismatch", tampered, plain, ok_transport));
    }
    // 5. Side-effect flag tampered.
    {
        ToolProposal tampered = valid;
        tampered.has_side_effects = false;
        tampered.operation_id = OperationId::generate();
        MIRA_CHECK(parity("side effects mismatch", tampered, plain, ok_transport));
    }
    // 6. Duplicate operation id after a successful first dispatch.
    {
        BuiltinToolRegistry registry;
        BuiltinToolSpec registered;
        registered.tool_id = spec.tool_id;
        registered.version = spec.version;
        registered.wire_name = spec.wire_name;
        registered.description = spec.description;
        registered.parameters_schema = spec.parameters_schema;
        registered.has_side_effects = spec.has_side_effects;
        BuiltinToolHandler handler = [](const JsonValue &,
                                        const OperationContext &) -> Result<JsonValue> {
            return JsonValue{JsonValue::Object{{"echo", JsonValue{std::int64_t{1}}}}};
        };
        MIRA_CHECK(registry.register_tool(registered, handler).has_value());
        const Verdict builtin_one = classify(registry.execute(valid, plain));
        const Verdict builtin_two = classify(registry.execute(valid, plain));
        MIRA_CHECK(builtin_one.kind == OutcomeKind::OkRecord);
        MIRA_CHECK(builtin_two.kind == OutcomeKind::Error &&
                   builtin_two.code == ErrorCode::AlreadyExists);

        auto dispatcher = McpToolDispatcher::make(single_exposure(spec), limits);
        MIRA_CHECK(dispatcher.has_value());
        EchoTransport transport;
        const Verdict mcp_one =
            classify(dispatcher.value()->execute(valid, plain, executor, transport));
        const Verdict mcp_two =
            classify(dispatcher.value()->execute(valid, plain, executor, transport));
        dispatcher.value()->close(std::chrono::milliseconds{100});
        MIRA_CHECK(mcp_one.kind == OutcomeKind::OkRecord);
        MIRA_CHECK(mcp_two.kind == OutcomeKind::Error && mcp_two.code == ErrorCode::AlreadyExists);
        record_row("duplicate operation id", builtin_two, mcp_two, nullptr);
    }
    // 7. Argument schema violations -> failed record on both.
    {
        const ToolProposal bad_args =
            make_proposal(spec, json_or_abort(R"json({"x":"not-an-integer"})json"), "call_bad");
        const Verdict builtin = run_builtin(spec, bad_args, plain, ok_transport);
        const Verdict mcp = run_mcp(spec, bad_args, plain, executor, ok_transport, limits);
        const char *why = nullptr;
        MIRA_CHECK(verdicts_match(builtin, mcp, why));
        MIRA_CHECK(builtin.kind == OutcomeKind::FailedRecord);
        MIRA_CHECK(builtin.summary.find("arguments failed schema validation") != std::string::npos);
        MIRA_CHECK(builtin.summary.size() <= 512 + 3);
        MIRA_CHECK(builtin.call.value == "call_bad");
        MIRA_CHECK(builtin.tool == tool_id);
        record_row("schema violation (type)", builtin, mcp, why);

        const ToolProposal missing =
            make_proposal(spec, json_or_abort(R"json({"other":1})json"), "call_missing");
        const Verdict builtin_missing = run_builtin(spec, missing, plain, ok_transport);
        const Verdict mcp_missing = run_mcp(spec, missing, plain, executor, ok_transport, limits);
        const char *why_missing = nullptr;
        MIRA_CHECK(verdicts_match(builtin_missing, mcp_missing, why_missing));
        MIRA_CHECK(builtin_missing.kind == OutcomeKind::FailedRecord);
        record_row("schema violation (missing required)", builtin_missing, mcp_missing,
                   why_missing);
    }
    // 8. Engine error (handler/transport failure) -> failed record on both.
    {
        EchoTransport failing(EchoTransport::Mode::Error, "engine refused the call");
        MIRA_CHECK(parity("engine error", valid, plain, failing));
    }
    // 9. Cancellation -> Cancelled error result on both (never a record). The
    // builtin handler observes the cancelled context directly; the MCP side
    // needs a probe-honoring transport so the stop flag reaches it before the
    // value returns.
    {
        const OperationContext cancelled = cancelled_context();
        const Verdict builtin = run_builtin(spec, valid, cancelled, ok_transport);
        ProbePollingTransport transport;
        const Verdict mcp = run_mcp(spec, valid, cancelled, executor, transport, limits);
        const char *why = nullptr;
        MIRA_CHECK(verdicts_match(builtin, mcp, why));
        MIRA_CHECK(builtin.kind == OutcomeKind::Error && builtin.code == ErrorCode::Cancelled);
        record_row("cancelled context", builtin, mcp, why);
    }
    // 10. Oversize result -> failed record on both under the same bridge cap.
    {
        McpDispatchLimits wide = limits;
        wide.max_result_bytes = kDefaultToolBridgeLimits.max_result_bytes;
        JsonValue::Object payload;
        payload.emplace_back("blob",
                             std::string(kDefaultToolBridgeLimits.max_result_bytes + 512, 'z'));
        EchoTransport big(EchoTransport::Mode::Value, {}, JsonValue(std::move(payload)));
        const Verdict builtin = run_builtin(spec, valid, plain, big);
        const Verdict mcp = run_mcp(spec, valid, plain, executor, big, wide);
        const char *why = nullptr;
        MIRA_CHECK(verdicts_match(builtin, mcp, why));
        MIRA_CHECK(builtin.kind == OutcomeKind::FailedRecord);
        MIRA_CHECK(builtin.summary.find("exceeds the size limit") != std::string::npos);
        record_row("oversize result", builtin, mcp, why);
    }

    MIRA_CHECK(mismatches == 0);
    MIRA_CHECK(rows == 11);
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// G4: cancellation, deadline and shutdown closure (M7-MCP-G4)
// ---------------------------------------------------------------------------

int g4_probe_and_make_limits_validation() {
    // Probe semantics: default never fires; an owned flag/deadline do.
    const McpInvocationProbe idle;
    MIRA_CHECK(!idle.stop_requested());
    MIRA_CHECK(!idle.deadline_expired());
    auto flag = std::make_shared<std::atomic<bool>>(true);
    const McpInvocationProbe stopped(flag, std::nullopt);
    MIRA_CHECK(stopped.stop_requested());
    MIRA_CHECK(!stopped.deadline_expired());
    const McpInvocationProbe expired(std::make_shared<std::atomic<bool>>(false),
                                     std::chrono::steady_clock::now() -
                                         std::chrono::milliseconds(1));
    MIRA_CHECK(expired.deadline_expired());

    // Dispatcher construction fails closed on invalid limits (each dimension
    // alone, everything else valid).
    const ToolExposure exposure = single_exposure(echo_spec(ToolId::generate()));
    const auto made = McpToolDispatcher::make(exposure, fast_limits());
    MIRA_CHECK(made.has_value());
    MIRA_CHECK(!made.value()->closed());
    made.value()->close(std::chrono::milliseconds{0});

    struct BadLimits {
        const char *name;
        McpDispatchLimits limits;
    };
    std::vector<BadLimits> cases;
    {
        McpDispatchLimits l = fast_limits();
        l.max_concurrent_invocations = 0;
        cases.push_back({"zero concurrency", l});
    }
    {
        McpDispatchLimits l = fast_limits();
        l.max_result_bytes = 0;
        cases.push_back({"zero result budget", l});
    }
    {
        McpDispatchLimits l = fast_limits();
        l.max_pending_invocations = 0;
        cases.push_back({"zero pending budget", l});
    }
    {
        McpDispatchLimits l = fast_limits();
        l.max_invocation_duration = std::chrono::milliseconds{0};
        cases.push_back({"zero duration", l});
    }
    {
        McpDispatchLimits l = fast_limits();
        l.cancellation_grace = std::chrono::milliseconds{0};
        cases.push_back({"zero grace", l});
    }
    {
        McpDispatchLimits l = fast_limits();
        l.default_close_drain_budget = std::chrono::milliseconds{0};
        cases.push_back({"zero drain budget", l});
    }
    for (const auto &entry : cases) {
        auto rejected = McpToolDispatcher::make(exposure, entry.limits);
        MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidArgument));
        MIRA_CHECK(failed_in_tool_executor_domain(rejected));
    }
    return 0;
}

int g4_normal_completion_and_stats() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());
    EchoTransport transport;

    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":1})json"));
    const JsonValue arguments = proposal.arguments;
    auto settled = dispatcher.value()->execute(proposal, plain_context(), executor, transport);
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(!settled.value().failed);
    MIRA_CHECK(settled.value().provider_call_id.value == "call_1");
    MIRA_CHECK(settled.value().tool_id == spec.tool_id);
    MIRA_CHECK(to_json_string(settled.value().result) ==
               to_json_string(JsonValue{JsonValue::Object{
                   {"echo", arguments}, {"wire", JsonValue{std::string("cmp.echo")}}}}));

    const McpDispatcherStats stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.dispatched == 1);
    MIRA_CHECK(stats.completed == 1);
    MIRA_CHECK(stats.failed_records == 0);
    MIRA_CHECK(stats.cancelled == 0);
    MIRA_CHECK(stats.deadline_exceeded == 0);
    MIRA_CHECK(stats.in_flight == 0);
    MIRA_CHECK(stats.pending == 0);
    MIRA_CHECK(!stats.closed);

    // Terminal close: idempotent, visible in stats, rejects afterwards.
    const McpDispatcherCloseReport report = dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(report.drained == 0);
    MIRA_CHECK(report.abandoned == 0);
    MIRA_CHECK(report.still_pending == 0);
    MIRA_CHECK(dispatcher.value()->closed());
    MIRA_CHECK(dispatcher.value()->stats().closed);
    const McpDispatcherCloseReport repeat = dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(repeat.drained == 0 && repeat.still_pending == 0);

    auto rejected = dispatcher.value()->execute(proposal, plain_context(), executor, transport);
    MIRA_CHECK(failed_with(rejected, ErrorCode::InvalidState));
    MIRA_CHECK(failed_in_tool_executor_domain(rejected));
    MIRA_CHECK(dispatcher.value()->stats().closed_rejections == 1);

    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_submission_rejection_and_rollback_retry() {
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());
    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":2})json"));
    EchoTransport transport;

    // Stopped executor: synchronous rejection surfaces as an explicit error
    // and the operation reservation is rolled back.
    {
        executor::Executor stopped;
        MIRA_CHECK(make_executor(stopped, 1, 1));
        MIRA_CHECK(stopped.shutdown(true) == executor::ShutdownResult::Completed);
        auto rejected = dispatcher.value()->execute(proposal, plain_context(), stopped, transport);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable ||
                   rejected.error().code == ErrorCode::ResourceExhausted);
        MIRA_CHECK(failed_in_tool_executor_domain(rejected));
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.submission_rejections == 1);
        MIRA_CHECK(stats.dispatched == 0);
        MIRA_CHECK(stats.in_flight == 0);
    }
    // The rejected operation id is not poisoned: the same proposal succeeds
    // on a healthy executor through the same dispatcher (reservation rollback).
    {
        executor::Executor fresh;
        MIRA_CHECK(make_executor(fresh, 1, 2));
        auto settled = dispatcher.value()->execute(proposal, plain_context(), fresh, transport);
        MIRA_CHECK(settled.has_value());
        MIRA_CHECK(!settled.value().failed);
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.dispatched == 1);
        MIRA_CHECK(stats.completed == 1);
        MIRA_CHECK(stats.submission_rejections == 1);
        MIRA_CHECK(stats.in_flight == 0);
        MIRA_CHECK(fresh.shutdown(true) == executor::ShutdownResult::Completed);
    }
    // Uninitialized executor: the pinned Executor facade (v0.5.0) accepts
    // submit_auto() on a default-constructed executor and runs the task, so
    // there is no rejection to fold on that path — the "uninitialized"
    // rejection surface of the folding contract is therefore covered by the
    // stopped path above. Pin the observable behavior with a fresh operation:
    // the call completes and the dispatcher never manufactures a rejection
    // that never happened.
    {
        executor::Executor uninit;
        const ToolProposal other =
            make_proposal(spec, json_or_abort(R"json({"x":15})json"), "call_uninit");
        auto settled = dispatcher.value()->execute(other, plain_context(), uninit, transport);
        MIRA_CHECK(settled.has_value());
        MIRA_CHECK(!settled.value().failed);
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.dispatched == 2);
        MIRA_CHECK(stats.completed == 2);
        MIRA_CHECK(stats.submission_rejections == 1);
    }
    dispatcher.value()->close(std::chrono::milliseconds{0});
    return 0;
}

int g4_transport_exception_and_error_folding() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());

    // Escaping exception -> folded into a failed record with a bounded summary.
    EchoTransport thrower(EchoTransport::Mode::Throw);
    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":3})json"));
    auto folded = dispatcher.value()->execute(proposal, plain_context(), executor, thrower);
    MIRA_CHECK(folded.has_value());
    MIRA_CHECK(folded.value().failed);
    MIRA_CHECK(!folded.value().safe_error_summary.empty());
    MIRA_CHECK(folded.value().safe_error_summary.size() <= 512 + 3);
    MIRA_CHECK(folded.value().provider_call_id.value == "call_1");
    MIRA_CHECK(folded.value().tool_id == spec.tool_id);
    MIRA_CHECK(dispatcher.value()->stats().failed_records == 1);

    // Transport-reported error -> failed record carrying the bounded message.
    EchoTransport failing(EchoTransport::Mode::Error, "server said no");
    const ToolProposal second = make_proposal(spec, json_or_abort(R"json({"x":4})json"), "call_2");
    auto failed = dispatcher.value()->execute(second, plain_context(), executor, failing);
    MIRA_CHECK(failed.has_value());
    MIRA_CHECK(failed.value().failed);
    MIRA_CHECK(failed.value().safe_error_summary == "server said no");
    MIRA_CHECK(dispatcher.value()->stats().failed_records == 2);

    // Every future was consumed by its owner: nothing pending at close.
    const McpDispatcherStats stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.in_flight == 0);
    MIRA_CHECK(stats.pending == 0);
    dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_inflight_and_immediate_cancellation() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());

    // Three-layer propagation: the transport raises the caller's cancel flag,
    // the dispatcher propagates it into the probe, the transport observes the
    // stop and returns Cancelled; execute() reports a Cancelled error.
    {
        std::atomic<bool> cancel_flag{false};
        ProbePollingTransport transport(&cancel_flag);
        const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":5})json"));
        auto outcome = dispatcher.value()->execute(proposal, cancelled_context(&cancel_flag),
                                                   executor, transport);
        MIRA_CHECK(failed_with(outcome, ErrorCode::Cancelled));
        MIRA_CHECK(failed_in_tool_executor_domain(outcome));
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.cancelled == 1);
        MIRA_CHECK(stats.in_flight == 0);
        MIRA_CHECK(stats.pending == 0);
    }
    // Pre-cancelled context: same verdict through the same surface.
    {
        ProbePollingTransport transport;
        const ToolProposal proposal =
            make_proposal(spec, json_or_abort(R"json({"x":6})json"), "call_cancel_now");
        auto outcome =
            dispatcher.value()->execute(proposal, cancelled_context(), executor, transport);
        MIRA_CHECK(failed_with(outcome, ErrorCode::Cancelled));
        MIRA_CHECK(failed_in_tool_executor_domain(outcome));
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.cancelled == 2);
        MIRA_CHECK(stats.in_flight == 0);
        MIRA_CHECK(stats.pending == 0);
    }
    dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_deadline_and_close_drain() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    McpDispatchLimits limits = fast_limits();
    limits.max_invocation_duration = std::chrono::milliseconds{50};
    limits.cancellation_grace = std::chrono::milliseconds{100};
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), limits);
    MIRA_CHECK(dispatcher.has_value());

    // Defective transport ignores the probe and sleeps past the deadline:
    // execute() abandons the wait after the grace and returns the deadline
    // failed record; the abandoned future stays pending for close().
    SleepTransport transport(std::chrono::milliseconds{300});
    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":7})json"));
    auto outcome = dispatcher.value()->execute(proposal, plain_context(), executor, transport);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().failed);
    MIRA_CHECK(outcome.value().safe_error_summary.find("invocation exceeded its deadline") !=
               std::string::npos);
    MIRA_CHECK(outcome.value().provider_call_id.value == "call_1");

    McpDispatcherStats stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.deadline_exceeded == 1);
    MIRA_CHECK(stats.in_flight == 1); // abandoned slot retained for the drain
    MIRA_CHECK(stats.pending == 1);

    // close() drains the abandoned future within its budget.
    const McpDispatcherCloseReport report =
        dispatcher.value()->close(std::chrono::milliseconds{2'000});
    MIRA_CHECK(report.drained >= 1);
    MIRA_CHECK(report.still_pending == 0);
    stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.pending_drained >= 1);
    MIRA_CHECK(stats.in_flight == 0);
    MIRA_CHECK(stats.pending == 0);
    MIRA_CHECK(stats.completed == 0);

    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_cancel_grace_exhaustion_and_close_drain() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    McpDispatchLimits limits = fast_limits();
    limits.cancellation_grace = std::chrono::milliseconds{100};
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), limits);
    MIRA_CHECK(dispatcher.has_value());

    // Cancelled caller + transport that never observes the probe: the grace
    // expires, execute() returns Cancelled, close() drains the leftover.
    SleepTransport transport(std::chrono::milliseconds{300});
    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":8})json"));
    auto outcome = dispatcher.value()->execute(proposal, cancelled_context(), executor, transport);
    MIRA_CHECK(failed_with(outcome, ErrorCode::Cancelled));
    MIRA_CHECK(failed_in_tool_executor_domain(outcome));

    McpDispatcherStats stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.cancelled == 1);
    MIRA_CHECK(stats.in_flight == 1);
    MIRA_CHECK(stats.pending == 1);

    const McpDispatcherCloseReport report =
        dispatcher.value()->close(std::chrono::milliseconds{2'000});
    MIRA_CHECK(report.drained >= 1);
    MIRA_CHECK(report.still_pending == 0);
    stats = dispatcher.value()->stats();
    MIRA_CHECK(stats.pending_drained >= 1);
    MIRA_CHECK(stats.in_flight == 0);

    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_concurrency_cap_and_oversize_result() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 2, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());

    // Aggregate concurrency cap of 1: while one invocation holds the slot the
    // second is explicitly rejected (no queueing).
    {
        std::atomic<bool> release{false};
        GateTransport transport(release);
        const ToolProposal first =
            make_proposal(spec, json_or_abort(R"json({"x":9})json"), "call_a");
        const OperationContext context = plain_context();
        auto holder = executor.submit([&dispatcher, &first, &context, &executor, &transport] {
            return dispatcher.value()->execute(first, context, executor, transport);
        });
        bool slot_observed = false;
        for (int poll = 0; poll < 2000; ++poll) {
            if (dispatcher.value()->stats().in_flight == 1) {
                slot_observed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        MIRA_CHECK(slot_observed);
        const ToolProposal second =
            make_proposal(spec, json_or_abort(R"json({"x":10})json"), "call_b");
        auto rejected = dispatcher.value()->execute(second, plain_context(), executor, transport);
        MIRA_CHECK(failed_with(rejected, ErrorCode::ResourceExhausted));
        MIRA_CHECK(failed_in_tool_executor_domain(rejected));
        MIRA_CHECK(dispatcher.value()->stats().concurrency_rejections == 1);

        release.store(true, std::memory_order_release);
        auto settled = holder.get(); // consume the holder future
        MIRA_CHECK(settled.has_value());
        MIRA_CHECK(!settled.value().failed);
        const McpDispatcherStats stats = dispatcher.value()->stats();
        MIRA_CHECK(stats.dispatched == 1);
        MIRA_CHECK(stats.completed == 1);
        MIRA_CHECK(stats.in_flight == 0);
    }
    // Result over the per-invocation budget -> failed record, counted.
    {
        McpDispatchLimits tight = fast_limits();
        tight.max_result_bytes = 64;
        auto bounded = McpToolDispatcher::make(single_exposure(spec), tight);
        MIRA_CHECK(bounded.has_value());
        JsonValue::Object payload;
        payload.emplace_back("blob", std::string(200, 'y'));
        EchoTransport big(EchoTransport::Mode::Value, {}, JsonValue(std::move(payload)));
        const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":11})json"));
        auto outcome = bounded.value()->execute(proposal, plain_context(), executor, big);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().failed);
        MIRA_CHECK(outcome.value().safe_error_summary.find("exceeds the size limit") !=
                   std::string::npos);
        const McpDispatcherStats stats = bounded.value()->stats();
        MIRA_CHECK(stats.oversize_results == 1);
        MIRA_CHECK(stats.failed_records == 1);
        MIRA_CHECK(stats.completed == 0);
        MIRA_CHECK(stats.in_flight == 0);
        bounded.value()->close(std::chrono::milliseconds{0});
    }
    dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(dispatcher.value()->stats().in_flight == 0);
    MIRA_CHECK(dispatcher.value()->stats().pending == 0);
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int g4_submit_and_consume_folding() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    EchoTransport transport;
    const JsonValue arguments = json_or_abort(R"json({"x":12})json");

    // Normal round trip: the future resolves to the transport's value.
    auto submitted =
        submit_mcp_invocation(executor, transport, "cmp.echo", arguments, McpInvocationProbe{});
    MIRA_CHECK(submitted.has_value());
    auto consumed = consume_mcp_invocation(std::move(submitted.value()));
    MIRA_CHECK(consumed.has_value());
    const auto *echo = consumed.value().find("echo");
    MIRA_CHECK(echo != nullptr && echo->is_object());

    // Throwing transport folds into an Internal error result, never escapes.
    EchoTransport thrower(EchoTransport::Mode::Throw);
    auto thrown =
        submit_mcp_invocation(executor, thrower, "cmp.echo", arguments, McpInvocationProbe{});
    MIRA_CHECK(thrown.has_value());
    auto folded = consume_mcp_invocation(std::move(thrown.value()));
    MIRA_CHECK(failed_with(folded, ErrorCode::Internal));
    MIRA_CHECK(failed_in_tool_executor_domain(folded));

    // Stopped executor: synchronous rejection, no future escapes.
    {
        executor::Executor stopped;
        MIRA_CHECK(make_executor(stopped, 1, 1));
        MIRA_CHECK(stopped.shutdown(true) == executor::ShutdownResult::Completed);
        auto rejected =
            submit_mcp_invocation(stopped, transport, "cmp.echo", arguments, McpInvocationProbe{});
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable ||
                   rejected.error().code == ErrorCode::ResourceExhausted);
        MIRA_CHECK(failed_in_tool_executor_domain(rejected));
    }
    // Admission rejection delivered through a ready-with-exception future is
    // folded by consume_mcp_invocation() into ResourceExhausted.
    {
        executor::Executor bounded;
        executor::ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.queue_capacity = 4;
        config.max_in_flight_tasks = 1;
        MIRA_CHECK(bounded.initialize(config));

        std::promise<void> gate;
        auto gate_future = gate.get_future().share();
        auto blocker = bounded.submit([gate_future] { gate_future.wait(); });

        auto rejected =
            submit_mcp_invocation(bounded, transport, "cmp.echo", arguments, McpInvocationProbe{});
        if (rejected.has_value()) {
            auto folded_rejection = consume_mcp_invocation(std::move(rejected.value()));
            MIRA_CHECK(failed_with(folded_rejection, ErrorCode::ResourceExhausted));
            MIRA_CHECK(failed_in_tool_executor_domain(folded_rejection));
        } else {
            MIRA_CHECK(rejected.error().code == ErrorCode::ResourceExhausted);
        }
        gate.set_value();
        blocker.get();
        MIRA_CHECK(bounded.shutdown(true) == executor::ShutdownResult::Completed);
    }
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// G5: untrusted data discipline and redaction (M7-MCP-G5)
// ---------------------------------------------------------------------------

int g5_description_bound_rejects_whole_group() {
    ToolModuleLimits tight;
    tight.max_description_bytes = 32;
    const McpAdmissionOptions options = admission_options();

    // At the limit: accepted, stored verbatim.
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.exact", std::string(32, 'd')));
        auto result =
            convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core(), tight);
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().tools[0].description.size() == 32);
    }
    // One byte over on one member rejects the whole group (no silent
    // truncation into the manifest, co-member included).
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.fine", "short and fine"));
        listing.tools.push_back(descriptor("mcp.huge", std::string(33, 'd')));
        auto result =
            convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core(), tight);
        MIRA_CHECK(failed_in_tool_module_domain(result));
    }
    // Same listing under the default limits is accepted (proves the rejection
    // is the bound, not the content).
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.fine", "short and fine"));
        listing.tools.push_back(descriptor("mcp.huge", std::string(33, 'd')));
        auto result = convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().tools.size() == 2);
    }
    return 0;
}

int g5_untrusted_projection_and_result_authority() {
    const std::string marker_description = "UNTRUSTED-DESC-4b9c02-marker";
    const std::string marker_signature = "UNTRUSTED-SIG-8e17f3-material";

    // Admission projection never carries description or signature material.
    {
        McpServerListing listing;
        listing.tools.push_back(descriptor("mcp.marked", marker_description));
        McpAdmissionOptions options = admission_options();
        options.signature = marker_signature;
        auto converted = convert_mcp_listing_to_module(listing, options, CapabilityCatalog::core());
        MIRA_CHECK(converted.has_value());
        const std::string projection =
            canonical_json_string(mcp_admission_to_json(converted.value()));
        MIRA_CHECK(projection.find(marker_description) == std::string::npos);
        MIRA_CHECK(projection.find(marker_signature) == std::string::npos);
        MIRA_CHECK(projection.find("mcp.marked") != std::string::npos);
    }

    // Transport results ride back only as tool-output payloads: the wire items
    // are function_call_output / tool-role messages and never carry
    // System/Developer authority semantics.
    const std::string marker_payload = "UNTRUSTED-SERVER-PAYLOAD-<script>alert(1)</script>";
    {
        ToolExecutionRecord record;
        record.provider_call_id = ProviderToolCallId{"call_untrusted"};
        record.tool_id = ToolId::generate();
        JsonValue::Object payload;
        payload.emplace_back("server_text", marker_payload);
        record.result = JsonValue(std::move(payload));

        const auto responses =
            build_tool_result_input(ProtocolDialect::OpenAIResponsesV1, std::span{&record, 1});
        MIRA_CHECK(responses.has_value());
        MIRA_CHECK(responses.value().size() == 1);
        const JsonValue &item = responses.value()[0];
        const auto *type = item.find("type");
        MIRA_CHECK(type != nullptr && type->as_string() != nullptr &&
                   *type->as_string() == "function_call_output");
        MIRA_CHECK(item.find("role") == nullptr);
        MIRA_CHECK(item.find("authority") == nullptr);
        const auto *output = item.find("output");
        MIRA_CHECK(output != nullptr && output->as_string() != nullptr);
        MIRA_CHECK(output->as_string()->find(marker_payload) != std::string::npos);

        const auto chat = build_tool_result_input(ProtocolDialect::OpenAIChatCompletionsV1,
                                                  std::span{&record, 1});
        MIRA_CHECK(chat.has_value());
        MIRA_CHECK(chat.value().size() == 1);
        const JsonValue &message = chat.value()[0];
        const auto *role = message.find("role");
        MIRA_CHECK(role != nullptr && role->as_string() != nullptr &&
                   *role->as_string() == "tool"); // never system/developer
        MIRA_CHECK(message.find("authority") == nullptr);
        const auto *content = message.find("content");
        MIRA_CHECK(content != nullptr && content->as_string() != nullptr);
        MIRA_CHECK(content->as_string()->find(marker_payload) != std::string::npos);
    }
    // A failed record feeds back as an error output with the bounded summary.
    {
        ToolExecutionRecord failed;
        failed.provider_call_id = ProviderToolCallId{"call_failed"};
        failed.failed = true;
        failed.safe_error_summary = "bounded reason";
        const auto items =
            build_tool_result_input(ProtocolDialect::OpenAIResponsesV1, std::span{&failed, 1});
        MIRA_CHECK(items.has_value());
        const auto *output = items.value()[0].find("output");
        MIRA_CHECK(output != nullptr && output->as_string() != nullptr);
        auto parsed = parse_json(*output->as_string());
        MIRA_CHECK(parsed.has_value());
        const auto *status = parsed.value().find("status");
        MIRA_CHECK(status != nullptr && status->as_string() != nullptr &&
                   *status->as_string() == "error");
        const auto *error = parsed.value().find("error");
        MIRA_CHECK(error != nullptr && error->as_string() != nullptr &&
                   *error->as_string() == "bounded reason");
    }
    return 0;
}

int g5_error_summary_bounded() {
    executor::Executor executor;
    MIRA_CHECK(make_executor(executor, 1, 2));
    const ExposedToolSpec spec = echo_spec(ToolId::generate());
    auto dispatcher = McpToolDispatcher::make(single_exposure(spec), fast_limits());
    MIRA_CHECK(dispatcher.has_value());

    // A transport error with an oversized message is truncated to the frozen
    // 512+3 bound before it can reach the model.
    const std::string huge(2'000, 'e');
    EchoTransport failing(EchoTransport::Mode::Error, huge);
    const ToolProposal proposal = make_proposal(spec, json_or_abort(R"json({"x":13})json"));
    auto outcome = dispatcher.value()->execute(proposal, plain_context(), executor, failing);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().failed);
    MIRA_CHECK(outcome.value().safe_error_summary.size() <= 512 + 3);
    MIRA_CHECK(outcome.value().safe_error_summary.size() >= 512); // truncated, not dropped
    MIRA_CHECK(outcome.value().safe_error_summary.compare(512, 3, "...") == 0);

    // The dispatcher's own deadline failure text is fixed and bounded.
    McpDispatchLimits limits = fast_limits();
    limits.max_invocation_duration = std::chrono::milliseconds{50};
    limits.cancellation_grace = std::chrono::milliseconds{50};
    auto short_dispatcher = McpToolDispatcher::make(single_exposure(spec), limits);
    MIRA_CHECK(short_dispatcher.has_value());
    SleepTransport sleeper(std::chrono::milliseconds{300});
    const ToolProposal second =
        make_proposal(spec, json_or_abort(R"json({"x":14})json"), "call_deadline");
    auto deadline = short_dispatcher.value()->execute(second, plain_context(), executor, sleeper);
    MIRA_CHECK(deadline.has_value());
    MIRA_CHECK(deadline.value().failed);
    MIRA_CHECK(deadline.value().safe_error_summary == "invocation exceeded its deadline");
    const McpDispatcherCloseReport report =
        short_dispatcher.value()->close(std::chrono::milliseconds{2'000});
    MIRA_CHECK(report.still_pending == 0);

    dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// G6: consumer closure (M7-MCP-G6)
// ---------------------------------------------------------------------------

int g6_public_surface_closure() {
    // The header closes over its own vocabulary from a standalone consumer:
    // constants, pure functions, default construction and the dispatcher
    // factory are usable without any MCP protocol dependency.
    MIRA_CHECK(kMcpAdmissionSchema == "mira.tool_module.mcp.admission.v1");

    const McpSessionPlan plan =
        plan_mcp_session_action(McpSessionEvent::ServerConnected, nullptr, false, false);
    MIRA_CHECK(plan.action == McpAdmissionAction::AdmitModule);

    McpAdmissionLimits admission_limits;
    admission_limits.max_descriptors = 4;
    admission_limits.max_risk_overrides = 4;
    McpAdmissionOptions options = admission_options("consumer.mcp.sample");
    auto converted = convert_mcp_listing_to_module(sample_listing(), options,
                                                   CapabilityCatalog::core(), {}, admission_limits);
    MIRA_CHECK(converted.has_value());
    const JsonValue projection = mcp_admission_to_json(converted.value());
    MIRA_CHECK(projection.is_object());

    McpDispatchLimits dispatch_limits = fast_limits();
    auto dispatcher = McpToolDispatcher::make(ToolExposure{}, dispatch_limits);
    MIRA_CHECK(dispatcher.has_value());
    MIRA_CHECK(dispatcher.value()->stats().in_flight == 0);
    const McpDispatcherCloseReport report = dispatcher.value()->close(std::chrono::milliseconds{0});
    MIRA_CHECK(report.still_pending == 0);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report [path]: canonical report of the fixed scenarios for
    // cross-process byte comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const std::string report = build_mcp_report();
        if (argc > 2) {
            std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "cannot open report path: " << argv[2] << '\n';
                return 2;
            }
            out << report << '\n';
            return out.good() ? 0 : 2;
        }
        std::cout << report << '\n';
        return 0;
    }

    struct Gate {
        const char *name;
        int (*run)();
    };
    const Gate gates[] = {
        {"G1 hint risk mapping", g1_hint_risk_mapping},
        {"G1 conversion positive matrix", g1_conversion_positive_matrix},
        {"G1 conversion negative matrix", g1_conversion_negative_matrix},
        {"G1 empty listing and determinism", g1_empty_listing_and_determinism},
        {"G1 admission projection shape and redaction",
         g1_admission_projection_shape_and_redaction},
        {"G1 report determinism", g1_report_is_deterministic},
        {"G2 policy matrix pure", g2_policy_matrix_pure},
        {"G2 admit full chain", g2_admit_full_chain},
        {"G2 runtime downgrade and pinned settlement", g2_runtime_downgrade_and_pinned_settlement},
        {"G2 window duplicate and close semantics", g2_window_duplicate_and_close_semantics},
        {"G2 lifecycle events redacted", g2_lifecycle_events_redacted},
        {"G3 gate parity with builtin", g3_gate_parity_with_builtin},
        {"G4 probe and make limits validation", g4_probe_and_make_limits_validation},
        {"G4 normal completion and stats", g4_normal_completion_and_stats},
        {"G4 submission rejection and rollback retry", g4_submission_rejection_and_rollback_retry},
        {"G4 transport exception and error folding", g4_transport_exception_and_error_folding},
        {"G4 inflight and immediate cancellation", g4_inflight_and_immediate_cancellation},
        {"G4 deadline and close drain", g4_deadline_and_close_drain},
        {"G4 cancel grace exhaustion and close drain", g4_cancel_grace_exhaustion_and_close_drain},
        {"G4 concurrency cap and oversize result", g4_concurrency_cap_and_oversize_result},
        {"G4 submit and consume folding", g4_submit_and_consume_folding},
        {"G5 description bound rejects whole group", g5_description_bound_rejects_whole_group},
        {"G5 untrusted projection and result authority",
         g5_untrusted_projection_and_result_authority},
        {"G5 error summary bounded", g5_error_summary_bounded},
        {"G6 public surface closure", g6_public_surface_closure},
    };

    std::cout << "M7 MCP tool module admission verification (" << sizeof(gates) / sizeof(gates[0])
              << " gates)\n";
    for (const Gate &gate : gates) {
        if (gate.run() != 0) {
            std::cerr << "FAILED gate: " << gate.name << '\n';
            return 1;
        }
        std::cout << "[ok] " << gate.name << '\n';
    }
    std::cout << "all gates passed\n";
    return 0;
}
