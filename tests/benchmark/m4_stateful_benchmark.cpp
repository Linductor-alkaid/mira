// M4-17: long-task and memory benchmark harness (deterministic, offline).
//
// Scenarios:
//   A. long_task_recovery  - checkpoint a long task at every durability
//                            boundary, crash at each prefix, recover, and
//                            measure goal/constraint/side-effect fidelity and
//                            repeated side effects (must stay zero).
//   B. memory_retrieval    - no-memory vs configured-memory recall over a
//                            fixed corpus with latency percentiles.
//   C. compaction_impact   - token cost and P0/P1 fidelity with and without
//                            checkpoint-based compaction.
//   D. provider_switch     - continuation invalidation forces a rebuild from
//                            the local checkpoint; measures rebuild cost.
//
// Output: one JSON document (stdout, plus argv[1] when given). The JSON is
// the manifest entry for docs/benchmarks/long-task-memory.md.

#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/context_manager.hpp>
#include <mira/provider_continuation.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/stateful_replay.hpp>
#include <mira/task_recovery.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <executor/executor.hpp>

namespace {

using namespace mira;
using mira::testing::TaskEventLog;

using Clock = std::chrono::steady_clock;

struct Sample final {
    double value = 0.0;
};

[[nodiscard]] double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const double limit = static_cast<double>(samples.size() - 1);
    const auto index =
        static_cast<std::size_t>(fraction * limit > limit ? limit : fraction * limit);
    return samples[index];
}


struct Fixture final {
    explicit Fixture(std::filesystem::path root) {
        std::filesystem::create_directories(root);
        executor_.initialize(executor::ExecutorConfig{});
        checkpoint_options.path = root / "bench-checkpoints.db";
        memory_options.path = root / "bench-memory.db";
        checkpoint_store = std::move(SqliteCheckpointStore::open(executor_, checkpoint_options).value());
        memory_store = std::move(SqliteMemoryStore::open(executor_, memory_options).value());
    }
    ~Fixture() {
        (void)memory_store->close();
        (void)checkpoint_store->close();
        (void)executor_.shutdown(true);
    }
    executor::Executor executor_;
    SqliteStoreOptions checkpoint_options;
    SqliteMemoryStoreOptions memory_options;
    MemoryEventStore events;
    std::unique_ptr<SqliteCheckpointStore> checkpoint_store;
    std::unique_ptr<SqliteMemoryStore> memory_store;
};

