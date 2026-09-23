// M26 (DEC-037 Stage T1) temporal-policy IVA gate matrix - induction /
// testing / promotion lifecycle half (T1-G3), defined in its own
// translation unit for the max-file-lines budget (M24 precedent) and driven
// by the case table in m26_temporal_policy_test.cpp.
//
//   - anchor selection is deterministic: the first entity in world order
//     with a non-empty motion, independent of entity position;
//   - below min_support nothing is induced; empty batch input is
//     EpisodeSamplesInvalid; agent_handled=false never counts;
//   - the induced candidate is the frozen canonical form: single
//     transition, fixed condition order [Motion Eq, MotionPhase Ge min,
//     MotionConfidence Ge min, per-fact-key Le max in key order],
//     sorted-deduplicated provenance, support count, content-derived id;
//   - induction is pure and idempotent (byte-identical candidates, zero
//     events); RuleCandidateInduced fires exactly once at the adopt point;
//   - testing is per-rule isolated with the frozen counters and stable
//     report digest; promotion is evidence-gated fail-closed;
//   - the demote/retire/promote transition matrix is pinned cell by cell,
//     including retire terminal idempotence and the closed reason sets.

#include "m26_cases.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mira::m26 {
namespace {

struct ScopedRuntime final {
    RecordingSink sink;
    PolicyRuntimeOptions options;
    ReactivePolicyRuntime runtime;

    ScopedRuntime() : runtime(options, &sink) {}
    explicit ScopedRuntime(PolicyRuntimeOptions opts)
        : options(std::move(opts)), runtime(options, &sink) {}
};

[[nodiscard]] std::span<const PolicyEpisodeSample>
as_span(const std::vector<PolicyEpisodeSample> &samples) {
    return std::span<const PolicyEpisodeSample>(samples.data(), samples.size());
}

// The training log: `count` byte-identical handled slash samples.
[[nodiscard]] std::vector<PolicyEpisodeSample> slash_log(std::uint64_t base_tick, std::size_t count,
                                                         bool handled = true) {
    std::vector<PolicyEpisodeSample> episodes;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t tick = base_tick + static_cast<std::uint64_t>(index);
        auto world = make_world({make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92,
                                             "dataset://m26/enemy/" + std::to_string(tick))},
                                {make_fact("distance", 2.7)});
        episodes.push_back(
            make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), handled));
    }
    return episodes;
}

} // namespace

// ---------------------------------------------------------------------------
// Anchor selection: first entity in order with a non-empty motion.
// ---------------------------------------------------------------------------

int induction_anchor_is_first_motion_entity() {
    ScopedRuntime scoped;
    std::vector<PolicyEpisodeSample> episodes;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        // ally_1 carries no motion; the anchor must be enemy_1 even though
        // it sits second in the world order.
        auto world = make_world(
            {make_entity("ally_1", "", 0.0, 0.0, "dataset://m26/ally/" + std::to_string(tick)),
             make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92,
                         "dataset://m26/enemy/" + std::to_string(tick))},
            {make_fact("distance", 2.7)});
        episodes.push_back(
            make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), true));
    }
    auto candidates = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);
    MIRA_CHECK(candidates.value()[0].transitions.size() == 1);
    MIRA_CHECK(candidates.value()[0].transitions[0].when.size() == 4);
    MIRA_CHECK(candidates.value()[0].transitions[0].when[0].field == RuleCondition::Field::Motion);
    MIRA_CHECK(candidates.value()[0].transitions[0].when[0].text_value == "heavy_slash_a");
    MIRA_CHECK(candidates.value()[0].support_count == 3);
    MIRA_CHECK(scoped.sink.events.empty()); // induction is pure computation

    // Entity order must not change the anchor: the flipped world induces
    // the byte-identical rule.
    ScopedRuntime flipped_runtime;
    std::vector<PolicyEpisodeSample> flipped;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        auto world = make_world(
            {make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92,
                         "dataset://m26/enemy/" + std::to_string(tick)),
             make_entity("ally_1", "", 0.0, 0.0, "dataset://m26/ally/" + std::to_string(tick))},
            {make_fact("distance", 2.7)});
        flipped.push_back(
            make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), true));
    }
    auto again = flipped_runtime.runtime.induce_candidate_rules(as_span(flipped));
    MIRA_CHECK(again.has_value() && again.value().size() == 1);
    MIRA_CHECK(again.value()[0].to_json() == candidates.value()[0].to_json());
    return 0;
}

