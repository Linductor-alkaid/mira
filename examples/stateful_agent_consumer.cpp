// Offline durable-state composition. seed and inspect can run in separate
// processes against a host-owned directory; only the no-argument smoke run
// owns and removes its temporary directory. No external model is called.
#include <mira/context_memory_supervisor.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/stateful_replay.hpp>
#include <mira/task_checkpoint.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>

#include <kairo/executor.hpp>

namespace {
using namespace mira;

// Fixture identities only. Real hosts persist authenticated Principal/Session
// identities and derive authorized scopes; external chat IDs are not grants.
const auto session = *SessionId::parse("00fba3e97a204203a007674ec9ca3001");
const auto task = *TaskId::parse("00fba3e97a204203a007674ec9ca3002");
const auto runtime = *RuntimeId::parse("00fba3e97a204203a007674ec9ca3003");

bool seed(kairo::Executor &executor, FileEventStore &events, SqliteMemoryStore &memory,
          SqliteCheckpointStore &checkpoints, const MemoryScope &scope) {
    AppendRequest fact;
    fact.event_id = *EventId::parse("00fba3e97a204203a007674ec9ca3004");
    fact.runtime_id = runtime;
    fact.session_id = session;
    fact.task_id = task;
    fact.payload = {"TaskFactVerified",
                    to_json_string(JsonValue::Object{{"key", "locale"}, {"value", "de-DE"}}),
                    EventClass::State};
    const auto appended = events.append(fact);
    if (!appended || appended.value().achieved != Durability::ProcessCrash)
        return false;
    // Use the committed envelope, including its actual sequence/time, rather
    // than inventing a second source record for consolidation.
    auto source = events.read({session, appended.value().session_sequence - 1, 1});
    if (!source || source.value().events.size() != 1)
        return false;
    const auto report =
        MemoryConsolidator{}.consolidate(memory, source.value().events, scope, Timestamp::now());
    if (!report || report.value().count_of(CandidateDisposition::Applied) != 1)
        return false;
    AppendRequest goal;
    goal.event_id = *EventId::parse("00fba3e97a204203a007674ec9ca3005");
    goal.runtime_id = runtime;
    goal.session_id = session;
    goal.task_id = task;
    goal.payload = {"TaskGoalSet", R"({"statement":"demo goal"})", EventClass::State};
    if (!events.append(goal))
        return false;
    CheckpointCoordinator coordinator(events, checkpoints);
    ContextMemorySupervisor supervisor(executor, SupervisorConfig{}, &events, runtime, session);
    const auto stored = supervisor
                            .schedule_checkpoint(coordinator, task, session,
                                                 CheckpointTrigger::Pause, Timestamp::now())
                            .get();
    const auto latest = checkpoints.latest(task);
    const auto shutdown = supervisor.begin_shutdown();
    // A repeat seed may be a no-op; the existing checkpoint must remain valid.
    return stored && latest && latest.value() && shutdown.critical_drain_complete;
}

bool inspect(kairo::Executor &executor, FileEventStore &events, SqliteMemoryStore &memory,
             SqliteCheckpointStore &checkpoints, const MemoryScope &scope) {
    const auto recovered = events.recover({});
    const auto checkpoint = checkpoints.latest(task);
    if (!recovered || recovered.value().status != RecoveryStatus::Clean || !checkpoint ||
        !checkpoint.value() || checkpoint.value()->goal_statement != "demo goal" ||
        checkpoint.value()->verified_facts.size() != 1 ||
        checkpoint.value()->verified_facts.front().value != "de-DE")
        return false;
    // No extraction, vector supply or model call occurs during inspect.
    ContextMemorySupervisor supervisor(executor, SupervisorConfig{});
    MemoryQuery query;
    query.scopes = {scope};
    query.text = "locale";
    const auto recalled = supervisor.schedule_memory_query(memory, query).get();
    query.text = "unknownlexicalterm";
    const auto missing = supervisor.schedule_memory_query(memory, query).get();
    query.text = "locale";
    query.scopes.front().subject_id = "another-device";
    const auto denied = supervisor.schedule_memory_query(memory, query).get();
    const auto shutdown = supervisor.begin_shutdown();
    AnalysisReplay replay(events, &checkpoints, &memory, nullptr);
    const auto view = replay.inspect(task, session, scope);
    CheckpointCoordinator coordinator(events, checkpoints);
    const auto consistency = coordinator.rebuild(task, session, Timestamp::now(), false);
    return recalled && recalled.value().quality.fts_leg_ran &&
           recalled.value().records.size() == 1 &&
           recalled.value().records.front().statement == "locale=de-DE" && missing &&
           missing.value().records.empty() && denied && denied.value().records.empty() &&
           shutdown.critical_drain_complete && view && view.value().checkpoint_present &&
           consistency && consistency.value().consistent;
}

int run(const std::filesystem::path &root, bool write) {
    kairo::Executor executor;
    if (!executor.initialize(kairo::ExecutorConfig{}))
        return 1;
    bool passed = false;
    {
        SqliteStoreOptions checkpoint_options;
        checkpoint_options.path = root / "stateful-checkpoints.db";
        auto checkpoints = SqliteCheckpointStore::open(executor, checkpoint_options);
        SqliteMemoryStoreOptions memory_options;
        memory_options.path = root / "stateful-memory.db";
        auto memory = SqliteMemoryStore::open(executor, memory_options);
        if (checkpoints && memory) {
            FileEventStore events(root / "events");
            MemoryScope scope;
            scope.kind = MemoryScopeKind::Environment;
            scope.subject_id = "example-device";
            passed = write
                         ? seed(executor, events, *memory.value(), *checkpoints.value(), scope)
                         : inspect(executor, events, *memory.value(), *checkpoints.value(), scope);
            // Stop producers/drain supervisor above, then close both writers
            // before the external Executor owner performs final shutdown.
            const auto memory_closed = memory.value()->close();
            const auto checkpoints_closed = checkpoints.value()->close();
            passed = passed && memory_closed && checkpoints_closed;
        }
    }
    const auto stopped = executor.shutdown(true);
    passed = passed && stopped == kairo::ShutdownResult::Completed;
    std::cout << (passed ? "stateful agent example: OK\n" : "stateful agent example: failed\n");
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 3 &&
        (std::string_view{argv[1]} == "seed" || std::string_view{argv[1]} == "inspect")) {
        const std::filesystem::path root{argv[2]};
        if (root.empty())
            return 2;
        std::filesystem::create_directories(root);
        return run(root, std::string_view{argv[1]} == "seed");
    }
    if (argc > 2) {
        std::cerr
            << "usage: mira_stateful_consumer [directory | seed directory | inspect directory]\n";
        return 2;
    }
    const bool temporary = argc == 1;
    const auto root = temporary ? std::filesystem::temp_directory_path() /
                                      ("mira-stateful-" + Id128::generate().to_string())
                                : std::filesystem::path{argv[1]};
    std::filesystem::create_directories(root);
    auto code = run(root, true);
    if (code == 0)
        code = run(root, false);
    if (temporary)
        std::filesystem::remove_all(root);
    return code;
}
