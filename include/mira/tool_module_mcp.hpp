#pragma once

#include <mira/environment.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_tool.hpp>
#include <mira/security.hpp>
#include <mira/tool_module.hpp>
#include <mira/tool_module_exposure.hpp>
#include <mira/tool_module_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// ---------------------------------------------------------------------------
// MCP tool module admission (M7 MCP stage; DEC-039, DEC-009, DEC-015, DEC-042).
//
// This header freezes the Core-side contracts of MCP admission
// (docs/design/mcp_tool_admission_design.md): the controlled subset of an MCP
// server listing, the deterministic descriptor -> out-of-process ToolModule
// conversion (routed through the real manifest parser so every TM0 gate stays
// in force), the session-lifecycle mapping that keeps runtime transitions
// downgrade-only, and the execution adapter that runs registered MCP tools
// through the same DEC-015 admission discipline as BuiltIn tools.
//
// Boundary: Core never speaks the MCP protocol. The host adapter owns the
// client I/O, transport selection and server process lifecycle; Core consumes
// structured descriptors and structured results. Descriptions and results are
// untrusted server data (RULE-09): bounded at conversion, excluded from
// redaction-sensitive projections, and never carrying prompt authority.
//
// Threading: conversion, the session policy and McpModuleAdmission are
// serial-control-plane components (like ModuleRegistry) — pure computation or
// registry bookkeeping, no threads, no I/O, no clock reads. McpToolDispatcher
// is called on Executor-routed workers; its bookkeeping is mutex-guarded and
// the out-of-process call itself is submitted through submit_auto() with a
// future that is always consumed (immediately, or by the bounded close drain).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Listing subset and conversion (design §4/§5)
// ---------------------------------------------------------------------------

// One MCP tool as summarized by the host from tools/list. Only the fields
// Core consumes; everything else stays host-side. `name` must satisfy the
// governed member charset, `description` is untrusted server text bounded at
// conversion, and `input_schema` must pass the same JSON Schema subset gate
// as resolve_tool_calls.
struct McpToolDescriptor final {
    std::string name;
    std::string description;
    JsonSchema input_schema;
    bool read_only_hint = false;   // MCP annotations.readOnlyHint
    bool destructive_hint = false; // MCP annotations.destructiveHint
};

struct McpServerListing final {
    // An empty listing fails closed through the real parser (the frozen
    // manifest contract requires 1..256 member tools); hosts skip admission
    // for servers that expose no tools.
    std::vector<McpToolDescriptor> tools;
};

// Deploy-time admission options supplied by the host pipeline; never
// model-controllable. The signer triple is the OutOfProcess trust material
// verified by TM1 verify_module_trust against the registry's ModuleTrustConfig.
struct McpAdmissionOptions final {
    std::string module_id;
    SemanticVersion module_version{1, 0, 0};
    std::string signer;
    std::string signature_algorithm;
    std::string signature;
    // Host-mapped capability requirements (MCP descriptors carry none).
    std::vector<CapabilityId> required_capabilities;
    std::vector<std::string> conflicts_with;
    // Explicit bounded aggregate budgets (zero values fail the parser).
    ModuleResourceLimits resources;
    // Per-member risk overrides: may only RAISE the tier derived from the
    // hints; any entry that would lower it fails the whole conversion.
    std::vector<std::pair<std::string, ActionRisk>> risk_overrides;
};

// Conversion-side bounds (RULE-08) on top of the manifest limits.
struct McpAdmissionLimits final {
    std::size_t max_descriptors = 256;
    std::size_t max_risk_overrides = 256;
};

inline constexpr McpAdmissionLimits kDefaultMcpAdmissionLimits{};

// Deterministic hint -> ActionRisk mapping (design §5.2): readOnlyHint ->
// read_only, destructiveHint -> critical, default -> user_visible. Server
// hints are untrusted self-reports; the conservative default and the
// raise-only override rule are the safety margin.
[[nodiscard]] ActionRisk mcp_hint_risk(const McpToolDescriptor &descriptor);

// Converts one listing into an origin out_of_process ToolModuleManifest by
// building the manifest JSON and running it through the REAL
// parse_tool_module_manifest — the whole TM0 validation matrix (member
// charset, in-module uniqueness, schema subset, capability catalog, resource
// bounds, ABI) applies unchanged. Fail-closed pre-checks specific to MCP:
// non-empty descriptions, object input schemas, listing/override bounds,
// override targets that exist, raise-only overrides, and a complete signer
// triple. Any failure rejects the whole listing with one error; no partial
// manifest escapes. Pure: no I/O, no clock, no randomness — the same inputs
// always produce the same manifest and digest across processes.
[[nodiscard]] Result<ToolModuleManifest>
convert_mcp_listing_to_module(const McpServerListing &listing, const McpAdmissionOptions &options,
                              const CapabilityCatalog &catalog,
                              const ToolModuleLimits &limits = kDefaultToolModuleLimits,
                              const McpAdmissionLimits &mcp_limits = kDefaultMcpAdmissionLimits);