// ---------------------------------------------------------------------------
// min_support floor and empty batch input.
// ---------------------------------------------------------------------------

int induction_below_min_support_zero_candidates() {
    ScopedRuntime scoped;
    auto episodes = slash_log(1, 2); // two identical samples < min_support 3
    auto candidates = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(candidates.has_value());
    MIRA_CHECK(candidates.value().empty());
    MIRA_CHECK(scoped.sink.events.empty());

    // Empty batch input is rejected.
    MIRA_CHECK(expect_error(scoped.runtime.induce_candidate_rules({}),
                            TemporalPolicyDomainCode::EpisodeSamplesInvalid) == 0);

    // Exactly at min_support the candidate appears.
    auto at_floor = slash_log(10, 3);
    auto reached = scoped.runtime.induce_candidate_rules(as_span(at_floor));
    MIRA_CHECK(reached.has_value() && reached.value().size() == 1);
    MIRA_CHECK(reached.value()[0].support_count == 3);
    return 0;
}

// ---------------------------------------------------------------------------
// agent_handled=false samples never participate.
// ---------------------------------------------------------------------------

int induction_excludes_unhandled_samples() {
    ScopedRuntime scoped;
    // Two handled + five unhandled of the same signature: the support stays
    // below min_support, so nothing is induced even though the pattern
    // repeated seven times.
    std::vector<PolicyEpisodeSample> episodes = slash_log(1, 2, true);
    const auto unhandled = slash_log(10, 5, false);
    episodes.insert(episodes.end(), unhandled.begin(), unhandled.end());
    auto none = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(none.has_value() && none.value().empty());

    // Three handled slash samples plus five unhandled jab samples: the
    // rule is induced from the handled signature only.
    ScopedRuntime other;
    std::vector<PolicyEpisodeSample> mixed = slash_log(1, 3, true);
    for (std::size_t index = 0; index < 5; ++index) {
        const std::uint64_t tick = 50 + static_cast<std::uint64_t>(index);
        auto world = make_world(
            {make_entity("enemy_1", "jab", 0.4, 0.7, "dataset://m26/jab/" + std::to_string(tick))},
            {make_fact("distance", 1.5)});
        mixed.push_back(
            make_sample(tick, std::move(world), "combat", make_action("parry", ""), false));
    }
    auto candidates = other.runtime.induce_candidate_rules(as_span(mixed));
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);
    MIRA_CHECK(candidates.value()[0].transitions[0].when[0].text_value == "heavy_slash_a");
    MIRA_CHECK(candidates.value()[0].support_count == 3);
    return 0;
}

// ---------------------------------------------------------------------------
// The induced candidate is the frozen canonical form.
// ---------------------------------------------------------------------------

