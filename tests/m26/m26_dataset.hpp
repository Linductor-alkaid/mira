#pragma once

// M26 (DEC-037 Stage T1) frozen deterministic evaluation dataset.
//
// Pure data: this header deliberately does NOT include the temporal-policy
// contract header, so the dataset digest below is computable and pinnable
// before/independently of the implementation (M20/M24 precedent: the digest
// is computed, not assumed, and anchored as a constant in the harness).
// m26_temporal_policy_eval.cpp converts these samples into
// PolicyEpisodeSample values mechanically.
//
// Scenario domain = temporal_policy_design.md §6.1 example: an enemy entity
// performing `heavy_slash_a` past the phase threshold at close distance is
// dodged (parameter "left"). Construction satisfies the frozen induction
// feasibility premises (milestone §4.4):
//   (a) the training pattern repeats with a byte-identical signature
//       >= min_support (3) times: motion=heavy_slash_a, phase=0.67,
//       confidence=0.92, facts {distance=2.7}, action dodge/left;
//   (b) every sample carries 1 fact key <= max_conditions_per_transition-3;
//   (c) the held-out positives stay inside the support closure (phase >=
//       0.67, confidence >= 0.92, distance <= 2.7) with at least one
//       strictly non-boundary value, >= min_test_support (2) of them;
//   (d) samples the rule does not match carry semantics that never enter
//       any count (one idle pair below min_support, one unhandled sample).
// The replay part repeats the training signature with agent_handled=false
// and includes the exact support boundary phase value 0.67 (the Ge
// self-cover assertion for T1-G5).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <mira/event_store.hpp> // header-only digest_string