// Versioned admission projection (additive-evolution, DEC-002): module
// identity, version, manifest digest and per-member (name, side effect,
// arguments-schema digest). Redaction-safe by construction — it never carries
// description text or signature material. The determinism anchor for events,
// replay and cross-process gates.
inline constexpr std::string_view kMcpAdmissionSchema = "mira.tool_module.mcp.admission.v1";

[[nodiscard]] JsonValue mcp_admission_to_json(const ToolModuleManifest &manifest);

// ---------------------------------------------------------------------------
// Session lifecycle mapping (design §6)
// ---------------------------------------------------------------------------

enum class McpSessionEvent : std::uint8_t { ServerConnected, ServerDisconnected, ToolListChanged };

[[nodiscard]] std::string_view mcp_session_event_name(McpSessionEvent event);

enum class McpAdmissionAction : std::uint8_t {
    AdmitModule,  // deploy window: convert + trust + register via admit_server
    RevokeModule, // runtime downgrade-only path
    RejectEvent,  // forbidden transition (sealed/closed window, duplicate admit)
    NoAction      // event already reflected (terminal/unknown module)
};

[[nodiscard]] std::string_view mcp_admission_action_name(McpAdmissionAction action);

// The planned reaction to one session event. `reason` is bounded and
// sanitized; it is the revoke reason and rejection diagnostic.
struct McpSessionPlan final {
    McpAdmissionAction action = McpAdmissionAction::NoAction;
    std::string reason;
};

// Pure policy (design §6 matrix): server connection admits only inside the
// open registration window and only once per module_id; disconnects and tool
// list changes map registered, non-terminal modules to revoke — runtime
// transitions never expand the exposure. `record` is the registry's current
// record for the module (null when unregistered).
[[nodiscard]] McpSessionPlan plan_mcp_session_action(McpSessionEvent event,
                                                     const ModuleRecord *record,
                                                     bool registry_sealed, bool registry_closed);

// Serial-plane helper binding one admitted MCP module to a registry.
// admit_server runs the frozen deploy-window path (convert -> policy check ->
// ModuleRegistry::register_module, with TM1 origin trust verified inside the
// registry); promotion to Staged/Active stays an explicit host action on the
// registry. on_session_event applies the downgrade-only matrix; an
// AdmitModule plan is an explicit error there because events carry no listing.
class McpModuleAdmission final {
  public:
    McpModuleAdmission(CapabilityCatalog catalog, ModuleRegistry &registry);

    McpModuleAdmission(const McpModuleAdmission &) = delete;
    McpModuleAdmission &operator=(const McpModuleAdmission &) = delete;

    // Returns the canonical manifest digest on success. Fails closed when the
    // window is closed, the module_id is already admitted (or the component
    // is bound to a different one), or conversion/registration rejects.
    [[nodiscard]] Result<Hash>
    admit_server(const McpServerListing &listing, const McpAdmissionOptions &options,
                 const ToolModuleLimits &limits = kDefaultToolModuleLimits,
                 const McpAdmissionLimits &mcp_limits = kDefaultMcpAdmissionLimits);

    // Applies the policy matrix for the bound module; RevokeModule performs
    // the registry downgrade with a bounded sanitized reason.
    [[nodiscard]] Result<McpSessionPlan> on_session_event(McpSessionEvent event);

    [[nodiscard]] const std::string &module_id() const noexcept { return module_id_; }

  private:
    CapabilityCatalog catalog_;
    ModuleRegistry &registry_;
    std::string module_id_; // empty until an admission succeeds
};

// ---------------------------------------------------------------------------
// Execution adapter (design §7)
// ---------------------------------------------------------------------------

// Cooperative cancellation probe handed to transports. Built by the
// dispatcher; implementations must poll stop_requested()/deadline_expired()
// between blocking slices and return Cancelled promptly once it fires. Core
// never preempts — deadline and cancellation are cooperative.
class McpInvocationProbe final {
  public:
    McpInvocationProbe() noexcept = default;

    // flag and deadline are owned by the dispatcher's invocation slot.
    explicit McpInvocationProbe(std::shared_ptr<const std::atomic<bool>> stop_flag,
                                std::optional<std::chrono::steady_clock::time_point> deadline)
        : flag_(std::move(stop_flag)), deadline_(deadline) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool deadline_expired() const noexcept {
        return deadline_.has_value() && std::chrono::steady_clock::now() >= *deadline_;
    }

  private:
    std::shared_ptr<const std::atomic<bool>> flag_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

// Host-implemented boundary for the out-of-process call: the actual MCP
// client I/O, serialization and server-side timeout. Transport selection
// never enters Core contracts. Implementations must be safe for concurrent
// use, must poll the probe between blocking slices, and must not throw
// (escaping exceptions are folded into explicit failed results by Core).
class IMcpToolTransport {
  public:
    virtual ~IMcpToolTransport() = default;

