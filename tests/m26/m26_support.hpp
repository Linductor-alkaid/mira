#pragma once

// Shared fixtures for the M26 temporal-policy IVA test matrix (DEC-037
// Stage T1; milestone plan §4 frozen contract semantics, §6 gates
// T1-G1..G5). Consumers: tests/m26/*.cpp.
//
// Everything here pins the §4 frozen surface: the mira.temporal_policy
// error domain (12 explicit int32 codes + stable names + ErrorCode
// mapping), the nine mira.policy.*.v1 event schemas with their exact
// payload key sets, the frame rule_id classification, and deterministic
// builders for rules, worlds and episode samples. All timestamps come from
// the dataset clock, never from the system clock (§4.5).

#include "../support/test.hpp"

#include <mira/json.hpp>
#include <mira/temporal_policy.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mira::m26 {

// ---------------------------------------------------------------------------
// Frozen error domain table (milestone §4.4 "错误域冻结面", DEC-002 stable
// public values: explicit int32 values 1..12 and stable member names).
// ---------------------------------------------------------------------------

struct DomainCodeEntry final {
    TemporalPolicyDomainCode code;
    std::string_view name;
    ErrorCode mapped; // frozen deterministic ErrorCode assignment
};

inline const DomainCodeEntry kDomainCodes[] = {
    {TemporalPolicyDomainCode::OptionsInvalid, "OptionsInvalid", ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::BoundsExceeded, "BoundsExceeded", ErrorCode::InvalidState},
    {TemporalPolicyDomainCode::EntityInvalid, "EntityInvalid", ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::WorldViewInvalid, "WorldViewInvalid", ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::TickNotMonotonic, "TickNotMonotonic", ErrorCode::InvalidState},
    {TemporalPolicyDomainCode::RuleSchemaInvalid, "RuleSchemaInvalid", ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::RuleUnknown, "RuleUnknown", ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::RuleStateInvalid, "RuleStateInvalid", ErrorCode::InvalidState},
    {TemporalPolicyDomainCode::PromotionEvidenceMissing, "PromotionEvidenceMissing",
     ErrorCode::InvalidState},
    {TemporalPolicyDomainCode::EpisodeSamplesInvalid, "EpisodeSamplesInvalid",
     ErrorCode::InvalidArgument},
    {TemporalPolicyDomainCode::HistorySequenceNotAdvancing, "HistorySequenceNotAdvancing",
     ErrorCode::InvalidState},
    {TemporalPolicyDomainCode::HistoryCapacityInvalid, "HistoryCapacityInvalid",
     ErrorCode::InvalidArgument},
};

// ---------------------------------------------------------------------------
// Frozen event schema table (milestone §4.4 event table; T1 subset of nine).
// ---------------------------------------------------------------------------

struct EventSchemaEntry final {
    PolicyEventType type;
    std::string_view schema;
    std::vector<std::string_view> payload_keys; // exactly these, no more
    bool frame_carries_rule_id;
};

inline const EventSchemaEntry kEventSchemas[] = {
    {PolicyEventType::PolicyActivated, "mira.policy.policy-activated.v1", {"state"}, false},
    {PolicyEventType::PolicyDeactivated, "mira.policy.policy-deactivated.v1", {"state"}, false},
    {PolicyEventType::RuleCandidateInduced,
     "mira.policy.rule-candidate-induced.v1",
     {"source_ref_count", "state", "support_count"},
     true},
    {PolicyEventType::RuleTestingResulted,
     "mira.policy.rule-testing-resulted.v1",
     {"false_trigger_count", "passed_count", "sample_count"},
     true},
    {PolicyEventType::RulePromoted,
     "mira.policy.rule-promoted.v1",
     {"passed_count", "state", "support_count"},
     true},
    {PolicyEventType::RuleDemoted,
     "mira.policy.rule-demoted.v1",
     {"from_status", "reason", "to_status"},
     true},
    {PolicyEventType::RuleConflicted,
     "mira.policy.rule-conflicted.v1",
     {"matched_rule_ids", "state"},
     false},
    {PolicyEventType::RuleTriggered,
     "mira.policy.rule-triggered.v1",
     {"actions", "entity_key", "state"},
     true},
    {PolicyEventType::PolicyEscalatedToAgent,
     "mira.policy.policy-escalated-to-agent.v1",
     {"matched_rule_ids", "reason", "state"},
     false},
};

// ---------------------------------------------------------------------------
// Failure helpers with detail output (case functions return int like the
// M24/M25 matrices; MIRA_CHECK guards the call sites).
// ---------------------------------------------------------------------------

template <typename T> int expect_error(const Result<T> &result, TemporalPolicyDomainCode code) {
    if (result.has_value()) {
        std::cerr << "m26: expected error " << temporal_policy_domain_code_name(code)
                  << " but the call succeeded\n";
        return 1;
    }
    const Error &error = result.error();
    if (error.domain != temporal_policy_domain()) {
        std::cerr << "m26: error domain '" << error.domain << "' != '" << temporal_policy_domain()
                  << "'\n";
        return 1;
    }
    if (error.domain_code != static_cast<std::int32_t>(code)) {
        std::cerr << "m26: error domain_code " << error.domain_code
                  << " != " << static_cast<std::int32_t>(code) << " ("
                  << temporal_policy_domain_code_name(code) << ")\n";
        return 1;
    }
    if (temporal_policy_domain_code_name(static_cast<TemporalPolicyDomainCode>(
            error.domain_code)) != temporal_policy_domain_code_name(code)) {
        std::cerr << "m26: stable name mapping mismatch for code " << error.domain_code << "\n";
        return 1;
    }
    return 0;
}

