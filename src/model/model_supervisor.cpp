#include <mira/model_supervisor.hpp>

#include <string>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] bool same_domain(const Error &error, ModelDomainCode code) {
    return error.domain == "mira.model" &&
           error.domain_code == static_cast<std::int32_t>(code);
}

} // namespace

std::string circuit_state_name(CircuitState state) {
    switch (state) {
    case CircuitState::Unknown:
        return "Unknown";
    case CircuitState::Healthy:
        return "Healthy";
    case CircuitState::Degraded:
        return "Degraded";
    case CircuitState::OpenCircuit:
        return "OpenCircuit";
    case CircuitState::Probing:
        return "Probing";
    }
    return "Unknown";
}

ProviderCircuit::ProviderCircuit(CircuitConfig config) : config_(std::move(config)) {}

void ProviderCircuit::record_success() {
    ++successes_;
    failures_ = 0;
    switch (state_) {
    case CircuitState::Unknown:
    case CircuitState::Degraded:
    case CircuitState::Probing:
        state_ = successes_ >= config_.consecutive_successes_to_close ? CircuitState::Healthy
                                                                     : CircuitState::Degraded;
        break;
    case CircuitState::Healthy:
    case CircuitState::OpenCircuit:
        break;
    }
}

void ProviderCircuit::record_failure() {
    ++failures_;
    successes_ = 0;
    if (state_ == CircuitState::Probing) {
        state_ = CircuitState::OpenCircuit;
        return;
    }
    if (failures_ >= config_.consecutive_failures_to_open) {
        state_ = CircuitState::OpenCircuit;
        return;
    }
    state_ = CircuitState::Degraded;
}

bool ProviderCircuit::admits_requests() const noexcept {
    return state_ != CircuitState::OpenCircuit;
}

RequestStage classify_stage(bool write_started, bool write_completed,
                            bool stream_ended_without_terminal) {
    if (stream_ended_without_terminal) {
        return RequestStage::StreamBroken;
    }
    if (write_completed) {
        return RequestStage::AwaitingResponse;
    }
    if (write_started) {
        return RequestStage::PartialWrite;
    }
    return RequestStage::PreWriteFailure;
}

RetryDecision ProviderSupervisor::evaluate(
    const Error &failure, RequestStage stage,
    const std::optional<std::chrono::milliseconds> &retry_after, const RetryBudget &budget,
    const ProviderCircuit &circuit) const {
    RetryDecision decision;

    // Budgets first: no retry beyond attempts, time or circuit admission.
    if (budget.attempts_used >= budget.max_attempts) {
        decision.action = RetryAction::GiveUp;
        decision.reason = "retry attempt budget exhausted";
        return decision;
    }
    if (budget.elapsed >= budget.total_budget) {
        decision.action = RetryAction::GiveUp;
        decision.reason = "retry time budget exhausted";
        return decision;
    }
    if (!circuit.admits_requests()) {
        decision.action = RetryAction::GiveUp;
        decision.reason = "circuit is open";
        return decision;
    }

    // Ambiguous completions: the remote may have processed the request, so
    // the default is to stop, not to blindly resend.
    if (same_domain(failure, ModelDomainCode::AmbiguousCompletion)) {
        decision.action = RetryAction::GiveUp;
        decision.reason = "ambiguous completion requires recovery policy, not retry";
        return decision;
    }

    switch (stage) {
    case RequestStage::LocalRejection:
        decision.action = RetryAction::GiveUp;
        decision.reason = "local rejection never reaches the wire";
        return decision;
    case RequestStage::PreWriteFailure:
        if (!failure.retryable) {
            decision.action = RetryAction::GiveUp;
            decision.reason = "failure is not marked retryable";
            return decision;
        }
        decision.action = RetryAction::RetryNow;
        decision.reason = "pre-write transport failures are safe to resend";
        return decision;
    case RequestStage::PartialWrite:
    case RequestStage::AwaitingResponse:
    case RequestStage::StreamBroken:
        if (same_domain(failure, ModelDomainCode::RateLimited) ||
            same_domain(failure, ModelDomainCode::ProviderOverloaded)) {
            break; // Honour Retry-After below.
        }
        decision.action = RetryAction::GiveUp;
        decision.reason =
            "request bytes left the process; resend requires idempotency evidence";
        return decision;
    }

    // Rate limits and explicit overload: bounded, server-paced retry.
    if (retry_after.has_value()) {
        decision.delay = std::min(*retry_after, budget.retry_after_cap);
        // A delay beyond the remaining budget is not waited out.
        if (budget.elapsed + decision.delay > budget.total_budget) {
            decision.action = RetryAction::GiveUp;
            decision.reason = "retry-after delay exceeds the retry budget";
            return decision;
        }
        decision.action = RetryAction::RetryAfter;
        decision.reason = "retryable after the server-provided delay";
        return decision;
    }
    decision.delay = std::chrono::milliseconds(250) * (1U << std::min(budget.attempts_used, 4U));
    if (budget.elapsed + decision.delay > budget.total_budget) {
        decision.action = RetryAction::GiveUp;
        decision.reason = "backoff delay exceeds the retry budget";
        return decision;
    }
    decision.action = RetryAction::RetryAfter;
    decision.reason = "retryable with bounded backoff";
    return decision;
}

} // namespace mira