    [[nodiscard]] virtual Result<JsonValue> invoke(const std::string &wire_name,
                                                   const JsonValue &arguments,
                                                   const McpInvocationProbe &probe) = 0;
};

// Dispatcher bounds (RULE-08). Every invocation is bounded even when the
// caller context carries no deadline; the concurrency and result budgets come
// from the module's declared aggregate resources.
struct McpDispatchLimits final {
    std::uint32_t max_concurrent_invocations = 1; // >= 1
    std::uint64_t max_result_bytes = 64 * 1024;   // >= 1, per invocation
    std::chrono::milliseconds max_invocation_duration{30'000};
    // Bounded grace after the probe fires before the wait is abandoned; the
    // abandoned future is retained and drained by close().
    std::chrono::milliseconds cancellation_grace{2'000};
    std::size_t max_pending_invocations = 64; // >= 1, abandoned-wait backlog
    std::chrono::milliseconds default_close_drain_budget{10'000};
};

struct McpDispatcherStats final {
    std::uint64_t dispatched = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed_records = 0; // model-attributable failures
    std::uint64_t cancelled = 0;
    std::uint64_t deadline_exceeded = 0;
    std::uint64_t identity_rejections = 0; // unknown tool, mismatch, dup op
    std::uint64_t concurrency_rejections = 0;
    std::uint64_t submission_rejections = 0;
    std::uint64_t oversize_results = 0;
    std::uint64_t closed_rejections = 0;
    std::uint64_t pending_drained = 0;   // abandoned futures resolved in close
    std::uint64_t pending_abandoned = 0; // still unresolved after the budget
    std::size_t in_flight = 0;
    std::size_t pending = 0;
    bool closed = false;
};

struct McpDispatcherCloseReport final {
    std::uint64_t drained = 0;
    std::uint64_t abandoned = 0;
    std::size_t still_pending = 0;
};

// Executes resolved proposals against one pinned TM2 exposure with the same
// admission discipline as BuiltinToolRegistry (DEC-015): exposure identity
// match (tool_id/wire_name/version/side effects), at-most-once dispatch per
// OperationId (reserved before submission, rolled back only on a definite
// non-dispatch), local argument schema validation with model-attributable
// failures returned as failed ToolExecutionRecords, and cancellation mapped
// to Cancelled. System conditions (closed dispatcher, identity mismatch,
// duplicate operation, concurrency cap, submission rejection) return error
// results that never feed the model. The transport call itself is submitted
// through submit_mcp_invocation(); the caller's worker waits bounded,
// propagates cancellation into the probe, and abandons the wait after the
// grace period — the retained future is drained by close().
class McpToolDispatcher final {
  public:
    // Fails closed on an invalid limits set (zero concurrency/result/pending
    // or non-positive durations).
    [[nodiscard]] static Result<std::unique_ptr<McpToolDispatcher>>
    make(ToolExposure exposure, McpDispatchLimits limits = {});

    ~McpToolDispatcher();

    McpToolDispatcher(const McpToolDispatcher &) = delete;
    McpToolDispatcher &operator=(const McpToolDispatcher &) = delete;

    [[nodiscard]] Result<ToolExecutionRecord> execute(const ToolProposal &proposal,
                                                      const OperationContext &context,
                                                      executor::Executor &executor,
                                                      IMcpToolTransport &transport);

    // Terminal close: rejects new dispatches, signals every abandoned wait,
    // drains pending futures within the budget and reports the outcome.
    // Idempotent; the destructor closes with the default budget if needed.
    McpDispatcherCloseReport close(std::chrono::milliseconds drain_budget);

    [[nodiscard]] bool closed() const;
    [[nodiscard]] McpDispatcherStats stats() const;

  private:
    McpToolDispatcher(ToolExposure exposure, McpDispatchLimits limits);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Executor-routed invocation (design §7.3; same pattern as TM1 verification)
// ---------------------------------------------------------------------------

// Routes one transport invocation onto the Executor as a bounded finite task
// (submit_auto()). The transport is captured by reference and must outlive
// the future. Synchronous submission rejections (stopping, capacity
// exhausted, uninitialized submission path) return an error result instead of
// a future; admission rejections delivered through the future never escape —
// consume it through consume_mcp_invocation(), which folds every outcome into
// one Result. Futures must be consumed; nothing is silently dropped.
[[nodiscard]] Result<std::future<Result<JsonValue>>>
submit_mcp_invocation(executor::Executor &executor, IMcpToolTransport &transport,
                      std::string wire_name, JsonValue arguments, McpInvocationProbe probe);

// The single well-defined consumption point for an invocation future: the
// task's Result, or an explicit error for submission-rejection exceptions the
// Executor delivered. Never throws, never swallows.
[[nodiscard]] Result<JsonValue> consume_mcp_invocation(std::future<Result<JsonValue>> future);

} // namespace mira
