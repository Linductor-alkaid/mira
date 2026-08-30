#pragma once

#include <mira/core_contracts.hpp>
#include <mira/observation.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Input contracts
// ---------------------------------------------------------------------------

// One discrete platform input event. The payload is canonical, already
// redacted data produced by the action compiler; environments treat it as
// untrusted text and never log it verbatim.
struct InputEvent final {
    std::string kind;    // e.g. "tap", "long_press", "swipe", "type", "back".
    std::string payload; // canonical coordinates or text, redacted upstream.
};

// A compiled input sequence targeted at one display. A nil display selects
// the environment's primary display; actions never assume a global
// coordinate space shared by all displays.
struct InputSequence final {
    std::optional<DisplayId> display;
    std::vector<InputEvent> events;
};

enum class ExecutionStatus : std::uint8_t {
    Dispatched, // Platform accepted the sequence; completion is not known yet.
    Completed,  // Platform finished the sequence.
    Rejected,   // Refused before any side effect occurred.
    Unknown,    // Platform could not confirm; a side effect may have occurred.
};

struct ExecutionReceipt final {
    ExecutionStatus status = ExecutionStatus::Dispatched;
    // True when the platform cannot exclude that input reached the device,
    // including rejected-after-dispatch and lost-callback outcomes.
    bool side_effect_may_have_occurred = false;
    EnvironmentEpoch environment_epoch = 0;
    std::string safe_message;
};

// ---------------------------------------------------------------------------
// Environment capabilities
// ---------------------------------------------------------------------------

// What an environment can honestly deliver on every request. Adapters derive
// this from platform state; they must not declare a capability they cannot
// honor, and Core never guesses capabilities from platform names.
struct EnvironmentCapabilities final {
    bool screen_capture = false;
    bool ui_tree = false;
    bool foreground_app = false;
    bool device_state = false;
    std::size_t perception_sources = 0;
    // The platform can return components captured in one transaction or
    // frame; environments that merely batch requests must not set this.
    bool atomic_observation = false;
    // Declared worst-case capture skew between components of one
    // observation; zero means no bound can be declared.
    std::chrono::nanoseconds max_component_skew{0};
    bool discrete_input = false;
    // interrupt() can release in-flight input and unblock platform waits.
    bool input_release = false;
    // The environment bumps EnvironmentEpoch when coordinates may have
    // become invalid (rotation, topology, permission or session changes).
    bool epoch_invalidation = false;
};

// Names the required components of a request the capabilities cannot
// deliver. An empty result means the request is supported; environments use
// it to fail closed instead of returning a silently incomplete observation.
[[nodiscard]] inline std::vector<std::string>
unsupported_required_components(const EnvironmentCapabilities &capabilities,
                                const ObservationRequest &request) {
    std::vector<std::string> unsupported;
    if (request.required.screen && !capabilities.screen_capture) {
        unsupported.push_back("screen");
    }
    if (request.required.structure && !capabilities.ui_tree) {
        unsupported.push_back("structure");
    }
    if (request.required.foreground && !capabilities.foreground_app) {
        unsupported.push_back("foreground");
    }
    if (request.required.device && !capabilities.device_state) {
        unsupported.push_back("device");
    }
    if (request.required.perception > capabilities.perception_sources) {
        unsupported.push_back("perception");
    }
    return unsupported;
}

// ---------------------------------------------------------------------------
// Operation context
// ---------------------------------------------------------------------------

// Carries the identity, deadline and cooperative cancellation state of one
// bounded environment operation. Contexts are copied per component capture;
// long-running captures must poll cancelled() between blocking steps.
struct OperationContext final {
    SessionId session;
    TaskId task;
    StepId step;
    OperationId operation;
    std::uint64_t task_epoch = 0;
    Timestamp started_at;
    // Absolute deadline on the steady clock; nullopt means no deadline.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    // Cooperative cancellation probe owned by the operation supervisor.
    std::function<bool()> cancellation_requested;

    [[nodiscard]] bool cancelled() const noexcept {
        return cancellation_requested != nullptr && cancellation_requested();
    }
    [[nodiscard]] bool expired(const Timestamp &now) const noexcept {
        return deadline.has_value() && now.monotonic >= *deadline;
    }
    [[nodiscard]] bool cancelled_or_expired(const Timestamp &now) const noexcept {
        return cancelled() || expired(now);
    }
};

// Minimal context for control-plane calls (close, shutdown, takeover) that
// originate outside a task step.
[[nodiscard]] inline OperationContext make_control_context() {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

// ---------------------------------------------------------------------------
// Environment interface
// ---------------------------------------------------------------------------

// Aggregates environment observation and final input dispatch behind one
// platform-neutral contract. execute() success only means the platform
// accepted or completed the input; goal success is decided by Verify.
class IEnvironment {
  public:
    virtual ~IEnvironment() = default;

    [[nodiscard]] virtual EnvironmentCapabilities capabilities() const = 0;
    virtual Result<Observation> observe(const ObservationRequest &request,
                                        const OperationContext &context) = 0;
    virtual Result<ExecutionReceipt> execute(const InputSequence &input,
                                             const OperationContext &context) = 0;
    // Best-effort release of in-flight input and platform waits. Must be
    // idempotent; late completions must not revive settled operations.
    virtual Result<void> interrupt(const OperationContext &context) = 0;
};

} // namespace mira
