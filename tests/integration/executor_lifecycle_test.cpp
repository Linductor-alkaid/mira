#include "support/test.hpp"

#include <mira/runtime_baseline.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace {

class BlockingProbe final : public executor::IBlockingIoWorker {
public:
    explicit BlockingProbe(std::promise<void> &started) : started_(started) {}

    void run(executor::StopToken stop_token) override {
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

    // EXE-20260830-002/003 compatibility dispatch supports a small multi-worker
    // pool without blocking facade wrappers.
    mira::RuntimeBaseline multi_worker({2, 2, 8});
    MIRA_CHECK(multi_worker.initialize());
    MIRA_CHECK(multi_worker.request_shutdown());
    multi_worker.finish_shutdown();

    // EXE-20260830-001: Mira rejects beyond its explicit in-flight bound even
    // though Executor's local worker queue capacity is not a total bound.
    mira::RuntimeBaseline bounded({1, 1, 1});
    MIRA_CHECK(bounded.initialize());
    MIRA_CHECK(bounded.submit({10, 10, 0, mira::BaselineCommandKind::Command}).admitted);
    const auto capacity_rejection =
        bounded.submit({11, 11, 0, mira::BaselineCommandKind::Command});
    MIRA_CHECK(!capacity_rejection.admitted);
    MIRA_CHECK(capacity_rejection.rejection.has_value());
    MIRA_CHECK(capacity_rejection.rejection->code == mira::BaselineResultCode::Rejected);
    MIRA_CHECK(bounded.wait(10, 2s).code == mira::BaselineResultCode::Applied);
    MIRA_CHECK(bounded.request_shutdown());
    bounded.finish_shutdown();

    mira::RuntimeBaseline runtime({1, 2, 8});
    MIRA_CHECK(runtime.initialize());

    MIRA_CHECK(runtime.submit({1, 9, 0, mira::BaselineCommandKind::Command}).admitted);
    MIRA_CHECK(runtime.wait(1, 2s).code == mira::BaselineResultCode::Applied);

    MIRA_CHECK(runtime.submit({2, 9, 0, mira::BaselineCommandKind::DiagnosticFailure}).admitted);
    MIRA_CHECK(runtime.wait(2, 2s).code == mira::BaselineResultCode::Failed);

    bool saw_queued_cancellation = false;
    for (std::uint64_t attempt = 0; attempt < 64 && !saw_queued_cancellation; ++attempt) {
        const auto command_id = 100 + attempt;
        MIRA_CHECK(runtime.submit({command_id, 1000 + attempt, 0,
                                   mira::BaselineCommandKind::Command})
                       .admitted);
        const auto cancel = runtime.cancel(command_id);
        const auto result = runtime.wait(command_id, 2s);
        saw_queued_cancellation = cancel.code == mira::BaselineResultCode::Cancelled &&
                                  result.code == mira::BaselineResultCode::Cancelled;
    }
    MIRA_CHECK(saw_queued_cancellation);

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
    executor::Executor direct_executor;
    executor::ExecutorConfig direct_config;
    direct_config.min_threads = 1;
    direct_config.max_threads = 1;
    direct_config.queue_capacity = 1;
    MIRA_CHECK(direct_executor.initialize(direct_config));
    executor::SerialExecutionContext stopped_context;
    stopped_context.shutdown();
    auto stopped_future = direct_executor.submit_on(stopped_context, [] { return 1; });
    bool saw_context_stopped = false;
    try {
        static_cast<void>(stopped_future.get());
    } catch (const executor::ExecutorStopping &) {
        saw_context_stopped = true;
    }
    MIRA_CHECK(saw_context_stopped);

    // Long-lived and timed paths have separate handles and must be stopped
    // before final facade shutdown; default-pool idle is not used as proof.
    std::promise<void> worker_started;
    auto worker_started_future = worker_started.get_future();
    executor::BlockingIoConfig io_config;
    io_config.thread_name = "mira-m0-io";
    executor::BlockingWorkerSpec worker_spec;
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
    executor::RealtimeThreadConfig realtime_config;
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

    auto delayed = direct_executor.submit_delayed_with_handle(10'000, [] { return 7; });
    MIRA_CHECK(delayed.handle.cancel() == executor::TimerOperationResult::CancelledBeforeDispatch);
    bool saw_timer_cancelled = false;
    try {
        static_cast<void>(delayed.future.get());
    } catch (const executor::TaskCancelled &) {
        saw_timer_cancelled = true;
    }
    MIRA_CHECK(saw_timer_cancelled);
    MIRA_CHECK(direct_executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}
