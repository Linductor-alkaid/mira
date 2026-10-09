#pragma once

#include <mira/agent_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/tool_executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// MIRA-20261004-001 (issue #73): bounds for the no-observation conversational
// loop. Same RULE-08 shape as AgentLoopConfig — every quantity is a declared
// bound, never an implicit one. The derived whole-run envelope
// `max_output_tokens_per_turn * (max_turns + max_recoveries + 1)` must not
// exceed the target profile's `capabilities.limits.max_output_tokens` (the
// router rejects larger task budgets); the defaults below fit the default
// profile limit (16384).
struct ConversationLoopConfig final {
    // Whole-run bound on model turns; exhausting it settles as MaxTurns.
    std::uint32_t max_turns = 12;
    // Bounded retries for recoverable model failures per run.
    std::uint32_t max_recoveries = 2;
    std::chrono::milliseconds model_call_deadline{30'000};
    // Whole-run budget for tool executions.
    std::uint32_t max_tool_executions = 32;
    // Per-turn generation bound.
    std::uint64_t max_output_tokens_per_turn = 1024;
    std::optional<ReasoningEffort> reasoning_effort{};
    std::optional<ThinkingMode> thinking{};
    InferOptions inference{};
};

enum class ConversationOutcome : std::uint8_t {
    Answered,  // The model produced a terminal text answer.
    Failed,    // Non-recoverable failure, refusal or filtering.
    Cancelled, // Cancellation or admission rejection.
    MaxTurns,  // Turn budget exhausted before a terminal answer.
};

[[nodiscard]] std::string conversation_outcome_name(ConversationOutcome outcome);

struct ConversationTurnRecord final {
    std::uint32_t turn = 0;
    ModelRequestId model_request;
    std::string summary; // "tool:<wire_name>[;...]" or "answer".
    std::string note;
};

struct ConversationLoopResult final {
    ConversationOutcome outcome = ConversationOutcome::Failed;
    std::vector<ConversationTurnRecord> turns;
    // Terminal assistant text; empty unless the outcome is Answered.
    std::string answer;
    std::uint32_t recoveries = 0;
    std::string safe_summary;
    ModelUsage last_usage{};
    std::uint32_t tool_executions = 0;
    // Supplied-input seam totals across the run (DEC-053): items accepted into
    // requests, items dropped by validation or bounds, and supplier failures
    // degraded to diagnostics. All zero without a supplier.
    std::uint32_t supplied_input_items = 0;
    std::uint32_t supplied_input_dropped = 0;
    std::uint32_t supply_degradations = 0;
};

// Declared per-request bounds (RULE-08) for the supplied-input seam. Items
// beyond a bound are dropped in supply order and counted on the event surface
// and in ConversationLoopResult — never silently.
struct ModelInputSupplyOptions final {
    // Max supplied items folded into one request.
    std::size_t max_items = 8;
    // Max summed ImagePart artifact bytes folded into one request; an item
    // whose images would push the total over the bound is dropped whole.
    std::uint64_t max_image_bytes = 8ULL * 1024ULL * 1024ULL;

    [[nodiscard]] Result<void> validate() const;
};

// Host-controlled per-request structured input supply (DEC-053, issue #85 /
// MIRA-20261009-001): lets a host tool hand images (e.g. a screenshot artifact)
// to the next model turn as canonical ImageParts. Resolved exactly once per
// assembled request, after tool rounds complete and before the model call; an
// empty vector is the normal no-injection state. An Error return is a supply
// failure that degrades to a visible diagnostic instead of blocking the loop.
// Identity gating (task / session / epoch) closes inside the callback, which
// receives the running spec. Called synchronously on the run() caller's worker:
// keep it bounded and free of blocking work, like WorkingContextSupplier.
using ModelInputSupplier =
    std::function<Result<std::vector<ModelInputItem>>(const AgentLoopSpec &)>;

// The no-observation conversational loop (DEC-047): model -> tool proposal ->
// execute -> result -> model, driven over the same ModelGateway, admission and
// budget contracts as the device agent loop. There is no environment, no
// observation and no device verification: a plain text answer is the terminal
// state. Tool rounds ride the canonical ToolCallPart/ToolResultPart items, so
// providers see native function_call/function_call_output semantics instead of
// flattened JSON text.
//
// Like AgentLoop, each turn is a bounded work unit; cancellation, admission
// rejection and terminal answers stop the loop before any new tool executes.
// The loop owns no threads and no queues — hosts run it on an Executor worker
// exactly like AgentLoop::run().
class ConversationLoop final {
  public:
    ConversationLoop(ModelGateway &gateway,
                     ConversationLoopConfig config = ConversationLoopConfig{});

    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime, SessionId session);
    void set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools);

    // Attaches the optional supplied-input seam (DEC-053) and its per-request
    // bounds. Without a supplier, request assembly stays unchanged byte for
    // byte. With one, each assembled request resolves the callback exactly
    // once and folds the surviving content parts into that request's primary
    // user item under the loop's own envelope (User role, request.v1
    // provenance, Internal authority — host input is never elevated);
    // supply attribution rides the ModelInputSupplied event and the result
    // counters. The run's tool-history replay never copies supplied content,
    // so nothing accumulates across turns (hosts re-offer per turn if
    // wanted). Only TextPart/ImagePart/FilePart are accepted; items that fail
    // validation or bounds are dropped with a visible count. Supply failures
    // (Error or exception) degrade to one diagnostic and never block the
    // loop. Wire once before run(), like the other setters.
    void set_model_input_supplier(ModelInputSupplier supplier,
                                  ModelInputSupplyOptions options = ModelInputSupplyOptions{});

    [[nodiscard]] Result<ConversationLoopResult> run(const AgentLoopSpec &spec,
                                                     const OperationContext &context);

  private:
    struct SuppliedInput final {
        std::vector<ModelContentPart> parts;
        std::size_t offered = 0;
        std::size_t accepted = 0;
        std::size_t dropped = 0;
        std::map<std::string, std::int64_t> drop_reasons;
        std::uint64_t image_bytes = 0;
        bool degraded = false;
    };

    [[nodiscard]] Result<ModelRequest> build_request(const AgentLoopSpec &spec,
                                                     const std::vector<ModelInputItem> &history,
                                                     const std::string &feedback,
                                                     const std::vector<ModelContentPart> &supplied);
    // Resolves the seam once for the request being assembled (DEC-053):
    // validates and bounds the offered items, forces the untrusted envelope and
    // flattens the surviving parts. Never fails — supply errors and exceptions
    // degrade to a visible diagnostic and an empty part list.
    [[nodiscard]] SuppliedInput resolve_supplied_input(const AgentLoopSpec &spec);
    void emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
              EventClass classification) const;

    ModelGateway &gateway_;
    ConversationLoopConfig config_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
    std::shared_ptr<BuiltinToolRegistry> tools_;
    ModelInputSupplier input_supplier_;
    ModelInputSupplyOptions supply_options_{};
};

} // namespace mira
