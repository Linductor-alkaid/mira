#pragma once

#include <mira/model_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace mira {

// ---------------------------------------------------------------------------
// Provider supervision: request-stage retry table and circuit state (M3-13)
// ---------------------------------------------------------------------------

// How far a failed exchange progressed. The stage decides whether the remote
// may have received (and billed) the request.
enum class RequestStage : std::uint8_t {
    LocalRejection,    // Failed before any bytes left the process.
    PreWriteFailure,   // DNS/connect/TLS failed before request bytes.
    PartialWrite,      // Some request bytes left; billing is possible.
    AwaitingResponse,  // Fully written; outcome unknown on transport loss.
    StreamBroken,      // SSE stream ended without a terminal event.
};

enum class RetryAction : std::uint8_t {
    RetryNow,    // Safe to resend immediately (stage + error allow it).
    RetryAfter,  // Retryable, but honour the computed delay first.
    GiveUp,      // Not retryable at this stage, or budgets exhausted.
};

struct RetryBudget final {
    std::uint32_t attempts_used = 0;
    std::uint32_t max_attempts = 2;
    std::chrono::milliseconds total_budget{10'000};
    std::chrono::milliseconds elapsed{0};
    // Cap applied to server-provided Retry-After values.
    std::chrono::milliseconds retry_after_cap{30'000};
};

struct RetryDecision final {
    RetryAction action = RetryAction::GiveUp;
    std::chrono::milliseconds delay{0};
    std::string reason;
    // True when this failure transitioned the circuit to open.
    bool circuit_opened = false;
};

// Health states per design model_provider §6. Health only affects routing
// and retry admission; it never changes authorization.
enum class CircuitState : std::uint8_t {
    Unknown,
    Healthy,
    Degraded,
    OpenCircuit,
    Probing,
};

[[nodiscard]] std::string circuit_state_name(CircuitState state);

struct CircuitConfig final {
    std::uint32_t consecutive_failures_to_open = 3;
    std::uint32_t consecutive_successes_to_close = 1;
    std::chrono::milliseconds cooldown{5'000};
};

// Consecutive-failure circuit breaker with a probing half-open state.
class ProviderCircuit final {
  public:
    explicit ProviderCircuit(CircuitConfig config = CircuitConfig{});

    void record_success();
    void record_failure();
    [[nodiscard]] CircuitState state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return failures_; }

    // Requests admission: open circuits reject except in probing state.
    [[nodiscard]] bool admits_requests() const noexcept;

  private:
    CircuitConfig config_;
    CircuitState state_ = CircuitState::Unknown;
    std::uint32_t failures_ = 0;
    std::uint32_t successes_ = 0;
};

// Pure, table-driven retry evaluation (design LLM API §10.2). The caller
// executes delays through Executor timers; this type never sleeps.
class ProviderSupervisor final {
  public:
    // `retryable` is the mechanism hint from the model error; the stage and
    // budgets make the final decision.
    [[nodiscard]] RetryDecision evaluate(const Error &failure, RequestStage stage,
                                         const std::optional<std::chrono::milliseconds> &retry_after,
                                         const RetryBudget &budget,
                                         const ProviderCircuit &circuit) const;
};

// Classifies the request stage from a transport failure, given whether the
// request body had started (or finished) leaving the process.
[[nodiscard]] RequestStage classify_stage(bool write_started, bool write_completed,
                                          bool stream_ended_without_terminal);

} // namespace mira
