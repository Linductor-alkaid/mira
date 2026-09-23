#pragma once

// Case declarations for the M26 temporal-policy gate matrix. The case table
// in m26_temporal_policy_test.cpp drives every case; the T1-G3 lifecycle
// cases are defined in m26_lifecycle.cpp (split by the max-file-lines
// budget along the M24 precedent, assertions unchanged).

#include "m26_support.hpp"

namespace mira::m26 {

// ---------------------------------------------------------------------------
// Shared lifecycle helpers (both gate TUs). All return 0 on success so call
// sites can guard with MIRA_CHECK; failures print their own detail line.
// ---------------------------------------------------------------------------

// The §6.1-pattern evidence set: two handled slash samples inside the
// trigger closure with the agent action the rule is expected to reproduce.
[[nodiscard]] inline std::vector<PolicyEpisodeSample> slash_evidence(std::uint64_t base_tick) {
    std::vector<PolicyEpisodeSample> samples;
    samples.push_back(make_sample(base_tick, slash_world(0.67, 2.7), "combat",
                                  make_action("dodge", "left"), true));
    samples.push_back(make_sample(base_tick + 1, slash_world(0.8, 2.0), "combat",
                                  make_action("dodge", "left"), true));
    return samples;
}

// Distance-only evidence for fact-only rules (idle entity, close distance).
[[nodiscard]] inline std::vector<PolicyEpisodeSample> close_evidence(std::uint64_t base_tick) {
    std::vector<PolicyEpisodeSample> samples;
    for (std::uint64_t offset = 0; offset < 2; ++offset) {
        auto world = make_world({make_entity("enemy_1", "idle", 0.1, 0.5,
                                             "dataset://m26/idle/" + std::to_string(offset))},
                                {make_fact("distance", 2.0)});
        samples.push_back(make_sample(base_tick + offset, std::move(world), "combat",
                                      make_action("dodge", "left"), true));
    }
    return samples;
}

// Adopt one rule, assert its isolated test row, then promote it through the
// frozen evidence gate. Emits the full RuleCandidateInduced /
// RuleTestingResulted / RulePromoted event trail.
inline int promote_via_evidence(ReactivePolicyRuntime &runtime, const ReactiveRule &rule,
                                const std::vector<PolicyEpisodeSample> &evidence,
                                std::uint32_t want_passed, std::uint32_t want_false) {
    const auto adopted = runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&rule, 1});
    if (!adopted.has_value() || adopted.value() != 1) {
        std::cerr << "m26: adopt failed for " << rule.rule_id << '\n';
        return 1;
    }
    auto report = runtime.test_candidate_rules(
        std::span<const PolicyEpisodeSample>(evidence.data(), evidence.size()));
    if (!report.has_value()) {
        std::cerr << "m26: test failed for " << rule.rule_id << '\n';
        return 1;
    }
    const RuleTestingReport::RuleResult *row = nullptr;
    for (const auto &entry : report.value().results) {
        if (entry.rule_id == rule.rule_id) {
            row = &entry;
        }
    }
    if (row == nullptr) {
        std::cerr << "m26: no test row for " << rule.rule_id << '\n';
        return 1;
    }
    if (row->passed_count != want_passed || row->false_trigger_count != want_false) {
        std::cerr << "m26: test row for " << rule.rule_id << " was passed=" << row->passed_count
                  << " false=" << row->false_trigger_count << ", wanted " << want_passed << "/"
                  << want_false << '\n';
        return 1;
    }
    if (!runtime.promote_rule(rule.rule_id).has_value()) {
        std::cerr << "m26: promote failed for " << rule.rule_id << '\n';
        return 1;
    }
    return 0;
}

// Reads a string member out of a canonical event payload.
[[nodiscard]] inline std::string payload_string(const PolicyEvent &event, std::string_view key) {
    const auto parsed = parse_json(event.payload_json);
    if (!parsed.has_value()) {
        return {};
    }
    const auto *value = parsed.value().find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return *value->as_string();
}

// Case declarations: T1-G1 contract and serialization.
int history_ring_sequence_capacity_digest();
int options_and_input_validate_rejections();
int rule_schema_fail_closed();
int rule_wire_json_round_trip_and_version_policy();
int error_domain_twelve_codes_triple_mapping();
int event_schema_names_exact_nine();
int event_payload_key_sets_frozen();
int source_digest_empty_on_reconstruction_path();

// T1-G2: step evaluation semantics.
int inactive_state_zero_evaluation_zero_events();
int tick_monotonic_rejection_zero_state_change();
int match_order_fixed_priority_then_rule_id();
int single_match_triggers_actions_and_event();
int conflict_fail_closed_zero_actions_full_demotion();
int activate_deactivate_idempotent_and_immediate();
int adopt_bounds_fail_closed_whole_batch();
int fact_only_rule_matches_entity_key_empty();
int entity_scoped_rule_empty_entities_zero_match();
int frame_rule_id_and_tick_per_event_type();

// T1-G3: induction / testing / promotion lifecycle (m26_lifecycle.cpp).
int induction_anchor_is_first_motion_entity();
int induction_below_min_support_zero_candidates();
int induction_excludes_unhandled_samples();
int induced_rule_canonical_form_fields();
int induction_idempotent_byte_identical();
int adopt_idempotent_and_event_at_adopt_point();
int testing_isolated_per_rule_counts();
int promote_evidence_gate_fail_closed();
int demote_transition_matrix();
int retire_transition_matrix_and_terminal_idempotence();

} // namespace mira::m26
