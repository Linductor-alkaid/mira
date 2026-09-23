// M26 (DEC-037 Stage T1) temporal-policy IVA gate matrix - contract and
// step-evaluation halves, over the milestone §4 frozen semantics (fixtures:
// tests/m26/m26_support.hpp, case table driving tests/m26/m26_lifecycle.cpp
// as well):
//
//   T1-G1 contract and serialization:
//     - TemporalHistory bounded ring: strictly advancing sequences, FIFO
//       eviction at capacity, cross-instance digest equality, default 64;
//     - validate rejection paths for TemporalHistoryOptions,
//       TrackedEntity, PolicyWorldView and PolicyRuntimeOptions (incl. the
//       history_options fold-in);
//     - ReactiveRule fail-closed schema: empty when/do, unknown enums,
//       condition/transition bounds, provenance required from Candidate on;
//     - mira.policy.rule.v1 canonical wire: byte-identical round trip,
//       canonical serialization, DEC-002 version policy ({1,x} reads,
//       older/newer major rejected), content-derived rule_id;
//     - mira.temporal_policy error domain: all twelve codes with the
//       frozen triple mapping (domain string, int32 domain_code, stable
//       name) and the deterministic ErrorCode assignment;
//     - nine event schemas with exact names, exact payload key sets,
//       canonical payloads and the frame rule_id classification;
//     - source_digest empty on the rebuild projection path (§4.1 B2).
//
//   T1-G2 step evaluation semantics:
//     - inactive states evaluate nothing and emit nothing;
//     - tick regressions/repeats rejected with zero events and zero state
//       change;
//     - fixed match order (priority ascending, rule_id lexicographic on
//       ties) observable through the conflict demotion order;
//     - single match triggers with the transition actions and the frozen
//       RuleTriggered frame/payload;
//     - multi-match conflicts fail closed: zero actions, RuleConflicted +
//       PolicyEscalatedToAgent, every matched rule demoted;
//     - activation idempotence, empty-state rejection, immediate
//       deactivation;
//     - adoption bounds fail closed as whole batches (RULE-08);
//     - fact-only rules match without an anchor and carry an empty
//       entity_key; entity-scoped rules never match without an anchor;
//     - frame tick semantics: current tick for step events, last successful
//       step tick for lifecycle events, 0 before the first step.

#include "m26_cases.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mira::m26 {
namespace {

// Scoped runtime over an externally declared sink, so cases assert on the
// same RecordingSink the runtime was constructed with.
struct ScopedRuntime final {
    RecordingSink &sink;
    PolicyRuntimeOptions options;
    ReactivePolicyRuntime runtime;

    explicit ScopedRuntime(RecordingSink &out_sink) : sink(out_sink), runtime(options, &sink) {}
    ScopedRuntime(RecordingSink &out_sink, PolicyRuntimeOptions opts)
        : sink(out_sink), options(std::move(opts)), runtime(options, &sink) {}
};

} // namespace

// ---------------------------------------------------------------------------
// T1-G1: TemporalHistory ring semantics.
// ---------------------------------------------------------------------------

