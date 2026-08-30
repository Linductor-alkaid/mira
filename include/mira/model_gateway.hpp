#pragma once

#include <mira/event_store.hpp>
#include <mira/model_budget.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_provider.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_supervisor.hpp>
#include <mira/model_tool.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

struct InferOptions final {
    bool stream = false;
    bool capture_raw_response = false;
};

// The aggregate outcome of one gateway call: three success layers stay
// separate (transport/protocol accepted, canonical response, semantic
// decision) so no layer can masquerade as the next.
struct ModelCallOutcome final {
    RouteDecision route;
    ModelResponse response;
    DecisionParseResult parse;
    std::optional<ToolProposalBatch> tool_proposals;
    std::optional<BudgetReservation> reservation;
    BudgetSettlement settlement;
    Hash wire_request_digest{};
    SseStreamStats sse_stats;
    std::uint32_t attempts = 1;
    // True when the response was rejected by admission instead of settling.
    bool admitted = true;
    std::string rejection_reason;
};

struct ModelGatewayConfig final {
    RetryBudget retry_budget{};
    CircuitConfig circuit_config{};
    RepairPolicy repair_policy{};
    // When true, a missing price entry rejects the reservation instead of
    // proceeding with an unknown cost.
    bool require_known_price = false;
};

// Epoch and lifecycle admission for model completions. The coordinator (not
// the provider) decides whether a response may still influence the task.
class TaskAdmissionGate {
  public:
    virtual ~TaskAdmissionGate() = default;
    [[nodiscard]] virtual bool admit(const TaskId &task, std::uint64_t epoch) const = 0;
};

// Gate used by tests and by loops without an external coordinator.
class SimpleAdmissionGate final : public TaskAdmissionGate {
  public:
    struct Entry final {
        std::uint64_t epoch = 0;
        bool active = true;
    };

    void activate(const TaskId &task, std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        entries_[task] = Entry{epoch, true};
    }
    void advance(const TaskId &task, std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        entries_[task] = Entry{epoch, true};
    }
    void deactivate(const TaskId &task) {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(task);
        if (found != entries_.end()) {
            found->second.active = false;
        }
    }
    [[nodiscard]] bool admit(const TaskId &task, std::uint64_t epoch) const override {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(task);
        return found != entries_.end() && found->second.active &&
               found->second.epoch == epoch;
    }

  private:
    mutable std::mutex mutex_;
    std::map<TaskId, Entry> entries_;
};

// Orchestrates route -> provider -> retry/circuit supervision -> budget
// reconciliation -> local decision parsing and tool resolution. The gateway
// never advances task state itself; admission is asked through the gate.
class ModelGateway final {
  public:
    ModelGateway(executor::Executor &executor, ModelRouter router,
                 std::shared_ptr<IArtifactSource> artifacts, PriceTable prices,
                 ModelGatewayConfig config = ModelGatewayConfig{});

    void register_provider(std::shared_ptr<IModelProvider> provider);
    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                         SessionId session);
    void set_admission_gate(std::shared_ptr<const TaskAdmissionGate> gate);

    [[nodiscard]] Result<ModelCallOutcome> infer(const ModelRequest &request,
                                                 const OperationContext &context,
                                                 const InferOptions &options = InferOptions{});

    [[nodiscard]] const BudgetLedger &ledger() const noexcept { return ledger_; }
    [[nodiscard]] CircuitState circuit_state(const ModelProfileId &profile) const;
    [[nodiscard]] const ModelRouter &router() const noexcept { return router_; }
    // Registration surface for hosts assembling profiles after construction.
    [[nodiscard]] ModelRouter &mutable_router() noexcept { return router_; }

  private:
    [[nodiscard]] Result<ModelCallOutcome> run_one_attempt(const ModelRequest &request,
                                                           const OperationContext &context,
                                                           const InferOptions &options,
                                                           const ModelProfile &profile,
                                                           IModelProvider &provider);
    void emit(const ModelRequest &request, std::string type, JsonValue summary,
              EventClass classification) const;

    executor::Executor &executor_;
    ModelRouter router_;
    std::shared_ptr<IArtifactSource> artifacts_;
    ModelGatewayConfig config_;
    BudgetLedger ledger_;
    std::vector<std::shared_ptr<IModelProvider>> providers_;
    std::map<ModelProfileId, ProviderCircuit> circuits_;
    std::shared_ptr<const TaskAdmissionGate> admission_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
    RepairPolicy repair_policy_;
};

} // namespace mira
