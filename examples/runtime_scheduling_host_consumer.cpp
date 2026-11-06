#include <kairo/executor.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace mira;
using namespace std::chrono_literals;

constexpr std::array routing_fields{
    std::pair{"accepted", &SchedulingMetricsSnapshot::accepted},
    std::pair{"accepted_degraded", &SchedulingMetricsSnapshot::accepted_degraded},
    std::pair{"rejected", &SchedulingMetricsSnapshot::rejected},
    std::pair{"deadline_rejected", &SchedulingMetricsSnapshot::deadline_rejected},
    std::pair{"backend_unavailable_rejected",
              &SchedulingMetricsSnapshot::backend_unavailable_rejected},
    std::pair{"capacity_rejected", &SchedulingMetricsSnapshot::capacity_rejected},
    std::pair{"resource_rejected", &SchedulingMetricsSnapshot::resource_rejected},
    std::pair{"affinity_mismatch", &SchedulingMetricsSnapshot::affinity_mismatch},
    std::pair{"deadline_missed", &SchedulingMetricsSnapshot::deadline_missed},
    std::pair{"feedback_reported", &SchedulingMetricsSnapshot::feedback_reported},
    std::pair{"adaptive_history", &SchedulingMetricsSnapshot::adaptive_history},
    std::pair{"load_shedding_rejected", &SchedulingMetricsSnapshot::load_shedding_rejected},
    std::pair{"priority_promoted", &SchedulingMetricsSnapshot::priority_promoted}};

bool check(bool condition, const char *label) {
    if (!condition)
        std::cerr << "scheduling host validation failed: " << label << '\n';
    return condition;
}

struct Sample final {
    SchedulingMetricsSnapshot routing;
    RuntimeState state;
    unsigned submitted = 0;
    unsigned admitted_handles = 0;
    unsigned submit_rejected = 0;
    unsigned capacity_rejected = 0;
    unsigned stopping_rejected = 0;
    unsigned pending = 0;
    unsigned settled = 0;
    unsigned failed = 0;
};

// Bounded local diagnostics, not a global task ledger or a scheduling queue.
class HostView final {
  public:
    template <class T> bool record(const Result<T> &result) {
        ++submitted_;
        if (!result) {
            ++rejected_;
            capacity_ += result.error().code == ErrorCode::ResourceExhausted;
            stopping_ += result.error().code == ErrorCode::InvalidState;
            return true;
        }
        if (!check(handles_.size() < 16, "host retention bound"))
            return false;
        if constexpr (std::is_same_v<T, CommandHandle>)
            handles_.push_back(result.value());
        else
            handles_.push_back(result.value().command);
        return true;
    }

    Sample sample(kairo::Executor &sampler, const MiraRuntime &runtime) const {
        // Capture host-owned state by value; the Runtime owner keeps the
        // instance alive until this future is consumed. No records_mutex reads.
        auto pending = sampler.submit_auto([view = *this, &runtime] {
            Sample result{runtime.scheduling_metrics(), runtime.state()};
            result.submitted = view.submitted_;
            result.admitted_handles = static_cast<unsigned>(view.handles_.size());
            result.submit_rejected = view.rejected_;
            result.capacity_rejected = view.capacity_;
            result.stopping_rejected = view.stopping_;
            for (const auto &handle : view.handles_) {
                auto outcome = handle.outcome(0ms);
                if (!outcome && outcome.error().code == ErrorCode::DeadlineExceeded) {
                    ++result.pending; // A poll is not an execution-timeout counter.
                } else {
                    ++result.settled;
                    result.failed += !outcome || outcome.value().status == SettlementStatus::Failed;
                }
            }
            return result;
        });
        return pending.get();
    }

  private:
    unsigned submitted_ = 0, rejected_ = 0, capacity_ = 0, stopping_ = 0;
    std::vector<CommandHandle> handles_;
};

bool serial_routing_is_zero(const Sample &sample) {
    for (const auto &[name, member] : routing_fields) {
        static_cast<void>(name);
        if (sample.routing.*member != 0)
            return false;
    }
    return true;
}