int history_ring_sequence_capacity_digest() {
    // Documented default capacity (RULE-08).
    TemporalHistory default_history;
    MIRA_CHECK(default_history.capacity() == 64);
    MIRA_CHECK(default_history.size() == 0);
    MIRA_CHECK(default_history.latest() == nullptr);
    MIRA_CHECK(default_history.entries().empty());

    TemporalHistoryOptions options;
    options.capacity = 3;
    MIRA_CHECK(options.validate().has_value());
    TemporalHistory history{options};
    MIRA_CHECK(history.capacity() == 3);

    auto entry = [](std::uint64_t sequence, double x, std::string motion) {
        TemporalHistoryEntry created;
        created.sequence = sequence;
        created.sampled_at = dataset_time(sequence);
        created.position_x = x;
        created.position_y = x * 2.0;
        created.motion = std::move(motion);
        created.motion_phase = 0.5;
        created.source_ref = "dataset://m26/ring/" + std::to_string(sequence);
        return created;
    };

    MIRA_CHECK(history.append(entry(1, 1.0, "idle")).has_value());
    MIRA_CHECK(history.append(entry(2, 2.0, "heavy_slash_a")).has_value());
    MIRA_CHECK(history.append(entry(3, 3.0, "idle")).has_value());
    MIRA_CHECK(history.size() == 3);
    MIRA_CHECK(history.latest() != nullptr && history.latest()->sequence == 3);

    // Duplicate and regressing sequences are rejected; the ring is untouched.
    MIRA_CHECK(expect_error(history.append(entry(3, 9.0, "idle")),
                            TemporalPolicyDomainCode::HistorySequenceNotAdvancing) == 0);
    MIRA_CHECK(expect_error(history.append(entry(2, 9.0, "idle")),
                            TemporalPolicyDomainCode::HistorySequenceNotAdvancing) == 0);
    MIRA_CHECK(history.size() == 3 && history.latest()->sequence == 3);
    MIRA_CHECK(history.entries()[0].position_x == 1.0);

    // Capacity eviction is FIFO-oldest-first.
    MIRA_CHECK(history.append(entry(4, 4.0, "dodge")).has_value());
    MIRA_CHECK(history.size() == 3);
    MIRA_CHECK(history.entries()[0].sequence == 2);
    MIRA_CHECK(history.entries()[1].sequence == 3);
    MIRA_CHECK(history.entries()[2].sequence == 4);
    MIRA_CHECK(history.latest() != nullptr && history.latest()->motion == "dodge");

    // digest() is byte-stable across instances with equal content and
    // differs on any content change.
    TemporalHistory twin{options};
    MIRA_CHECK(twin.append(entry(2, 2.0, "heavy_slash_a")).has_value());
    MIRA_CHECK(twin.append(entry(3, 3.0, "idle")).has_value());
    MIRA_CHECK(twin.append(entry(4, 4.0, "dodge")).has_value());
    MIRA_CHECK(twin.digest() == history.digest());
    MIRA_CHECK(twin.append(entry(5, 5.0, "idle")).has_value());
    MIRA_CHECK(twin.digest() != history.digest());
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: validate rejection paths for options and input contracts.
// ---------------------------------------------------------------------------

int options_and_input_validate_rejections() {
    // TrackedEntity: empty key, phase and confidence outside [0,1].
    auto entity = make_entity("enemy_1", "heavy_slash_a", 0.67, 0.92, "dataset://m26/e1");
    MIRA_CHECK(entity.validate().has_value());
    auto empty_key = entity;
    empty_key.entity_key.clear();
    MIRA_CHECK(expect_error(empty_key.validate(), TemporalPolicyDomainCode::EntityInvalid) == 0);
    auto phase_above = entity;
    phase_above.motion_phase = 1.5;
    MIRA_CHECK(expect_error(phase_above.validate(), TemporalPolicyDomainCode::EntityInvalid) == 0);
    auto phase_below = entity;
    phase_below.motion_phase = -0.01;
    MIRA_CHECK(expect_error(phase_below.validate(), TemporalPolicyDomainCode::EntityInvalid) == 0);
    auto confidence_above = entity;
    confidence_above.motion_confidence = 1.0001;
    MIRA_CHECK(expect_error(confidence_above.validate(), TemporalPolicyDomainCode::EntityInvalid) ==
               0);
    auto confidence_below = entity;
    confidence_below.motion_confidence = -0.5;
    MIRA_CHECK(expect_error(confidence_below.validate(), TemporalPolicyDomainCode::EntityInvalid) ==
               0);
    // Boundaries are legal.
    auto boundary = entity;
    boundary.motion_phase = 0.0;
    boundary.motion_confidence = 1.0;
    MIRA_CHECK(boundary.validate().has_value());

    // PolicyWorldView: duplicate fact keys are the frozen rejection.
    PolicyWorldView world = make_world({entity}, {make_fact("distance", 2.7)});
    MIRA_CHECK(world.validate().has_value());
    PolicyWorldView duplicate =
        make_world({entity}, {make_fact("distance", 2.7), make_fact("distance", 3.0)});
    MIRA_CHECK(expect_error(duplicate.validate(), TemporalPolicyDomainCode::WorldViewInvalid) == 0);
    // An invalid nested entity fails validation inside the domain.
    auto broken = entity;
    broken.motion_phase = 2.0;
    PolicyWorldView nested = make_world({broken}, {});
    const auto nested_result = nested.validate();
    MIRA_CHECK(!nested_result.has_value());
    MIRA_CHECK(nested_result.error().domain == temporal_policy_domain());

    // PolicyRuntimeOptions: documented defaults validate; every zeroed
    // bound or support floor fails as OptionsInvalid.
    PolicyRuntimeOptions defaults;
    MIRA_CHECK(defaults.validate().has_value());
    PolicyRuntimeOptions invalid = defaults;
    invalid.max_rules = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    invalid = defaults;
    invalid.max_rules_per_state = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    invalid = defaults;
    invalid.max_conditions_per_transition = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    invalid = defaults;
    invalid.max_transitions_per_rule = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    invalid = defaults;
    invalid.min_support = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    invalid = defaults;
    invalid.min_test_support = 0;
    MIRA_CHECK(expect_error(invalid.validate(), TemporalPolicyDomainCode::OptionsInvalid) == 0);
    // The history options fold in (capacity 0 fails validation inside the
    // domain; the propagated code is implementation-pinned by the fold).
    invalid = defaults;
    invalid.history_options.capacity = 0;
    const auto folded = invalid.validate();
    MIRA_CHECK(!folded.has_value());
    MIRA_CHECK(folded.error().domain == temporal_policy_domain());
    // And directly: capacity 0 is HistoryCapacityInvalid.
    TemporalHistoryOptions capacityless;
    capacityless.capacity = 0;
    MIRA_CHECK(expect_error(capacityless.validate(),
                            TemporalPolicyDomainCode::HistoryCapacityInvalid) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: ReactiveRule fail-closed schema.
// ---------------------------------------------------------------------------

int rule_schema_fail_closed() {
    const auto valid = slash_rule(1);
    MIRA_CHECK(valid.validate().has_value());

    auto broken = valid;
    broken.transitions.clear();
    MIRA_CHECK(expect_error(broken.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);

    broken = valid;
    broken.transitions[0].when.clear();
    MIRA_CHECK(expect_error(broken.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);

    broken = valid;
    broken.transitions[0].do_actions.clear();
    MIRA_CHECK(expect_error(broken.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);

    // Unknown enum values (closed sets) are rejected.
    broken = valid;
    broken.transitions[0].when[0].field = static_cast<RuleCondition::Field>(77);
    MIRA_CHECK(expect_error(broken.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    broken = valid;
    broken.transitions[0].when[0].compare = static_cast<RuleCondition::Compare>(200);
    MIRA_CHECK(expect_error(broken.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);

    // Condition bound (8): nine well-formed conditions in one transition
    // fail; eight validate and exercise the full closed set at once.
    std::vector<RuleCondition> nine;
    nine.push_back(motion_eq("heavy_slash_a"));
    nine.push_back(
        numeric_condition(RuleCondition::Field::MotionPhase, RuleCondition::Compare::Gt, 0.1));
    nine.push_back(numeric_condition(RuleCondition::Field::MotionConfidence,
                                     RuleCondition::Compare::Le, 0.99));
    nine.push_back(fact_condition(RuleCondition::Compare::Eq, "f1", 1.0));
    nine.push_back(fact_condition(RuleCondition::Compare::Lt, "f2", 2.0));
    nine.push_back(fact_condition(RuleCondition::Compare::Le, "f3", 3.0));
    nine.push_back(fact_condition(RuleCondition::Compare::Gt, "f4", 4.0));
    nine.push_back(fact_condition(RuleCondition::Compare::Ge, "f5", 5.0));
    MIRA_CHECK(nine.size() == 8);
    const auto eight_conditions =
        make_rule("combat", 0, {make_transition(nine, {make_action("dodge", "left")})});
    MIRA_CHECK(eight_conditions.validate().has_value());
    nine.push_back(motion_eq("jab"));
    const auto nine_conditions =
        make_rule("combat", 0, {make_transition(nine, {make_action("dodge", "left")})});
    MIRA_CHECK(
        expect_error(nine_conditions.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);

    // Transition bound (8): nine transitions fail, eight validate.
    std::vector<RuleTransition> nine_transitions;
    for (std::size_t index = 0; index < 9; ++index) {
        nine_transitions.push_back(make_transition(
            {fact_condition(RuleCondition::Compare::Eq, "f" + std::to_string(index), 1.0)},
            {make_action("dodge", "left")}));
    }
    const auto nine_rule = make_rule("combat", 0, nine_transitions);
    MIRA_CHECK(expect_error(nine_rule.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) ==
               0);
    std::vector<RuleTransition> eight_transitions(nine_transitions.begin(),
                                                  nine_transitions.begin() + 8);
    const auto eight_rule = make_rule("combat", 0, eight_transitions);
    MIRA_CHECK(eight_rule.validate().has_value());

    // Provenance is mandatory from Candidate on (RULE-07: no naked rules).
    auto naked = slash_rule(0);
    naked.source_refs.clear();
    MIRA_CHECK(expect_error(naked.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    naked.status = ReactiveRuleStatus::Testing;
    MIRA_CHECK(expect_error(naked.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    naked.status = ReactiveRuleStatus::Runtime;
    MIRA_CHECK(expect_error(naked.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    naked.status = ReactiveRuleStatus::Retired;
    MIRA_CHECK(expect_error(naked.validate(), TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: mira.policy.rule.v1 canonical wire and DEC-002 version policy.
// ---------------------------------------------------------------------------

int rule_wire_json_round_trip_and_version_policy() {
    const auto rule = slash_rule(3);
    const std::string wire = rule.to_json();
    MIRA_CHECK(!wire.empty());

    // Byte-identical round trip preserving the frozen fields.
    const auto read_back = ReactiveRule::from_json(wire);
    MIRA_CHECK(read_back.has_value());
    MIRA_CHECK(read_back.value().to_json() == wire);
    MIRA_CHECK(read_back.value().rule_id == rule.rule_id);
    MIRA_CHECK(read_back.value().state == "combat");
    MIRA_CHECK(read_back.value().priority == 3);
    MIRA_CHECK((read_back.value().schema_version == SchemaVersion{1, 0}));
    MIRA_CHECK(read_back.value().transitions.size() == 1);
    MIRA_CHECK(read_back.value().transitions[0].when.size() == 3);
    MIRA_CHECK(read_back.value().transitions[0].when[1].compare == RuleCondition::Compare::Ge);
    MIRA_CHECK(read_back.value().transitions[0].do_actions.size() == 1);
    MIRA_CHECK(read_back.value().transitions[0].do_actions[0].kind == "dodge");
    MIRA_CHECK(read_back.value().source_refs == rule.source_refs);

    // The wire is canonical JSON: byte-stable across processes.
    const auto parsed = parse_json(wire);
    MIRA_CHECK(parsed.has_value());
    MIRA_CHECK(canonical_json_string(parsed.value()) == wire);

    // DEC-002 version policy. Repo wire convention for versioned contracts:
    // "schema_version": {"major": M, "minor": m}.
    auto rule_minor = slash_rule(3);
    rule_minor.schema_version = SchemaVersion{1, 1};
    const auto read_minor = ReactiveRule::from_json(rule_minor.to_json());
    MIRA_CHECK(read_minor.has_value());
    MIRA_CHECK((read_minor.value().schema_version == SchemaVersion{1, 1}));

    auto rewrite_version = [](const std::string &wire_text, std::uint16_t major,
                              std::uint16_t minor) {
        auto document = parse_json(wire_text);
        if (!document.has_value()) {
            return std::string();
        }
        JsonValue *version = document.value().find("schema_version");
        if (version == nullptr || !version->is_object()) {
            return std::string();
        }
        *version =
            JsonValue(JsonValue::Object{{"major", JsonValue(static_cast<std::int64_t>(major))},
                                        {"minor", JsonValue(static_cast<std::int64_t>(minor))}});
        return to_json_string(document.value());
    };
    const std::string older = rewrite_version(wire, 0, 9);
    MIRA_CHECK(!older.empty());
    MIRA_CHECK(!ReactiveRule::from_json(older).has_value());
    const std::string newer = rewrite_version(wire, 2, 0);
    MIRA_CHECK(!newer.empty());
    MIRA_CHECK(!ReactiveRule::from_json(newer).has_value());
    MIRA_CHECK(!ReactiveRule::from_json("{not json").has_value());

    // Content-derived identity: priority is not semantic content; the do
    // action is. Ids are frozen-format content digests derived at rule
    // construction, so the variant rule is built through the same
    // derivation with the mutated action.
    MIRA_CHECK(valid_rule_id(rule.rule_id));
    auto twin = slash_rule(9);
    MIRA_CHECK(twin.rule_id == rule.rule_id);
    MIRA_CHECK(twin.digest() == rule.digest());
    auto other =
        make_rule("combat", 3,
                  {make_transition({motion_eq("heavy_slash_a"),
                                    numeric_condition(RuleCondition::Field::MotionPhase,
                                                      RuleCondition::Compare::Ge, 0.55),
                                    fact_condition(RuleCondition::Compare::Lt, "distance", 3.0)},
                                   {make_action("dodge", "right")})});
    MIRA_CHECK(other.digest() != rule.digest());
    MIRA_CHECK(other.rule_id != rule.rule_id);
    MIRA_CHECK(other.rule_id == "rr-" + other.digest().to_string().substr(0, 16));
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: mira.temporal_policy error domain - twelve codes, triple mapping.
// ---------------------------------------------------------------------------

int error_domain_twelve_codes_triple_mapping() {
    MIRA_CHECK(temporal_policy_domain() == "mira.temporal_policy");
    int seen = 0;
    for (const auto &entry : kDomainCodes) {
        const Error error = make_temporal_policy_error(entry.code, "m26 synthetic probe");
        MIRA_CHECK(error.domain == "mira.temporal_policy");
        MIRA_CHECK(error.domain_code == static_cast<std::int32_t>(entry.code));
        MIRA_CHECK(temporal_policy_domain_code_name(entry.code) == entry.name);
        MIRA_CHECK(temporal_policy_domain_code_name(
                       static_cast<TemporalPolicyDomainCode>(error.domain_code)) == entry.name);
        MIRA_CHECK(error.code == entry.mapped);
        MIRA_CHECK(!error.retryable);
        const Error retryable = make_temporal_policy_error(entry.code, "m26 retryable probe", true);
        MIRA_CHECK(retryable.retryable);
        ++seen;
    }
    MIRA_CHECK(seen == 12);
    // The int32 values themselves are DEC-002 stable public values 1..12.
    for (std::int32_t value = 1; value <= 12; ++value) {
        const auto code = static_cast<TemporalPolicyDomainCode>(value);
        MIRA_CHECK(temporal_policy_domain_code_name(code) == kDomainCodes[value - 1].name);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: the nine event schema names.
// ---------------------------------------------------------------------------

int event_schema_names_exact_nine() {
    int seen = 0;
    for (const auto &entry : kEventSchemas) {
        MIRA_CHECK(policy_event_schema_name(entry.type) == entry.schema);
        ++seen;
    }
    MIRA_CHECK(seen == 9);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: payload key sets + frame classification over a scenario that emits
// all nine event types.
// ---------------------------------------------------------------------------

int event_payload_key_sets_frozen() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;

    // Disjoint triggers, shared action: both rules collect clean evidence
    // and both can sit in Runtime together for the conflict leg.
    auto motion_rule =
        make_rule("combat", 1,
                  {make_transition({motion_eq("heavy_slash_a"),
                                    numeric_condition(RuleCondition::Field::MotionPhase,
                                                      RuleCondition::Compare::Ge, 0.5)},
                                   {make_action("dodge", "left")})});
    auto fact_rule =
        make_rule("combat", 2,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 3.0)},
                                   {make_action("dodge", "left")})});

    MIRA_CHECK(runtime.activate("combat").has_value());
    std::vector<PolicyEpisodeSample> evidence;
    // motion_rule evidence: slash anchor, far distance (fact_rule silent).
    evidence.push_back(
        make_sample(101, slash_world(0.67, 6.0), "combat", make_action("dodge", "left"), true));
    evidence.push_back(
        make_sample(102, slash_world(0.8, 6.0), "combat", make_action("dodge", "left"), true));
    // fact_rule evidence: idle anchor and empty world at close distance.
    auto idle_close = make_world({make_entity("enemy_1", "idle", 0.1, 0.5, "dataset://m26/idle")},
                                 {make_fact("distance", 2.0)});
    evidence.push_back(
        make_sample(103, std::move(idle_close), "combat", make_action("dodge", "left"), true));
    evidence.push_back(make_sample(104, make_world({}, {make_fact("distance", 2.5)}), "combat",
                                   make_action("dodge", "left"), true));

    std::vector<ReactiveRule> batch{motion_rule, fact_rule};
    const auto adopted = runtime.adopt_candidate_rules(batch);
    MIRA_CHECK(adopted.has_value() && adopted.value() == 2);
    const auto report = runtime.test_candidate_rules(
        std::span<const PolicyEpisodeSample>(evidence.data(), evidence.size()));
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().results.size() == 2);
    MIRA_CHECK(std::is_sorted(
        report.value().results.begin(), report.value().results.end(),
        [](const auto &left, const auto &right) { return left.rule_id < right.rule_id; }));
    for (const auto &row : report.value().results) {
        MIRA_CHECK(row.passed_count == 2);
        MIRA_CHECK(row.false_trigger_count == 0);
        MIRA_CHECK(row.sample_count == 2);
    }
    MIRA_CHECK(runtime.promote_rule(motion_rule.rule_id).has_value());
    MIRA_CHECK(runtime.promote_rule(fact_rule.rule_id).has_value());

    // Step 1: exactly one match (distance 6.0 keeps the fact rule silent).
    const auto actions = runtime.step(slash_world(0.67, 6.0), make_tick(1));
    MIRA_CHECK(actions.has_value() && actions.value().size() == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 1);

    // host-explicit demote, then re-promote on the persisted evidence.
    MIRA_CHECK(runtime.demote_rule(motion_rule.rule_id, "host-explicit").has_value());
    MIRA_CHECK(runtime.promote_rule(motion_rule.rule_id).has_value());

    // Deactivate/reactivate with an idempotent repeat.
    MIRA_CHECK(runtime.deactivate("combat").has_value());
    MIRA_CHECK(runtime.activate("combat").has_value());
    MIRA_CHECK(runtime.activate("combat").has_value());

    // Step 2: both rules match -> conflict fail-closed.
    const auto conflicted = runtime.step(slash_world(0.67, 2.0), make_tick(2));
    MIRA_CHECK(conflicted.has_value() && conflicted.value().empty());
    MIRA_CHECK(sink.count(PolicyEventType::RuleConflicted) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::PolicyEscalatedToAgent) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleDemoted) == 3); // 1 host + 2 conflict

    // 2 activated + 1 deactivated + 2 induced + 2 testing + 3 promoted +
    // 1 triggered + 3 demoted + 1 conflicted + 1 escalated = 16.
    MIRA_CHECK(sink.events.size() == 16);

    // Every event: exact schema name, exact payload key set, canonical
    // payload, and the frozen frame rule_id classification.
    for (const auto &event : sink.events) {
        const auto &entry = schema_entry(event.type);
        MIRA_CHECK(policy_event_schema_name(event.type) == entry.schema);
        MIRA_CHECK(expect_payload_keys(event, entry) == 0);
        if (entry.frame_carries_rule_id) {
            MIRA_CHECK(!event.rule_id.empty());
        } else {
            MIRA_CHECK(event.rule_id.empty());
        }
    }

    // All nine types appeared in one scenario.
    std::vector<PolicyEventType> seen;
    for (const auto &event : sink.events) {
        if (std::find(seen.begin(), seen.end(), event.type) == seen.end()) {
            seen.push_back(event.type);
        }
    }
    MIRA_CHECK(seen.size() == 9);

    // Single-rule events name exactly their rule.
    const auto *triggered = sink.single(PolicyEventType::RuleTriggered);
    MIRA_CHECK(triggered != nullptr && triggered->rule_id == motion_rule.rule_id);
    const auto induced = sink.of_type(PolicyEventType::RuleCandidateInduced);
    MIRA_CHECK(induced.size() == 2);
    MIRA_CHECK(induced[0].rule_id != induced[1].rule_id);

    // Conflict payloads: sorted matched ids, closed reason set.
    std::vector<std::string> matched{motion_rule.rule_id, fact_rule.rule_id};
    std::sort(matched.begin(), matched.end());
    const auto *conflicted_event = sink.single(PolicyEventType::RuleConflicted);
    MIRA_CHECK(conflicted_event != nullptr);
    MIRA_CHECK(payload_string(*conflicted_event, "state") == "combat");
    const auto conflicted_parsed = parse_json(conflicted_event->payload_json);
    MIRA_CHECK(conflicted_parsed.has_value());
    const auto *ids = conflicted_parsed.value().find("matched_rule_ids");
    MIRA_CHECK(ids != nullptr && ids->is_array() && ids->as_array()->size() == 2);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto *entry = ids->as_array()->at(index).as_string();
        MIRA_CHECK(entry != nullptr && *entry == matched[index]);
    }
    const auto *escalated = sink.single(PolicyEventType::PolicyEscalatedToAgent);
    MIRA_CHECK(escalated != nullptr);
    MIRA_CHECK(payload_string(*escalated, "reason") == "conflict");
    MIRA_CHECK(payload_string(*escalated, "state") == "combat");

    // Demotion payloads stay inside the frozen closures.
    for (const auto &event : sink.of_type(PolicyEventType::RuleDemoted)) {
        const std::string reason = payload_string(event, "reason");
        const bool known_reason = reason == "conflict" || reason == "host-explicit";
        MIRA_CHECK(known_reason);
        MIRA_CHECK(payload_string(event, "from_status") == "Runtime");
        MIRA_CHECK(payload_string(event, "to_status") == "Candidate");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G1: source_digest empty on the rebuild projection path.
// ---------------------------------------------------------------------------

int source_digest_empty_on_reconstruction_path() {
    std::vector<PolicyEpisodeSample> samples;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        samples.push_back(make_sample(tick, slash_world(0.5 + 0.1 * static_cast<double>(tick), 2.5),
                                      "combat", make_action("dodge", "left"), true));
    }
    const auto rebuilt = rebuild_temporal_histories(samples);
    MIRA_CHECK(rebuilt.has_value());
    MIRA_CHECK(rebuilt.value().size() == 1);
    const auto found = rebuilt.value().find("enemy_1");
    MIRA_CHECK(found != rebuilt.value().end());
    const TemporalHistory &history = found->second;
    MIRA_CHECK(history.size() == 3);
    for (const auto &entry : history.entries()) {
        // Frozen T1 semantics (§4.1, review finding B2): no payload digests
        // exist in T1 inputs, so the field stays empty by contract.
        MIRA_CHECK(entry.source_digest.empty());
        MIRA_CHECK(!entry.source_ref.empty());
        MIRA_CHECK(entry.motion == "heavy_slash_a");
        // Entries mirror the entity snapshot verbatim (make_entity sets
        // x = y = 5.0 for the slash world).
        MIRA_CHECK(entry.position_x == 5.0);
        MIRA_CHECK(entry.position_y == 5.0);
    }
    // Per-entity sequences advance strictly.
    MIRA_CHECK(history.entries()[0].sequence < history.entries()[1].sequence);
    MIRA_CHECK(history.entries()[1].sequence < history.entries()[2].sequence);
    // The clock is the dataset clock, never the system clock.
    MIRA_CHECK(history.entries()[0].sampled_at.wall.time_since_epoch() ==
               dataset_time(1).wall.time_since_epoch());
    // The schema default is the empty string as well.
    TemporalHistoryEntry fresh;
    MIRA_CHECK(fresh.source_digest.empty());
    MIRA_CHECK(fresh.motion.empty());

    // Reconstruction is a pure projection: replaying the same episodes
    // yields a byte-identical digest.
    const auto replayed = rebuild_temporal_histories(samples);
    MIRA_CHECK(replayed.has_value() && replayed.value().size() == rebuilt.value().size());
    MIRA_CHECK(replayed.value().find("enemy_1")->second.digest() == history.digest());

    // The step() accumulation path (the second frozen construction path):
    // drive a runtime over the same samples with the same history options
    // and observe its per-entity accumulator through the additive read-only
    // accessor (DEC-002 evolution; closes the observation gap reported in
    // verification round 1). Both paths must agree byte-for-byte (T1-G4).
    RecordingSink sink;
    PolicyRuntimeOptions options;
    ReactivePolicyRuntime runtime{options, &sink};
    for (const auto &sample : samples) {
        const auto stepped = runtime.step(sample.world, sample.tick);
        MIRA_CHECK(stepped.has_value()); // no rules adopted: plain accumulation
    }
    MIRA_CHECK(sink.events.empty());
    const TemporalHistory *accumulated = runtime.temporal_history("enemy_1");
    MIRA_CHECK(accumulated != nullptr);
    MIRA_CHECK(accumulated->size() == 3);
    for (const auto &entry : accumulated->entries()) {
        // The step path keeps source_digest empty as well (frozen §4.1 B2).
        MIRA_CHECK(entry.source_digest.empty());
        MIRA_CHECK(!entry.source_ref.empty());
        MIRA_CHECK(entry.motion == "heavy_slash_a");
    }
    MIRA_CHECK(accumulated->digest() == history.digest());
    // An unknown entity has no accumulated history.
    MIRA_CHECK(runtime.temporal_history("nobody") == nullptr);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: inactive states evaluate nothing.
// ---------------------------------------------------------------------------

int inactive_state_zero_evaluation_zero_events() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);
    MIRA_CHECK(promote_via_evidence(runtime, rule, slash_evidence(101), 2, 0) == 0);
    MIRA_CHECK(!runtime.is_active("combat"));
    const std::size_t lifecycle_trail = sink.events.size();
    MIRA_CHECK(lifecycle_trail == 3); // induced + testing + promoted

    // Matching world, but the state was never activated: zero actions,
    // zero events (history accumulation is internal and audit-only).
    const auto actions = runtime.step(slash_world(0.8, 2.0), make_tick(1));
    MIRA_CHECK(actions.has_value() && actions.value().empty());
    MIRA_CHECK(sink.events.size() == lifecycle_trail);

    // Host activation arms the very next tick.
    MIRA_CHECK(runtime.activate("combat").has_value());
    MIRA_CHECK(runtime.is_active("combat"));
    const auto after = runtime.step(slash_world(0.8, 2.0), make_tick(2));
    MIRA_CHECK(after.has_value() && after.value().size() == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: tick monotonicity.
// ---------------------------------------------------------------------------

int tick_monotonic_rejection_zero_state_change() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);
    MIRA_CHECK(promote_via_evidence(runtime, rule, slash_evidence(101), 2, 0) == 0);
    MIRA_CHECK(runtime.activate("combat").has_value());

    const auto first = runtime.step(slash_world(0.8, 2.0), make_tick(5));
    MIRA_CHECK(first.has_value() && first.value().size() == 1);
    const std::size_t baseline = sink.events.size();

    // Repeated tick.
    const auto repeat = runtime.step(slash_world(0.8, 2.0), make_tick(5));
    MIRA_CHECK(expect_error(repeat, TemporalPolicyDomainCode::TickNotMonotonic) == 0);
    // Regressed tick.
    const auto regression = runtime.step(slash_world(0.8, 2.0), make_tick(4));
    MIRA_CHECK(expect_error(regression, TemporalPolicyDomainCode::TickNotMonotonic) == 0);
    // Zero events from both rejections.
    MIRA_CHECK(sink.events.size() == baseline);
    // Zero state change: the rule is still a live Runtime member and the
    // next strictly-advancing tick fires it again.
    const auto next = runtime.step(slash_world(0.8, 2.0), make_tick(6));
    MIRA_CHECK(next.has_value() && next.value().size() == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 2);
    MIRA_CHECK(sink.count(PolicyEventType::RuleDemoted) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: fixed match order (priority, then rule_id on ties).
// ---------------------------------------------------------------------------

int match_order_fixed_priority_then_rule_id() {
    // Three rules over the same world with identical actions so each
    // collects clean evidence; conflict collection order is observable
    // through the per-rule demotion sequence.
    auto wide =
        make_rule("combat", 5,
                  {make_transition({motion_eq("heavy_slash_a")}, {make_action("dodge", "left")})});
    auto narrow = make_rule("combat", 5,
                            {make_transition({motion_eq("heavy_slash_a"),
                                              numeric_condition(RuleCondition::Field::MotionPhase,
                                                                RuleCondition::Compare::Ge, 0.4)},
                                             {make_action("dodge", "left")})});
    auto close =
        make_rule("combat", 5,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 5.0)},
                                   {make_action("dodge", "left")})});

    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    MIRA_CHECK(runtime.activate("combat").has_value());
    const auto evidence = slash_evidence(201);
    MIRA_CHECK(promote_via_evidence(runtime, wide, evidence, 2, 0) == 0);
    MIRA_CHECK(promote_via_evidence(runtime, narrow, evidence, 2, 0) == 0);
    MIRA_CHECK(promote_via_evidence(runtime, close, evidence, 2, 0) == 0);

    // (a) Equal priorities: demotions follow rule_id lexicographic order.
    const auto actions = runtime.step(slash_world(0.67, 2.7), make_tick(301));
    MIRA_CHECK(actions.has_value() && actions.value().empty());
    std::vector<std::string> tied{wide.rule_id, narrow.rule_id, close.rule_id};
    std::sort(tied.begin(), tied.end());
    const auto demotions = sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions.size() == 3);
    for (std::size_t index = 0; index < demotions.size(); ++index) {
        MIRA_CHECK(demotions[index].rule_id == tied[index]);
        MIRA_CHECK(payload_string(demotions[index], "reason") == "conflict");
        MIRA_CHECK(payload_string(demotions[index], "from_status") == "Runtime");
        MIRA_CHECK(payload_string(demotions[index], "to_status") == "Candidate");
    }
    // The conflict payloads list the same set in lexicographic order.
    const auto *conflicted = sink.single(PolicyEventType::RuleConflicted);
    MIRA_CHECK(conflicted != nullptr && conflicted->rule_id.empty());
    const auto parsed = parse_json(conflicted->payload_json);
    MIRA_CHECK(parsed.has_value());
    const auto *ids = parsed.value().find("matched_rule_ids");
    MIRA_CHECK(ids != nullptr && ids->is_array() && ids->as_array()->size() == 3);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto *entry = ids->as_array()->at(index).as_string();
        MIRA_CHECK(entry != nullptr && *entry == tied[index]);
    }
    const auto *escalated = sink.single(PolicyEventType::PolicyEscalatedToAgent);
    MIRA_CHECK(escalated != nullptr && escalated->rule_id.empty());

    // (b) Distinct priorities: priority ascending wins regardless of the
    // adoption order.
    RecordingSink sink_b;
    ScopedRuntime scoped_b{sink_b};
    auto &runtime_b = scoped_b.runtime;
    MIRA_CHECK(runtime_b.activate("combat").has_value());
    auto low_priority =
        make_rule("combat", 1,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 5.0)},
                                   {make_action("dodge", "left")})});
    auto high_priority =
        make_rule("combat", 10,
                  {make_transition({motion_eq("heavy_slash_a")}, {make_action("dodge", "left")})});
    MIRA_CHECK(promote_via_evidence(runtime_b, high_priority, evidence, 2, 0) == 0);
    MIRA_CHECK(promote_via_evidence(runtime_b, low_priority, evidence, 2, 0) == 0);
    const auto actions_b = runtime_b.step(slash_world(0.67, 2.7), make_tick(302));
    MIRA_CHECK(actions_b.has_value() && actions_b.value().empty());
    const auto demotions_b = sink_b.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions_b.size() == 2);
    MIRA_CHECK(demotions_b[0].rule_id == low_priority.rule_id);  // priority 1 first
    MIRA_CHECK(demotions_b[1].rule_id == high_priority.rule_id); // priority 10 second
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: single match triggers with the frozen frame and payload.
// ---------------------------------------------------------------------------

int single_match_triggers_actions_and_event() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);
    MIRA_CHECK(promote_via_evidence(runtime, rule, slash_evidence(101), 2, 0) == 0);
    MIRA_CHECK(runtime.activate("combat").has_value());
    const std::size_t step_event_baseline = sink.events.size();

    const auto actions = runtime.step(slash_world(0.7, 2.5), make_tick(1));
    MIRA_CHECK(actions.has_value() && actions.value().size() == 1);
    MIRA_CHECK(actions.value()[0].kind == "dodge");
    MIRA_CHECK(actions.value()[0].parameter == "left");

    const auto *triggered = sink.single(PolicyEventType::RuleTriggered);
    MIRA_CHECK(triggered != nullptr);
    MIRA_CHECK(triggered->rule_id == rule.rule_id);
    MIRA_CHECK(triggered->tick == 1);
    MIRA_CHECK(payload_string(*triggered, "state") == "combat");
    MIRA_CHECK(payload_string(*triggered, "entity_key") == "enemy_1");
    const auto payload = parse_json(triggered->payload_json);
    MIRA_CHECK(payload.has_value());
    const auto *action_array = payload.value().find("actions");
    MIRA_CHECK(action_array != nullptr && action_array->is_array() &&
               action_array->as_array()->size() == 1);
    const auto &action_object = action_array->as_array()->at(0);
    const auto *kind = action_object.find("kind");
    const auto *parameter = action_object.find("parameter");
    MIRA_CHECK(kind != nullptr && kind->is_string() && *kind->as_string() == "dodge");
    MIRA_CHECK(parameter != nullptr && parameter->is_string() && *parameter->as_string() == "left");

    // A non-matching world is the normal zero-action, zero-event state.
    const auto quiet = runtime.step(slash_world(0.2, 9.0), make_tick(2));
    MIRA_CHECK(quiet.has_value() && quiet.value().empty());
    MIRA_CHECK(sink.events.size() == step_event_baseline + 1);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: conflict fail-closed.
// ---------------------------------------------------------------------------

int conflict_fail_closed_zero_actions_full_demotion() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    auto first = make_rule("combat", 1,
                           {make_transition({motion_eq("heavy_slash_a"),
                                             numeric_condition(RuleCondition::Field::MotionPhase,
                                                               RuleCondition::Compare::Ge, 0.5)},
                                            {make_action("dodge", "left")})});
    auto second =
        make_rule("combat", 2,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 3.0)},
                                   {make_action("dodge", "left")})});
    MIRA_CHECK(runtime.activate("combat").has_value());
    MIRA_CHECK(promote_via_evidence(runtime, first, slash_evidence(101), 2, 0) == 0);
    MIRA_CHECK(promote_via_evidence(runtime, second, close_evidence(201), 2, 0) == 0);

    const auto actions = runtime.step(slash_world(0.67, 2.0), make_tick(1));
    MIRA_CHECK(actions.has_value() && actions.value().empty());

    MIRA_CHECK(sink.count(PolicyEventType::RuleConflicted) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::PolicyEscalatedToAgent) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleDemoted) == 2);
    for (const auto &event : sink.of_type(PolicyEventType::RuleDemoted)) {
        MIRA_CHECK(payload_string(event, "reason") == "conflict");
        MIRA_CHECK(payload_string(event, "to_status") == "Candidate");
    }

    // Both matched rules are back to Candidate: re-demoting them is now an
    // illegal transition (they have no runtime face left), with zero events.
    MIRA_CHECK(expect_error(runtime.demote_rule(first.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(expect_error(runtime.demote_rule(second.rule_id, "conflict"),
                            TemporalPolicyDomainCode::RuleStateInvalid) == 0);
    MIRA_CHECK(sink.count(PolicyEventType::RuleDemoted) == 2);

    // Control returned to the agent: the same world no longer auto-executes.
    const auto repeat = runtime.step(slash_world(0.67, 2.0), make_tick(2));
    MIRA_CHECK(repeat.has_value() && repeat.value().empty());
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: activation idempotence and immediate deactivation.
// ---------------------------------------------------------------------------

int activate_deactivate_idempotent_and_immediate() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);
    MIRA_CHECK(promote_via_evidence(runtime, rule, slash_evidence(101), 2, 0) == 0);

    MIRA_CHECK(!runtime.is_active("combat"));
    MIRA_CHECK(runtime.activate("combat").has_value());
    MIRA_CHECK(runtime.activate("combat").has_value()); // idempotent NoOp
    MIRA_CHECK(runtime.is_active("combat"));
    MIRA_CHECK(sink.count(PolicyEventType::PolicyActivated) == 1); // NoOp is silent
    MIRA_CHECK(expect_error(runtime.activate(""), TemporalPolicyDomainCode::RuleSchemaInvalid) ==
               0);

    // Deactivation is immediate: the next matching tick does nothing and
    // does not wait for any rule consent.
    MIRA_CHECK(runtime.deactivate("combat").has_value());
    MIRA_CHECK(!runtime.is_active("combat"));
    const auto blocked = runtime.step(slash_world(0.8, 2.0), make_tick(1));
    MIRA_CHECK(blocked.has_value() && blocked.value().empty());
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 0);

    // Deactivating an inactive state is an idempotent NoOp.
    MIRA_CHECK(runtime.deactivate("combat").has_value());
    MIRA_CHECK(sink.count(PolicyEventType::PolicyDeactivated) == 1);

    // Reactivation re-arms on the next tick.
    MIRA_CHECK(runtime.activate("combat").has_value());
    const auto rearmed = runtime.step(slash_world(0.8, 2.0), make_tick(2));
    MIRA_CHECK(rearmed.has_value() && rearmed.value().size() == 1);
    MIRA_CHECK(sink.count(PolicyEventType::PolicyActivated) == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: adoption bounds fail closed as whole batches (RULE-08).
// ---------------------------------------------------------------------------

int adopt_bounds_fail_closed_whole_batch() {
    auto combat_one = slash_rule(1);
    auto explore_one = make_rule(
        "explore", 1, {make_transition({motion_eq("patrol")}, {make_action("follow", "")})});
    auto combat_two =
        make_rule("combat", 2, {make_transition({motion_eq("jab")}, {make_action("guard", "")})});
    auto explore_two = make_rule(
        "explore", 2, {make_transition({motion_eq("search")}, {make_action("scan", "")})});
    auto retreat_one =
        make_rule("retreat", 1,
                  {make_transition({fact_condition(RuleCondition::Compare::Gt, "distance", 8.0)},
                                   {make_action("flee", "")})});

    RecordingSink sink;
    PolicyRuntimeOptions options;
    options.max_rules = 3;
    ScopedRuntime scoped{sink, options};
    auto &runtime = scoped.runtime;

    std::vector<ReactiveRule> first_batch{combat_one, explore_one};
    const auto first = runtime.adopt_candidate_rules(first_batch);
    MIRA_CHECK(first.has_value() && first.value() == 2);
    MIRA_CHECK(sink.count(PolicyEventType::RuleCandidateInduced) == 2);

    // A batch that would exceed max_rules is rejected whole: zero events,
    // zero partial adoption (combat_two would still fit afterwards).
    std::vector<ReactiveRule> overflow{combat_two, explore_two, retreat_one};
    MIRA_CHECK(expect_error(runtime.adopt_candidate_rules(overflow),
                            TemporalPolicyDomainCode::BoundsExceeded) == 0);
    MIRA_CHECK(sink.count(PolicyEventType::RuleCandidateInduced) == 2);
    const auto single =
        runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&combat_two, 1});
    MIRA_CHECK(single.has_value() && single.value() == 1);
    const auto full = runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&explore_two, 1});
    MIRA_CHECK(expect_error(full, TemporalPolicyDomainCode::BoundsExceeded) == 0);

    // Per-state bound: the third combat rule trips it and the whole mixed
    // batch is rejected (explore_one was legal but is not partially added).
    RecordingSink scoped_sink;
    PolicyRuntimeOptions per_state;
    per_state.max_rules_per_state = 2;
    ReactivePolicyRuntime scoped_runtime{per_state, &scoped_sink};
    std::vector<ReactiveRule> combat_batch{combat_one, combat_two};
    MIRA_CHECK(scoped_runtime.adopt_candidate_rules(combat_batch).has_value());
    auto combat_three =
        make_rule("combat", 3, {make_transition({motion_eq("feint")}, {make_action("parry", "")})});
    std::vector<ReactiveRule> mixed{combat_three, explore_one};
    MIRA_CHECK(expect_error(scoped_runtime.adopt_candidate_rules(mixed),
                            TemporalPolicyDomainCode::BoundsExceeded) == 0);
    const auto explore_only =
        scoped_runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&explore_one, 1});
    MIRA_CHECK(explore_only.has_value() && explore_only.value() == 1);
    MIRA_CHECK(scoped_sink.count(PolicyEventType::RuleCandidateInduced) == 3);

    // Duplicate rule_id anywhere is an idempotent skip inside the batch.
    std::vector<ReactiveRule> duplicate{explore_two, explore_two};
    const auto repeated = scoped_runtime.adopt_candidate_rules(duplicate);
    MIRA_CHECK(repeated.has_value() && repeated.value() == 1);
    MIRA_CHECK(scoped_sink.count(PolicyEventType::RuleCandidateInduced) == 4);

    // A schema-invalid rule is rejected.
    auto invalid = slash_rule(0);
    invalid.transitions.clear();
    MIRA_CHECK(expect_error(
                   scoped_runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&invalid, 1}),
                   TemporalPolicyDomainCode::RuleSchemaInvalid) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: fact-only rules match without an anchor; entity_key stays empty.
