#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/context_memory_supervisor.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/task_checkpoint.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>

#include <kairo/executor.hpp>

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
        for (const auto &record : records) {
            result.records.push_back(record);
        }
        return result;
    }
    Result<std::optional<MemoryRecord>> get(MemoryId, const MemoryScope &) const override {
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
    // The records a retrieval sees; the repair pass scans them.
    std::vector<MemoryRecord> records;
};

// A projection index double: it keeps the attached vectors and reports how
// many records it still does not cover.
class FakeEmbeddingIndex final : public IMemoryEmbeddingIndex {
  public:
    Result<void> index_embedding(MemoryId record, std::span<const float> vector) override {
        vectors_[record.to_string()] = std::vector<float>(vector.begin(), vector.end());
        return Result<void>{};
    }
    Result<bool> has_embedding(MemoryId record) const override {
        return vectors_.count(record.to_string()) != 0;
    }
    Result<std::size_t> index_lag() const override {
        const std::size_t attached = vectors_.size();
        return covered > attached ? covered - attached : 0;
    }
    Result<std::size_t> clear_embeddings() override {
        vectors_.clear();
        return Result<std::size_t>(0);
    }

    std::size_t covered = 0;
    std::map<std::string, std::vector<float>> vectors_;
};

// A producer double: it either yields a vector or refuses, so the repair
// pass has a failure to report.
class FixedProvider final : public IEmbeddingProvider {
  public:
    Result<std::vector<float>> embed(std::string_view) override {
        if (fail) {
            return test_error(ErrorCode::Unavailable, "producer down");
        }
        ++calls;
        return std::vector<float>{1.0F, 0.5F};
    }
    bool fail = false;
    std::size_t calls = 0;
};

// The recorded event types of one session (paged read).
[[nodiscard]] std::vector<std::string> recorded_event_types(const IEventStore &store,
                                                            const SessionId &session) {
    std::vector<std::string> types;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            types.push_back(envelope.payload.type);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return types;
}

[[nodiscard]] bool has_event(const std::vector<std::string> &types, const std::string &name) {
    return std::find(types.begin(), types.end(), name) != types.end();
}

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
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// The design §20 event surface: every event the memory plane names must
// actually be emitted, under its own name, and the payload must carry
// counts rather than content.
int memory_plane_events_are_named_and_sanitized() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        FakeMemory memory;
        MemoryEventStore events;
        const auto runtime = RuntimeId::generate();
        const auto session = SessionId::generate();
        ContextMemorySupervisor supervisor(exec, SupervisorConfig{}, &events, runtime, session);

        // A context build: Started and Finished.
        auto build = supervisor.submit<int>("context_prepare", SupervisedOpClass::Interactive,
                                            [](SupervisorToken) { return Result<int>(1); });
        MIRA_CHECK(build.get().has_value());

        // A memory query: Started and Finished.
        auto query = supervisor.schedule_memory_query(memory, MemoryQuery{});
        MIRA_CHECK(query.get().has_value());

        MemoryMutation mutation;
        mutation.id = MutationId::generate();
        mutation.scope = env_scope();
        mutation.proposed.scope = env_scope();
        mutation.proposed.statement = "volume=30";
        mutation.proposed.provenance = {EventId::generate()};
        mutation.evidence = mutation.proposed.provenance;
        // An applied mutation.
        MIRA_CHECK(supervisor.schedule_mutation(memory, mutation).get().has_value());
        // A supersede: the transition is named, not the generic completion.
        MemoryMutation supersede = mutation;
        supersede.id = MutationId::generate();
        supersede.type = MemoryMutationType::Supersede;
        supersede.target = MemoryId::generate();
        supersede.expected_version = 1;
        MIRA_CHECK(supervisor.schedule_mutation(memory, supersede).get().has_value());
        // A tombstone: the record left the retrieval plane.
        MemoryMutation tombstone = supersede;
        tombstone.id = MutationId::generate();
        tombstone.type = MemoryMutationType::Tombstone;
        MIRA_CHECK(supervisor.schedule_mutation(memory, tombstone).get().has_value());
        // A rejected mutation.
        memory.fail_applies = true;
        MIRA_CHECK(!supervisor.schedule_mutation(memory, mutation).get().has_value());

        const auto types = recorded_event_types(events, session);
        MIRA_CHECK(has_event(types, "ContextBuildStarted"));
        MIRA_CHECK(has_event(types, "ContextBuildFinished"));
        MIRA_CHECK(has_event(types, "MemoryQueryStarted"));
        MIRA_CHECK(has_event(types, "MemoryQueryFinished"));
        MIRA_CHECK(has_event(types, "MemoryMutationProposed"));
        MIRA_CHECK(has_event(types, "MemoryMutationApplied"));
        MIRA_CHECK(has_event(types, "MemoryMutationRejected"));
        MIRA_CHECK(has_event(types, "MemorySuperseded"));
        MIRA_CHECK(has_event(types, "MemoryTombstoned"));
        // The generic fallback is no longer where the named events land.
        MIRA_CHECK(!has_event(types, "ContextMemoryOperationFinished"));
        MIRA_CHECK(!has_event(types, "ContextMemoryOperationFailed"));

        // The payload carries counts and no content: the statement never
        // reaches the event stream.
        EventQuery read;
        read.session_id = session;
        const auto page = events.read(read);
        MIRA_CHECK(page.has_value());
        bool saw_mutation_payload = false;
        for (const auto &envelope : page.value().events) {
            const auto payload = parse_json(envelope.payload.data);
            MIRA_CHECK(payload.has_value());
            if (!payload.has_value()) {
                continue;
            }
            const auto *label = payload.value().find("label");
            const auto *class_name = payload.value().find("class");
            const auto *outcome = payload.value().find("outcome");
            MIRA_CHECK(label != nullptr && class_name != nullptr && outcome != nullptr);
            MIRA_CHECK(payload.value().find("statement") == nullptr);
            MIRA_CHECK(payload.value().find("text") == nullptr);
            if (label != nullptr && label->is_string() && *label->as_string() == "mutation") {
                saw_mutation_payload = true;
            }
        }
        MIRA_CHECK(saw_mutation_payload);
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