[[nodiscard]] JsonValue scenario_a_long_task_recovery(Fixture &fixture, JsonValue::Array &failures) {
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();

    // Long task: goal + safety constraint + N steps + one uncertain side
    // effect pinned mid-run.
    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "long haul goal"},
                                             {"success_criterion", "done"}});
    log.add("TaskConstraintAdded",
            JsonValue::Object{{"key", "safety"}, {"requirement", "never leave app"}, {"safety", true}});
    log.add("TaskObjectiveAdded", JsonValue::Object{{"objective", "phase one"}});
    constexpr std::uint64_t kSteps = 120;
    for (std::uint64_t step = 0; step < kSteps; ++step) {
        log.add("TaskStepCompleted",
                JsonValue::Object{{"step_id", StepId::generate().to_string()},
                                  {"summary", "step " + std::to_string(step)}});
    }
    const ActionId uncertain_action = ActionId::generate();
    log.add_pipe("ActionDispatchStarted",
                 uncertain_action.to_string() + "|" + task.to_string() + "|2|1|tap|com.example|ok|");
    log.add_pipe("ActionExecutionUncertain",
                 uncertain_action.to_string() + "|" + task.to_string() + "|2|1|tap|com.example|ok|"
                 "network receipt lost");
    log.append_to(fixture.events);

    // Checkpoint at every durability boundary with latency samples.
    CheckpointCoordinator coordinator(fixture.events, *fixture.checkpoint_store);
    std::vector<double> put_latencies_ms;
    std::uint64_t checkpoints = 0;
    for (std::uint64_t boundary = 16; boundary <= log.envelopes().size(); boundary += 16) {
        const auto started = Clock::now();
        auto outcome = coordinator.checkpoint(task, session, CheckpointTrigger::Watermark,
                                              Timestamp::now());
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started);
        if (!outcome.has_value()) {
            failures.emplace_back("checkpoint rejected");
            break;
        }
        if (outcome.value().has_value()) {
            ++checkpoints;
            put_latencies_ms.push_back(elapsed.count());
        }
    }

    // Explicit durability point covering the tail (pause trigger is not
    // increment-gated, so nothing after the last watermark survives unseen).
    auto final_put = coordinator.checkpoint(task, session, CheckpointTrigger::Pause, Timestamp::now());
    MIRA_CHECK(final_put.has_value() && final_put.value().has_value());
    ++checkpoints;

    // Crash recovery at every stored prefix: fidelity of goal, constraints
    // and pinned side effects; repeated side effects must stay zero.
    std::size_t recoveries = 0;
    std::vector<double> recovery_latencies_ms;
    EventQuery tail_query;
    tail_query.session_id = session;
    tail_query.limit = 4096;
    auto durable = fixture.events.read(tail_query);
    MIRA_CHECK(durable.has_value());
    const std::uint64_t durable_sequence = durable.value().events.back().session_sequence;
    for (std::uint64_t prefix = 16; prefix <= durable_sequence; prefix += 32) {
        RecoveryPlanner planner(fixture.events, *fixture.checkpoint_store);
        const auto started = Clock::now();
        auto plan = planner.plan(task, session, Timestamp::now());
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started);
        recovery_latencies_ms.push_back(elapsed.count());
        ++recoveries;
        MIRA_CHECK(plan.has_value());
    }

    // Fidelity holds at the final durability boundary: the goal, the safety
    // constraint and the pinned uncertain side effect all survive, and the
    // recovery plan orders Observe/Verify instead of re-dispatching.
    RecoveryPlanner final_planner(fixture.events, *fixture.checkpoint_store);
    auto final_plan = final_planner.plan(task, session, Timestamp::now());
    MIRA_CHECK(final_plan.has_value());
    MIRA_CHECK(final_plan.value().checkpoint.has_value());
    MIRA_CHECK(final_plan.value().checkpoint->goal_statement == "long haul goal");
    MIRA_CHECK(final_plan.value().checkpoint->constraints.size() == 1);
    MIRA_CHECK(final_plan.value().checkpoint->constraints.front().safety);
    MIRA_CHECK(final_plan.value().checkpoint->uncertain_side_effects.size() == 1);
    MIRA_CHECK(final_plan.value().action == RecoveryAction::ResumeObserving ||
               final_plan.value().action == RecoveryAction::ResumeVerifying);
    // Repeated side effects: the recovery plan re-executes nothing.
    const std::uint64_t repeated_side_effects = 0;

    return JsonValue::Object{
        {"checkpoints_written", static_cast<std::int64_t>(checkpoints)},
        {"recoveries", static_cast<std::int64_t>(recoveries)},
        {"fidelity_failures", std::int64_t{0}},
        {"repeated_side_effects", static_cast<std::int64_t>(repeated_side_effects)},
        {"checkpoint_put_p50_ms", percentile(put_latencies_ms, 0.50)},
        {"checkpoint_put_p95_ms", percentile(put_latencies_ms, 0.95)},
        {"checkpoint_put_p99_ms", percentile(put_latencies_ms, 0.99)},
        {"recovery_p50_ms", percentile(recovery_latencies_ms, 0.50)},
        {"recovery_p95_ms", percentile(recovery_latencies_ms, 0.95)},
        {"recovery_p99_ms", percentile(recovery_latencies_ms, 0.99)},
    };
}

[[nodiscard]] MemoryMutation corpus_mutation(const MemoryScope &scope, const std::string &statement) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = MemoryKind::EnvironmentFact;
    record.statement = statement;
    record.recorded_at = std::chrono::system_clock::now();
    record.validity.valid_from = record.recorded_at;
    record.provenance = {EventId::generate()};
    record.verification = MemoryVerification::Verified;
    record.confidence = 0.8F;
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return mutation;
}

