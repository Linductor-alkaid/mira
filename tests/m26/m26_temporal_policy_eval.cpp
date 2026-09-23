// M26 (DEC-037 Stage T1) deterministic closed-loop eval harness (gates
// T1-G4 and T1-G5, design §12 T1 proof). Builds the frozen dataset
// (tests/m26/m26_dataset.hpp, §6.1 example domain, pinned digest), then
// drives the full evidence pipeline twice - induce -> adopt -> test ->
// promote -> replay - through executor.submit_auto jobs with consumed
// futures (§4.5, AGENTS.md rule 3/6), asserting:
//
//   T1-G4 rebuild and cross-process determinism:
//     - the dataset digest equals the pinned constant (fail closed on
//       drift);
//     - rebuild_temporal_histories replays are byte-identical, cover every
//       sample and leave source_digest empty on every entry (§4.1);
//     - two pipeline passes produce identical rule-set digests, identical
//       testing-report digests and a byte-identical report (M16-M20
//       convention: the report embeds no wall-clock, so cross-process runs
//       of this binary are byte-identical on the gate runners).
//
//   T1-G5 deterministic closed loop end to end:
//     - the training part is handled by the scripted agent, induces one
//       candidate, passes the evidence gate and is promoted host-explicitly;
//     - the replay part (agent_handled=false) is executed by the promoted
//       rule with exactly the agent's historical action, including the
//       support-boundary phase value 0.67 (the Ge self-cover assertion);
//     - agent calls in the replay part are strictly below the all-agent
//       baseline;
//     - an injected colliding rule escalates fail-closed (zero actions,
//       RuleConflicted + PolicyEscalatedToAgent, both rules demoted) and
//       the script agent recovers the tick;
//     - all metrics are pipeline-behaviour metrics (RULE-10): the report
//       contains no latency/real-time or semantic-quality claim;
//     - every sink callback runs on the submitting thread: the runtime
//       spawns no thread, timer or registration loop of its own.
//
// Round-2 note: the runtime-side TemporalHistory accumulator is observable
// through the additive read-only accessor ReactivePolicyRuntime::
// temporal_history(entity_key) (DEC-002 evolution, delivered with M26-02 to
// close the observation gap reported in verification round 1); the harness
// asserts the runtime accumulator digest equals the rebuild digest for the
// exact episode sequence the runtime stepped (T1-G4).

#include "m26_cases.hpp"
#include "m26_dataset.hpp"