int induced_rule_canonical_form_fields() {
    ScopedRuntime scoped;
    std::vector<PolicyEpisodeSample> episodes;
    std::vector<std::string> refs;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        refs.push_back("dataset://m26/enemy/" + std::to_string(tick));
        auto world = make_world({make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92, refs.back())},
                                {make_fact("alpha", 1.0), make_fact("beta", 2.0)});
        episodes.push_back(
            make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), true));
    }
    auto candidates = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);
    const auto &rule = candidates.value()[0];

    MIRA_CHECK(rule.status == ReactiveRuleStatus::Candidate);
    MIRA_CHECK(rule.state == "combat");
    MIRA_CHECK((rule.schema_version == SchemaVersion{1, 0}));
    MIRA_CHECK(rule.transitions.size() == 1);
    MIRA_CHECK(rule.validate().has_value());

    // Fixed condition order: Motion Eq, MotionPhase Ge (support minimum),
    // MotionConfidence Ge (support minimum), then per-fact-key Le (support
    // maximum) in key lexicographic order - the self-covering canonical
    // form that always matches its own support set.
    const auto &when = rule.transitions[0].when;
    MIRA_CHECK(when.size() == 5);
    MIRA_CHECK(when[0].field == RuleCondition::Field::Motion);
    MIRA_CHECK(when[0].compare == RuleCondition::Compare::Eq);
    MIRA_CHECK(when[0].text_value == "heavy_slash_a");
    MIRA_CHECK(when[1].field == RuleCondition::Field::MotionPhase);
    MIRA_CHECK(when[1].compare == RuleCondition::Compare::Ge);
    MIRA_CHECK(when[1].number_value == 0.67);
    MIRA_CHECK(when[2].field == RuleCondition::Field::MotionConfidence);
    MIRA_CHECK(when[2].compare == RuleCondition::Compare::Ge);
    MIRA_CHECK(when[2].number_value == 0.92);
    MIRA_CHECK(when[3].field == RuleCondition::Field::Fact);
    MIRA_CHECK(when[3].compare == RuleCondition::Compare::Le);
    MIRA_CHECK(when[3].fact_key == "alpha");
    MIRA_CHECK(when[3].number_value == 1.0);
    MIRA_CHECK(when[4].field == RuleCondition::Field::Fact);
    MIRA_CHECK(when[4].compare == RuleCondition::Compare::Le);
    MIRA_CHECK(when[4].fact_key == "beta");
    MIRA_CHECK(when[4].number_value == 2.0);

    MIRA_CHECK(rule.transitions[0].do_actions.size() == 1);
    MIRA_CHECK(rule.transitions[0].do_actions[0].kind == "dodge");
    MIRA_CHECK(rule.transitions[0].do_actions[0].parameter == "left");
    MIRA_CHECK(rule.source_refs == refs); // sorted and deduplicated
    MIRA_CHECK(rule.support_count == 3);
    MIRA_CHECK(valid_rule_id(rule.rule_id));
    MIRA_CHECK(rule.rule_id == "rr-" + rule.digest().to_string().substr(0, 16));

    // The canonical form covers its own support set: evidence samples carry
    // the support world shape (alpha/beta facts the rule conditions on) with
    // the boundary phase value triggering (Ge self-cover; the frozen
    // alternative "strictly Gt min" would zero-trigger the support set and
    // is rejected by design).
    ScopedRuntime adopter;
    MIRA_CHECK(adopter.runtime.activate("combat").has_value());
    std::vector<PolicyEpisodeSample> self_cover;
    for (std::uint64_t tick = 401; tick <= 402; ++tick) {
        auto world = make_world({make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92,
                                             "dataset://m26/enemy/" + std::to_string(tick))},
                                {make_fact("alpha", 1.0), make_fact("beta", 2.0)});
        self_cover.push_back(
            make_sample(tick, std::move(world), "combat", make_action("dodge", "left"), true));
    }
    MIRA_CHECK(promote_via_evidence(adopter.runtime, rule, self_cover, 2, 0) == 0);
    auto boundary_world = make_world(
        {make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92, "dataset://m26/enemy/boundary")},
        {make_fact("alpha", 1.0), make_fact("beta", 2.0)});
    const auto boundary = adopter.runtime.step(std::move(boundary_world), make_tick(1));
    MIRA_CHECK(boundary.has_value() && boundary.value().size() == 1);
    MIRA_CHECK(boundary.value()[0].kind == "dodge");
    return 0;
}

// ---------------------------------------------------------------------------
// Induction is idempotent and emits nothing.
// ---------------------------------------------------------------------------

int induction_idempotent_byte_identical() {
    ScopedRuntime scoped;
    auto episodes = slash_log(1, 3);
    auto first = scoped.runtime.induce_candidate_rules(as_span(episodes));
    auto second = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(first.has_value() && second.has_value());
    MIRA_CHECK(first.value().size() == second.value().size());
    for (std::size_t index = 0; index < first.value().size(); ++index) {
        MIRA_CHECK(first.value()[index].to_json() == second.value()[index].to_json());
        MIRA_CHECK(first.value()[index].rule_id == second.value()[index].rule_id);
    }
    MIRA_CHECK(scoped.sink.events.empty());

    // A different runtime instance derives the same identity (the basis of
    // idempotent adoption across runs).
    ScopedRuntime twin;
    auto repeated = twin.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(repeated.has_value() && repeated.value().size() == first.value().size());
    for (std::size_t index = 0; index < repeated.value().size(); ++index) {
        MIRA_CHECK(repeated.value()[index].rule_id == first.value()[index].rule_id);
        MIRA_CHECK(repeated.value()[index].to_json() == first.value()[index].to_json());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Adoption idempotence; the induced event fires at the adopt point only.
// ---------------------------------------------------------------------------

int adopt_idempotent_and_event_at_adopt_point() {
    ScopedRuntime scoped;
    auto episodes = slash_log(1, 3);
    auto candidates = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(candidates.has_value() && candidates.value().size() == 1);
    MIRA_CHECK(scoped.sink.events.empty()); // induce is pure: zero events

    const auto adopted = scoped.runtime.adopt_candidate_rules(candidates.value());
    MIRA_CHECK(adopted.has_value() && adopted.value() == 1);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleCandidateInduced) == 1);
    const auto *induced = scoped.sink.single(PolicyEventType::RuleCandidateInduced);
    MIRA_CHECK(induced != nullptr && induced->rule_id == candidates.value()[0].rule_id);
    MIRA_CHECK(payload_string(*induced, "state") == "combat");

    // Re-adopting the same candidates skips every rule_id: zero new rules,
    // zero new events.
    auto again = scoped.runtime.induce_candidate_rules(as_span(episodes));
    MIRA_CHECK(again.has_value() && again.value().size() == 1);
    const auto repeated = scoped.runtime.adopt_candidate_rules(again.value());
    MIRA_CHECK(repeated.has_value() && repeated.value() == 0);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleCandidateInduced) == 1);

    // And re-inducing the same log stays byte-identical (T1-G4 leg).
    MIRA_CHECK(again.value()[0].to_json() == candidates.value()[0].to_json());
    return 0;
}