// ---------------------------------------------------------------------------

int fact_only_rule_matches_entity_key_empty() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule =
        make_rule("combat", 1,
                  {make_transition({fact_condition(RuleCondition::Compare::Le, "distance", 3.0)},
                                   {make_action("dodge", "left")})});
    MIRA_CHECK(promote_via_evidence(runtime, rule, close_evidence(101), 2, 0) == 0);
    MIRA_CHECK(runtime.activate("combat").has_value());
    const std::size_t step_event_baseline = sink.events.size();

    // Entities present but matching nothing: the fact-only rule still
    // matches, with the frozen empty entity_key (review finding B-C).
    auto with_entities =
        runtime.step(make_world({make_entity("enemy_1", "idle", 0.0, 0.0, "dataset://m26/idle")},
                                {make_fact("distance", 2.0)}),
                     make_tick(1));
    MIRA_CHECK(with_entities.has_value() && with_entities.value().size() == 1);
    const auto *triggered = sink.single(PolicyEventType::RuleTriggered);
    MIRA_CHECK(triggered != nullptr);
    MIRA_CHECK(payload_string(*triggered, "entity_key").empty());
    const auto payload = parse_json(triggered->payload_json);
    MIRA_CHECK(payload.has_value());
    MIRA_CHECK(payload.value().find("entity_key") != nullptr); // key present, value empty

    // Empty entities: no anchor is needed for a pure fact rule.
    const auto without_entities =
        runtime.step(make_world({}, {make_fact("distance", 2.0)}), make_tick(2));
    MIRA_CHECK(without_entities.has_value() && without_entities.value().size() == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleTriggered) == 2);

    // Beyond the fact bound: no match, no events.
    const auto far = runtime.step(make_world({}, {make_fact("distance", 9.0)}), make_tick(3));
    MIRA_CHECK(far.has_value() && far.value().empty());
    MIRA_CHECK(sink.events.size() == step_event_baseline + 2);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: entity-scoped rules never match without an anchor entity.