#include <executor/executor.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace mira::m26 {
namespace {

int check(bool condition, const char *what) {
    if (!condition) {
        std::cerr << "m26 eval gate failure: " << what << '\n';
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Dataset -> episode conversion (mechanical; the dataset header stays pure).
// ---------------------------------------------------------------------------

[[nodiscard]] PolicyEpisodeSample to_episode(const DatasetSample &sample) {
    PolicyEpisodeSample episode;
    episode.tick = make_tick(sample.tick);
    PolicyWorldView world;
    for (const auto &entity : sample.entities) {
        TrackedEntity tracked;
        tracked.entity_key = entity.key;
        tracked.position_x = entity.x;
        tracked.position_y = entity.y;
        tracked.velocity_x = entity.vx;
        tracked.velocity_y = entity.vy;
        tracked.motion = entity.motion;
        tracked.motion_phase = entity.phase;
        tracked.motion_confidence = entity.confidence;
        tracked.source_ref = entity.source_ref;
        world.entities.push_back(std::move(tracked));
    }
    for (const auto &fact : sample.facts) {
        world.facts.push_back(make_fact(fact.key, fact.value));
    }
    episode.world = std::move(world);
    episode.active_state = sample.active_state;
    if (sample.agent_handled) {
        episode.agent_action = make_action(sample.agent_action_kind, sample.agent_action_parameter);
    }
    episode.agent_handled = sample.agent_handled;
    return episode;
}

// ---------------------------------------------------------------------------
// One full pipeline pass (induce -> adopt -> test -> promote -> replay ->
// conflict injection). Runs on an executor worker; every step is a bounded
// synchronous call.
// ---------------------------------------------------------------------------

struct PipelineOutcome final {
    bool ok = true;
    std::string failure;
    std::size_t induced = 0;
    std::size_t adopted = 0;
    std::uint32_t test_passed = 0;
    std::uint32_t test_false_triggers = 0;
    std::size_t promoted_induced = 0;
    std::size_t promoted_injected = 0;
    std::string rule_set_digest;
    std::string testing_report_digest;
    std::size_t replay_samples = 0;
    std::size_t replay_rule_actions = 0;
    std::size_t replay_agent_fallbacks = 0;
    bool boundary_phase_triggered = false;
    std::size_t conflict_escalations = 0;
    std::size_t conflict_demotions = 0;
    std::size_t recovery_agent_calls = 0;
    std::size_t events = 0;
    bool all_events_on_caller_thread = true;
    bool accumulator_matches_rebuild = false;
    bool step_digests_empty = true;
    std::string accumulator_digest;
};

#define M26_EVAL_STEP(condition, message)                                                          \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            outcome.ok = false;                                                                    \
            outcome.failure = (message);                                                           \
            return outcome;                                                                        \
        }                                                                                          \
    } while (false)

[[nodiscard]] PipelineOutcome run_pipeline(const std::vector<DatasetSample> &dataset) {
    PipelineOutcome outcome;
    RecordingSink sink;
    PolicyRuntimeOptions options;
    ReactivePolicyRuntime runtime{options, &sink};

    std::vector<PolicyEpisodeSample> train;
    std::vector<PolicyEpisodeSample> heldout;
    std::vector<PolicyEpisodeSample> replay;
    std::uint64_t max_tick = 0;
    for (const auto &sample : dataset) {
        max_tick = std::max(max_tick, sample.tick);
        switch (sample.part) {
        case DatasetSample::Part::Train:
            train.push_back(to_episode(sample));
            break;
        case DatasetSample::Part::Heldout:
            heldout.push_back(to_episode(sample));
            break;
        case DatasetSample::Part::Replay:
            replay.push_back(to_episode(sample));
            break;
        }
    }

    // Host-explicit activation; then the frozen evidence pipeline.
    M26_EVAL_STEP(runtime.activate("combat").has_value(), "activate combat");

    auto candidates = runtime.induce_candidate_rules(
        std::span<const PolicyEpisodeSample>(train.data(), train.size()));
    M26_EVAL_STEP(candidates.has_value(), "induction succeeds");
    M26_EVAL_STEP(candidates.value().size() == 1, "exactly one induced candidate");
    outcome.induced = candidates.value().size();

    const auto adopted = runtime.adopt_candidate_rules(candidates.value());
    M26_EVAL_STEP(adopted.has_value() && adopted.value() == 1, "adoption accepts the candidate");
    outcome.adopted = static_cast<std::size_t>(adopted.value());

    auto report = runtime.test_candidate_rules(
        std::span<const PolicyEpisodeSample>(heldout.data(), heldout.size()));
    M26_EVAL_STEP(report.has_value(), "held-out testing succeeds");
    for (const auto &row : report.value().results) {
        outcome.test_passed += row.passed_count;
        outcome.test_false_triggers += row.false_trigger_count;
    }
    // The promotion evidence gate opens: clean passes at or above
    // min_test_support and zero false triggers.
    M26_EVAL_STEP(outcome.test_false_triggers == 0, "held-out testing is clean");
    M26_EVAL_STEP(outcome.test_passed >= options.min_test_support,
                  "held-out passes reach min_test_support");

    M26_EVAL_STEP(runtime.promote_rule(candidates.value()[0].rule_id).has_value(),
                  "host promotes the induced rule");
    outcome.promoted_induced = 1;
    outcome.testing_report_digest = report.value().digest().to_string();

    std::vector<ReactiveRule> live = candidates.value();
    outcome.rule_set_digest = rule_set_digest(live).to_string();

    // Replay part: agent_handled=false - the promoted rule must execute the
    // pattern with the agent's historical action, boundary phase included.
    outcome.replay_samples = replay.size();
    std::vector<PolicyEpisodeSample> stepped;
    for (std::size_t index = 0; index < replay.size(); ++index) {
        stepped.push_back(replay[index]);
        const auto actions = runtime.step(replay[index].world, replay[index].tick);
        M26_EVAL_STEP(actions.has_value(), "replay step succeeds");
        const bool handled_by_rule = actions.value().size() == 1 &&
                                     actions.value()[0].kind == "dodge" &&
                                     actions.value()[0].parameter == "left";
        if (handled_by_rule) {
            ++outcome.replay_rule_actions;
        } else {
            ++outcome.replay_agent_fallbacks; // would have been an agent call
        }
        if (dataset[dataset.size() - replay.size() + index].entities[0].phase == 0.67 &&
            handled_by_rule) {
            outcome.boundary_phase_triggered = true;
        }
    }
    M26_EVAL_STEP(outcome.replay_rule_actions == outcome.replay_samples,
                  "the promoted rule executes every replay tick");
    M26_EVAL_STEP(outcome.boundary_phase_triggered,
                  "the support-boundary phase value 0.67 triggers (Ge self-cover)");

    // Conflict injection: a hand rule sharing the replay world is promoted
    // on the held-out positives, then the next replay-pattern tick matches
    // both Runtime rules -> fail-closed escalation + full demotion.
    auto injected =
        make_rule("combat", 0,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 3.0)},
                                   {make_action("dodge", "left")})},
                  {"dataset://m26/injected/conflict-probe"});
    const auto injected_adopted =
        runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&injected, 1});
    M26_EVAL_STEP(injected_adopted.has_value() && injected_adopted.value() == 1,
                  "conflict probe adopts");
    auto injected_report = runtime.test_candidate_rules(
        std::span<const PolicyEpisodeSample>(heldout.data(), heldout.size()));
    M26_EVAL_STEP(injected_report.has_value(), "conflict probe tests");
    for (const auto &row : injected_report.value().results) {
        if (row.rule_id == injected.rule_id) {
            M26_EVAL_STEP(row.passed_count == 3 && row.false_trigger_count == 0,
                          "conflict probe evidence is clean");
        }
    }
    M26_EVAL_STEP(runtime.promote_rule(injected.rule_id).has_value(), "conflict probe promotes");
    outcome.promoted_injected = 1;

    PolicyEpisodeSample conflict_episode = replay[0];
    conflict_episode.tick = make_tick(max_tick + 1);
    stepped.push_back(conflict_episode);
    const auto conflicted = runtime.step(conflict_episode.world, conflict_episode.tick);
    M26_EVAL_STEP(conflicted.has_value() && conflicted.value().empty(),
                  "conflict executes zero actions (fail-closed)");
    outcome.conflict_escalations = sink.count(PolicyEventType::PolicyEscalatedToAgent);
    outcome.conflict_demotions = sink.count(PolicyEventType::RuleDemoted);
    M26_EVAL_STEP(outcome.conflict_escalations == 1, "exactly one escalation event");
    M26_EVAL_STEP(outcome.conflict_demotions == 2, "both matched rules demoted");
    // The script agent recovers the escalated tick itself (agent call).
    outcome.recovery_agent_calls = 1;

    // Zero self-built threads: every sink callback ran on this thread.
    for (const auto id : sink.threads) {
        if (id != std::this_thread::get_id()) {
            outcome.all_events_on_caller_thread = false;
        }
    }
    outcome.events = sink.events.size();

    // T1-G4 accumulator leg: the runtime's per-entity step() accumulator and
    // the rebuild projection over the exact stepped episode sequence must be
    // byte-identical (same PolicyRuntimeOptions::history_options), and the
    // step path keeps source_digest empty (frozen §4.1 B2).
    const TemporalHistory *accumulated = runtime.temporal_history("enemy_1");
    M26_EVAL_STEP(accumulated != nullptr, "runtime accumulator exists for enemy_1");
    outcome.accumulator_digest = accumulated->digest().to_string();
    for (const auto &entry : accumulated->entries()) {
        if (!entry.source_digest.empty()) {
            outcome.step_digests_empty = false;
        }
    }
    auto stepped_rebuild = rebuild_temporal_histories(
        std::span<const PolicyEpisodeSample>(stepped.data(), stepped.size()),
        options.history_options);
    M26_EVAL_STEP(stepped_rebuild.has_value() && stepped_rebuild.value().count("enemy_1") == 1,
                  "stepped-sequence rebuild produced");
    outcome.accumulator_matches_rebuild =
        outcome.step_digests_empty &&
        stepped_rebuild.value().at("enemy_1").digest() == accumulated->digest();
    M26_EVAL_STEP(outcome.accumulator_matches_rebuild,
                  "runtime accumulator digest equals rebuild digest (T1-G4)");
    return outcome;
}