// ---------------------------------------------------------------------------
// Testing is per-rule isolated with the frozen counters.
// ---------------------------------------------------------------------------

int testing_isolated_per_rule_counts() {
    ScopedRuntime scoped;
    auto mover = make_rule("combat", 1,
                           {make_transition({motion_eq("heavy_slash_a"),
                                             numeric_condition(RuleCondition::Field::MotionPhase,
                                                               RuleCondition::Compare::Ge, 0.5)},
                                            {make_action("dodge", "left")})});
    auto factful =
        make_rule("combat", 2,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 9.0)},
                                   {make_action("block", "")})});
    std::vector<ReactiveRule> batch{mover, factful};
    MIRA_CHECK(scoped.runtime.adopt_candidate_rules(batch).has_value());

    // Three trigger-matching samples (agent dodge/left) and two non-matching
    // idle samples (agent hold): mover passes three and never fires on the
    // idle pair; factful fires on all five and matches nothing.
    std::vector<PolicyEpisodeSample> episodes;
    episodes.push_back(
        make_sample(501, slash_world(0.67, 2.7), "combat", make_action("dodge", "left"), true));
    episodes.push_back(
        make_sample(502, slash_world(0.8, 2.5), "combat", make_action("dodge", "left"), true));
    episodes.push_back(
        make_sample(503, slash_world(0.9, 2.0), "combat", make_action("dodge", "left"), true));
    for (std::uint64_t tick = 504; tick <= 505; ++tick) {
        auto world = make_world({make_entity("enemy_1", "idle", 0.1, 0.5,
                                             "dataset://m26/idle/" + std::to_string(tick))},
                                {make_fact("distance", 6.0)});
        episodes.push_back(
            make_sample(tick, std::move(world), "combat", make_action("hold", ""), true));
    }

    const auto report = scoped.runtime.test_candidate_rules(as_span(episodes));
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().results.size() == 2);
    MIRA_CHECK(std::is_sorted(
        report.value().results.begin(), report.value().results.end(),
        [](const auto &left, const auto &right) { return left.rule_id < right.rule_id; }));
    for (const auto &row : report.value().results) {
        if (row.rule_id == mover.rule_id) {
            MIRA_CHECK(row.passed_count == 3);
            MIRA_CHECK(row.false_trigger_count == 0);
            MIRA_CHECK(row.sample_count == 3); // denominator: triggering samples only
        } else if (row.rule_id == factful.rule_id) {
            MIRA_CHECK(row.passed_count == 0);
            MIRA_CHECK(row.false_trigger_count == 5);
            MIRA_CHECK(row.sample_count == 5);
        } else {
            std::cerr << "m26: unexpected testing row: " << row.rule_id << '\n';
            return 1;
        }
    }
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleTestingResulted) == 2);
    for (const auto &event : scoped.sink.of_type(PolicyEventType::RuleTestingResulted)) {
        MIRA_CHECK(event.rule_id == mover.rule_id || event.rule_id == factful.rule_id);
    }

    // The report digest is stable across an identical rerun.
    ScopedRuntime twin;
    std::vector<ReactiveRule> twin_batch{mover, factful};
    MIRA_CHECK(twin.runtime.adopt_candidate_rules(twin_batch).has_value());
    const auto twin_report = twin.runtime.test_candidate_rules(as_span(episodes));
    MIRA_CHECK(twin_report.has_value());
    MIRA_CHECK(twin_report.value().digest() == report.value().digest());
    return 0;
}

