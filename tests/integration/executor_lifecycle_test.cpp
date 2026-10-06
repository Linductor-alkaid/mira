#include "support/test.hpp"

#include <mira/runtime_baseline.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace {

class BlockingProbe final : public kairo::IBlockingIoWorker {
  public:
    explicit BlockingProbe(std::promise<void> &started) : started_(started) {}

    void run(kairo::StopToken stop_token) override {
        started_.set_value();
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return stop_token.stop_requested() || wakeup_requested_; });
    }

    void wakeup() noexcept override {
        {
            std::lock_guard lock(mutex_);
            wakeup_requested_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::promise<void> &started_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool wakeup_requested_ = false;
};

} // namespace

int main() {
    using namespace std::chrono_literals;

    // The upstream facade now provides non-blocking serial dispatch for a
    // small multi-worker pool.
    mira::RuntimeBaseline multi_worker({2, 2, 8});
    MIRA_CHECK(multi_worker.initialize());
    MIRA_CHECK(multi_worker.request_shutdown());
    multi_worker.finish_shutdown();

    // Native Executor admission is now authoritative; the baseline exposes
    // the same in-flight count after each settled command.
    mira::RuntimeBaseline bounded({1, 1, 1});
    MIRA_CHECK(bounded.initialize());
    MIRA_CHECK(bounded.submit({10, 10, 0, mira::BaselineCommandKind::Command}).admitted);
    MIRA_CHECK(bounded.wait(10, 2s).code == mira::BaselineResultCode::Applied);
    MIRA_CHECK(bounded.status().in_flight == 0);
    MIRA_CHECK(bounded.submit({11, 11, 0, mira::BaselineCommandKind::Command}).admitted);
    MIRA_CHECK(bounded.wait(11, 2s).code == mira::BaselineResultCode::Applied);
    MIRA_CHECK(bounded.request_shutdown());
    bounded.finish_shutdown();

    mira::RuntimeBaseline runtime({1, 2, 8});
    MIRA_CHECK(runtime.initialize());

    MIRA_CHECK(runtime.submit({1, 9, 0, mira::BaselineCommandKind::Command}).admitted);
    MIRA_CHECK(runtime.wait(1, 2s).code == mira::BaselineResultCode::Applied);

    MIRA_CHECK(runtime.submit({2, 9, 0, mira::BaselineCommandKind::DiagnosticFailure}).admitted);
    MIRA_CHECK(runtime.wait(2, 2s).code == mira::BaselineResultCode::Failed);

    // Every admitted command settles promptly regardless of where the
    // cancellation lands: before start (queued -> Cancelled), while running
    // (cooperative token; the command still applies), or after completion
    // (idempotent Applied). Hitting the queued window through this facade
    // depends on worker dequeue timing -- an idle worker wins the race, so a
    // loop that must observe the queued window flakes on fast machines.
    // Only the interleaving-independent settlement contract is asserted here;
    // the queued-cancellation mechanism itself is proven deterministically in
    // the dedicated executor section at the end of this file.
    for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
        const auto command_id = 100 + attempt;
        MIRA_CHECK(
            runtime.submit({command_id, 1000 + attempt, 0, mira::BaselineCommandKind::Command})
                .admitted);
        const auto cancel = runtime.cancel(command_id);
        MIRA_CHECK(cancel.code == mira::BaselineResultCode::Cancelled ||
                   cancel.code == mira::BaselineResultCode::Applied);
        const auto result = runtime.wait(command_id, 2s);
        MIRA_CHECK(result.code == mira::BaselineResultCode::Applied ||
                   result.code == mira::BaselineResultCode::Cancelled);
    }
    // Observed command ids are retired: a late cancellation reports NotFound
    // instead of resurrecting terminal state.
    MIRA_CHECK(runtime.cancel(2).code == mira::BaselineResultCode::NotFound);

    MIRA_CHECK(runtime.request_shutdown());
    const auto rejected = runtime.submit({999, 1, 0, mira::BaselineCommandKind::Command});
    MIRA_CHECK(!rejected.admitted);
    MIRA_CHECK(rejected.rejection.has_value());
    runtime.finish_shutdown();
    const auto status = runtime.status();
    MIRA_CHECK(status.state == mira::BaselineRuntimeState::Stopped);
    MIRA_CHECK(status.in_flight == 0);
    MIRA_CHECK(status.unobserved_results == 0);

    // A closed context settles a future with an explicit stopping error.
    kairo::Executor direct_executor;
    kairo::ExecutorConfig direct_config;
    direct_config.min_threads = 1;
    direct_config.max_threads = 1;
    direct_config.queue_capacity = 1;
    direct_config.max_in_flight_tasks = 1;
    MIRA_CHECK(direct_executor.initialize(direct_config));

    std::promise<void> admission_release;
    auto admission_release_future = admission_release.get_future().share();
    auto admission_blocker =
        direct_executor.submit([&admission_release_future] { admission_release_future.wait(); });
    auto capacity_rejection = direct_executor.submit([] { return 2; });
    MIRA_CHECK(capacity_rejection.wait_for(2s) == std::future_status::ready);
    bool saw_capacity = false;
    try {
        static_cast<void>(capacity_rejection.get());
    } catch (const kairo::CapacityExhaustedException &) {
        saw_capacity = true;
    }
    MIRA_CHECK(saw_capacity);
    admission_release.set_value();
    admission_blocker.get();
    MIRA_CHECK(direct_executor.get_in_flight_submissions() == 0);

    kairo::SerialExecutionContext stopped_context;
    stopped_context.shutdown();
    auto stopped_future = direct_executor.submit_on(stopped_context, [] { return 1; });
    bool saw_context_stopped = false;
    try {
        static_cast<void>(stopped_future.get());
    } catch (const kairo::ExecutorStopping &) {
        saw_context_stopped = true;
    }
    MIRA_CHECK(saw_context_stopped);

    // Long-lived and timed paths have separate handles and must be stopped
    // before final facade shutdown; default-pool idle is not used as proof.
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    kairo::BlockingIoConfig io_config;
    io_config.thread_name = "mira-m0-io";
    kairo::BlockingWorkerSpec worker_spec;
    worker_spec.name = "mira-m0-io";
    worker_spec.config = io_config;
    worker_spec.worker = std::make_unique<BlockingProbe>(worker_started);
    auto worker = direct_executor.start_worker(std::move(worker_spec));
    MIRA_CHECK(worker.started());
    MIRA_CHECK(worker_started_future.wait_for(2s) == std::future_status::ready);
    worker.request_stop();
    worker.stop();
    MIRA_CHECK(!worker.status().is_running);

    std::atomic<std::uint64_t> realtime_cycles{0};
    kairo::RealtimeThreadConfig realtime_config;
    realtime_config.thread_name = "mira-m0-rt";
    realtime_config.cycle_period_ns = 1'000'000;
    realtime_config.cycle_callback = [&] {
        realtime_cycles.fetch_add(1, std::memory_order_relaxed);
    };
    MIRA_CHECK(direct_executor.register_realtime_task("mira-m0-rt", realtime_config));
    MIRA_CHECK(direct_executor.start_realtime_task("mira-m0-rt"));
    const auto realtime_deadline = std::chrono::steady_clock::now() + 2s;
    while (realtime_cycles.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < realtime_deadline) {
        std::this_thread::yield();
    }
    MIRA_CHECK(realtime_cycles.load(std::memory_order_relaxed) > 0);
    direct_executor.stop_realtime_task("mira-m0-rt");

    auto delayed = direct_executor.submit_delayed(10'000, [] { return 7; });
    MIRA_CHECK(delayed.handle.cancel() == kairo::TimerOperationResult::CancelledBeforeDispatch);
    bool saw_timer_cancelled = false;
    try {
        static_cast<void>(delayed.future.get());
    } catch (const kairo::TaskCancelled &) {
        saw_timer_cancelled = true;
    }
    MIRA_CHECK(saw_timer_cancelled);
    MIRA_CHECK(direct_executor.shutdown(true) == kairo::ShutdownResult::Completed);

    // Queued cancellation, deterministically: the occupier pins the only
    // worker, so the victim is provably still queued when the cancel request
    // lands. The executor contract then guarantees the victim never runs and
    // its future settles with TaskCancelled -- the same semantics the runtime
    // baseline maps to a Cancelled command result.
    kairo::Executor queued_cancel_executor;
    kairo::ExecutorConfig queued_cancel_config;
    queued_cancel_config.min_threads = 1;
    queued_cancel_config.max_threads = 1;
    queued_cancel_config.queue_capacity = 2;
    queued_cancel_config.max_in_flight_tasks = 2;
    MIRA_CHECK(queued_cancel_executor.initialize(queued_cancel_config));

    std::promise<void> occupier_started;
    auto occupier_started_future = occupier_started.get_future().share();
    std::promise<void> occupier_release;
    auto occupier_release_future = occupier_release.get_future().share();
    auto occupier = queued_cancel_executor.submit_with_handle([&] {
        occupier_started.set_value();
        occupier_release_future.wait();
    });
    MIRA_CHECK(occupier_started_future.wait_for(2s) == std::future_status::ready);

    auto victim = queued_cancel_executor.submit_with_handle([] { return 1; });
    MIRA_CHECK(queued_cancel_executor.request_task_cancel(victim.handle).result ==
               kairo::TaskCancellationResult::RequestedBeforeStart);
    occupier_release.set_value();
    MIRA_CHECK(occupier.future.wait_for(2s) == std::future_status::ready);
    bool victim_cancelled = false;
    try {
        MIRA_CHECK(victim.future.wait_for(2s) == std::future_status::ready);
        static_cast<void>(victim.future.get());
    } catch (const kairo::TaskCancelled &) {
        victim_cancelled = true;
    }
    MIRA_CHECK(victim_cancelled);
    MIRA_CHECK(queued_cancel_executor.get_in_flight_submissions() == 0);
    MIRA_CHECK(queued_cancel_executor.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}