// ---------------------------------------------------------------------------

int entity_scoped_rule_empty_entities_zero_match() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);
    MIRA_CHECK(promote_via_evidence(runtime, rule, slash_evidence(101), 2, 0) == 0);
    MIRA_CHECK(runtime.activate("combat").has_value());
    const std::size_t step_event_baseline = sink.events.size();

    // Empty entity list: nothing can anchor, so the rule cannot match and
    // nothing is emitted (even with satisfying facts on the world).
    const auto none = runtime.step(make_world({}, {make_fact("distance", 2.0)}), make_tick(1));
    MIRA_CHECK(none.has_value() && none.value().empty());
    MIRA_CHECK(sink.events.size() == step_event_baseline);

    // Entities present but none satisfying the entity-scoped conditions.
    const auto idle_world =
        runtime.step(make_world({make_entity("enemy_1", "idle", 0.1, 0.5, "dataset://m26/idle")},
                                {make_fact("distance", 2.0)}),
                     make_tick(2));
    MIRA_CHECK(idle_world.has_value() && idle_world.value().empty());
    MIRA_CHECK(sink.events.size() == step_event_baseline);

    // Sanity: the satisfying anchor fires on the next tick.
    const auto match = runtime.step(slash_world(0.8, 2.0), make_tick(3));
    MIRA_CHECK(match.has_value() && match.value().size() == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// T1-G2: frame rule_id classification and frame tick semantics.
// ---------------------------------------------------------------------------

int frame_rule_id_and_tick_per_event_type() {
    RecordingSink sink;
    ScopedRuntime scoped{sink};
    auto &runtime = scoped.runtime;
    const auto rule = slash_rule(1);

    // Lifecycle before any successful step: frame tick 0 for every event.
    MIRA_CHECK(runtime.adopt_candidate_rules(std::span<const ReactiveRule>{&rule, 1}).has_value());
    const auto evidence = slash_evidence(201);
    MIRA_CHECK(runtime
                   .test_candidate_rules(
                       std::span<const PolicyEpisodeSample>(evidence.data(), evidence.size()))
                   .has_value());
    MIRA_CHECK(runtime.promote_rule(rule.rule_id).has_value());
    MIRA_CHECK(runtime.activate("combat").has_value());
    MIRA_CHECK(sink.events.size() == 4); // induced + testing + promoted + activated
    for (const auto &event : sink.events) {
        MIRA_CHECK(event.tick == 0);
    }
    MIRA_CHECK(sink.count(PolicyEventType::RuleCandidateInduced) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RuleTestingResulted) == 1);
    MIRA_CHECK(sink.count(PolicyEventType::RulePromoted) == 1);
    for (const auto &event : sink.events) {
        const auto &entry = schema_entry(event.type);
        if (entry.frame_carries_rule_id) {
            MIRA_CHECK(event.rule_id == rule.rule_id);
        } else {
            MIRA_CHECK(event.rule_id.empty());
        }
    }

    // Step events carry the current tick.
    const auto actions = runtime.step(slash_world(0.8, 2.0), make_tick(7));
    MIRA_CHECK(actions.has_value() && actions.value().size() == 1);
    const auto *triggered = sink.single(PolicyEventType::RuleTriggered);
    MIRA_CHECK(triggered != nullptr && triggered->tick == 7);

    // Lifecycle events carry the last successful step tick.
    MIRA_CHECK(runtime.demote_rule(rule.rule_id, "conflict").has_value());
    const auto demotions = sink.of_type(PolicyEventType::RuleDemoted);
    MIRA_CHECK(demotions.size() == 1 && demotions[0].tick == 7);
    MIRA_CHECK(runtime.deactivate("combat").has_value());
    const auto deactivated = sink.single(PolicyEventType::PolicyDeactivated);
    MIRA_CHECK(deactivated != nullptr && deactivated->tick == 7);

    // A rejected step does not advance the frame tick.
    MIRA_CHECK(expect_error(runtime.step(slash_world(0.8, 2.0), make_tick(6)),
                            TemporalPolicyDomainCode::TickNotMonotonic) == 0);
    MIRA_CHECK(runtime.activate("combat").has_value());
    const auto activated = sink.of_type(PolicyEventType::PolicyActivated);
    MIRA_CHECK(activated.size() == 2 && activated[1].tick == 7);
    // Evidence persists across demotion, so the rule can come back; the
    // promotion frame still reports the last successful step tick.
    MIRA_CHECK(runtime.promote_rule(rule.rule_id).has_value());
    const auto promoted = sink.of_type(PolicyEventType::RulePromoted);
    MIRA_CHECK(promoted.size() == 2 && promoted[1].tick == 7);
    return 0;
}

} // namespace mira::m26