// ---------------------------------------------------------------------------
// Promotion is fail-closed on the evidence gate.
// ---------------------------------------------------------------------------

int promote_evidence_gate_fail_closed() {
    ScopedRuntime scoped;
    const auto rule = slash_rule(1);
    MIRA_CHECK(
        scoped.runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&rule, 1}).has_value());

    // (a) No testing evidence at all.
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(rule.rule_id),
                            TemporalPolicyDomainCode::PromotionEvidenceMissing) == 0);

    // (b) One clean pass is below min_test_support (2).
    const auto one_pass = slash_log(601, 1);
    auto thin = scoped.runtime.test_candidate_rules(as_span(one_pass));
    MIRA_CHECK(thin.has_value());
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(rule.rule_id),
                            TemporalPolicyDomainCode::PromotionEvidenceMissing) == 0);

    // (c) False triggers block promotion even with passes present.
    std::vector<PolicyEpisodeSample> mismatched;
    for (std::uint64_t tick = 611; tick <= 612; ++tick) {
        mismatched.push_back(
            make_sample(tick, slash_world(0.7, 2.5), "combat", make_action("jab", ""), true));
    }
    auto dirty = scoped.runtime.test_candidate_rules(as_span(mismatched));
    MIRA_CHECK(dirty.has_value());
    for (const auto &row : dirty.value().results) {
        if (row.rule_id == rule.rule_id) {
            MIRA_CHECK(row.false_trigger_count == 2);
        }
    }
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(rule.rule_id),
                            TemporalPolicyDomainCode::PromotionEvidenceMissing) == 0);

    // (d) The most recent clean evidence opens the gate.
    const auto clean = slash_log(621, 2);
    auto good = scoped.runtime.test_candidate_rules(as_span(clean));
    MIRA_CHECK(good.has_value());
    MIRA_CHECK(scoped.runtime.promote_rule(rule.rule_id).has_value());
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RulePromoted) == 1);
    const auto *promoted = scoped.sink.single(PolicyEventType::RulePromoted);
    MIRA_CHECK(promoted != nullptr);
    MIRA_CHECK(promoted->rule_id == rule.rule_id);
    MIRA_CHECK(payload_string(*promoted, "state") == "combat");
    const auto payload = parse_json(promoted->payload_json);
    MIRA_CHECK(payload.has_value());
    const auto *passed = payload.value().find("passed_count");
    MIRA_CHECK(passed != nullptr && passed->is_integer() && passed->as_integer() == 2);

    // (e) Already-Runtime and unknown ids are illegal promotions.
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(rule.rule_id),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule("rr-0000000000000000"),
                            TemporalPolicyDomainCode::RuleUnknown) == 0);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RulePromoted) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// The demote column of the frozen migration matrix.
// ---------------------------------------------------------------------------