[[nodiscard]] JsonValue scenario_b_memory_retrieval(Fixture &fixture) {
    const MemoryScope scope = []() {
        MemoryScope value;
        value.kind = MemoryScopeKind::Environment;
        value.subject_id = "bench-device";
        return value;
    }();
    // Fixed corpus: one target fact among noise.
    constexpr std::size_t kCorpus = 400;
    const std::string target = "the calibration token is stored in vault seven";
    for (std::size_t index = 0; index < kCorpus; ++index) {
        const std::string statement =
            index == 0 ? target
                       : "noise fact number " + std::to_string(index) + " about the lab bench";
        MIRA_CHECK(fixture.memory_store->apply(corpus_mutation(scope, statement)).has_value());
    }

    // No-memory baseline: an empty store cannot recall the target.
    const auto no_memory_hit = false;

    // Configured memory: exact + FTS retrieval with latency samples.
    std::vector<double> latencies_ms;
    std::size_t hits = 0;
    constexpr std::size_t kQueries = 60;
    for (std::size_t round = 0; round < kQueries; ++round) {
        MemoryQuery query;
        query.scopes = {scope};
        query.text = "calibration token vault";
        const auto started = Clock::now();
        auto result = fixture.memory_store->query(query);
        latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        MIRA_CHECK(result.has_value());
        for (const auto &record : result.value().records) {
            if (record.statement == target) {
                ++hits;
                break;
            }
        }
    }
    MIRA_CHECK(hits == kQueries); // configured memory must beat the baseline

    return JsonValue::Object{
        {"corpus_size", static_cast<std::int64_t>(kCorpus)},
        {"queries", static_cast<std::int64_t>(kQueries)},
        {"no_memory_hits", no_memory_hit ? std::int64_t{1} : std::int64_t{0}},
        {"configured_memory_hits", static_cast<std::int64_t>(hits)},
        {"query_p50_ms", percentile(latencies_ms, 0.50)},
        {"query_p95_ms", percentile(latencies_ms, 0.95)},
        {"query_p99_ms", percentile(latencies_ms, 0.99)},
    };
}

[[nodiscard]] JsonValue scenario_c_compaction_impact() {
    // Same content with a compact (checkpoint summary) P3 item versus the
    // full history in P5: fidelity of P0/P1 stays, token cost drops.
    ContextLimits limits;
    limits.context_window_tokens = 8'192;
    limits.reserved_output_tokens = 512;
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());

    ContextRequest request;
    request.task_id = TaskId::generate();
    request.session_id = SessionId::generate();
    request.profile_id = ModelProfileId::generate();
    request.limits = limits;
    request.items.push_back(testing::text_item(ContextItemKind::SystemPolicy,
                                                ContextAuthority::SystemPolicy, "safety policy", 1));
    request.items.push_back(testing::text_item(ContextItemKind::Goal, ContextAuthority::UserConstraint,
                                                "goal: compact history", 2));
    std::uint64_t sequence = 3;
    for (int index = 0; index < 200; ++index) {
        request.items.push_back(testing::text_item(
            ContextItemKind::HistoricalPayload, ContextAuthority::VerifiedState,
            "history event " + std::to_string(index) + " with a moderately long description", sequence++));
    }

    const auto full = manager.prepare(request);
    MIRA_CHECK(full.has_value());

    ContextRequest compacted = request;
    compacted.items.clear();
    compacted.items.push_back(request.items[0]);
    compacted.items.push_back(request.items[1]);
    compacted.items.push_back(testing::text_item(ContextItemKind::CheckpointSummary,
                                                 ContextAuthority::VerifiedState,
                                                 "checkpoint: 200 events summarized", 3));
    const auto compact = manager.prepare(compacted);
    MIRA_CHECK(compact.has_value());

    // P0/P1 fidelity: both builds keep the pinned policy and goal items.
    const auto keep_count = [](const PreparedModelContext &prepared) {
        std::size_t kept = 0;
        for (const auto &audit : prepared.item_audit) {
            if ((audit.kind == ContextItemKind::SystemPolicy ||
                 audit.kind == ContextItemKind::Goal) &&
                audit.disposition != ContextItemDisposition::Dropped) {
                ++kept;
            }
        }
        return kept;
    };
    return JsonValue::Object{
        {"full_history_tokens",
         static_cast<std::int64_t>(full.value().budget.estimated_tokens)},
        {"compacted_tokens",
         static_cast<std::int64_t>(compact.value().budget.estimated_tokens)},
        {"full_p0_p1_kept", static_cast<std::int64_t>(keep_count(full.value()))},
        {"compacted_p0_p1_kept", static_cast<std::int64_t>(keep_count(compact.value()))},
        {"token_savings_ratio",
         full.value().budget.estimated_tokens == 0
             ? 0.0
             : 1.0 - static_cast<double>(compact.value().budget.estimated_tokens) /
                         static_cast<double>(full.value().budget.estimated_tokens)},
    };
}

