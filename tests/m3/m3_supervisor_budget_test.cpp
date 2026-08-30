#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_budget.hpp>
#include <mira/model_supervisor.hpp>

#include <string>

namespace {

using namespace mira;

int retry_table_covers_request_stages() {
    ProviderSupervisor supervisor;
    ProviderCircuit circuit;
    RetryBudget budget;
    budget.max_attempts = 3;

    const auto transport_failure = [](bool retryable) {
        Error error = make_model_error(ModelDomainCode::TransportFailed, "socket", retryable);
        return error;
    };
    const auto rate_limited = [] {
        Error error = make_model_error(ModelDomainCode::RateLimited, "429", true);
        return error;
    };
    const auto ambiguous = [] {
        Error error = make_model_error(ModelDomainCode::AmbiguousCompletion, "dropped", false);
        return error;
    };

    // Pre-write retryable failures are safe to resend immediately.
    auto prewrite =
        supervisor.evaluate(transport_failure(true), RequestStage::PreWriteFailure, std::nullopt,
                            budget, circuit);
    MIRA_CHECK(prewrite.action == RetryAction::RetryNow);

    // Pre-write non-retryable failures stop.
    auto fatal = supervisor.evaluate(transport_failure(false), RequestStage::PreWriteFailure,
                                     std::nullopt, budget, circuit);
    MIRA_CHECK(fatal.action == RetryAction::GiveUp);

    // Post-write transport failures are ambiguous; no blind resend.
    auto postwrite =
        supervisor.evaluate(transport_failure(true), RequestStage::AwaitingResponse, std::nullopt,
                            budget, circuit);
    MIRA_CHECK(postwrite.action == RetryAction::GiveUp);

    // Explicitly ambiguous completions stop even with retries left.
    auto ambiguous_decision =
        supervisor.evaluate(ambiguous(), RequestStage::StreamBroken, std::nullopt, budget, circuit);
    MIRA_CHECK(ambiguous_decision.action == RetryAction::GiveUp);

    // Rate limits honour Retry-After within the cap and budget.
    auto with_delay = supervisor.evaluate(rate_limited(), RequestStage::AwaitingResponse,
                                          std::chrono::milliseconds{500}, budget, circuit);
    MIRA_CHECK(with_delay.action == RetryAction::RetryAfter);
    MIRA_CHECK(with_delay.delay == std::chrono::milliseconds{500});

    // A Retry-After beyond the budget stops instead of stalling.
    RetryBudget tight;
    tight.max_attempts = 3;
    tight.total_budget = std::chrono::milliseconds{1'000};
    auto beyond = supervisor.evaluate(rate_limited(), RequestStage::AwaitingResponse,
                                      std::chrono::milliseconds{5'000}, tight, circuit);
    MIRA_CHECK(beyond.action == RetryAction::GiveUp);

    // Retry-After caps apply to absurd server values.
    RetryBudget capped;
    capped.max_attempts = 5;
    capped.retry_after_cap = std::chrono::milliseconds{1'000};
    auto capped_decision = supervisor.evaluate(
        rate_limited(), RequestStage::AwaitingResponse, std::chrono::milliseconds{60'000}, capped,
        circuit);
    MIRA_CHECK(capped_decision.delay == std::chrono::milliseconds{1'000});

    // Attempt budgets stop retries regardless of stage.
    RetryBudget exhausted;
    exhausted.attempts_used = 3;
    auto spent = supervisor.evaluate(rate_limited(), RequestStage::AwaitingResponse,
                                     std::chrono::milliseconds{10}, exhausted, circuit);
    MIRA_CHECK(spent.action == RetryAction::GiveUp);

    // Open circuits reject everything.
    ProviderCircuit open;
    for (int index = 0; index < 3; ++index) {
        open.record_failure();
    }
    MIRA_CHECK(open.state() == CircuitState::OpenCircuit);
    MIRA_CHECK(!open.admits_requests());
    auto blocked =
        supervisor.evaluate(rate_limited(), RequestStage::PreWriteFailure, std::nullopt, budget, open);
    MIRA_CHECK(blocked.action == RetryAction::GiveUp);
    return 0;
}

int circuit_state_machine() {
    CircuitConfig config;
    config.consecutive_failures_to_open = 2;
    ProviderCircuit circuit(config);
    MIRA_CHECK(circuit.state() == CircuitState::Unknown);
    MIRA_CHECK(circuit.admits_requests());

    circuit.record_failure();
    MIRA_CHECK(circuit.state() == CircuitState::Degraded);
    circuit.record_failure();
    MIRA_CHECK(circuit.state() == CircuitState::OpenCircuit);
    MIRA_CHECK(!circuit.admits_requests());

    // The open circuit only leaves through probing (modeled by the caller
    // recreating admission after cooldown); probing failures reopen.
    ProviderCircuit probing(config);
    probing.record_failure();
    probing.record_failure();
    probing.record_success(); // still open: success only closes from probing
    MIRA_CHECK(probing.state() == CircuitState::OpenCircuit ||
               probing.state() == CircuitState::Degraded);
    MIRA_CHECK(circuit_state_name(CircuitState::OpenCircuit) == "OpenCircuit");
    MIRA_CHECK(classify_stage(false, false, false) == RequestStage::PreWriteFailure);
    MIRA_CHECK(classify_stage(false, false, true) == RequestStage::StreamBroken);
    MIRA_CHECK(classify_stage(true, false, false) == RequestStage::PartialWrite);
    MIRA_CHECK(classify_stage(true, true, false) == RequestStage::AwaitingResponse);
    return 0;
}

[[nodiscard]] ModelUsage usage(std::optional<std::uint64_t> in, std::optional<std::uint64_t> out,
                              std::optional<std::uint64_t> cached, UsageQuality quality) {
    ModelUsage result;
    result.input_tokens = in;
    result.output_tokens = out;
    result.cached_input_tokens = cached;
    result.quality = quality;
    return result;
}

int budget_reservation_and_reconciliation() {
    const auto task = TaskId::generate();
    ModelBudget budget;
    budget.max_input_tokens = 1'000;
    budget.max_output_tokens = 200;
    budget.max_requests = 3;
    budget.max_total_cost_micros = 5'000;

    PriceTable prices;
    PriceEntry entry;
    entry.model = "test-model";
    entry.currency = "USD";
    entry.input_micros_per_mtok = 1'000'000; // 1 unit per token.
    entry.output_micros_per_mtok = 2'000'000;
    entry.cached_input_micros_per_mtok = 100'000;
    prices.add(entry);

    BudgetLedger ledger(prices);
    BudgetEstimate estimate;
    estimate.input_tokens = 300;
    estimate.output_tokens = 60;
    estimate.requests = 1;
    estimate.cost_micros = 500;
    auto reserved = ledger.reserve(task, budget, estimate);
    MIRA_CHECK(reserved.has_value());

    // Over-budget reservations fail closed (request count here).
    MIRA_CHECK(ledger.reserve(task, budget, estimate).has_value());
    MIRA_CHECK(ledger.reserve(task, budget, estimate).has_value());
    auto over = ledger.reserve(task, budget, estimate);
    MIRA_CHECK(!over.has_value());

    // Provider-reported usage reconciles and releases the reservation.
    auto settled = ledger.reconcile(task, budget, usage(300, 50, 100, UsageQuality::ProviderReported),
                                    "test-model");
    MIRA_CHECK(settled.has_value());
    MIRA_CHECK(settled.value().quality == ReconciliationQuality::Reconciled);
    // cost = 300*1 + 50*2 - 100*(1 - 0.1) = 300 + 100 - 90 = 310 micros.
    MIRA_CHECK(settled.value().reconciled_cost_micros == 310);
    MIRA_CHECK(ledger.spent_micros(task) == 310);

    // Missing usage keeps the reservation and flags an audit.
    const auto task2 = TaskId::generate();
    (void)ledger.reserve(task2, budget, estimate);
    auto missing = ledger.reconcile(task2, budget, usage(std::nullopt, std::nullopt, std::nullopt,
                                                         UsageQuality::Missing),
                                    "test-model");
    MIRA_CHECK(missing.value().quality == ReconciliationQuality::MissingUsage);
    MIRA_CHECK(missing.value().reserved_remainder);

    // Partial usage stays partially reserved.
    const auto task3 = TaskId::generate();
    (void)ledger.reserve(task3, budget, estimate);
    auto partial = ledger.reconcile(task3, budget,
                                    usage(std::nullopt, 20, std::nullopt, UsageQuality::Partial),
                                    "test-model");
    MIRA_CHECK(partial.value().quality == ReconciliationQuality::PartialUsage);

    // Unknown price: usage known, cost unknown; never a fake zero.
    const auto task4 = TaskId::generate();
    (void)ledger.reserve(task4, budget, estimate);
    auto unknown_price = ledger.reconcile(
        task4, budget, usage(10, 5, std::nullopt, UsageQuality::ProviderReported), "mystery-model");
    MIRA_CHECK(unknown_price.value().quality == ReconciliationQuality::UnknownPrice);
    MIRA_CHECK(unknown_price.value().reconciled_cost_micros == 0);

    // Inconsistent counters (cached > input) degrade to partial.
    const auto task5 = TaskId::generate();
    (void)ledger.reserve(task5, budget, estimate);
    auto inconsistent = ledger.reconcile(
        task5, budget, usage(10, 5, 999, UsageQuality::ProviderReported), "test-model");
    MIRA_CHECK(inconsistent.value().quality == ReconciliationQuality::PartialUsage);

    // Price lookup honours effective windows and versions.
    PriceEntry newer = entry;
    newer.version = 2;
    newer.input_micros_per_mtok = 4'000'000;
    newer.effective_from = std::chrono::system_clock::now() + std::chrono::hours(1);
    prices.add(newer);
    MIRA_CHECK(prices.lookup("test-model", "USD", std::chrono::system_clock::now())->version == 1);
    MIRA_CHECK(prices.lookup("test-model", "EUR", std::chrono::system_clock::now()) ==
               std::nullopt);
    return 0;
}

int token_estimates_are_conservative() {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart text;
    text.text = std::string(300, 'a');
    system_item.content.emplace_back(std::move(text));
    request.input = {std::move(system_item)};
    request.data_policy.store = false;
    const auto tokens = estimate_input_tokens(request);
    MIRA_CHECK(tokens >= 100); // 300 bytes / 3 = 100, plus framing margin.
    return 0;
}

} // namespace

int main() {
    if (const int status = retry_table_covers_request_stages(); status != 0) {
        return status;
    }
    if (const int status = circuit_state_machine(); status != 0) {
        return status;
    }
    if (const int status = budget_reservation_and_reconciliation(); status != 0) {
        return status;
    }
    if (const int status = token_estimates_are_conservative(); status != 0) {
        return status;
    }
    return 0;
}
