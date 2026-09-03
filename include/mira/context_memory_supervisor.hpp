#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/task_checkpoint.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// ---------------------------------------------------------------------------
// Executor supervision for Context/Memory operations (M4-16)
// ---------------------------------------------------------------------------

// Operation classes and their shutdown semantics:
// - Critical: checkpoint writes and privacy erasure. Never silently dropped;
//   shutdown waits (bounded) for accepted critical work to settle.
// - Interactive: context preparation and retrieval on the Task path.
//   Deadline-bounded, rejected once closing starts.
// - Deferrable: consolidation, index rebuild and GC. Cancelled cooperatively
//   at shutdown; their futures resolve with a Cancelled error.
enum class SupervisedOpClass : std::uint8_t { Critical, Interactive, Deferrable };

[[nodiscard]] std::string supervised_op_class_name(SupervisedOpClass op_class);

// Cooperative cancellation probe handed to supervised operations. A tiny
// portable stand-in for std::stop_token: the Android NDK (API 24, r26)
// libc++ does not ship <stop_token>, and Mira's public headers must build
// on every supported platform. Deferrable operations receive a live probe;
// critical and interactive operations receive an always-false probe.
class SupervisorToken final {
  public:
    SupervisorToken() noexcept = default;
    // Constructed by the supervisor with its shared stop flag.
    explicit SupervisorToken(std::shared_ptr<const std::atomic<bool>> flag) noexcept
        : flag_(std::move(flag)) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<const std::atomic<bool>> flag_;
};

struct SupervisorConfig final {
    // Bounded in-flight operations; submissions beyond it fail with
    // ResourceExhausted instead of unbounded queueing.
    std::size_t max_in_flight = 64;
    std::chrono::milliseconds critical_drain_timeout{10'000};
    [[nodiscard]] Result<void> validate() const;
};

struct SupervisorStats final {
    std::uint64_t admitted = 0;
    std::uint64_t rejected_closed = 0;
    std::uint64_t rejected_capacity = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t events_emitted = 0;
    std::uint64_t event_sink_failures = 0;
};

struct SupervisorShutdownReport final {
    std::uint64_t critical_settled = 0;
    std::uint64_t critical_failed = 0;
    std::uint64_t deferrable_cancelled = 0;
    bool critical_drain_complete = true;
    std::string note;
};

// Routes Context/Memory work through the Executor with visible ownership,
// bounded admission, cooperative cancellation and the design §17.2 shutdown
// order: stop producers, cancel deferrable work, settle critical work, then
// let the host close the stores and the Executor.
class ContextMemorySupervisor final {
  public:
    // The Executor must outlive the supervisor. The optional event sink
    // receives sanitized diagnostic events (design Context/Memory §20); the
    // runtime/session ids stamp those events.
    ContextMemorySupervisor(executor::Executor &executor,
                            SupervisorConfig config = SupervisorConfig{},
                            IEventStore *event_sink = nullptr, RuntimeId runtime_id = RuntimeId{},
                            SessionId session_id = SessionId{});
    ~ContextMemorySupervisor();

    ContextMemorySupervisor(const ContextMemorySupervisor &) = delete;
    ContextMemorySupervisor &operator=(const ContextMemorySupervisor &) = delete;

    // Submits one operation. The returned future always resolves: with the
    // operation result, a rejection error, or a Cancelled error when a
    // deferrable operation is cancelled at shutdown.
    template <typename T>
    [[nodiscard]] std::future<Result<T>>
    submit(std::string label, SupervisedOpClass op_class,
           std::function<Result<T>(SupervisorToken)> op);

    // Convenience wrappers for the common routing decisions (§5 of the M4
    // plan). All of them are synchronous store/component calls wrapped in
    // Executor-managed tasks; futures must be consumed by the caller.
    [[nodiscard]] std::future<Result<std::optional<TaskCheckpoint>>>
    schedule_checkpoint(CheckpointCoordinator &coordinator, TaskId task, SessionId session,
                        CheckpointTrigger trigger, Timestamp now);
    [[nodiscard]] std::future<Result<MemoryQueryResult>>
    schedule_memory_query(IMemory &memory, MemoryQuery query);
    [[nodiscard]] std::future<Result<MemoryMutationResult>>
    schedule_mutation(IMemory &memory, MemoryMutation mutation);
    [[nodiscard]] std::future<Result<ErasureResult>>
    schedule_erasure(IMemory &memory, ErasureRequest request);
    [[nodiscard]] std::future<Result<MemoryCompactionResult>>
    schedule_retention_sweep(IMemory &memory, MemoryScope scope);

    // Executes the §17.2 shutdown order once; subsequent submissions are
    // rejected. Does not close stores or the Executor (host-owned).
    SupervisorShutdownReport begin_shutdown();

    [[nodiscard]] bool closed() const;
    [[nodiscard]] SupervisorStats stats() const;

  private:
    class Impl;
    // Type-erased admission path so the public template never needs the
    // Impl definition. The thunk resolves the caller's promise itself and
    // reports {failed, degraded} for accounting and event emission.
    [[nodiscard]] Result<void>
    submit_erased(const std::string &label, SupervisedOpClass op_class,
                  std::function<std::pair<bool, bool>(SupervisorToken)> op);

    std::unique_ptr<Impl> impl_;
};

namespace supervisor_detail {
[[nodiscard]] inline Error make_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.context_memory_supervisor";
    error.safe_message = message;
    return error;
}
} // namespace supervisor_detail

template <typename T>
std::future<Result<T>> ContextMemorySupervisor::submit(std::string label,
                                                       SupervisedOpClass op_class,
                                                       std::function<Result<T>(SupervisorToken)> op) {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    const auto rejection = submit_erased(
        label, op_class,
        [promise, op = std::move(op)](SupervisorToken token) mutable -> std::pair<bool, bool> {
            Result<T> outcome =
                Result<T>(supervisor_detail::make_error(ErrorCode::Internal,
                                                        "supervised operation produced no outcome"));
            try {
                if (op) {
                    outcome = op(token);
                } else {
                    outcome = Result<T>(supervisor_detail::make_error(
                        ErrorCode::InvalidArgument, "supervised operation is empty"));
                }
            } catch (const std::bad_alloc &) {
                outcome = Result<T>(supervisor_detail::make_error(
                    ErrorCode::ResourceExhausted, "supervised operation ran out of memory"));
            } catch (...) {
                outcome = Result<T>(supervisor_detail::make_error(
                    ErrorCode::Internal, "supervised operation failed with an exception"));
            }
            bool degraded = false;
            if constexpr (std::is_same_v<T, MemoryQueryResult>) {
                if (outcome.has_value()) {
                    degraded = outcome.value().quality.degraded;
                }
            }
            const bool failed = !outcome.has_value();
            promise->set_value(std::move(outcome));
            return {failed, degraded};
        });
    if (!rejection) {
        Error error = rejection.error();
        promise->set_value(Result<T>(std::move(error)));
    }
    return future;
}

} // namespace mira