namespace mira::m26 {

struct DatasetEntity final {
    std::string key;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    std::string motion;
    double phase = 0.0;
    double confidence = 0.0;
    std::string source_ref;
};

struct DatasetFact final {
    std::string key;
    double value = 0.0;
};

struct DatasetSample final {
    enum class Part : std::uint8_t { Train, Heldout, Replay };
    Part part = Part::Train;
    std::uint64_t tick = 0;
    std::int64_t time_ms = 0; // deterministic dataset clock, never wall time
    std::string active_state;
    std::vector<DatasetEntity> entities;
    std::vector<DatasetFact> facts;
    bool agent_handled = true;
    std::string agent_action_kind; // empty + unhandled => no agent action
    std::string agent_action_parameter;
};

// Fixed harness configuration (milestone §6 T1-G4/G5; runtime options stay at
// the documented defaults: min_support=3, min_test_support=2, capacity=64).
struct FrozenDatasetConfig final {
    std::size_t support_samples = 4;   // >= min_support, identical signature
    std::size_t below_support = 2;     // idle signature, < min_support
    std::size_t unhandled_samples = 1; // excluded from induction entirely
    std::size_t heldout_positives = 3; // >= min_test_support, in closure
    std::size_t heldout_negatives = 1; // non-matching, counts nothing
    std::size_t replay_samples = 3;    // includes the boundary phase value
    std::int64_t time_base_ms = 1'700'000'000'000;
};

// ---------------------------------------------------------------------------
// Deterministic rendering used by the dataset digest (stable field order,
// fixed formatting; no wall-clock anywhere).
// ---------------------------------------------------------------------------

inline void append_number(std::string &out, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out += buffer;
}

inline void append_sample_line(std::string &out, const DatasetSample &sample, std::size_t index) {
    out += "S ";
    out += std::to_string(index);
    switch (sample.part) {
    case DatasetSample::Part::Train:
        out += " part=train";
        break;
    case DatasetSample::Part::Heldout:
        out += " part=heldout";
        break;
    case DatasetSample::Part::Replay:
        out += " part=replay";
        break;
    }
    out += " tick=" + std::to_string(sample.tick);
    out += " time_ms=" + std::to_string(sample.time_ms);
    out += " state=" + sample.active_state;
    for (const auto &entity : sample.entities) {
        out += " E key=" + entity.key + " x=";
        append_number(out, entity.x);
        out += " y=";
        append_number(out, entity.y);
        out += " vx=";
        append_number(out, entity.vx);
        out += " vy=";
        append_number(out, entity.vy);
        out += " motion=" + entity.motion + " phase=";
        append_number(out, entity.phase);
        out += " conf=";
        append_number(out, entity.confidence);
        out += " src=" + entity.source_ref;
    }
    for (const auto &fact : sample.facts) {
        out += " F key=" + fact.key + " value=";
        append_number(out, fact.value);
    }
    out += " handled=";
    out += sample.agent_handled ? "1" : "0";
    out += " action=" +
           (sample.agent_action_kind.empty() ? std::string("-") : sample.agent_action_kind);
    out += "/" + (sample.agent_action_parameter.empty() ? std::string("-")
                                                        : sample.agent_action_parameter);
    out += '\n';
}

// ---------------------------------------------------------------------------
// The frozen dataset: combat-state dodge loop over one enemy entity.
// ---------------------------------------------------------------------------

[[nodiscard]] inline DatasetEntity enemy_entity(std::uint64_t index) {
    DatasetEntity entity;
    entity.key = "enemy_1";
    entity.x = 5.0;
    entity.y = 5.0;
    entity.vx = -0.5;
    entity.vy = 0.25;
    entity.motion = "heavy_slash_a";
    entity.phase = 0.67;
    entity.confidence = 0.92;
    entity.source_ref = "dataset://m26/observation/" + std::to_string(index);
    return entity;
}

[[nodiscard]] inline DatasetEntity idle_entity(std::uint64_t index) {
    DatasetEntity entity;
    entity.key = "enemy_1";
    entity.x = 9.0;
    entity.y = 9.0;
    entity.vx = 0.0;
    entity.vy = 0.0;
    entity.motion = "idle";
    entity.phase = 0.1;
    entity.confidence = 0.5;
    entity.source_ref = "dataset://m26/observation/" + std::to_string(index);
    return entity;
}

[[nodiscard]] inline std::vector<DatasetSample>
build_m26_dataset(const FrozenDatasetConfig &config) {
    std::vector<DatasetSample> samples;
    std::uint64_t tick = 0;
    std::uint64_t observation = 0;
    auto next_tick = [&tick]() { return ++tick; };
    auto next_time = [&config](std::uint64_t step) {
        return config.time_base_ms + static_cast<std::int64_t>(step) * 10;
    };

    // Training support: identical signature repeated (feasibility premise a).
    for (std::size_t index = 0; index < config.support_samples; ++index) {
        DatasetSample sample;
        sample.part = DatasetSample::Part::Train;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        sample.entities.push_back(enemy_entity(++observation));
        sample.facts.push_back({"distance", 2.7});
        sample.agent_handled = true;
        sample.agent_action_kind = "dodge";
        sample.agent_action_parameter = "left";
        samples.push_back(std::move(sample));
    }

    // Idle pair below min_support: must induce nothing (premise d).
    for (std::size_t index = 0; index < config.below_support; ++index) {
        DatasetSample sample;
        sample.part = DatasetSample::Part::Train;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        sample.entities.push_back(idle_entity(++observation));
        sample.facts.push_back({"distance", 6.0});
        sample.agent_handled = true;
        sample.agent_action_kind = "hold";
        samples.push_back(std::move(sample));
    }

    // Same training signature but agent_handled=false: excluded from
    // induction, so the support count stays at support_samples.
    for (std::size_t index = 0; index < config.unhandled_samples; ++index) {
        DatasetSample sample;
        sample.part = DatasetSample::Part::Train;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        sample.entities.push_back(enemy_entity(++observation));
        sample.facts.push_back({"distance", 2.7});
        sample.agent_handled = false;
        samples.push_back(std::move(sample));
    }

    // Held-out test positives: inside the support closure with at least one
    // strictly non-boundary value (premise c); the scripted agent handles
    // them with the same action so the candidate accumulates clean evidence.
    struct HeldoutCase final {
        double phase;
        double confidence;
        double distance;
    };
    const HeldoutCase heldout_cases[] = {{0.67, 0.92, 2.7}, {0.8, 0.95, 2.5}, {0.9, 0.92, 2.7}};
    static_assert(sizeof(heldout_cases) / sizeof(heldout_cases[0]) >= 2);
    for (std::size_t index = 0; index < config.heldout_positives; ++index) {
        const auto &entry = heldout_cases[index % 3];
        DatasetSample sample;
        sample.part = DatasetSample::Part::Heldout;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        auto entity = enemy_entity(++observation);
        entity.phase = entry.phase;
        entity.confidence = entry.confidence;
        sample.entities.push_back(std::move(entity));
        sample.facts.push_back({"distance", entry.distance});
        sample.agent_handled = true;
        sample.agent_action_kind = "dodge";
        sample.agent_action_parameter = "left";
        samples.push_back(std::move(sample));
    }

    // Held-out negative: the pattern does not match, so no counter may move.
    for (std::size_t index = 0; index < config.heldout_negatives; ++index) {
        DatasetSample sample;
        sample.part = DatasetSample::Part::Heldout;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        sample.entities.push_back(idle_entity(++observation));
        sample.facts.push_back({"distance", 6.0});
        sample.agent_handled = true;
        sample.agent_action_kind = "hold";
        samples.push_back(std::move(sample));
    }

    // Replay part: the training signature with agent_handled=false. The
    // first sample carries the exact support boundary phase 0.67 - the Ge
    // self-cover assertion - and the others stay inside the closure.
    const double replay_phases[] = {0.67, 0.75, 0.99};
    for (std::size_t index = 0; index < config.replay_samples; ++index) {
        DatasetSample sample;
        sample.part = DatasetSample::Part::Replay;
        sample.tick = next_tick();
        sample.time_ms = next_time(sample.tick);
        sample.active_state = "combat";
        auto entity = enemy_entity(++observation);
        entity.phase = replay_phases[index % 3];
        entity.confidence = 0.92 + 0.005 * static_cast<double>(index);
        sample.entities.push_back(std::move(entity));
        sample.facts.push_back({"distance", 2.7 - 0.1 * static_cast<double>(index)});
        sample.agent_handled = false;
        samples.push_back(std::move(sample));
    }
    return samples;
}

// Digest over the deterministic rendering; anchored below as a frozen
// constant and asserted by the eval harness on every run.
[[nodiscard]] inline std::string dataset_canonical_text(const std::vector<DatasetSample> &samples) {
    std::string text;
    text.reserve(64 + samples.size() * 160);
    text += "m26-temporal-policy-t1-dataset-v1\n";
    text += "samples=" + std::to_string(samples.size()) + "\n";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        append_sample_line(text, samples[index], index);
    }
    return text;
}

[[nodiscard]] inline Sha256Digest dataset_digest(const FrozenDatasetConfig &config,
                                                 const std::vector<DatasetSample> &samples) {
    (void)config; // the digest covers the frozen sample set only
    return digest_string(dataset_canonical_text(samples));
}

[[nodiscard]] inline std::string to_hex(const Sha256Digest &digest) {
    static const char *kHex = "0123456789abcdef";
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        text.push_back(kHex[byte >> 4U]);
        text.push_back(kHex[byte & 0x0FU]);
    }
    return text;
}

// Frozen anchor: computed from the first deterministic build of this header
// on 2026-09-24 (14 samples, g++ -std=c++20, /tmp pin program linking
// digest_bytes from mira_core), then pinned (M20/M24 precedent); the eval
// harness asserts equality on every run and fails closed on any drift.
inline constexpr char kPinnedDatasetDigest[] =
    "a6e18b441ea22636fc55ff693a5d466c8987ddd4bff393eaae4b8210be8c73fc";

} // namespace mira::m26
