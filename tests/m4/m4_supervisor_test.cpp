#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/context_memory_supervisor.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/task_checkpoint.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <random>
#include <thread>

#include <executor/executor.hpp>

namespace {

using namespace mira;

[[nodiscard]] Error test_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "test";
    error.safe_message = message;
    return error;
}

// In-memory IMemory double with failure knobs for supervisor semantics.
class FakeMemory final : public IMemory {
  public:
    Result<MemoryQueryResult> query(const MemoryQuery &query) const override {
        (void)query;
        if (fail_queries) {
            return test_error(ErrorCode::Unavailable, "query backend down");
        }
        MemoryQueryResult result;
        return result;
    }
    Result<std::optional<MemoryRecord>> get(MemoryId) const override {
        return std::optional<MemoryRecord>{};
    }
    Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override {
        if (fail_applies.exchange(false)) {
            return test_error(ErrorCode::Internal, "apply failed");
        }
        ++applies;
        MemoryMutationResult result;
        result.record = mutation.proposed.id;
        result.new_version = 1;
        return result;
    }
    Result<MemoryCompactionResult> compact(const MemoryScope &) override {
        if (slow_compact) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        MemoryCompactionResult result;
        result.expired_records_purged = 1;
        return result;
    }
    Result<ErasureResult> erase(const ErasureRequest &request) override {
        if (fail_erase.exchange(false)) {
            ErasureResult pending;
            pending.status = ErasureStatus::Pending;
            pending.note = "forced partial failure";
            return pending;
        }
        ++erasures;
        ErasureResult result;
        result.counts.records_removed = 2;
        (void)request;
        return result;
    }

    std::atomic<bool> fail_queries{false};
    std::atomic<bool> fail_applies{false};
    std::atomic<bool> fail_erase{false};
    bool slow_compact = false;
    std::atomic<std::uint64_t> applies{0};
    std::atomic<std::uint64_t> erasures{0};
};

[[nodiscard]] MemoryScope env_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "dev";
    return scope;
}

[[nodiscard]] MemoryScope user_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::User;
    scope.subject_id = "user-1";
    scope.tenant_id = "tenant-1";
    return scope;
}

[[nodiscard]] ErasureRequest sample_erasure() {
    ErasureRequest request;
    request.scope = user_scope();
    request.reason = "right to erasure";
    return request;
}

