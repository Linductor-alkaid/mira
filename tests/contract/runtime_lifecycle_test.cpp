#include "support/test.hpp"
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime.hpp>

#include <kairo/executor.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace mira;
using namespace std::chrono_literals;

Error probe_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.lifecycle-test";
    error.safe_message = std::move(message);
    return error;
}

class ProbeEnvironment final : public IEnvironment {
  public:
    EnvironmentCapabilities capabilities() const override { return simulator.capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        return simulator.observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        return simulator.execute(input, context);
    }
    Result<void> interrupt(const OperationContext &) override {
        ++interrupts;
        return on_interrupt ? on_interrupt() : Result<void>{};
    }
    std::function<Result<void>()> on_interrupt;
    std::atomic<unsigned> interrupts{0};

  private:
    adapters::simulator::SimulatorEnvironment simulator{
        adapters::simulator::SimulatorSetup::single_display()};
};

int runtime_transition_contract_is_closed() {
    // Rows in RuntimeState declaration order; masks pin the public policy,
    // including cleanup after failure and idempotent settled states.
    constexpr std::array<unsigned, 7> edges{0b0001011, 0b1001100, 0b1001000, 0b1011000,
                                            0b0110000, 0b0100000, 0b1001000};
    for (unsigned from = 0; from <= 255; ++from) {
        for (unsigned to = 0; to <= 255; ++to) {
            const bool expected =
                from < edges.size() && to < edges.size() && (edges[from] & (1U << to)) != 0;
            MIRA_CHECK(valid_runtime_transition(static_cast<RuntimeState>(from),
                                                static_cast<RuntimeState>(to)) == expected);
        }
    }
    MIRA_CHECK(is_terminal(RuntimeState::Failed));
    return 0;
}