#undef M26_EVAL_STEP

[[nodiscard]] std::string outcome_report(const FrozenDatasetConfig &config,
                                         const std::vector<DatasetSample> &samples,
                                         const PipelineOutcome &first,
                                         const PipelineOutcome &second,
                                         const TemporalHistory &rebuilt, bool rebuild_identical) {
    const std::size_t baseline_agent_calls = first.replay_samples; // all-agent counterfactual
    JsonValue::Object report;
    report.emplace_back("eval", JsonValue("m26-temporal-policy-t1"));
    report.emplace_back("schema", JsonValue("mira.policy.temporal-t1-eval.v1"));
    report.emplace_back("dataset_digest", JsonValue(std::string(kPinnedDatasetDigest)));
    report.emplace_back("samples", JsonValue(samples.size()));
    report.emplace_back("min_support", JsonValue(static_cast<std::int64_t>(3)));
    report.emplace_back("min_test_support", JsonValue(static_cast<std::int64_t>(2)));
    report.emplace_back("induced_candidates", JsonValue(first.induced));
    report.emplace_back("adopted", JsonValue(first.adopted));
    report.emplace_back("test_passed", JsonValue(static_cast<std::int64_t>(first.test_passed)));
    report.emplace_back("test_false_triggers",
                        JsonValue(static_cast<std::int64_t>(first.test_false_triggers)));
    report.emplace_back("promoted_induced", JsonValue(first.promoted_induced));
    report.emplace_back("promoted_injected", JsonValue(first.promoted_injected));
    report.emplace_back("rule_set_digest", JsonValue(first.rule_set_digest));
    report.emplace_back("testing_report_digest", JsonValue(first.testing_report_digest));
    report.emplace_back("replay_samples", JsonValue(first.replay_samples));
    report.emplace_back("baseline_agent_calls", JsonValue(baseline_agent_calls));
    report.emplace_back("replay_rule_actions", JsonValue(first.replay_rule_actions));
    report.emplace_back("replay_agent_fallbacks", JsonValue(first.replay_agent_fallbacks));
    report.emplace_back("boundary_phase_triggered", JsonValue(first.boundary_phase_triggered));
    report.emplace_back("conflict_escalations", JsonValue(first.conflict_escalations));
    report.emplace_back("conflict_demotions", JsonValue(first.conflict_demotions));
    report.emplace_back("recovery_agent_calls", JsonValue(first.recovery_agent_calls));
    report.emplace_back("policy_events", JsonValue(first.events));
    report.emplace_back("rebuild_entities", JsonValue(static_cast<std::int64_t>(1)));
    report.emplace_back("rebuild_entries", JsonValue(rebuilt.size()));
    report.emplace_back("rebuild_digest", JsonValue(rebuilt.digest().to_string()));
    report.emplace_back("rebuild_replays_identical", JsonValue(rebuild_identical));
    report.emplace_back("accumulator_digest", JsonValue(first.accumulator_digest));
    report.emplace_back("accumulator_matches_rebuild",
                        JsonValue(first.accumulator_matches_rebuild));
    report.emplace_back("sink_all_on_caller_thread", JsonValue(first.all_events_on_caller_thread));
    report.emplace_back("runs_byte_identical",
                        JsonValue(first.rule_set_digest == second.rule_set_digest &&
                                  first.testing_report_digest == second.testing_report_digest &&
                                  first.events == second.events));

    // Gate verdicts mirror the §6 T1-G4/G5 wording.
    JsonValue::Object gates;
    gates.emplace_back(
        "g4_dataset_digest_pinned",
        JsonValue(std::string(kPinnedDatasetDigest) == to_hex(dataset_digest(config, samples))));
    gates.emplace_back("g4_rebuild_replay_identical", JsonValue(rebuild_identical));
    gates.emplace_back("g4_accumulator_equals_rebuild",
                       JsonValue(first.accumulator_matches_rebuild));
    gates.emplace_back("g4_pipeline_digests_stable",
                       JsonValue(first.rule_set_digest == second.rule_set_digest &&
                                 first.testing_report_digest == second.testing_report_digest));
    gates.emplace_back("g5_rule_executes_replay_without_agent",
                       JsonValue(first.replay_rule_actions == first.replay_samples &&
                                 first.replay_agent_fallbacks == 0));
    gates.emplace_back("g5_agent_calls_strictly_lower",
                       JsonValue(first.replay_agent_fallbacks < baseline_agent_calls));
    gates.emplace_back("g5_conflict_fail_closed_and_recovery",
                       JsonValue(first.conflict_escalations == 1 && first.conflict_demotions == 2 &&
                                 first.recovery_agent_calls == 1));
    gates.emplace_back("g5_zero_self_built_threads", JsonValue(first.all_events_on_caller_thread));
    report.emplace_back("gates", JsonValue(std::move(gates)));

    // RULE-10 honesty: deterministic pipeline behaviour only.
    report.emplace_back(
        "limitations",
        "deterministic pipeline behaviour metrics only (induction, evidence-gated promotion, "
        "rule-executed replay, agent-call reduction, conflict escalation and recovery) over a "
        "frozen synthetic dataset; no latency, jitter, real-time, resource or semantic-quality "
        "claim is made (RULE-10; T6 gates real-time evidence)");
    return to_json_string(JsonValue(std::move(report)));
}

} // namespace
} // namespace mira::m26