// Asserts the parsed payload carries exactly the frozen key set.
inline int expect_payload_keys(const PolicyEvent &event, const EventSchemaEntry &entry) {
    const auto parsed = parse_json(event.payload_json);
    if (!parsed.has_value() || !parsed.value().is_object()) {
        std::cerr << "m26: payload does not parse as a JSON object: " << event.payload_json << '\n';
        return 1;
    }
    std::vector<std::string> keys;
    for (const auto &member : *parsed.value().as_object()) {
        keys.push_back(member.first);
    }
    std::sort(keys.begin(), keys.end());
    auto expected = entry.payload_keys;
    std::sort(expected.begin(), expected.end());
    if (keys.size() != expected.size()) {
        std::cerr << "m26: payload key count " << keys.size() << " != " << expected.size()
                  << " for " << entry.schema << ": " << event.payload_json << '\n';
        return 1;
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (keys[index] != expected[index]) {
            std::cerr << "m26: payload key '" << keys[index] << "' != '" << expected[index]
                      << "' for " << entry.schema << '\n';
            return 1;
        }
    }
    // Payloads are canonical JSON (canonical serialization is byte-stable).
    if (canonical_json_string(parsed.value()) != event.payload_json) {
        std::cerr << "m26: payload is not canonical JSON: " << event.payload_json << '\n';
        return 1;
    }
    return 0;
}

inline const EventSchemaEntry &schema_entry(PolicyEventType type) {
    for (const auto &entry : kEventSchemas) {
        if (entry.type == type) {
            return entry;
        }
    }
    std::cerr << "m26: no frozen schema entry for event type " << static_cast<int>(type) << '\n';
    std::abort();
}

// ---------------------------------------------------------------------------
// Recording sink (contract: the sink is vector-append; it also records the
// thread id of every callback so the eval can prove the runtime performs
// all work inline on the caller thread - zero self-built threads).
// ---------------------------------------------------------------------------

class RecordingSink final : public IPolicyEventSink {
  public:
    void on_policy_event(const PolicyEvent &event) override {
        events.push_back(event);
        threads.push_back(std::this_thread::get_id());
    }

    [[nodiscard]] std::size_t count(PolicyEventType type) const {
        std::size_t total = 0;
        for (const auto &event : events) {
            if (event.type == type) {
                ++total;
            }
        }
        return total;
    }

    [[nodiscard]] std::vector<PolicyEvent> of_type(PolicyEventType type) const {
        std::vector<PolicyEvent> found;
        for (const auto &event : events) {
            if (event.type == type) {
                found.push_back(event);
            }
        }
        return found;
    }

    [[nodiscard]] const PolicyEvent *single(PolicyEventType type) const {
        const PolicyEvent *found = nullptr;
        for (const auto &event : events) {
            if (event.type == type) {
                if (found != nullptr) {
                    return nullptr; // not single
                }
                found = &event;
            }
        }
        return found;
    }

    std::vector<PolicyEvent> events;
    std::vector<std::thread::id> threads;
};

// ---------------------------------------------------------------------------
// Deterministic builders (dataset clock only; §4.5 - the runtime never reads
// the system clock and neither do the tests).
// ---------------------------------------------------------------------------

