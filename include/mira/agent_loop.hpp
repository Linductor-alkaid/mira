#pragma once

#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_schema.hpp>
#include <mira/observation.hpp>

#include <cstdint>
#include <memory>
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
class AgentLoop final {
  public:
    AgentLoop(std::shared_ptr<IEnvironment> environment, ModelGateway &gateway,
              AgentLoopConfig config = AgentLoopConfig{});

    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                         SessionId session);

    [[nodiscard]] Result<AgentLoopResult> run(const AgentLoopSpec &spec,
                                              const OperationContext &context,
                                              ILoopVerifier &verifier);

  private:
    [[nodiscard]] Result<Observation> observe_once(const AgentLoopSpec &spec,
                                                   const OperationContext &context,
                                                   ObservationMode mode);
    [[nodiscard]] Result<ModelRequest>
    build_request(const AgentLoopSpec &spec, const Observation &observation,
                  const std::string &extra_instruction);
    void emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
              EventClass classification) const;

    std::shared_ptr<IEnvironment> environment_;
    ModelGateway &gateway_;
    AgentLoopConfig config_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
};

// Compiles one validated decision into the platform-neutral input sequence.
// Coordinates are canonical [0, 1]; anything outside fails closed.
[[nodiscard]] Result<InputSequence> compile_discrete_action(const JsonValue &decision);

} // namespace mira