int demote_transition_matrix() {
    ScopedRuntime scoped;
    const auto rule = slash_rule(1);
    MIRA_CHECK(
        scoped.runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&rule, 1}).has_value());
    MIRA_CHECK(scoped.runtime.test_candidate_rules(as_span(slash_log(701, 2))).has_value());

    // Candidate -> demote is illegal (no runtime face; removal is retire).
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule(rule.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleDemoted) == 0);

    MIRA_CHECK(scoped.runtime.promote_rule(rule.rule_id).has_value());

    // Unknown reasons are schema violations; zero events, zero state change.
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule(rule.rule_id, "bogus"),
                            TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleDemoted) == 0);

    // Runtime -> Candidate with reason=conflict.
    MIRA_CHECK(scoped.runtime.demote_rule(rule.rule_id, "conflict").has_value());
    auto demotions = scoped.sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions.size() == 1);
    MIRA_CHECK(demotions[0].rule_id == rule.rule_id);
    MIRA_CHECK(payload_string(demotions[0], "from_status") == "Runtime");
    MIRA_CHECK(payload_string(demotions[0], "to_status") == "Candidate");
    MIRA_CHECK(payload_string(demotions[0], "reason") == "conflict");

    // A demoted (Candidate) rule has no runtime face left.
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule(rule.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);

    // The persisted evidence allows re-promotion; host-explicit demote then
    // returns it to Candidate again with its own reason on the wire.
    MIRA_CHECK(scoped.runtime.promote_rule(rule.rule_id).has_value());
    MIRA_CHECK(scoped.runtime.demote_rule(rule.rule_id, "host-explicit").has_value());
    demotions = scoped.sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions.size() == 2);
    MIRA_CHECK(payload_string(demotions[1], "reason") == "host-explicit");
    MIRA_CHECK(payload_string(demotions[1], "from_status") == "Runtime");
    MIRA_CHECK(payload_string(demotions[1], "to_status") == "Candidate");

    // Retired rules reject demotion; unknown ids are RuleUnknown. The retire
    // above itself emitted the third RuleDemoted (to_status Retired); the
    // rejected transitions must add nothing further.
    MIRA_CHECK(scoped.runtime.retire_rule(rule.rule_id).has_value());
    const std::size_t demotions_total = scoped.sink.count(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions_total == 3); // conflict + host-explicit + retire
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule(rule.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule("rr-ffffffffffffffff", "conflict"),
                            TemporalPolicyDomainCode::RuleUnknown) == 0);
    MIRA_CHECK(scoped.sink.count(PolicyEventType::RuleDemoted) == demotions_total);
    return 0;
}

// ---------------------------------------------------------------------------
// The retire column, terminal idempotence, and the closed reason set.
// ---------------------------------------------------------------------------

int retire_transition_matrix_and_terminal_idempotence() {
    ScopedRuntime scoped;
    auto runtime_rule = slash_rule(1); // retires from Runtime
    auto candidate_rule =
        make_rule("combat", 2, {make_transition({motion_eq("jab")}, {make_action("guard", "")})});
    std::vector<ReactiveRule> batch{runtime_rule, candidate_rule};
    MIRA_CHECK(scoped.runtime.adopt_candidate_rules(batch).has_value());

    // Candidate -> Retired with the frozen fixed payload.
    MIRA_CHECK(scoped.runtime.retire_rule(candidate_rule.rule_id).has_value());
    auto retirements = scoped.sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(retirements.size() == 1);
    MIRA_CHECK(retirements[0].rule_id == candidate_rule.rule_id);
    MIRA_CHECK(payload_string(retirements[0], "from_status") == "Candidate");
    MIRA_CHECK(payload_string(retirements[0], "to_status") == "Retired");
    MIRA_CHECK(payload_string(retirements[0], "reason") == "host-explicit");

    // Runtime -> Retired.
    MIRA_CHECK(scoped.runtime.test_candidate_rules(as_span(slash_log(801, 2))).has_value());
    MIRA_CHECK(scoped.runtime.promote_rule(runtime_rule.rule_id).has_value());
    MIRA_CHECK(scoped.runtime.retire_rule(runtime_rule.rule_id).has_value());
    retirements = scoped.sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(retirements.size() == 2);
    MIRA_CHECK(retirements[1].rule_id == runtime_rule.rule_id);
    MIRA_CHECK(payload_string(retirements[1], "from_status") == "Runtime");
    MIRA_CHECK(payload_string(retirements[1], "to_status") == "Retired");
    MIRA_CHECK(payload_string(retirements[1], "reason") == "host-explicit");

    // Terminal idempotence: re-retiring a Retired rule is a NoOp with zero
    // events and zero state change.
    const std::size_t before = scoped.sink.events.size();
    MIRA_CHECK(scoped.runtime.retire_rule(runtime_rule.rule_id).has_value());
    MIRA_CHECK(scoped.runtime.retire_rule(candidate_rule.rule_id).has_value());
    MIRA_CHECK(scoped.sink.events.size() == before);

    // Retired rules reject every other transition; unknown ids stay unknown.
    MIRA_CHECK(expect_error(scoped.runtime.demote_rule(runtime_rule.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(runtime_rule.rule_id),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(scoped.runtime.promote_rule(candidate_rule.rule_id),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(scoped.runtime.retire_rule("rr-0000000000000000"),
                            TemporalPolicyDomainCode::RuleUnknown) == 0);
    MIRA_CHECK(scoped.sink.events.size() == before);
    return 0;
}

} // namespace mira::m26
