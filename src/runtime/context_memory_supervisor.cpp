#include <mira/context_memory_supervisor.hpp>

#include <mira/json.hpp>

#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <executor/executor.hpp>

namespace mira {

namespace {

[[nodiscard]] Error supervisor_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.context_memory_supervisor";
    error.safe_message = std::move(message);
    return error;
}

// Design Context/Memory §20 event names for the supervised labels; unknown
// labels fall back to the generic operation events.
[[nodiscard]] std::string started_event_for(const std::string &label) {
    if (label == "context_prepare") {
        return "ContextBuildStarted";
    }
    if (label == "checkpoint") {
        return "ContextCheckpointRequested";
    }
    if (label == "memory_query") {
        return "MemoryQueryStarted";
    }
    if (label == "mutation") {
        return "MemoryMutationProposed";
    }
    if (label == "erasure") {
        return "MemoryErasureRequested";
    }
    if (label == "consolidation") {
        return "MemoryConsolidationStarted";
    }
    return "ContextMemoryOperationStarted";
}

[[nodiscard]] std::string finished_event_for(const std::string &label, bool failed,
                                             bool cancelled, bool degraded) {
    if (failed) {
        if (label == "context_prepare") {
            return "ContextBuildFailed";
        }
        if (label == "checkpoint") {
            return "ContextCheckpointFailed";
        }
        if (label == "consolidation") {
            return "MemoryConsolidationFailed";
        }
        if (label == "memory_query") {
            return "MemoryQueryDegraded";
        }
        return "ContextMemoryOperationFailed";
    }
    if (cancelled) {
        return "ContextMemoryOperationCancelled";
    }
    if (degraded && label == "memory_query") {
        return "MemoryQueryDegraded";
    }
    if (label == "checkpoint") {
        return "ContextCheckpointStored";
    }
    if (label == "mutation") {
        return "MemoryMutationApplied";
    }
    if (label == "erasure") {
        return "MemoryErased";
    }
    if (label == "consolidation") {
        return "MemoryConsolidationFinished";
    }
    return "ContextMemoryOperationFinished";
}

} // namespace

std::string supervised_op_class_name(SupervisedOpClass op_class) {
    switch (op_class) {
    case SupervisedOpClass::Critical:
        return "Critical";
    case SupervisedOpClass::Interactive:
        return "Interactive";
    case SupervisedOpClass::Deferrable:
        return "Deferrable";
    }
    return "Unknown";
}

Result<void> SupervisorConfig::validate() const {
    if (max_in_flight == 0) {
        return supervisor_error(ErrorCode::InvalidArgument,
                                "supervisor in-flight bound must be positive");
    }
    if (critical_drain_timeout.count() < 0) {
        return supervisor_error(ErrorCode::InvalidArgument,
                                "critical drain timeout must not be negative");
    }
    return Result<void>{};
}

class ContextMemorySupervisor::Impl final {
  public:
    Impl(executor::Executor &executor, SupervisorConfig config, IEventStore *event_sink,
         RuntimeId runtime_id, SessionId session_id)
        : executor_(executor), config_(config), event_sink_(event_sink),
          runtime_id_(runtime_id), session_id_(session_id) {}