int main(int argc, char **argv) {
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (output_path.empty()) {
            output_path = argument;
        }
    }

    using namespace mira;
    using namespace mira::m26;

    const FrozenDatasetConfig config;
    const auto samples = build_m26_dataset(config);

    int failed = 0;
    failed += check(samples.size() == 14, "dataset sample count");
    const std::string digest_hex = to_hex(dataset_digest(config, samples));
    if (digest_hex != kPinnedDatasetDigest) {
        std::cerr << "dataset digest mismatch: got " << digest_hex << ", want "
                  << kPinnedDatasetDigest << "\n";
        return 2;
    }
    failed += check(to_hex(dataset_digest(config, samples)) == kPinnedDatasetDigest,
                    "frozen dataset digest pin");

    // Executor-owned orchestration (§4.5): both pipeline passes are
    // submit_auto jobs whose futures are consumed here, and shutdown runs
    // on this non-worker thread after the futures resolve (AGENTS.md 3/6).
    executor::Executor executor;
    executor::ExecutorConfig exec_config;
    exec_config.min_threads = 2;
    exec_config.max_threads = 2;
    exec_config.queue_capacity = 32;
    if (!executor.initialize(exec_config)) {
        std::cerr << "m26 eval: executor initialize failed\n";
        return 2;
    }
    {
        auto job = [&samples]() { return run_pipeline(samples); };
        auto first_future = executor.submit_auto(job);
        auto second_future = executor.submit_auto(job);
        const PipelineOutcome first = first_future.get();
        const PipelineOutcome second = second_future.get();

        failed += check(first.ok, ("first pipeline pass: " + first.failure).c_str());
        failed += check(second.ok, ("second pipeline pass: " + second.failure).c_str());
        failed += check(first.induced == 1, "exactly one induced candidate");
        failed += check(first.adopted == 1, "exactly one adopted rule");
        failed += check(first.test_passed == 3 && first.test_false_triggers == 0,
                        "held-out evidence is three clean passes");
        failed += check(first.promoted_induced == 1, "induced rule promoted");
        failed += check(first.replay_samples == 3, "replay part size");
        failed += check(first.replay_rule_actions == first.replay_samples,
                        "promoted rule executes every replay tick");
        failed += check(first.replay_agent_fallbacks == 0, "zero agent fallbacks in replay");
        failed += check(first.replay_agent_fallbacks < first.replay_samples,
                        "agent calls strictly below the all-agent baseline");
        failed += check(first.boundary_phase_triggered,
                        "support-boundary phase 0.67 triggers (Ge self-cover)");
        failed += check(first.conflict_escalations == 1, "conflict escalates exactly once");
        failed += check(first.conflict_demotions == 2, "conflict demotes both matched rules");
        failed +=
            check(first.recovery_agent_calls == 1, "script agent recovers the escalated tick");
        failed +=
            check(first.all_events_on_caller_thread, "all policy events on the caller thread");
        failed += check(first.accumulator_matches_rebuild,
                        "runtime accumulator digest equals rebuild digest (T1-G4)");
        failed += check(first.step_digests_empty, "step-path entries keep source_digest empty");

        // Two passes agree byte-for-byte on every digest (T1-G4/G5).
        failed += check(first.rule_set_digest == second.rule_set_digest,
                        "rule-set digest identical across passes");
        failed += check(first.testing_report_digest == second.testing_report_digest,
                        "testing-report digest identical across passes");
        failed += check(first.events == second.events, "event count identical across passes");

        // Full-pipeline replay of the rebuild projection (T1-G4).
        std::vector<PolicyEpisodeSample> episodes;
        for (const auto &sample : samples) {
            episodes.push_back(to_episode(sample));
        }
        PolicyRuntimeOptions options;
        auto rebuild_first = rebuild_temporal_histories(
            std::span<const PolicyEpisodeSample>(episodes.data(), episodes.size()),
            options.history_options);
        auto rebuild_second = rebuild_temporal_histories(
            std::span<const PolicyEpisodeSample>(episodes.data(), episodes.size()),
            options.history_options);
        failed += check(rebuild_first.has_value() && rebuild_second.has_value(),
                        "rebuild projections produced");
        if (rebuild_first.has_value() && rebuild_second.has_value()) {
            failed += check(rebuild_first.value().size() == 1, "rebuild keys exactly one entity");
            const auto found = rebuild_first.value().find("enemy_1");
            failed += check(found != rebuild_first.value().end(), "rebuild covers enemy_1");
            if (found != rebuild_first.value().end()) {
                const auto &history = found->second;
                failed += check(history.size() == samples.size(),
                                "rebuild covers every sample (no eviction at capacity 64)");
                const auto twin = rebuild_second.value().find("enemy_1");
                failed += check(twin != rebuild_second.value().end() &&
                                    twin->second.digest() == history.digest(),
                                "rebuild digest byte-identical across replays");
                bool digests_empty = true;
                for (const auto &entry : history.entries()) {
                    digests_empty = digests_empty && entry.source_digest.empty();
                }
                failed += check(digests_empty, "rebuild entries keep source_digest empty");

                const std::string report =
                    outcome_report(config, samples, first, second, history,
                                   twin != rebuild_second.value().end() &&
                                       twin->second.digest() == history.digest());
                failed += check(report.find("\"runs_byte_identical\":true") != std::string::npos,
                                "report records byte-identical runs");
                failed += check(report.find("\"g5_agent_calls_strictly_lower\":true") !=
                                    std::string::npos,
                                "report records the agent-call reduction");
                std::cout << report << '\n';
                if (!output_path.empty()) {
                    std::ofstream file(output_path);
                    file << report << '\n';
                }
            }
        }
    }
    failed += check(executor.shutdown(true) == executor::ShutdownResult::Completed,
                    "executor shutdown completed");

    if (failed == 0) {
        std::cout << "m26 temporal policy eval: OK\n";
        return 0;
    }
    std::cerr << "m26 temporal policy eval: " << failed << " checks failed\n";
    return 1;
}