[[nodiscard]] inline Timestamp dataset_time(std::uint64_t tick) {
    Timestamp time;
    time.wall = std::chrono::system_clock::time_point{
        std::chrono::milliseconds(static_cast<std::int64_t>(1'700'000'000'000 + tick * 10))};
    time.monotonic = std::chrono::steady_clock::time_point{
        std::chrono::microseconds(static_cast<std::int64_t>(tick * 10'000))};
    return time;
}

[[nodiscard]] inline PolicyAction make_action(std::string kind, std::string parameter) {
    PolicyAction action;
    action.kind = std::move(kind);
    action.parameter = std::move(parameter);
    return action;
}

[[nodiscard]] inline TrackedEntity make_entity(std::string key, std::string motion, double phase,
                                               double confidence, std::string source_ref) {
    TrackedEntity entity;
    entity.entity_key = std::move(key);
    entity.position_x = 5.0;
    entity.position_y = 5.0;
    entity.velocity_x = -0.5;
    entity.velocity_y = 0.25;
    entity.motion = std::move(motion);
    entity.motion_phase = phase;
    entity.motion_confidence = confidence;
    entity.source_ref = std::move(source_ref);
    return entity;
}

[[nodiscard]] inline PolicyFact make_fact(std::string key, double value) {
    PolicyFact fact;
    fact.key = std::move(key);
    fact.value = value;
    return fact;
}

[[nodiscard]] inline PolicyWorldView make_world(std::vector<TrackedEntity> entities,
                                                std::vector<PolicyFact> facts) {
    PolicyWorldView world;
    world.entities = std::move(entities);
    world.facts = std::move(facts);
    return world;
}

// The §6.1 design example world: heavy_slash_a past threshold, close.
[[nodiscard]] inline PolicyWorldView slash_world(double phase, double distance) {
    return make_world({make_entity("enemy_1", "heavy_slash_a", phase, 0.92,
                                   "dataset://m26/observation/" +
                                       std::to_string(static_cast<int>(phase * 1000)))},
                      {make_fact("distance", distance)});
}

[[nodiscard]] inline PolicyTickContext make_tick(std::uint64_t tick) {
    PolicyTickContext context;
    context.tick = tick;
    context.tick_time = dataset_time(tick);
    return context;
}

[[nodiscard]] inline PolicyEpisodeSample make_sample(std::uint64_t tick, PolicyWorldView world,
                                                     std::string state, PolicyAction agent_action,
                                                     bool handled) {
    PolicyEpisodeSample sample;
    sample.tick = make_tick(tick);
    sample.world = std::move(world);
    sample.active_state = std::move(state);
    if (handled) {
        sample.agent_action = std::move(agent_action);
    }
    sample.agent_handled = handled;
    return sample;
}

// One handled §6.1-pattern training sample.
[[nodiscard]] inline PolicyEpisodeSample dodge_sample(std::uint64_t tick, std::string source_ref) {
    auto world = slash_world(0.67, 2.7);
    world.entities[0].source_ref = std::move(source_ref);
    return make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), true);
}

// ---------------------------------------------------------------------------
// Condition and rule builders (hand-built rules; the schema is general over
// all five compares, the reference induction emits only the canonical form).
// ---------------------------------------------------------------------------

[[nodiscard]] inline RuleCondition motion_eq(std::string text) {
    RuleCondition condition;
    condition.field = RuleCondition::Field::Motion;
    condition.compare = RuleCondition::Compare::Eq;
    condition.text_value = std::move(text);
    return condition;
}

[[nodiscard]] inline RuleCondition numeric_condition(RuleCondition::Field field,
                                                     RuleCondition::Compare compare, double value) {
    RuleCondition condition;
    condition.field = field;
    condition.compare = compare;
    condition.number_value = value;
    return condition;
}

[[nodiscard]] inline RuleCondition fact_condition(RuleCondition::Compare compare, std::string key,
                                                  double value) {
    RuleCondition condition;
    condition.field = RuleCondition::Field::Fact;
    condition.compare = compare;
    condition.fact_key = std::move(key);
    condition.number_value = value;
    return condition;
}

[[nodiscard]] inline RuleTransition make_transition(std::vector<RuleCondition> when,
                                                    std::vector<PolicyAction> do_actions) {
    RuleTransition transition;
    transition.when = std::move(when);
    transition.do_actions = std::move(do_actions);
    return transition;
}

// Hand-built rule with provenance and the frozen content-derived id
// ("rr-" + hex(digest(state+transitions))[0..16], §4.3 - priority is not
// part of the semantic content, so ids are stable across priority changes).
[[nodiscard]] inline ReactiveRule
make_rule(std::string state, std::uint32_t priority, std::vector<RuleTransition> transitions,
          std::vector<std::string> source_refs = {"dataset://m26/hand-built"}) {
    ReactiveRule rule;
    rule.state = std::move(state);
    rule.priority = priority;
    rule.transitions = std::move(transitions);
    rule.source_refs = std::move(source_refs);
    rule.rule_id = "rr-" + rule.digest().to_string().substr(0, 16);
    return rule;
}

// The §6.1 slash rule: motion==heavy_slash_a AND phase>=0.55 AND
// distance<3 -> dodge left, expressed in the frozen closed condition set.
[[nodiscard]] inline ReactiveRule slash_rule(std::uint32_t priority) {
    return make_rule("combat", priority,
                     {make_transition({motion_eq("heavy_slash_a"),
                                       numeric_condition(RuleCondition::Field::MotionPhase,
                                                         RuleCondition::Compare::Ge, 0.55),
                                       fact_condition(RuleCondition::Compare::Lt, "distance", 3.0)},
                                      {make_action("dodge", "left")})});
}

[[nodiscard]] inline bool valid_rule_id(std::string_view id) {
    if (id.size() != 19 || id.substr(0, 3) != "rr-") {
        return false;
    }
    for (const char character : id.substr(3)) {
        const bool hex =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

// Stable cross-run aggregate over a rule set (sorted wire forms).
[[nodiscard]] inline Sha256Digest rule_set_digest(const std::vector<ReactiveRule> &rules) {
    std::vector<std::string> wires;
    wires.reserve(rules.size());
    for (const auto &rule : rules) {
        wires.push_back(rule.to_json());
    }
    std::sort(wires.begin(), wires.end());
    std::string joined;
    for (const auto &wire : wires) {
        joined += wire;
        joined += '\n';
    }
    return digest_string(joined);
}

} // namespace mira::m26