    // Returns success when the thunk was admitted; the rejection otherwise.
    // The thunk invokes `settle` (accounting + events) before resolving the
    // caller's promise, so a resolved future implies settled accounting.
    [[nodiscard]] Result<void> dispatch(
        const std::string &label, SupervisedOpClass op_class,
        std::function<void(SupervisorToken,
                           const std::function<void(bool, bool, bool)> &)> thunk) {
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                ++stats_.rejected_closed;
                return supervisor_error(ErrorCode::Unavailable,
                                        "supervisor no longer accepts operations");
            }
            if (in_flight_ >= config_.max_in_flight) {
                ++stats_.rejected_capacity;
                return supervisor_error(ErrorCode::ResourceExhausted,
                                        "supervisor in-flight bound reached");
            }
            ++in_flight_;
            ++stats_.admitted;
            emit_locked(label, started_event_for(label), op_class, "admitted");
        }
        const SupervisorToken token =
            op_class == SupervisedOpClass::Deferrable
                ? SupervisorToken(deferrable_stop_)
                : SupervisorToken{};
        const auto settle = [this, label, op_class](bool failed, bool degraded, bool cancelled) {
            std::lock_guard lock(mutex_);
            if (cancelled) {
                ++stats_.cancelled;
                emit_locked(label, finished_event_for(label, false, true, false), op_class,
                            "cancelled");
                return;
            }
            if (failed) {
                ++stats_.failed;
            } else {
                ++stats_.completed;
            }
            emit_locked(label, finished_event_for(label, failed, false, degraded), op_class,
                        failed ? "failed" : "completed");
        };
        try {
            auto future = executor_.submit_auto([this, thunk = std::move(thunk), token,
                                                 settle]() mutable {
                thunk(token, settle);
                std::lock_guard lock(mutex_);
                --in_flight_;
            });
            std::lock_guard lock(mutex_);
            futures_.push_back(std::move(future));
            prune_ready_futures_locked();
            return Result<void>{};
        } catch (...) {
            std::lock_guard lock(mutex_);
            --in_flight_;
            ++stats_.failed;
            return supervisor_error(ErrorCode::Unavailable,
                                    "executor rejected the supervised operation");
        }
    }

    SupervisorShutdownReport begin_shutdown() {
        SupervisorShutdownReport report;
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        deferrable_stop_->store(true, std::memory_order_release);
        // Wait for admitted work to settle, bounded by the drain timeout.
        const auto deadline = std::chrono::steady_clock::now() + config_.critical_drain_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(mutex_);
                prune_ready_futures_locked();
                if (in_flight_ == 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Consume the retained executor futures; thunks never throw.
        std::vector<std::future<void>> pending;
        {
            std::lock_guard lock(mutex_);
            pending = std::move(futures_);
            futures_.clear();
        }
        for (auto &future : pending) {
            if (future.valid()) {
                future.wait();
            }
        }
        {
            std::lock_guard lock(mutex_);
            report.critical_settled = stats_.completed;
            report.critical_failed = stats_.failed;
            report.deferrable_cancelled = stats_.cancelled;
            report.critical_drain_complete = in_flight_ == 0;
            if (!report.critical_drain_complete) {
                report.note = "critical drain timed out; " + std::to_string(in_flight_) +
                              " operations still in flight";
            }
        }
        return report;
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closing_;
    }

    [[nodiscard]] SupervisorStats stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

  private:
    void prune_ready_futures_locked() {
        std::erase_if(futures_, [](std::future<void> &future) {
            return !future.valid() ||
                   future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    }

    // Requires mutex_ held. Sink failures never propagate to the hot path.
    void emit_locked(const std::string &label, const std::string &type,
                     SupervisedOpClass op_class, const std::string &outcome) {
        if (event_sink_ == nullptr || runtime_id_.is_nil() || session_id_.is_nil()) {
            return;
        }
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = runtime_id_;
        request.session_id = session_id_;
        request.payload.type = type;
        JsonValue::Object object;
        object.emplace_back("label", label);
        object.emplace_back("class", supervised_op_class_name(op_class));
        object.emplace_back("outcome", outcome);
        request.payload.data = to_json_string(JsonValue(std::move(object)));
        request.payload.classification = EventClass::Diagnostic;
        auto receipt = event_sink_->append(request);
        if (receipt) {
            ++stats_.events_emitted;
        } else {
            ++stats_.event_sink_failures;
        }
    }

    executor::Executor &executor_;
    SupervisorConfig config_;
    IEventStore *event_sink_ = nullptr;
    RuntimeId runtime_id_;
    SessionId session_id_;
    mutable std::mutex mutex_;
    std::size_t in_flight_ = 0;
    bool closing_ = false;
    std::shared_ptr<std::atomic<bool>> deferrable_stop_ =
        std::make_shared<std::atomic<bool>>(false);
    std::vector<std::future<void>> futures_;
    SupervisorStats stats_;
};

ContextMemorySupervisor::ContextMemorySupervisor(executor::Executor &executor,
                                                 SupervisorConfig config,
                                                 IEventStore *event_sink, RuntimeId runtime_id,
                                                 SessionId session_id) {
    const auto valid = config.validate();
    if (!valid) {
        throw std::invalid_argument(valid.error().safe_message);
    }
    impl_ = std::make_unique<Impl>(executor, config, event_sink, runtime_id, session_id);
}

ContextMemorySupervisor::~ContextMemorySupervisor() {
    (void)impl_->begin_shutdown();
}

Result<void> ContextMemorySupervisor::submit_erased(
    const std::string &label, SupervisedOpClass op_class,
    std::function<void(SupervisorToken,
                       const std::function<void(bool failed, bool degraded, bool cancelled)>
                           &settle)> op) {
    return impl_->dispatch(label, op_class, std::move(op));
}

std::future<Result<std::optional<TaskCheckpoint>>>
ContextMemorySupervisor::schedule_checkpoint(CheckpointCoordinator &coordinator, TaskId task,
                                             SessionId session, CheckpointTrigger trigger,
                                             Timestamp now) {
    return submit<std::optional<TaskCheckpoint>>(
        "checkpoint", SupervisedOpClass::Critical,
        [&coordinator, task, session, trigger, now](SupervisorToken) {
            return coordinator.checkpoint(task, session, trigger, now);
        });
}

std::future<Result<MemoryQueryResult>>
ContextMemorySupervisor::schedule_memory_query(IMemory &memory, MemoryQuery query) {
    return submit<MemoryQueryResult>(
        "memory_query", SupervisedOpClass::Interactive,
        [&memory, query = std::move(query)](SupervisorToken) { return memory.query(query); });
}

std::future<Result<ContextRetrievalResult>>
ContextMemorySupervisor::schedule_context_retrieval(IContextRetriever &retriever,
                                                    ContextQuery query, RetrievalBudget budget) {
    return submit<ContextRetrievalResult>(
        "context_retrieval", SupervisedOpClass::Interactive,
        [&retriever, query = std::move(query),
         budget](SupervisorToken) -> Result<ContextRetrievalResult> {
            return retriever.retrieve(query, budget);
        });
}

std::future<Result<std::vector<RankedContextItem>>>
ContextMemorySupervisor::schedule_context_rerank(IContextReranker &reranker, ContextQuery query,
                                                 std::vector<ContextCandidate> candidates) {
    return submit<std::vector<RankedContextItem>>(
        "context_rerank", SupervisedOpClass::Interactive,
        [&reranker, query = std::move(query),
         candidates = std::move(candidates)](SupervisorToken) -> Result<std::vector<RankedContextItem>> {
            return reranker.rerank(query, candidates);
        });
}

std::future<Result<ConversationCheckpoint>>
ContextMemorySupervisor::schedule_context_consolidation(ISemanticConsolidator &consolidator,
                                                        ConversationSegment segment,
                                                        ConsolidationOptions options) {
    return submit<ConversationCheckpoint>(
        "context_consolidation", SupervisedOpClass::Deferrable,
        [&consolidator, segment = std::move(segment),
         options = std::move(options)](SupervisorToken token) mutable
        -> Result<ConversationCheckpoint> {
            // The supervisor's stop flag becomes the cooperative cancellation
            // probe the consolidator honors (design §7); the five-tuple
            // commit validation stays the backstop for results that slip
            // past cancellation.
            options.cancellation_requested = [token]() { return token.stop_requested(); };
            return consolidator.consolidate(segment, options);
        });
}

std::future<Result<WorkingContextCommitOutcome>>
ContextMemorySupervisor::schedule_working_context_commit(IWorkingContextStore &store,
                                                         ConversationCheckpoint checkpoint,
                                                         WorkingContextIdentity identity,
                                                         WorkingContextCommitState live,
                                                         WorkingContextMergeOptions options) {
    return submit<WorkingContextCommitOutcome>(
        "working_context_commit", SupervisedOpClass::Deferrable,
        // WorkingContextMergeOptions is trivially copyable: a plain capture
        // copy, no move (performance-move-const-arg).
        [&store, checkpoint = std::move(checkpoint), identity, live, options](
            SupervisorToken) mutable
        -> Result<WorkingContextCommitOutcome> {
            // Deterministic projection plus monotonic commit in one
            // supervised step (design §8): the store's tuple validation is
            // the backstop for late or replayed results, so the pure merge
            // needs no extra cancellation plumbing. Stage W2 replaces the
            // projection with a model-mediated curator behind this same
            // route, keeping the shutdown semantics unchanged.
            auto candidate = working_context_from_checkpoint(checkpoint, identity, options);
            if (!candidate) {
                return candidate.error();
            }
            return commit_working_context(store, candidate.value(), live);
        });
}

std::future<Result<WorkingContextCommitOutcome>>
ContextMemorySupervisor::schedule_working_context_curate(
    IContextCurator &curator, IWorkingContextStore &store,
    std::optional<WorkingContextSnapshot> previous, ConversationCheckpoint checkpoint,
    std::vector<ConversationSegmentEntry> recent_events, WorkingContextCommitState live,
    ContextCurationOptions options) {
    return submit<WorkingContextCommitOutcome>(
        "working_context_curate", SupervisedOpClass::Deferrable,
        [&curator, &store, previous = std::move(previous), checkpoint = std::move(checkpoint),
         events = std::move(recent_events), live,
         options = std::move(options)](SupervisorToken token) mutable
        -> Result<WorkingContextCommitOutcome> {
            // The supervisor's stop flag becomes the cooperative cancellation
            // probe the curator honors (design §8); the store's commit
            // validation stays the backstop for results that slip past
            // cancellation. A curator failure leaves the store untouched —
            // the caller keeps the previous snapshot (design §9).
            options.cancellation_requested = [token]() { return token.stop_requested(); };
            auto candidate = curator.curate(previous ? &previous.value() : nullptr, checkpoint,
                                            events, options);
            if (!candidate) {
                return candidate.error();
            }
            return commit_working_context(store, candidate.value(), live);
        });
}

std::future<Result<MemoryMutationResult>>
ContextMemorySupervisor::schedule_mutation(IMemory &memory, MemoryMutation mutation) {
    return submit<MemoryMutationResult>(
        "mutation", SupervisedOpClass::Critical,
        [&memory, mutation = std::move(mutation)](SupervisorToken) {
            return memory.apply(mutation);
        });
}

std::future<Result<ErasureResult>>
ContextMemorySupervisor::schedule_erasure(IMemory &memory, ErasureRequest request) {
    return submit<ErasureResult>(
        "erasure", SupervisedOpClass::Critical,
        [&memory, request = std::move(request)](SupervisorToken) {
            return memory.erase(request);
        });
}

std::future<Result<MemoryCompactionResult>>
ContextMemorySupervisor::schedule_retention_sweep(IMemory &memory, MemoryScope scope) {
    return submit<MemoryCompactionResult>(
        "retention_sweep", SupervisedOpClass::Deferrable,
        [&memory, scope = std::move(scope)](SupervisorToken token) {
            if (token.stop_requested()) {
                return Result<MemoryCompactionResult>(
                    supervisor_error(ErrorCode::Cancelled, "retention sweep cancelled"));
            }
            return memory.compact(scope);
        });
}

SupervisorShutdownReport ContextMemorySupervisor::begin_shutdown() {
    return impl_->begin_shutdown();
}

bool ContextMemorySupervisor::closed() const {
    return impl_->closed();
}

SupervisorStats ContextMemorySupervisor::stats() const {
    return impl_->stats();
}

} // namespace mira