void print(std::string_view stage, const Sample &sample) {
    std::cout << stage << " runtime_state=" << static_cast<unsigned>(sample.state);
    for (const auto &[name, member] : routing_fields)
        std::cout << " runtime.routing." << name << '=' << sample.routing.*member;
    std::cout << " host.submitted=" << sample.submitted
              << " host.admitted_handles=" << sample.admitted_handles
              << " host.submit_rejected=" << sample.submit_rejected
              << " host.capacity_rejected=" << sample.capacity_rejected
              << " host.stopping_rejected=" << sample.stopping_rejected
              << " host.pending=" << sample.pending << " host.settled=" << sample.settled
              << " host.failed=" << sample.failed << '\n';
}

struct Gate final {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    bool opened = false; // External owner only.
    void open() {
        if (!opened) {
            opened = true;
            release.set_value();
        }
    }
};

class FixtureEnvironment final : public IEnvironment {
  public:
    explicit FixtureEnvironment(Gate *gate = nullptr, bool fail = false)
        : gate_(gate), fail_(fail) {}
    EnvironmentCapabilities capabilities() const override { return simulator_.capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        return simulator_.observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        return simulator_.execute(input, context);
    }
    Result<void> interrupt(const OperationContext &) override {
        if (fail_)
            throw std::runtime_error("fixture interrupt failure");
        if (gate_ && interrupts_.fetch_add(1) == 0) {
            gate_->entered.set_value();
            if (gate_->released.wait_for(5s) != std::future_status::ready) {
                Error error;
                error.code = ErrorCode::DeadlineExceeded;
                error.domain = "mira.scheduling-fixture";
                error.safe_message = "fixture gate wait expired";
                return error;
            }
        }
        return {};
    }

  private:
    adapters::simulator::SimulatorEnvironment simulator_{
        adapters::simulator::SimulatorSetup::single_display()};
    Gate *gate_;
    bool fail_;
    std::atomic<unsigned> interrupts_{0};
};

bool applied(const Result<CommandHandle> &submission) {
    if (!submission)
        return false;
    auto result = submission.value().outcome(2s);
    return result && result.value().status == SettlementStatus::Applied;
}

bool overload_and_shutdown(kairo::Executor &sampler) {
    Gate gate;
    auto entered = gate.entered.get_future();
    MiraRuntime runtime({1, 32, 4});
    struct ReleaseOnExit final {
        Gate &gate;
        ~ReleaseOnExit() { gate.open(); }
    } release_on_exit{gate}; // Unblock before Runtime teardown on every exit path.
    HostView view;
    const auto before = view.sample(sampler, runtime);
    if (!check(serial_routing_is_zero(before) && before.pending == 0, "before initialize") ||
        !check(runtime.initialize().has_value(), "initialize"))
        return false;
    auto first = runtime.open_session(std::make_shared<FixtureEnvironment>(&gate));
    auto second = runtime.open_session(std::make_shared<FixtureEnvironment>());
    if (!view.record(first) || !view.record(second) || !check(first && second, "sessions") ||
        !check(first.value().command.outcome(2s).has_value() &&
                   second.value().command.outcome(2s).has_value(),
               "session outcomes"))
        return false;
    auto task = runtime.submit_task(
        second.value().id, TaskSpec{.goal = "offline metric fixture", .user_constraints = {}});
    if (!view.record(task) || !check(task && task.value().command.outcome(2s), "task"))
        return false;
    auto takeover = runtime.request_human_takeover(second.value().id);
    if (!view.record(takeover) || !check(applied(takeover), "takeover"))
        return false;
    auto resume = runtime.release_human_takeover(second.value().id);
    if (!view.record(resume) || !check(applied(resume), "resume"))
        return false;
    const auto idle = view.sample(sampler, runtime);

    auto blocking = runtime.close_session(first.value().id);
    if (!view.record(blocking) || !check(blocking.has_value(), "blocking command") ||
        !check(entered.wait_for(2s) == std::future_status::ready, "gate entered"))
        return false;
    for (unsigned index = 0; index < 3; ++index) {
        auto pause =
            index == 1 ? runtime.resume_task(task.value().id) : runtime.pause_task(task.value().id);
        if (!view.record(pause) || !check(pause.has_value(), "queued command"))
            return false;
    }
    auto overloaded = runtime.pause_task(task.value().id);
    if (!view.record(overloaded) ||
        !check(!overloaded && overloaded.error().code == ErrorCode::ResourceExhausted, "capacity"))
        return false;
    auto waited = blocking.value().outcome(1ms);
    if (!check(!waited && waited.error().code == ErrorCode::DeadlineExceeded,
               "wait timeout does not terminate callback"))
        return false;
    auto shutdown = runtime.request_shutdown();
    if (!view.record(shutdown) ||
        !check(!shutdown && shutdown.error().code == ErrorCode::ResourceExhausted,
               "shutdown capacity rejection"))
        return false;
    auto stopping = runtime.pause_task(task.value().id);
    if (!view.record(stopping) ||
        !check(!stopping && stopping.error().code == ErrorCode::InvalidState, "stopping rejection"))
        return false;
    const auto busy = view.sample(sampler, runtime);
    if (!check(busy.pending == 4 && busy.capacity_rejected == 2 && busy.stopping_rejected == 1 &&
                   busy.submit_rejected == 3 && busy.state == RuntimeState::Stopping &&
                   serial_routing_is_zero(busy),
               "overload snapshot"))
        return false;
    gate.open();
    const auto report = runtime.finish_shutdown();
    const auto stopped = view.sample(sampler, runtime);
    if (!check(report.clean && report.pending_commands == 0 &&
                   report.diagnostic.find("admission retry") != std::string::npos,
               "shutdown convergence") ||
        !check(stopped.pending == 0 && stopped.failed == 0 && stopped.settled == 9 &&
                   stopped.submitted == 12 && stopped.state == RuntimeState::Stopped &&
                   serial_routing_is_zero(stopped),
               "settled snapshot") ||
        !check(runtime.task_snapshot(task.value().id).value().state == TaskState::Cancelled,
               "task cancelled"))
        return false;
    // Immutable samples are consumed only after control convergence. No stdout
    // work or diagnostic callbacks run inside the Runtime control context.
    print("before", before);
    print("idle", idle);
    print("overloaded-stopping", busy);
    print("stopped", stopped);
    return true;
}