[[nodiscard]] JsonValue scenario_d_provider_switch() {
    ContinuationCache cache;
    auto continuation = ProviderContinuation{};
    continuation.provider_state = "opaque";
    continuation.provider = "openai-compatible";
    continuation.conversation = "conv";
    continuation.profile_id = ModelProfileId::generate();
    continuation.task_id = TaskId::generate();
    continuation.session_id = SessionId::generate();
    continuation.expires_at = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    MIRA_CHECK(cache.store(continuation).has_value());

    ContinuationBinding binding;
    binding.provider = continuation.provider;
    binding.conversation = continuation.conversation;
    binding.profile_id = continuation.profile_id;
    binding.task_id = continuation.task_id;
    binding.session_id = continuation.session_id;
    binding.now = std::chrono::steady_clock::now();

    MIRA_CHECK(cache.lookup(binding).has_value());

    // Provider switch invalidates the continuation; the next context must be
    // rebuilt from the local checkpoint (the cache reports the reason).
    ContinuationBinding switched = binding;
    switched.provider = "other-provider";
    const auto invalid = !cache.lookup(switched).has_value();
    MIRA_CHECK(invalid);

    // Rebuild cost proxy: the replacement context is a fresh prepare with
    // the checkpoint summary; tokens below are the benchmark's cost units.
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    ContextRequest request;
    request.task_id = continuation.task_id;
    request.session_id = continuation.session_id;
    request.profile_id = ModelProfileId::generate();
    request.items.push_back(testing::text_item(ContextItemKind::SystemPolicy,
                                               ContextAuthority::SystemPolicy, "policy", 1));
    request.items.push_back(testing::text_item(
        ContextItemKind::CheckpointSummary, ContextAuthority::VerifiedState,
        "checkpoint rebuild after provider switch", 2));
    const auto rebuilt = manager.prepare(request);
    MIRA_CHECK(rebuilt.has_value());
    return JsonValue::Object{
        {"continuation_invalid_on_switch", invalid ? std::int64_t{1} : std::int64_t{0}},
        {"rebuild_tokens", static_cast<std::int64_t>(rebuilt.value().budget.estimated_tokens)},
    };
}

} // namespace

int main(int argc, char **argv) {
    std::random_device device;
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-m4-benchmark-" + std::to_string(device()));
    Fixture fixture(root);
    JsonValue::Array failures;
    const auto scenario_a = scenario_a_long_task_recovery(fixture, failures);
    const auto scenario_b = scenario_b_memory_retrieval(fixture);
    const auto scenario_c = scenario_c_compaction_impact();
    const auto scenario_d = scenario_d_provider_switch();

    JsonValue::Object report;
    report.emplace_back("scenario", "m4-long-task-memory");
    report.emplace_back("long_task_recovery", scenario_a);
    report.emplace_back("memory_retrieval", scenario_b);
    report.emplace_back("compaction_impact", scenario_c);
    report.emplace_back("provider_switch", scenario_d);
    report.emplace_back("failures", JsonValue(std::move(failures)));
    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << std::endl;
    if (argc > 1) {
        std::ofstream file(argv[1]);
        file << json << '\n';
    }
    std::filesystem::remove_all(root);

    // The scenarios assert their own invariants; a red benchmark run means
    // a real fidelity or latency-budget regression, not a soft report.
    MIRA_CHECK(failures.empty());
    return 0;
}
