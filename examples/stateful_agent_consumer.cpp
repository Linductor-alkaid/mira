// Stateful agent beta example (M4-18): durable checkpoints, long-term memory
// and supervised shutdown over one Executor. Runnable offline; the model
// side is intentionally absent — this shows the state substrate.

#include <mira/context_memory_supervisor.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/stateful_replay.hpp>
#include <mira/task_checkpoint.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

#include <executor/executor.hpp>

namespace {

using namespace mira;

int run(const std::filesystem::path &root) {
    executor::Executor exec;
    if (!exec.initialize(executor::ExecutorConfig{})) {
        std::cerr << "executor initialize failed\n";
        return 1;
    }
    int failures = 0;

    SqliteStoreOptions checkpoint_options;
    checkpoint_options.path = root / "stateful-checkpoints.db";
    auto checkpoint_store = SqliteCheckpointStore::open(exec, checkpoint_options);
    if (!checkpoint_store) {
        std::cerr << "checkpoint store open failed: " << checkpoint_store.error().safe_message
                  << "\n";
        return 1;
    }
    SqliteMemoryStoreOptions memory_options;
    memory_options.path = root / "stateful-memory.db";
    auto memory_store = SqliteMemoryStore::open(exec, memory_options);
    if (!memory_store) {
        std::cerr << "memory store open failed: " << memory_store.error().safe_message << "\n";
        return 1;
    }

    MemoryEventStore events;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    const MemoryScope scope = [] {
        MemoryScope value;
        value.kind = MemoryScopeKind::Environment;
        value.subject_id = "example-device";
        return value;
    }();

    // A verified fact lands in long-term memory through the deterministic
    // consolidation pipeline (no model needed).
    EventEnvelope fact;
    fact.event_id = EventId::generate();
    fact.runtime_id = runtime;
    fact.session_id = session;
    fact.task_id = task;
    fact.session_sequence = 1;
    fact.payload = EventPayload{"TaskFactVerified",
                                to_json_string(JsonValue::Object{{"key", "locale"},
                                                                 {"value", "de-DE"}}),
                                EventClass::State};
    AppendRequest fact_append;
    fact_append.event_id = fact.event_id;
    fact_append.runtime_id = runtime;
    fact_append.session_id = session;
    fact_append.task_id = task;
    fact_append.payload = fact.payload;
    if (!events.append(fact_append)) {
        ++failures;
    }
    MemoryConsolidator consolidator;
    const std::array<EventEnvelope, 1> fact_events{fact};
    auto report =
        consolidator.consolidate(*memory_store.value(), fact_events, scope, Timestamp::now());
    if (!report || report.value().count_of(CandidateDisposition::Applied) != 1) {
        std::cerr << "consolidation did not apply the verified fact\n";
        ++failures;
    }

    // Supervised routing: one critical checkpoint op, one interactive query.
    {
        EventEnvelope settled;
        settled.event_id = EventId::generate();
        settled.runtime_id = runtime;
        settled.session_id = session;
        settled.task_id = task;
        settled.session_sequence = 2;
        settled.payload = EventPayload{
            "TaskGoalSet", to_json_string(JsonValue::Object{{"statement", "demo goal"}}),
            EventClass::State};
        AppendRequest settled_append;
        settled_append.event_id = settled.event_id;
        settled_append.runtime_id = runtime;
        settled_append.session_id = session;
        settled_append.task_id = task;
        settled_append.payload = settled.payload;
        (void)events.append(settled_append);
        CheckpointCoordinator coordinator(events, *checkpoint_store.value());
        ContextMemorySupervisor supervisor(exec, SupervisorConfig{}, &events, runtime, session);
        auto stored = supervisor
                          .schedule_checkpoint(coordinator, task, session,
                                               CheckpointTrigger::Pause, Timestamp::now())
                          .get();
        if (!stored.has_value() || !stored.value().has_value()) {
            std::cerr << "supervised checkpoint failed\n";
            ++failures;
        }
        MemoryQuery query;
        query.scopes = {scope};
        query.text = "locale";
        auto recalled = supervisor.schedule_memory_query(*memory_store.value(), query).get();
        if (!recalled.has_value() || recalled.value().records.empty()) {
            std::cerr << "supervised memory query returned nothing\n";
            ++failures;
        }
        // §17.2 shutdown order: stop producers, settle critical work.
        const auto shutdown = supervisor.begin_shutdown();
        if (!shutdown.critical_drain_complete) {
            std::cerr << "supervisor drain incomplete\n";
            ++failures;
        }
    }

    // Read-only analysis replay over the durable state.
    AnalysisReplay replay(events, checkpoint_store.value().get(), memory_store.value().get(),
                          nullptr);
    auto view = replay.inspect(task, session, scope);
    if (!view.has_value() || !view.value().checkpoint_present ||
        view.value().memory.records.empty()) {
        std::cerr << "analysis replay missed durable state\n";
        ++failures;
    }

    (void)memory_store.value()->close();
    (void)checkpoint_store.value()->close();
    if (exec.shutdown(true) != executor::ShutdownResult::Completed) {
        ++failures;
    }
    if (failures == 0) {
        std::cout << "stateful agent example: OK\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    const auto root = std::filesystem::path{argc > 1 ? argv[1] : "mira-stateful-example"};
    std::filesystem::create_directories(root);
    const auto code = run(root);
    std::filesystem::remove_all(root);
    return code;
}
