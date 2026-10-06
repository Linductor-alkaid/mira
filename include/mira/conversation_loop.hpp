#pragma once

#include <mira/agent_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/tool_executor.hpp>

#include <chrono>
#include <cstdint>
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
};

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

    [[nodiscard]] Result<ConversationLoopResult> run(const AgentLoopSpec &spec,
                                                     const OperationContext &context);

  private:
    [[nodiscard]] Result<ModelRequest> build_request(const AgentLoopSpec &spec,
                                                     const std::vector<ModelInputItem> &history,
                                                     const std::string &feedback);
    void emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
              EventClass classification) const;

    ModelGateway &gateway_;
    ConversationLoopConfig config_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
    std::shared_ptr<BuiltinToolRegistry> tools_;
};

} // namespace mira
