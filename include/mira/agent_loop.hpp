#pragma once

#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_schema.hpp>
#include <mira/observation.hpp>
#include <mira/tool_executor.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// The standard discrete-action decision schema for the M3 agent loop. The
// schema is part of the loop contract: its digest is recorded with every
// request and replay validates against the same subset.
[[nodiscard]] JsonSchema agent_decision_schema();

enum class LoopOutcome : std::uint8_t {
    Completed, // Goal verified against a fresh observation.
    Failed,    // Non-recoverable failure or the model declared failure.
    Cancelled, // Cancellation or admission rejection.
    MaxSteps,  // Step budget exhausted before verification.
};

[[nodiscard]] std::string loop_outcome_name(LoopOutcome outcome);

enum class StepPhase : std::uint8_t {
    Observed,
    Reasoned,
    Acted,
    Verified,
    Recovering,
};

struct LoopStepRecord final {
    std::uint32_t step = 0;
    StepPhase phase = StepPhase::Observed;
    ObservationId observation;
    ModelRequestId model_request;
    std::optional<Hash> decision_digest;
    std::string action_summary;
    bool verified = false;
    std::string note;
};

struct AgentLoopResult final {
    LoopOutcome outcome = LoopOutcome::Failed;
    std::vector<LoopStepRecord> steps;
    std::string safe_summary;
    std::uint32_t recoveries = 0;
    std::uint32_t repairs = 0;
};

struct AgentLoopConfig final {
    std::uint32_t max_steps = 16;
    // Bounded retries for recoverable observation/model failures per step.
    std::uint32_t max_recoveries_per_step = 1;
    std::chrono::milliseconds model_call_deadline{30'000};
    // Observation freshness expectations for verification captures.
    std::chrono::milliseconds observation_max_age{2'000};
    // Whole-run budget for tool executions (RULE-08); exhausting it settles
    // the loop as Failed instead of queuing unbounded work.
    std::uint32_t max_tool_executions = 32;
    // Bounded user-message queue; overflow rejects the enqueue.
    std::size_t max_pending_user_messages = 16;
};

// Verifies progress against a fresh observation after each action. Returning
// Satisfied settles the loop as Completed; the model's own "done" claim never
// suffices without verification.
class ILoopVerifier {
  public:
    virtual ~ILoopVerifier() = default;
    enum class Verdict : std::uint8_t { Satisfied, NotSatisfied, Invalid };
    [[nodiscard]] virtual Verdict verify(const Observation &fresh,
                                         const DecisionCandidate &decision) = 0;
};

// Verifies only the model's terminal claim; used by tests that drive the
// loop through a scripted provider.
class ModelDoneVerifier final : public ILoopVerifier {
  public:
    [[nodiscard]] Verdict verify(const Observation &,
                                 const DecisionCandidate &decision) override;
};

struct AgentLoopSpec final {
    TaskId task_id;
    SessionId session_id;
    std::uint64_t task_epoch = 0;
    std::string goal;
    ModelProfileId profile_id;
};

// Drives Observe -> Reason -> Plan -> Act -> Verify over one environment and
// the model gateway. Each iteration is a bounded work unit; cancellation,
// admission rejection and terminal states stop the loop before any new
// action is dispatched.
//
// With a tool registry attached (DEC-015), tool proposals execute through the
// BuiltIn boundary and their results feed the next request. Without one, tool
// proposals settle the loop as Failed. User messages (DEC-016) can be enqueued
// from any thread while run() executes; they are drained at step boundaries
// and stay in context for the rest of the run.
class AgentLoop final {
  public:
    AgentLoop(std::shared_ptr<IEnvironment> environment, ModelGateway &gateway,
              AgentLoopConfig config = AgentLoopConfig{});

    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                         SessionId session);
    void set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools);

    // Queues one user message for injection at the next step boundary.
    // Rejects when the bounded queue is full or the message is empty. Text is
    // stored and replayed into model requests verbatim: hosts must apply
    // their own redaction policy before enqueueing (DEC-016).
    [[nodiscard]] Result<void> enqueue_user_message(std::string message);

    [[nodiscard]] Result<AgentLoopResult> run(const AgentLoopSpec &spec,
                                              const OperationContext &context,
                                              ILoopVerifier &verifier);

  private:
    [[nodiscard]] Result<Observation> observe_once(const AgentLoopSpec &spec,
                                                   const OperationContext &context,
                                                   ObservationMode mode);
    [[nodiscard]] Result<ModelRequest>
    build_request(const AgentLoopSpec &spec, const Observation &observation,
                  const std::string &extra_instruction,
                  const std::vector<std::string> &user_instructions,
                  const std::vector<ToolExecutionRecord> &tool_results);
    void emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
              EventClass classification) const;
    // Pops every pending message, records UserMessageInjected events and
    // appends the texts to the standing instructions for this run.
    void drain_user_messages(const AgentLoopSpec &spec,
                             std::vector<std::string> &user_instructions);

    std::shared_ptr<IEnvironment> environment_;
    ModelGateway &gateway_;
    AgentLoopConfig config_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
    std::shared_ptr<BuiltinToolRegistry> tools_;
    std::mutex pending_mutex_;
    std::deque<std::string> pending_user_messages_;
};

// Compiles one validated decision into the platform-neutral input sequence.
// Coordinates are canonical [0, 1]; anything outside fails closed.
[[nodiscard]] Result<InputSequence> compile_discrete_action(const JsonValue &decision);

} // namespace mira