// The projection's state is observable: a repair pass that rebuilt
// something says so, and a pass that left the index behind reports the lag.
int embedding_repair_reports_the_projection_state() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
    {
        FakeMemory memory;
        MemoryEventStore events;
        const auto runtime = RuntimeId::generate();
        const auto session = SessionId::generate();
        ContextMemorySupervisor supervisor(exec, SupervisorConfig{}, &events, runtime, session);

        // Two active records in the scope; the projection covers none of them.
        for (int index = 0; index < 2; ++index) {
            MemoryRecord record;
            record.id = MemoryId::generate();
            record.scope = env_scope();
            record.kind = MemoryKind::EnvironmentFact;
            record.statement = "volume=" + std::to_string(index);
            record.recorded_at = std::chrono::system_clock::now();
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId::generate()};
            record.verification = MemoryVerification::Observed;
            record.confidence = 0.8F;
            memory.records.push_back(record);
        }
        FakeEmbeddingIndex index;
        index.covered = memory.records.size();
        FixedProvider provider;
        MemoryEmbeddingIndexer indexer(index, provider);

        auto rebuilt = supervisor.schedule_embedding_repair(indexer, memory, env_scope());
        const auto report = rebuilt.get();
        MIRA_CHECK(report.has_value());
        MIRA_CHECK(report.value().repaired == 2);
        MIRA_CHECK(report.value().index_lag == 0);

        auto types = recorded_event_types(events, session);
        MIRA_CHECK(has_event(types, "MemoryIndexRebuilt"));
        // The projection is current again, so there is no lag to report.
        MIRA_CHECK(!has_event(types, "MemoryIndexLagged"));

        // A producer outage leaves the projection behind: the pass reports
        // the lag instead of a rebuild.
        MemoryEventStore failed_events;
        ContextMemorySupervisor failing(exec, SupervisorConfig{}, &failed_events, runtime, session);
        FakeEmbeddingIndex stale_index;
        stale_index.covered = memory.records.size();
        FixedProvider failing_provider;
        failing_provider.fail = true;
        MemoryEmbeddingIndexer failing_indexer(stale_index, failing_provider);
        auto failed = failing.schedule_embedding_repair(failing_indexer, memory, env_scope());
        const auto failure = failed.get();
        MIRA_CHECK(failure.has_value());
        MIRA_CHECK(failure.value().repaired == 0);
        MIRA_CHECK(failure.value().failed == 2);
        MIRA_CHECK(failure.value().index_lag == 2);

        const auto failed_types = recorded_event_types(failed_events, session);
        MIRA_CHECK(has_event(failed_types, "MemoryIndexLagged"));
        MIRA_CHECK(!has_event(failed_types, "MemoryIndexRebuilt"));

        // The lag event carries the count and no content.
        EventQuery read;
        read.session_id = session;
        const auto page = failed_events.read(read);
        MIRA_CHECK(page.has_value());
        bool saw_lag_count = false;
        for (const auto &envelope : page.value().events) {
            if (envelope.payload.type != "MemoryIndexLagged") {
                continue;
            }
            const auto payload = parse_json(envelope.payload.data);
            MIRA_CHECK(payload.has_value());
            if (!payload.has_value()) {
                continue;
            }
            const auto *lag = payload.value().find("lag");
            MIRA_CHECK(lag != nullptr);
            if (lag != nullptr && lag->is_integer()) {
                MIRA_CHECK(*lag->as_integer() == 2);
                saw_lag_count = true;
            }
            MIRA_CHECK(payload.value().find("statement") == nullptr);
        }
        MIRA_CHECK(saw_lag_count);
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int rejection_after_close_and_capacity_bound() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int deferrable_cancelled_critical_settles_at_shutdown() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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

        // The in-flight deferrable sweep resolves: cancelled with an error,
        // or completed if it already started. It must never hang.
        auto swept = sweep.get();
        if (!swept.has_value()) {
            MIRA_CHECK(swept.error().code == ErrorCode::Cancelled);
        }
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int failing_operations_report_without_poisoning() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int checkpoint_scheduling_routes_through_supervisor() {
    kairo::Executor exec;
    MIRA_CHECK(exec.initialize(kairo::ExecutorConfig{}));
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

        auto scheduled = supervisor
                             .schedule_checkpoint(coordinator, task, session,
                                                  CheckpointTrigger::Pause, Timestamp::now())
                             .get();
        MIRA_CHECK(scheduled.has_value() && scheduled.value().has_value());
        MIRA_CHECK(scheduled.value()->goal_statement == "supervised checkpoint");
        MIRA_CHECK(coordinator.latest_stored_sequence(task).has_value());
        MIRA_CHECK(coordinator.latest_stored_sequence(task).value() > 0);
    }
    MIRA_CHECK(exec.shutdown(true) == kairo::ShutdownResult::Completed);
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
    failures += memory_plane_events_are_named_and_sanitized();
    failures += embedding_repair_reports_the_projection_state();
    return failures == 0 ? 0 : 1;
}