bool callback_failure(kairo::Executor &sampler) {
    MiraRuntime runtime;
    HostView view;
    if (!check(runtime.initialize().has_value(), "failure runtime initialize"))
        return false;
    auto session = runtime.open_session(std::make_shared<FixtureEnvironment>(nullptr, true));
    if (!view.record(session) ||
        !check(session && session.value().command.outcome(2s), "failure session"))
        return false;
    auto takeover = runtime.request_human_takeover(session.value().id);
    if (!view.record(takeover) || !check(takeover.has_value(), "throwing callback admitted"))
        return false;
    auto outcome = takeover.value().outcome(2s);
    if (!check(outcome && outcome.value().status == SettlementStatus::Failed &&
                   outcome.value().error && outcome.value().error->code == ErrorCode::Internal,
               "callback failure visible"))
        return false;
    auto shutdown = runtime.request_shutdown();
    if (!view.record(shutdown) || !check(shutdown.has_value(), "failure shutdown admitted"))
        return false;
    auto stop = shutdown.value().outcome(2s);
    const auto report = runtime.finish_shutdown();
    const auto failed = view.sample(sampler, runtime);
    if (!check(stop && stop.value().status == SettlementStatus::Failed && !report.clean &&
                   report.state == RuntimeState::Failed && report.pending_commands == 0 &&
                   failed.pending == 0 && failed.failed == 2 && serial_routing_is_zero(failed),
               "failed shutdown remains visible"))
        return false;
    print("callback-failure", failed); // Do not print platform exception messages.
    return true;
}
} // namespace

int main() {
    kairo::Executor sampler;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    if (!sampler.initialize(config))
        return 1;
    bool ok = false;
    try {
        ok = overload_and_shutdown(sampler) && callback_failure(sampler);
        // This owner routes sampling tasks; it is not the Runtime's Executor.
        const auto sampling = sampler.get_scheduling_metrics();
        ok = check(sampling.accepted_count > 0, "independent sampler routing") && ok;
        std::cout << "sampler_executor.routing.accepted=" << sampling.accepted_count << '\n';
    } catch (...) {
        std::cerr << "scheduling host sampling/submission failed\n";
        ok = false;
    }
    if (sampler.shutdown(true) != kairo::ShutdownResult::Completed)
        ok = false;
    return ok ? 0 : 1;
}