int main() {
    const struct {
        const char *name;
        int (*fn)();
    } cases[] = {
        // T1-G1
        {"history_ring_sequence_capacity_digest", mira::m26::history_ring_sequence_capacity_digest},
        {"options_and_input_validate_rejections", mira::m26::options_and_input_validate_rejections},
        {"rule_schema_fail_closed", mira::m26::rule_schema_fail_closed},
        {"rule_wire_json_round_trip_and_version_policy",
         mira::m26::rule_wire_json_round_trip_and_version_policy},
        {"error_domain_twelve_codes_triple_mapping",
         mira::m26::error_domain_twelve_codes_triple_mapping},
        {"event_schema_names_exact_nine", mira::m26::event_schema_names_exact_nine},
        {"event_payload_key_sets_frozen", mira::m26::event_payload_key_sets_frozen},
        {"source_digest_empty_on_reconstruction_path",
         mira::m26::source_digest_empty_on_reconstruction_path},
        // T1-G2
        {"inactive_state_zero_evaluation_zero_events",
         mira::m26::inactive_state_zero_evaluation_zero_events},
        {"tick_monotonic_rejection_zero_state_change",
         mira::m26::tick_monotonic_rejection_zero_state_change},
        {"match_order_fixed_priority_then_rule_id",
         mira::m26::match_order_fixed_priority_then_rule_id},
        {"single_match_triggers_actions_and_event",
         mira::m26::single_match_triggers_actions_and_event},
        {"conflict_fail_closed_zero_actions_full_demotion",
         mira::m26::conflict_fail_closed_zero_actions_full_demotion},
        {"activate_deactivate_idempotent_and_immediate",
         mira::m26::activate_deactivate_idempotent_and_immediate},
        {"adopt_bounds_fail_closed_whole_batch", mira::m26::adopt_bounds_fail_closed_whole_batch},
        {"fact_only_rule_matches_entity_key_empty",
         mira::m26::fact_only_rule_matches_entity_key_empty},
        {"entity_scoped_rule_empty_entities_zero_match",
         mira::m26::entity_scoped_rule_empty_entities_zero_match},
        {"frame_rule_id_and_tick_per_event_type", mira::m26::frame_rule_id_and_tick_per_event_type},
        // T1-G3 (m26_lifecycle.cpp)
        {"induction_anchor_is_first_motion_entity",
         mira::m26::induction_anchor_is_first_motion_entity},
        {"induction_below_min_support_zero_candidates",
         mira::m26::induction_below_min_support_zero_candidates},
        {"induction_excludes_unhandled_samples", mira::m26::induction_excludes_unhandled_samples},
        {"induced_rule_canonical_form_fields", mira::m26::induced_rule_canonical_form_fields},
        {"induction_idempotent_byte_identical", mira::m26::induction_idempotent_byte_identical},
        {"adopt_idempotent_and_event_at_adopt_point",
         mira::m26::adopt_idempotent_and_event_at_adopt_point},
        {"testing_isolated_per_rule_counts", mira::m26::testing_isolated_per_rule_counts},
        {"promote_evidence_gate_fail_closed", mira::m26::promote_evidence_gate_fail_closed},
        {"demote_transition_matrix", mira::m26::demote_transition_matrix},
        {"retire_transition_matrix_and_terminal_idempotence",
         mira::m26::retire_transition_matrix_and_terminal_idempotence},
    };
    for (const auto &entry : cases) {
        if (const int code = entry.fn(); code != 0) {
            std::cerr << "m26_temporal_policy_test: case failed: " << entry.name << '\n';
            return code;
        }
    }
    std::cout << "m26_temporal_policy_test: OK (" << sizeof(cases) / sizeof(cases[0])
              << " cases)\n";
    return 0;
}