int uninitialized_and_failed_instances_can_be_finalized() {
    for (bool request_first : {false, true}) {
        MiraRuntime runtime;
        if (request_first) {
            auto request = runtime.request_shutdown();
            MIRA_CHECK(!request && request.error().code == ErrorCode::InvalidState);
            MIRA_CHECK(runtime.state() == RuntimeState::Stopping);
        }
        const auto report = runtime.finish_shutdown();
        MIRA_CHECK(report.clean && report.state == RuntimeState::Stopped);
        MIRA_CHECK(runtime.scheduling_metrics().accepted == 0);
        MIRA_CHECK(!runtime.initialize());
        MIRA_CHECK(!runtime.request_shutdown());
        MIRA_CHECK(runtime.finish_shutdown().diagnostic == report.diagnostic);
    }
    for (auto config : {RuntimeConfig{0, 8, 16}, RuntimeConfig{1, 0, 16}, RuntimeConfig{1, 8, 0}}) {
        MiraRuntime runtime(config);
        auto initialized = runtime.initialize();
        MIRA_CHECK(!initialized && initialized.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(runtime.state() == RuntimeState::Failed);
        MIRA_CHECK(!runtime.initialize());
        MIRA_CHECK(runtime.state() == RuntimeState::Failed);
        MIRA_CHECK(!runtime.open_session(std::make_shared<ProbeEnvironment>()));
        MIRA_CHECK(!runtime.request_shutdown());
        MIRA_CHECK(runtime.state() == RuntimeState::Stopping);
        const auto report = runtime.finish_shutdown();
        MIRA_CHECK(report.clean && report.state == RuntimeState::Stopped);
        MIRA_CHECK(runtime.scheduling_metrics().accepted == 0);
    }
    return 0;
}

int normal_shutdown_closes_admission_and_is_idempotent() {
    MiraRuntime runtime({1, 16, 32});
    MIRA_CHECK(runtime.initialize());
    MIRA_CHECK(!runtime.initialize());
    MIRA_CHECK(runtime.state() == RuntimeState::Running);
    auto session = runtime.open_session(std::make_shared<ProbeEnvironment>());
    MIRA_CHECK(session && session.value().command.outcome(2s));
    auto task =
        runtime.submit_task(session.value().id, TaskSpec{.goal = "goal", .user_constraints = {}});
    MIRA_CHECK(task && task.value().command.outcome(2s));
    auto stopped = runtime.request_shutdown();
    MIRA_CHECK(stopped);
    MIRA_CHECK(!runtime.begin_operation(task.value().id, StepId::generate()));
    MIRA_CHECK(!runtime.open_session(std::make_shared<ProbeEnvironment>()));
    MIRA_CHECK(!runtime.request_shutdown());
    auto settled = stopped.value().outcome(2s);
    MIRA_CHECK(settled && settled.value().status == SettlementStatus::Applied);
    MIRA_CHECK(runtime.state() == RuntimeState::Quiesced);
    auto report = runtime.finish_shutdown();
    MIRA_CHECK(report.clean && report.state == RuntimeState::Stopped);
    MIRA_CHECK(report.pending_commands == 0);
    MIRA_CHECK(runtime.task_snapshot(task.value().id).value().state == TaskState::Cancelled);
    MIRA_CHECK(runtime.session_snapshot(session.value().id).value().state == SessionState::Closed);
    MIRA_CHECK(runtime.finish_shutdown().diagnostic == report.diagnostic);
    MIRA_CHECK(!runtime.initialize());
    return 0;
}

struct InterruptGate final {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    bool opened = false;
    void open() {
        if (!opened) {
            opened = true;
            release.set_value();
        }
    }
};

int rejected_shutdown_still_settles_tasks() {
    InterruptGate gate;
    auto entered = gate.entered.get_future();
    MiraRuntime runtime({1, 32, 8});
    // Release before Runtime destruction on every early assertion failure.
    struct ReleaseOnExit final {
        InterruptGate &gate;
        ~ReleaseOnExit() { gate.open(); }
    } release_on_exit{gate};
    MIRA_CHECK(runtime.initialize());
    auto blocked_environment = std::make_shared<ProbeEnvironment>();
    blocked_environment->on_interrupt = [&]() -> Result<void> {
        gate.entered.set_value();
        if (gate.released.wait_for(5s) != std::future_status::ready) {
            return probe_error(ErrorCode::DeadlineExceeded, "probe interrupt gate timed out");
        }
        return {};
    };
    auto first = runtime.open_session(blocked_environment);
    MIRA_CHECK(first && first.value().command.outcome(2s));
    auto active_environment = std::make_shared<ProbeEnvironment>();
    auto second = runtime.open_session(active_environment);
    MIRA_CHECK(second && second.value().command.outcome(2s));
    auto task =
        runtime.submit_task(second.value().id, TaskSpec{.goal = "active", .user_constraints = {}});
    MIRA_CHECK(task && task.value().command.outcome(2s));
    auto blocking = runtime.close_session(first.value().id);
    MIRA_CHECK(blocking);
    MIRA_CHECK(entered.wait_for(2s) == std::future_status::ready);
    std::vector<CommandHandle> queued;
    for (unsigned index = 0; index < 7; ++index) {
        auto pause = runtime.pause_task(task.value().id);
        MIRA_CHECK(pause);
        queued.push_back(pause.value());
    }
    auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(!shutdown && shutdown.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(runtime.state() == RuntimeState::Stopping);
    auto denied = runtime.begin_operation(task.value().id, StepId::generate());
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::InvalidState);
    gate.open();
    auto report = runtime.finish_shutdown();
    MIRA_CHECK(report.clean && report.state == RuntimeState::Stopped);
    MIRA_CHECK(report.diagnostic.find("admission retry") != std::string::npos);
    MIRA_CHECK(report.pending_commands == 0);
    MIRA_CHECK(blocking.value().outcome(0ms));
    for (const auto &command : queued) {
        MIRA_CHECK(command.receipt(0ms));
        MIRA_CHECK(command.outcome(0ms));
    }
    MIRA_CHECK(runtime.task_snapshot(task.value().id).value().state == TaskState::Cancelled);
    MIRA_CHECK(active_environment->interrupts == 1);
    return 0;
}

int failed_release_is_not_reported_as_clean() {
    for (unsigned mode : {0U, 1U, 2U}) {
        MiraRuntime runtime({1, 16, 32});
        MIRA_CHECK(runtime.initialize());
        auto bad = std::make_shared<ProbeEnvironment>();
        bad->on_interrupt = [mode]() -> Result<void> {
            if (mode == 2)
                throw std::runtime_error("private platform failure");
            return probe_error(mode == 1 ? ErrorCode::DeadlineExceeded : ErrorCode::PlatformError,
                               "probe input release failed");
        };
        auto good = std::make_shared<ProbeEnvironment>();
        auto first = runtime.open_session(bad);
        auto second = runtime.open_session(good);
        MIRA_CHECK(first && second);
        MIRA_CHECK(first.value().command.outcome(2s) && second.value().command.outcome(2s));
        auto shutdown = runtime.request_shutdown();
        MIRA_CHECK(shutdown);
        auto outcome = shutdown.value().outcome(2s);
        MIRA_CHECK(outcome && outcome.value().status == SettlementStatus::Failed);
        MIRA_CHECK(outcome.value().error);
        MIRA_CHECK(outcome.value().error->code ==
                   (mode == 2
                        ? ErrorCode::Internal
                        : (mode == 1 ? ErrorCode::DeadlineExceeded : ErrorCode::PlatformError)));
        MIRA_CHECK(runtime.state() == RuntimeState::Failed);
        const auto report = runtime.finish_shutdown();
        MIRA_CHECK(!report.clean && report.state == RuntimeState::Failed);
        MIRA_CHECK(report.diagnostic.find("private platform failure") == std::string::npos);
        MIRA_CHECK(bad->interrupts == 1 && good->interrupts == 1);
        MIRA_CHECK(runtime.finish_shutdown().diagnostic == report.diagnostic);
        MIRA_CHECK(!runtime.request_shutdown());
        MIRA_CHECK(!runtime.initialize());
    }
    return 0;
}

int control_callback_cannot_finalize_itself() {
    MiraRuntime runtime;
    MIRA_CHECK(runtime.initialize());
    auto environment = std::make_shared<ProbeEnvironment>();
    ShutdownReport nested;
    environment->on_interrupt = [&]() -> Result<void> {
        nested = runtime.finish_shutdown();
        return {};
    };
    auto session = runtime.open_session(environment);
    MIRA_CHECK(session && session.value().command.outcome(2s));
    auto shutdown = runtime.request_shutdown();
    MIRA_CHECK(shutdown && shutdown.value().outcome(2s));
    MIRA_CHECK(!nested.clean && nested.state == RuntimeState::Stopping);
    MIRA_CHECK(nested.diagnostic.find("external owner") != std::string::npos);
    MIRA_CHECK(runtime.finish_shutdown().clean);
    return 0;
}

int initialization_and_stop_cannot_revive_finalized_runtime() {
    kairo::Executor callers;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    config.max_in_flight_tasks = 8;
    MIRA_CHECK(callers.initialize(config));
    for (unsigned round = 0; round < 24; ++round) {
        MiraRuntime runtime({1, 8, 16});
        std::promise<void> start;
        auto ready = start.get_future().share();
        auto init = callers.submit_auto([&] {
            ready.wait();
            return runtime.initialize();
        });
        auto stop = callers.submit_auto([&] {
            ready.wait();
            return runtime.request_shutdown();
        });
        start.set_value();
        // Only the test's external owner performs final teardown.
        auto report = runtime.finish_shutdown();
        MIRA_CHECK(init.wait_for(5s) == std::future_status::ready);
        MIRA_CHECK(stop.wait_for(5s) == std::future_status::ready);
        const auto initialized = init.get();
        const auto stopped = stop.get();
        MIRA_CHECK(initialized || initialized.error().code == ErrorCode::InvalidState);
        if (stopped) {
            auto settled = stopped.value().outcome(0ms);
            MIRA_CHECK(settled && settled.value().status == SettlementStatus::Applied);
        } else {
            MIRA_CHECK(stopped.error().code == ErrorCode::InvalidState);
        }
        MIRA_CHECK(report.clean && report.state == RuntimeState::Stopped);
        MIRA_CHECK(runtime.state() == RuntimeState::Stopped);
        MIRA_CHECK(!runtime.initialize());
    }
    MIRA_CHECK(callers.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(runtime_transition_contract_is_closed() == 0);
    MIRA_CHECK(uninitialized_and_failed_instances_can_be_finalized() == 0);
    MIRA_CHECK(normal_shutdown_closes_admission_and_is_idempotent() == 0);
    MIRA_CHECK(rejected_shutdown_still_settles_tasks() == 0);
    MIRA_CHECK(failed_release_is_not_reported_as_clean() == 0);
    MIRA_CHECK(control_callback_cannot_finalize_itself() == 0);
    MIRA_CHECK(initialization_and_stop_cannot_revive_finalized_runtime() == 0);
    return 0;
}