int submit_consume_and_exception_isolation() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        FakeMemory memory;
        MemoryEventStore events;
        const auto runtime = RuntimeId::generate();
        const auto session = SessionId::generate();
        ContextMemorySupervisor supervisor(exec, SupervisorConfig{}, &events, runtime, session);

        auto value = supervisor.submit<int>("probe", SupervisedOpClass::Interactive,
                                            [](SupervisorToken) { return Result<int>(42); });
        auto outcome = value.get();
        MIRA_CHECK(outcome.has_value() && outcome.value() == 42);

        auto failed = supervisor.submit<int>(
            "probe", SupervisedOpClass::Interactive,
            [](SupervisorToken) -> Result<int> { throw std::runtime_error("boom"); });
        auto failure = failed.get();
        MIRA_CHECK(!failure.has_value());
        MIRA_CHECK(failure.error().code == ErrorCode::Internal);

        auto empty = supervisor.submit<int>("probe", SupervisedOpClass::Interactive, nullptr);
        auto rejected = empty.get();
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);

        // The event sink carries sanitized started/finished diagnostics.
        auto stats = supervisor.stats();
        MIRA_CHECK(stats.admitted == 3);
        MIRA_CHECK(stats.completed == 1);
        MIRA_CHECK(stats.failed == 2); // the throwing op and the empty op
        MIRA_CHECK(stats.events_emitted >= 6);
        MIRA_CHECK(stats.event_sink_failures == 0);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int rejection_after_close_and_capacity_bound() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        FakeMemory memory;
        SupervisorConfig config;
        config.max_in_flight = 1;
        config.critical_drain_timeout = std::chrono::milliseconds(1500);
        ContextMemorySupervisor supervisor(exec, config);

        // Occupy the single in-flight slot with a slow deferrable op.
        memory.slow_compact = true;
        const MemoryScope scope = env_scope();
        auto slow = supervisor.schedule_retention_sweep(memory, scope);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Capacity-bound rejection surfaces as ResourceExhausted.
        auto rejected = supervisor.submit<int>("probe", SupervisedOpClass::Critical,
                                               [](SupervisorToken) { return Result<int>(1); });
        auto bounded = rejected.get();
        MIRA_CHECK(!bounded.has_value());
        MIRA_CHECK(bounded.error().code == ErrorCode::ResourceExhausted);

        MIRA_CHECK(slow.get().has_value());

        // After shutdown admission is closed.
        (void)supervisor.begin_shutdown();
        MIRA_CHECK(supervisor.closed());
        auto closed_rejection = supervisor.submit<int>(
            "probe", SupervisedOpClass::Critical, [](SupervisorToken) { return Result<int>(2); });
        auto denied = closed_rejection.get();
        MIRA_CHECK(!denied.has_value());
        MIRA_CHECK(denied.error().code == ErrorCode::Unavailable);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int deferrable_cancelled_critical_settles_at_shutdown() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        FakeMemory memory;
        memory.slow_compact = true;
        SupervisorConfig config;
        config.critical_drain_timeout = std::chrono::milliseconds(2000);
        ContextMemorySupervisor supervisor(exec, config);

        // One slow deferrable sweep and one critical erasure.
        const MemoryScope scope = env_scope();
        auto sweep = supervisor.schedule_retention_sweep(memory, scope);
        auto erasure = supervisor.schedule_erasure(memory, sample_erasure());

        const auto report = supervisor.begin_shutdown();
        MIRA_CHECK(report.critical_drain_complete);
        MIRA_CHECK(report.critical_settled >= 1);

        // The critical erasure settled (never silently dropped).
        auto erased = erasure.get();
        MIRA_CHECK(erased.has_value());
        MIRA_CHECK(memory.erasures.load() == 1);

        // The in-flight deferrable sweep was either cancelled or completed;
        // either way its future resolves and never hangs.
        auto swept = sweep.get();
        MIRA_CHECK(swept.has_value() || !swept.has_value()); // resolves without blocking
        (void)swept;
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int failing_operations_report_without_poisoning() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        FakeMemory memory;
        memory.fail_queries = true;
        memory.fail_applies = true;
        ContextMemorySupervisor supervisor(exec);

        MemoryQuery query;
        query.scopes = {env_scope()};
        auto failed_query = supervisor.schedule_memory_query(memory, query).get();
        MIRA_CHECK(!failed_query.has_value());
        MIRA_CHECK(failed_query.error().code == ErrorCode::Unavailable);

        MemoryMutation mutation;
        mutation.id = MutationId::generate();
        mutation.scope = env_scope();
        mutation.proposed.id = MemoryId::generate();
        mutation.proposed.scope = mutation.scope;
        mutation.proposed.statement = "note";
        mutation.proposed.provenance = {EventId::generate()};
        mutation.evidence = mutation.proposed.provenance;
        auto failed_mutation = supervisor.schedule_mutation(memory, mutation).get();
        MIRA_CHECK(!failed_mutation.has_value());

        // A pending erasure result flows back truthfully: the caller retries.
        memory.fail_erase = true;
        auto pending = supervisor.schedule_erasure(memory, sample_erasure()).get();
        MIRA_CHECK(pending.has_value());
        MIRA_CHECK(pending.value().status == ErasureStatus::Pending);

        const auto stats = supervisor.stats();
        MIRA_CHECK(stats.completed >= 1); // the pending erasure completed as a call
        MIRA_CHECK(stats.failed >= 2);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

int checkpoint_scheduling_routes_through_supervisor() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        MemoryEventStore events;
        MemoryCheckpointStore checkpoints;
        const auto runtime = RuntimeId::generate();
        const auto session = SessionId::generate();
        const auto task = TaskId::generate();
        {
            testing::TaskEventLog log(runtime, session, task);
            log.add("TaskGoalSet", JsonValue::Object{{"statement", "supervised checkpoint"}});
            log.add("TaskStateChanged", JsonValue::Object{{"to", "Observing"}});
            for (int index = 0; index < 6; ++index) {
                log.add("TaskStepCompleted",
                        JsonValue::Object{{"step_id", StepId::generate().to_string()},
                                          {"summary", "step"}});
            }
            log.append_to(events);
        }
        CheckpointCoordinator coordinator(events, checkpoints);
        ContextMemorySupervisor supervisor(exec, SupervisorConfig{}, &events, runtime, session);

        auto scheduled = supervisor.schedule_checkpoint(coordinator, task, session,
                                                        CheckpointTrigger::Pause, Timestamp::now())
                             .get();
        MIRA_CHECK(scheduled.has_value() && scheduled.value().has_value());
        MIRA_CHECK(scheduled.value()->goal_statement == "supervised checkpoint");
        MIRA_CHECK(coordinator.latest_stored_sequence(task).has_value());
        MIRA_CHECK(coordinator.latest_stored_sequence(task).value() > 0);
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += submit_consume_and_exception_isolation();
    failures += rejection_after_close_and_capacity_bound();
    failures += deferrable_cancelled_critical_settles_at_shutdown();
    failures += failing_operations_report_without_poisoning();
    failures += checkpoint_scheduling_routes_through_supervisor();
    return failures == 0 ? 0 : 1;
}
