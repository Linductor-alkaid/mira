// M26 (DEC-037 Stage T1) temporal-policy implementation. Contract source of
// truth: docs/plans/m26-temporal-policy-stage-t1.md §4 (frozen 2026-09-24).
// Every operation here is a bounded synchronous function driven by the
// caller: no thread, timer, Executor registration or system-clock read
// exists in this translation unit (§4.5).

#include <mira/temporal_policy.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace mira {
namespace {

using ConditionField = RuleCondition::Field;
using ConditionCompare = RuleCondition::Compare;

// Schema-level condition/transition bounds (the documented defaults; the
// per-runtime options bounds on the rule set are enforced at adoption).
constexpr std::size_t kMaxConditionsPerTransition = 8;
constexpr std::size_t kMaxTransitionsPerRule = 8;

constexpr std::string_view kFieldNames[] = {"Motion", "MotionPhase", "MotionConfidence", "Fact"};
constexpr std::string_view kCompareNames[] = {"Eq", "Lt", "Le", "Gt", "Ge"};
constexpr std::string_view kStatusNames[] = {"Candidate", "Testing", "Runtime", "Retired"};

bool parse_enum_name(std::string_view text, const std::string_view *names, std::size_t count,
                     std::uint8_t &out) {
    for (std::size_t index = 0; index < count; ++index) {
        if (text == names[index]) {
            out = static_cast<std::uint8_t>(index);
            return true;
        }
    }
    return false;
}

bool compare_holds(ConditionCompare compare, double left, double right) {
    switch (compare) {
    case ConditionCompare::Eq:
        return left == right;
    case ConditionCompare::Lt:
        return left < right;
    case ConditionCompare::Le:
        return left <= right;
    case ConditionCompare::Gt:
        return left > right;
    case ConditionCompare::Ge:
        return left >= right;
    }
    return false;
}

const PolicyFact *find_fact(const PolicyWorldView &world, std::string_view key) {
    for (const auto &fact : world.facts) {
        if (fact.key == key) {
            return &fact;
        }
    }
    return nullptr;
}

// Induction anchor: the first entity in world order with a non-empty motion.
const TrackedEntity *find_anchor(const PolicyWorldView &world) {
    for (const auto &entity : world.entities) {
        if (!entity.motion.empty()) {
            return &entity;
        }
    }
    return nullptr;
}

// True when the condition holds for the entity; Motion supports the closed
// string equality only - the other compares have no string ordering in the
// frozen semantics and never match. Fact conditions are world-scoped and are
// handled by the matcher before entity scoping.
bool entity_condition_holds(const RuleCondition &condition, const TrackedEntity &entity) {
    switch (condition.field) {
    case ConditionField::Motion:
        return condition.compare == ConditionCompare::Eq && entity.motion == condition.text_value;
    case ConditionField::MotionPhase:
        return compare_holds(condition.compare, entity.motion_phase, condition.number_value);
    case ConditionField::MotionConfidence:
        return compare_holds(condition.compare, entity.motion_confidence, condition.number_value);
    case ConditionField::Fact:
        return false;
    }
    return false;
}

JsonValue condition_to_json(const RuleCondition &condition) {
    JsonValue::Object object;
    object.emplace_back(
        "compare",
        JsonValue(std::string(kCompareNames[static_cast<std::size_t>(condition.compare)])));
    object.emplace_back("fact_key", JsonValue(condition.fact_key));
    object.emplace_back(
        "field", JsonValue(std::string(kFieldNames[static_cast<std::size_t>(condition.field)])));
    object.emplace_back("number_value", JsonValue(condition.number_value));
    object.emplace_back("text_value", JsonValue(condition.text_value));
    return JsonValue(std::move(object));
}

Result<RuleCondition> condition_from_json(const JsonValue &value) {
    if (!value.is_object()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule condition must be an object");
    }
    RuleCondition condition;
    const auto *field = value.find("field");
    const auto *compare = value.find("compare");
    if (field == nullptr || !field->is_string() || compare == nullptr || !compare->is_string()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule condition field/compare missing");
    }
    std::uint8_t index = 0;
    if (!parse_enum_name(*field->as_string(), kFieldNames, 4, index)) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "unknown condition field");
    }
    condition.field = static_cast<ConditionField>(index);
    if (!parse_enum_name(*compare->as_string(), kCompareNames, 5, index)) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "unknown condition compare");
    }
    condition.compare = static_cast<ConditionCompare>(index);
    if (const auto *fact_key = value.find("fact_key");
        fact_key != nullptr && fact_key->is_string()) {
        condition.fact_key = *fact_key->as_string();
    }
    if (const auto *text = value.find("text_value"); text != nullptr && text->is_string()) {
        condition.text_value = *text->as_string();
    }
    if (const auto *number = value.find("number_value"); number != nullptr && number->is_number()) {
        condition.number_value = *number->as_number();
    }
    return condition;
}

JsonValue action_to_json(const PolicyAction &action) {
    JsonValue::Object object;
    object.emplace_back("kind", JsonValue(action.kind));
    object.emplace_back("parameter", JsonValue(action.parameter));
    return JsonValue(std::move(object));
}

Result<PolicyAction> action_from_json(const JsonValue &value) {
    if (!value.is_object()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule action must be an object");
    }
    PolicyAction action;
    const auto *kind = value.find("kind");
    if (kind == nullptr || !kind->is_string()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule action kind missing");
    }
    action.kind = *kind->as_string();
    if (const auto *parameter = value.find("parameter");
        parameter != nullptr && parameter->is_string()) {
        action.parameter = *parameter->as_string();
    }
    return action;
}

JsonValue transitions_to_json(const std::vector<RuleTransition> &transitions) {
    JsonValue::Array array;
    for (const auto &transition : transitions) {
        JsonValue::Array actions;
        for (const auto &action : transition.do_actions) {
            actions.emplace_back(action_to_json(action));
        }
        JsonValue::Array when;
        for (const auto &condition : transition.when) {
            when.emplace_back(condition_to_json(condition));
        }
        JsonValue::Object object;
        object.emplace_back("do_actions", JsonValue(std::move(actions)));
        object.emplace_back("when", JsonValue(std::move(when)));
        array.emplace_back(JsonValue(std::move(object)));
    }
    return JsonValue(std::move(array));
}

Result<std::vector<RuleTransition>> transitions_from_json(const JsonValue &value) {
    if (!value.is_array()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule transitions must be an array");
    }
    std::vector<RuleTransition> transitions;
    for (const auto &entry : *value.as_array()) {
        if (!entry.is_object()) {
            return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                              "rule transition must be an object");
        }
        const auto *when = entry.find("when");
        const auto *actions = entry.find("do_actions");
        if (when == nullptr || !when->is_array() || actions == nullptr || !actions->is_array()) {
            return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                              "rule transition when/do_actions missing");
        }
        RuleTransition transition;
        for (const auto &condition_json : *when->as_array()) {
            auto condition = condition_from_json(condition_json);
            if (!condition.has_value()) {
                return condition.error();
            }
            transition.when.push_back(std::move(condition.value()));
        }
        for (const auto &action_json : *actions->as_array()) {
            auto action = action_from_json(action_json);
            if (!action.has_value()) {
                return action.error();
            }
            transition.do_actions.push_back(std::move(action.value()));
        }
        transitions.push_back(std::move(transition));
    }
    return transitions;
}

// Deterministic induction signature: exact byte-level identity over
// (active_state, anchor motion, phase, confidence, fact key-value set in key
// order, agent action). Doubles enter through their raw bit patterns so the
// "exact repeated signature" premise is byte-precise.
void append_double_key(std::string &key, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    key.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
}

std::string induction_signature(const PolicyEpisodeSample &sample, const TrackedEntity &anchor) {
    std::vector<const PolicyFact *> facts;
    facts.reserve(sample.world.facts.size());
    for (const auto &fact : sample.world.facts) {
        facts.push_back(&fact);
    }
    std::sort(facts.begin(), facts.end(), [](const PolicyFact *left, const PolicyFact *right) {
        return left->key < right->key;
    });
    std::string key = "state=" + sample.active_state +
                      "\x1f"
                      "motion=" +
                      anchor.motion + '\x1f';
    append_double_key(key, anchor.motion_phase);
    append_double_key(key, anchor.motion_confidence);
    for (const auto *fact : facts) {
        key += "\x1f"
               "fact=" +
               fact->key + '=';
        append_double_key(key, fact->value);
    }
    key += "\x1f"
           "action=";
    if (sample.agent_action.has_value()) {
        key += sample.agent_action->kind + "/" + sample.agent_action->parameter;
    }
    return key;
}

} // namespace

// ---------------------------------------------------------------------------
// Error domain (§4.4 frozen triple mapping; DEC-002 stable public values).
// ---------------------------------------------------------------------------

std::string temporal_policy_domain() noexcept { return "mira.temporal_policy"; }

std::string temporal_policy_domain_code_name(TemporalPolicyDomainCode code) {
    static constexpr std::pair<TemporalPolicyDomainCode, std::string_view> kNames[] = {
        {TemporalPolicyDomainCode::OptionsInvalid, "OptionsInvalid"},
        {TemporalPolicyDomainCode::BoundsExceeded, "BoundsExceeded"},
        {TemporalPolicyDomainCode::EntityInvalid, "EntityInvalid"},
        {TemporalPolicyDomainCode::WorldViewInvalid, "WorldViewInvalid"},
        {TemporalPolicyDomainCode::TickNotMonotonic, "TickNotMonotonic"},
        {TemporalPolicyDomainCode::RuleSchemaInvalid, "RuleSchemaInvalid"},
        {TemporalPolicyDomainCode::RuleUnknown, "RuleUnknown"},
        {TemporalPolicyDomainCode::RuleStateInvalid, "RuleStateInvalid"},
        {TemporalPolicyDomainCode::PromotionEvidenceMissing, "PromotionEvidenceMissing"},
        {TemporalPolicyDomainCode::EpisodeSamplesInvalid, "EpisodeSamplesInvalid"},
        {TemporalPolicyDomainCode::HistorySequenceNotAdvancing, "HistorySequenceNotAdvancing"},
        {TemporalPolicyDomainCode::HistoryCapacityInvalid, "HistoryCapacityInvalid"},
    };
    for (const auto &entry : kNames) {
        if (entry.first == code) {
            return std::string(entry.second);
        }
    }
    return "Unknown";
}

Error make_temporal_policy_error(TemporalPolicyDomainCode code, std::string safe_message,
                                 bool retryable) {
    static constexpr TemporalPolicyDomainCode kInvalidArgument[] = {
        TemporalPolicyDomainCode::OptionsInvalid,
        TemporalPolicyDomainCode::EntityInvalid,
        TemporalPolicyDomainCode::WorldViewInvalid,
        TemporalPolicyDomainCode::RuleSchemaInvalid,
        TemporalPolicyDomainCode::RuleUnknown,
        TemporalPolicyDomainCode::EpisodeSamplesInvalid,
        TemporalPolicyDomainCode::HistoryCapacityInvalid,
    };
    Error error;
    error.code = ErrorCode::InvalidState;
    for (const auto candidate : kInvalidArgument) {
        if (candidate == code) {
            error.code = ErrorCode::InvalidArgument;
            break;
        }
    }
    error.domain = temporal_policy_domain();
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = retryable;
    error.safe_message = std::move(safe_message);
    return error;
}

// ---------------------------------------------------------------------------
// TemporalHistory: bounded FIFO ring with canonical digest (§4.1).
// ---------------------------------------------------------------------------

Result<void> TemporalHistoryOptions::validate() const {
    if (capacity == 0) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::HistoryCapacityInvalid,
                                          "history capacity must be positive");
    }
    return Result<void>();
}

TemporalHistory::TemporalHistory(TemporalHistoryOptions options) : capacity_(options.capacity) {}

Result<void> TemporalHistory::append(TemporalHistoryEntry entry) {
    if (capacity_ == 0) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::HistoryCapacityInvalid,
                                          "history capacity is zero");
    }
    if (!entries_.empty() && entry.sequence <= entries_.back().sequence) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::HistorySequenceNotAdvancing,
                                          "history sequence must strictly advance");
    }
    if (entries_.size() == capacity_) {
        entries_.erase(entries_.begin()); // FIFO eviction of the oldest entry
    }
    entries_.push_back(std::move(entry));
    return Result<void>();
}

std::span<const TemporalHistoryEntry> TemporalHistory::entries() const noexcept { return entries_; }

const TemporalHistoryEntry *TemporalHistory::latest() const noexcept {
    return entries_.empty() ? nullptr : &entries_.back();
}

std::size_t TemporalHistory::size() const noexcept { return entries_.size(); }

std::size_t TemporalHistory::capacity() const noexcept { return capacity_; }

Sha256Digest TemporalHistory::digest() const {
    JsonValue::Array array;
    array.reserve(entries_.size());
    for (const auto &entry : entries_) {
        JsonValue::Object object;
        object.emplace_back("motion", JsonValue(entry.motion));
        object.emplace_back("motion_phase", JsonValue(entry.motion_phase));
        object.emplace_back("position_x", JsonValue(entry.position_x));
        object.emplace_back("position_y", JsonValue(entry.position_y));
        object.emplace_back("sampled_at_monotonic_ns",
                            JsonValue(static_cast<std::int64_t>(
                                entry.sampled_at.monotonic.time_since_epoch().count())));
        object.emplace_back(
            "sampled_at_wall_ns",
            JsonValue(static_cast<std::int64_t>(entry.sampled_at.wall.time_since_epoch().count())));
        object.emplace_back("sequence", JsonValue(static_cast<std::int64_t>(entry.sequence)));
        object.emplace_back("source_digest", JsonValue(entry.source_digest));
        object.emplace_back("source_ref", JsonValue(entry.source_ref));
        array.emplace_back(JsonValue(std::move(object)));
    }
    return canonical_json_digest(JsonValue(std::move(array)));
}

// ---------------------------------------------------------------------------
// Input contract validation (§4.2).
// ---------------------------------------------------------------------------

Result<void> TrackedEntity::validate() const {
    const bool in_unit_range = motion_phase >= 0.0 && motion_phase <= 1.0 &&
                               motion_confidence >= 0.0 && motion_confidence <= 1.0;
    if (entity_key.empty() || !in_unit_range) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::EntityInvalid,
                                          "entity key empty or phase/confidence out of [0,1]");
    }
    return Result<void>();
}

Result<void> PolicyWorldView::validate() const {
    for (const auto &entity : entities) {
        if (auto valid = entity.validate(); !valid.has_value()) {
            return valid;
        }
    }
    for (std::size_t outer = 0; outer < facts.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < facts.size(); ++inner) {
            if (facts[outer].key == facts[inner].key) {
                return make_temporal_policy_error(TemporalPolicyDomainCode::WorldViewInvalid,
                                                  "duplicate world fact key");
            }
        }
    }
    return Result<void>();
}

Result<void> PolicyRuntimeOptions::validate() const {
    if (max_rules == 0 || max_rules_per_state == 0 || max_conditions_per_transition == 0 ||
        max_transitions_per_rule == 0 || min_support == 0 || min_test_support == 0) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::OptionsInvalid,
                                          "runtime options bounds must be positive");
    }
    return history_options.validate(); // the history options fold in (review B-A)
}

// ---------------------------------------------------------------------------
// ReactiveRule: validation, wire schema mira.policy.rule.v1, content digest.
// ---------------------------------------------------------------------------

Result<void> ReactiveRule::validate() const {
    if (status != ReactiveRuleStatus::Candidate && status != ReactiveRuleStatus::Testing &&
        status != ReactiveRuleStatus::Runtime && status != ReactiveRuleStatus::Retired) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "unknown rule status");
    }
    if (transitions.empty() || transitions.size() > kMaxTransitionsPerRule) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule transition count out of bounds");
    }
    if (source_refs.empty()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule provenance missing (RULE-07: no naked rules)");
    }
    for (const auto &transition : transitions) {
        if (transition.when.empty() || transition.do_actions.empty() ||
            transition.when.size() > kMaxConditionsPerTransition) {
            return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                              "rule when/do empty or out of bounds");
        }
        for (const auto &condition : transition.when) {
            if (static_cast<std::uint8_t>(condition.field) > 3) {
                return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                                  "unknown condition field");
            }
            if (static_cast<std::uint8_t>(condition.compare) > 4) {
                return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                                  "unknown condition compare");
            }
        }
    }
    return Result<void>();
}

Sha256Digest ReactiveRule::digest() const {
    JsonValue::Object object;
    object.emplace_back("state", JsonValue(state));
    object.emplace_back("transitions", transitions_to_json(transitions));
    return canonical_json_digest(JsonValue(std::move(object)));
}

std::string ReactiveRule::to_json() const {
    JsonValue::Object version;
    version.emplace_back("major", JsonValue(static_cast<std::int64_t>(schema_version.major)));
    version.emplace_back("minor", JsonValue(static_cast<std::int64_t>(schema_version.minor)));
    JsonValue::Array refs;
    for (const auto &ref : source_refs) {
        refs.emplace_back(JsonValue(ref));
    }
    JsonValue::Object object;
    object.emplace_back("priority", JsonValue(static_cast<std::int64_t>(priority)));
    object.emplace_back("rule_id", JsonValue(rule_id));
    object.emplace_back("schema_version", JsonValue(std::move(version)));
    object.emplace_back("source_refs", JsonValue(std::move(refs)));
    object.emplace_back("state", JsonValue(state));
    object.emplace_back("status",
                        JsonValue(std::string(kStatusNames[static_cast<std::size_t>(status)])));
    object.emplace_back("support_count", JsonValue(static_cast<std::int64_t>(support_count)));
    object.emplace_back("transitions", transitions_to_json(transitions));
    return canonical_json_string(JsonValue(std::move(object)));
}

Result<ReactiveRule> ReactiveRule::from_json(std::string_view text) {
    auto parsed = parse_json(text);
    if (!parsed.has_value() || !parsed.value().is_object()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule wire text does not parse");
    }
    const JsonValue &document = parsed.value();
    ReactiveRule rule;
    const auto *version = document.find("schema_version");
    const auto *major = version != nullptr ? version->find("major") : nullptr;
    const auto *minor = version != nullptr ? version->find("minor") : nullptr;
    if (major == nullptr || !major->is_integer() || minor == nullptr || !minor->is_integer()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule schema_version malformed");
    }
    // DEC-002: {1,x} reads; older and unknown newer majors are rejected.
    if (major->as_integer() != 1) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "unsupported rule wire major version");
    }
    rule.schema_version.major = static_cast<std::uint16_t>(major->as_integer().value());
    rule.schema_version.minor = static_cast<std::uint16_t>(minor->as_integer().value());
    const auto *rule_id = document.find("rule_id");
    const auto *state = document.find("state");
    const auto *priority = document.find("priority");
    const auto *status = document.find("status");
    const auto *support = document.find("support_count");
    const auto *refs = document.find("source_refs");
    const auto *transitions = document.find("transitions");
    if (rule_id == nullptr || !rule_id->is_string() || state == nullptr || !state->is_string() ||
        priority == nullptr || !priority->is_integer() || status == nullptr ||
        !status->is_string() || support == nullptr || !support->is_integer() || refs == nullptr ||
        !refs->is_array() || transitions == nullptr) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule wire fields missing");
    }
    std::uint8_t status_index = 0;
    if (!parse_enum_name(*status->as_string(), kStatusNames, 4, status_index)) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "unknown rule status");
    }
    rule.status = static_cast<ReactiveRuleStatus>(status_index);
    if (priority->as_integer() < 0 || support->as_integer() < 0 ||
        static_cast<std::uint64_t>(priority->as_integer().value()) > 0xFFFFFFFFULL ||
        static_cast<std::uint64_t>(support->as_integer().value()) > 0xFFFFFFFFULL) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "rule priority/support_count out of range");
    }
    rule.priority = static_cast<std::uint32_t>(priority->as_integer().value());
    rule.support_count = static_cast<std::uint32_t>(support->as_integer().value());
    rule.rule_id = *rule_id->as_string();
    rule.state = *state->as_string();
    for (const auto &ref : *refs->as_array()) {
        if (!ref.is_string()) {
            return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                              "rule source_refs must be strings");
        }
        rule.source_refs.push_back(*ref.as_string());
    }
    auto loaded = transitions_from_json(*transitions);
    if (!loaded.has_value()) {
        return loaded.error();
    }
    rule.transitions = std::move(loaded.value());
    if (auto valid = rule.validate(); !valid.has_value()) {
        return valid.error();
    }
    return rule;
}

// ---------------------------------------------------------------------------
// Frozen event schema names and report digest (§4.4 event table).
// ---------------------------------------------------------------------------

std::string_view policy_event_schema_name(PolicyEventType type) {
    switch (type) {
    case PolicyEventType::PolicyActivated:
        return "mira.policy.policy-activated.v1";
    case PolicyEventType::PolicyDeactivated:
        return "mira.policy.policy-deactivated.v1";
    case PolicyEventType::RuleCandidateInduced:
        return "mira.policy.rule-candidate-induced.v1";
    case PolicyEventType::RuleTestingResulted:
        return "mira.policy.rule-testing-resulted.v1";
    case PolicyEventType::RulePromoted:
        return "mira.policy.rule-promoted.v1";
    case PolicyEventType::RuleDemoted:
        return "mira.policy.rule-demoted.v1";
    case PolicyEventType::RuleConflicted:
        return "mira.policy.rule-conflicted.v1";
    case PolicyEventType::RuleTriggered:
        return "mira.policy.rule-triggered.v1";
    case PolicyEventType::PolicyEscalatedToAgent:
        return "mira.policy.policy-escalated-to-agent.v1";
    }
    return "";
}

Sha256Digest RuleTestingReport::digest() const {
    JsonValue::Array array;
    array.reserve(results.size());
    for (const auto &result : results) {
        JsonValue::Object object;
        object.emplace_back("false_trigger_count",
                            JsonValue(static_cast<std::int64_t>(result.false_trigger_count)));
        object.emplace_back("passed_count",
                            JsonValue(static_cast<std::int64_t>(result.passed_count)));
        object.emplace_back("rule_id", JsonValue(result.rule_id));
        object.emplace_back("sample_count",
                            JsonValue(static_cast<std::int64_t>(result.sample_count)));
        array.emplace_back(JsonValue(std::move(object)));
    }
    return canonical_json_digest(JsonValue(std::move(array)));
}

// ---------------------------------------------------------------------------
// ReactivePolicyRuntime: activation, step evaluation, lifecycle (§4.4).
// ---------------------------------------------------------------------------

ReactivePolicyRuntime::ReactivePolicyRuntime(PolicyRuntimeOptions options, IPolicyEventSink *sink)
    : options_(options), sink_(sink) {} // PolicyRuntimeOptions is trivially copyable

const ReactiveRule *ReactivePolicyRuntime::find_rule(std::string_view rule_id) const noexcept {
    const auto found = rules_.find(std::string(rule_id));
    return found == rules_.end() ? nullptr : &found->second;
}

void ReactivePolicyRuntime::emit(PolicyEventType type, std::string rule_id,
                                 std::string payload_json, std::uint64_t tick) {
    if (sink_ == nullptr) {
        return;
    }
    PolicyEvent event;
    event.type = type;
    event.tick = tick;
    event.rule_id = std::move(rule_id);
    event.payload_json = std::move(payload_json);
    sink_->on_policy_event(event); // synchronous inline callback, caller thread
}

bool ReactivePolicyRuntime::transition_matches(const RuleTransition &transition,
                                               const PolicyWorldView &world,
                                               const TrackedEntity **anchor) const {
    // Fact conditions evaluate on the world view, independent of any anchor.
    for (const auto &condition : transition.when) {
        if (condition.field != ConditionField::Fact) {
            continue;
        }
        const auto *fact = find_fact(world, condition.fact_key);
        if (fact == nullptr ||
            !compare_holds(condition.compare, fact->value, condition.number_value)) {
            return false;
        }
    }
    // Entity-scoped conditions need an anchor: the first entity in world
    // order satisfying all of them. A pure-Fact rule needs no anchor (§4.4).
    bool has_entity_scope = false;
    for (const auto &condition : transition.when) {
        if (condition.field != ConditionField::Fact) {
            has_entity_scope = true;
            break;
        }
    }
    if (!has_entity_scope) {
        *anchor = nullptr;
        return true;
    }
    for (const auto &entity : world.entities) {
        const bool satisfied = std::all_of(transition.when.begin(), transition.when.end(),
                                           [&entity](const RuleCondition &condition) {
                                               return condition.field == ConditionField::Fact ||
                                                      entity_condition_holds(condition, entity);
                                           });
        if (satisfied) {
            *anchor = &entity;
            return true;
        }
    }
    return false;
}

std::vector<ReactivePolicyRuntime::Match>
ReactivePolicyRuntime::collect_matches(const PolicyWorldView &world) const {
    std::vector<const ReactiveRule *> ordered;
    for (const auto &entry : rules_) {
        if (entry.second.status == ReactiveRuleStatus::Runtime &&
            active_states_.count(entry.second.state) != 0) {
            ordered.push_back(&entry.second);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ReactiveRule *left, const ReactiveRule *right) {
                  if (left->priority != right->priority) {
                      return left->priority < right->priority; // priority ascending
                  }
                  return left->rule_id < right->rule_id; // rule_id lexicographic tie-break
              });
    std::vector<Match> matches;
    for (const auto *rule : ordered) {
        for (const auto &transition : rule->transitions) {
            const TrackedEntity *anchor = nullptr;
            if (transition_matches(transition, world, &anchor)) {
                matches.push_back({rule, &transition, anchor});
                break; // one rule contributes at most one match
            }
        }
    }
    return matches;
}

Result<void> ReactivePolicyRuntime::append_history(const TrackedEntity &entity,
                                                   Timestamp sampled_at) {
    TemporalHistoryEntry entry;
    entry.sequence = ++next_sequence_[entity.entity_key]; // per-entity 1-based counter
    entry.sampled_at = sampled_at;
    entry.position_x = entity.position_x;
    entry.position_y = entity.position_y;
    entry.motion = entity.motion;
    entry.motion_phase = entity.motion_phase;
    entry.source_ref = entity.source_ref;
    // source_digest stays empty: T1 inputs carry no payload digest (§4.1 B2).
    auto position =
        histories_.emplace(entity.entity_key, TemporalHistory{options_.history_options});
    return position.first->second.append(std::move(entry));
}

Result<void> ReactivePolicyRuntime::activate(std::string_view state) {
    if (state.empty()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "activation state name must be non-empty");
    }
    if (active_states_.count(std::string(state)) != 0) {
        return Result<void>(); // idempotent NoOp stays silent
    }
    active_states_.emplace(std::string(state));
    JsonValue::Object payload;
    payload.emplace_back("state", JsonValue(std::string(state)));
    emit(PolicyEventType::PolicyActivated, "", canonical_json_string(JsonValue(std::move(payload))),
         max_tick_);
    return Result<void>();
}

Result<void> ReactivePolicyRuntime::deactivate(std::string_view state) {
    if (state.empty()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "deactivation state name must be non-empty");
    }
    if (active_states_.erase(std::string(state)) == 0) {
        return Result<void>(); // idempotent NoOp stays silent
    }
    JsonValue::Object payload;
    payload.emplace_back("state", JsonValue(std::string(state)));
    emit(PolicyEventType::PolicyDeactivated, "",
         canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    return Result<void>();
}

bool ReactivePolicyRuntime::is_active(std::string_view state) const {
    return active_states_.count(std::string(state)) != 0;
}

Result<std::vector<PolicyAction>> ReactivePolicyRuntime::step(const PolicyWorldView &world,
                                                              const PolicyTickContext &tick) {
    // (1) Validate inputs; rejections leave zero events and zero state change.
    if (auto valid = world.validate(); !valid.has_value()) {
        return Result<std::vector<PolicyAction>>(valid.error());
    }
    if (has_stepped_ && tick.tick <= max_tick_) {
        return Result<std::vector<PolicyAction>>(make_temporal_policy_error(
            TemporalPolicyDomainCode::TickNotMonotonic, "policy tick must strictly advance"));
    }
    // (2) Accumulate history for every entity, active or not (§4.4 audit and
    // rebuild completeness).
    for (const auto &entity : world.entities) {
        if (auto appended = append_history(entity, tick.tick_time); !appended.has_value()) {
            return Result<std::vector<PolicyAction>>(appended.error());
        }
    }
    max_tick_ = tick.tick;
    has_stepped_ = true;
    // (3) Fixed-order match collection over Runtime rules in active states.
    auto matches = collect_matches(world);
    if (matches.empty()) {
        return std::vector<PolicyAction>{}; // non-match is the normal quiet state
    }
    if (matches.size() == 1) {
        const auto &match = matches.front();
        JsonValue::Array actions;
        for (const auto &action : match.transition->do_actions) {
            actions.emplace_back(action_to_json(action));
        }
        JsonValue::Object payload;
        payload.emplace_back("actions", JsonValue(std::move(actions)));
        payload.emplace_back("entity_key",
                             JsonValue(match.anchor != nullptr ? match.anchor->entity_key : ""));
        payload.emplace_back("state", JsonValue(match.rule->state));
        emit(PolicyEventType::RuleTriggered, match.rule->rule_id,
             canonical_json_string(JsonValue(std::move(payload))), max_tick_);
        return match.transition->do_actions;
    }
    // Multi-match conflict: fail closed - zero actions, explicit events, all
    // matched rules demoted back to Candidate (design §7.1).
    std::vector<std::string> conflicted_ids;
    conflicted_ids.reserve(matches.size());
    for (const auto &match : matches) {
        conflicted_ids.push_back(match.rule->rule_id);
    }
    std::sort(conflicted_ids.begin(), conflicted_ids.end());
    const auto id_array = [&conflicted_ids]() {
        JsonValue::Array ids;
        for (const auto &id : conflicted_ids) {
            ids.emplace_back(JsonValue(id));
        }
        return ids;
    };
    JsonValue::Object conflict_payload;
    conflict_payload.emplace_back("matched_rule_ids", JsonValue(id_array()));
    conflict_payload.emplace_back("state", JsonValue(matches.front().rule->state));
    emit(PolicyEventType::RuleConflicted, "",
         canonical_json_string(JsonValue(std::move(conflict_payload))), max_tick_);
    JsonValue::Object escalation_payload;
    escalation_payload.emplace_back("matched_rule_ids", JsonValue(id_array()));
    escalation_payload.emplace_back("reason", JsonValue("conflict"));
    escalation_payload.emplace_back("state", JsonValue(matches.front().rule->state));
    emit(PolicyEventType::PolicyEscalatedToAgent, "",
         canonical_json_string(JsonValue(std::move(escalation_payload))), max_tick_);
    for (const auto &match : matches) {
        rules_.at(match.rule->rule_id).status = ReactiveRuleStatus::Candidate;
    }
    // Demotions are emitted in the frozen collection order (priority, then
    // rule_id); each carries the conflict reason.
    for (const auto &match : matches) {
        JsonValue::Object payload;
        payload.emplace_back("from_status", JsonValue("Runtime"));
        payload.emplace_back("reason", JsonValue("conflict"));
        payload.emplace_back("to_status", JsonValue("Candidate"));
        emit(PolicyEventType::RuleDemoted, match.rule->rule_id,
             canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    }
    return std::vector<PolicyAction>{};
}

Result<std::vector<ReactiveRule>>
ReactivePolicyRuntime::induce_candidate_rules(std::span<const PolicyEpisodeSample> episodes) {
    if (episodes.empty()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::EpisodeSamplesInvalid,
                                          "induction episode batch is empty");
    }
    for (const auto &sample : episodes) {
        if (auto valid = sample.world.validate(); !valid.has_value()) {
            return Result<std::vector<ReactiveRule>>(valid.error());
        }
    }
    // Group handled, anchor-carrying samples by exact signature.
    std::map<std::string, std::vector<const PolicyEpisodeSample *>> groups;
    for (const auto &sample : episodes) {
        if (!sample.agent_handled || !sample.agent_action.has_value()) {
            continue; // escalated/unhandled samples never participate
        }
        const auto *anchor = find_anchor(sample.world);
        if (anchor == nullptr) {
            continue;
        }
        groups[induction_signature(sample, *anchor)].push_back(&sample);
    }
    std::vector<ReactiveRule> candidates;
    for (const auto &group : groups) {
        if (group.second.size() < options_.min_support) {
            continue; // below the evidence floor nothing is induced (RULE-10)
        }
        // Canonical self-covering form: Motion Eq, MotionPhase Ge (support
        // minimum), MotionConfidence Ge (support minimum), per-fact-key Le
        // (support maximum) in key order - the support set always matches the
        // rule it induced (§4.4).
        const auto *representative = group.second.front();
        const auto *anchor = find_anchor(representative->world);
        double min_phase = anchor->motion_phase;
        double min_confidence = anchor->motion_confidence;
        std::map<std::string, double> fact_max;
        std::vector<std::string> refs;
        for (const auto *sample : group.second) {
            const auto *sample_anchor = find_anchor(sample->world);
            min_phase = std::min(min_phase, sample_anchor->motion_phase);
            min_confidence = std::min(min_confidence, sample_anchor->motion_confidence);
            for (const auto &fact : sample->world.facts) {
                auto positioned = fact_max.emplace(fact.key, fact.value);
                if (!positioned.second) {
                    positioned.first->second = std::max(positioned.first->second, fact.value);
                }
            }
            refs.push_back(sample_anchor->source_ref);
        }
        ReactiveRule rule;
        rule.schema_version = SchemaVersion{1, 0};
        rule.state = representative->active_state;
        rule.status = ReactiveRuleStatus::Candidate;
        RuleTransition transition;
        RuleCondition motion;
        motion.field = ConditionField::Motion;
        motion.compare = ConditionCompare::Eq;
        motion.text_value = anchor->motion;
        transition.when.push_back(std::move(motion));
        transition.when.push_back(
            RuleCondition{ConditionField::MotionPhase, ConditionCompare::Ge, "", "", min_phase});
        transition.when.push_back(RuleCondition{ConditionField::MotionConfidence,
                                                ConditionCompare::Ge, "", "", min_confidence});
        for (const auto &fact : fact_max) { // map iterates in key order
            transition.when.push_back(RuleCondition{ConditionField::Fact, ConditionCompare::Le,
                                                    fact.first, "", fact.second});
        }
        transition.do_actions.push_back(*representative->agent_action);
        rule.transitions.push_back(std::move(transition));
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        rule.source_refs = std::move(refs);
        rule.support_count = static_cast<std::uint32_t>(group.second.size());
        rule.rule_id = "rr-" + rule.digest().to_string().substr(0, 16);
        candidates.push_back(std::move(rule));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const ReactiveRule &left, const ReactiveRule &right) {
                  return left.rule_id < right.rule_id;
              });
    return candidates;
}

Result<std::size_t>
ReactivePolicyRuntime::adopt_candidate_rules(std::span<const ReactiveRule> rules) {
    // Phase 1: whole-batch validation and bounds accounting - any failure
    // rejects the batch with zero changes (fail-closed, RULE-08).
    std::vector<const ReactiveRule *> fresh;
    for (const auto &rule : rules) {
        if (auto valid = rule.validate(); !valid.has_value()) {
            return Result<std::size_t>(valid.error());
        }
        if (rules_.find(rule.rule_id) != rules_.end()) {
            continue; // idempotent skip for any known rule_id
        }
        if (std::find_if(fresh.begin(), fresh.end(), [&rule](const ReactiveRule *candidate) {
                return candidate->rule_id == rule.rule_id;
            }) != fresh.end()) {
            continue; // in-batch duplicate
        }
        fresh.push_back(&rule);
    }
    if (rules_.size() + fresh.size() > options_.max_rules) {
        return Result<std::size_t>(make_temporal_policy_error(
            TemporalPolicyDomainCode::BoundsExceeded, "rule set bound exceeded"));
    }
    std::map<std::string, std::size_t> per_state;
    for (const auto &entry : rules_) {
        ++per_state[entry.second.state];
    }
    for (const auto *rule : fresh) {
        ++per_state[rule->state];
    }
    for (const auto &state : per_state) {
        if (state.second > options_.max_rules_per_state) {
            return Result<std::size_t>(make_temporal_policy_error(
                TemporalPolicyDomainCode::BoundsExceeded, "per-state rule bound exceeded"));
        }
    }
    // Phase 2: insert as Candidate; emit exactly one induced event per
    // adoption at this adopt point.
    std::size_t adopted = 0;
    for (const auto *rule : fresh) {
        ReactiveRule stored = *rule;
        stored.status = ReactiveRuleStatus::Candidate;
        auto position = rules_.emplace(stored.rule_id, std::move(stored));
        if (!position.second) {
            continue;
        }
        ++adopted;
        JsonValue::Object payload;
        payload.emplace_back("source_ref_count", JsonValue(static_cast<std::int64_t>(
                                                     position.first->second.source_refs.size())));
        payload.emplace_back("state", JsonValue(position.first->second.state));
        payload.emplace_back("support_count", JsonValue(static_cast<std::int64_t>(
                                                  position.first->second.support_count)));
        emit(PolicyEventType::RuleCandidateInduced, position.first->second.rule_id,
             canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    }
    return adopted;
}

Result<RuleTestingReport>
ReactivePolicyRuntime::test_candidate_rules(std::span<const PolicyEpisodeSample> episodes) {
    if (episodes.empty()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::EpisodeSamplesInvalid,
                                          "testing episode batch is empty");
    }
    for (const auto &sample : episodes) {
        if (auto valid = sample.world.validate(); !valid.has_value()) {
            return Result<RuleTestingReport>(valid.error());
        }
    }
    RuleTestingReport report;
    for (const auto &entry : rules_) { // map order == rule_id lexicographic
        const ReactiveRule &rule = entry.second;
        if (rule.status != ReactiveRuleStatus::Candidate) {
            continue; // isolated evaluation of Candidates only
        }
        std::uint32_t passed = 0;
        std::uint32_t false_triggers = 0;
        std::uint32_t triggered = 0;
        for (const auto &sample : episodes) {
            const TrackedEntity *anchor = nullptr;
            const RuleTransition *matched = nullptr;
            for (const auto &transition : rule.transitions) {
                if (transition_matches(transition, sample.world, &anchor)) {
                    matched = &transition;
                    break;
                }
            }
            if (matched == nullptr) {
                continue; // non-matching samples enter no count
            }
            ++triggered;
            const bool consistent = sample.agent_handled && sample.agent_action.has_value() &&
                                    matched->do_actions.size() == 1 &&
                                    matched->do_actions.front() == *sample.agent_action;
            if (consistent) {
                ++passed;
            } else {
                ++false_triggers;
            }
        }
        RuleTestingReport::RuleResult row;
        row.rule_id = rule.rule_id;
        row.passed_count = passed;
        row.false_trigger_count = false_triggers;
        row.sample_count = triggered;
        report.results.push_back(row);
        evidence_[rule.rule_id] = row; // the rule's latest testing evidence
        JsonValue::Object payload;
        payload.emplace_back("false_trigger_count",
                             JsonValue(static_cast<std::int64_t>(false_triggers)));
        payload.emplace_back("passed_count", JsonValue(static_cast<std::int64_t>(passed)));
        payload.emplace_back("sample_count", JsonValue(static_cast<std::int64_t>(triggered)));
        emit(PolicyEventType::RuleTestingResulted, rule.rule_id,
             canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    }
    return report;
}

Result<void> ReactivePolicyRuntime::promote_rule(std::string_view rule_id) {
    const auto found = rules_.find(std::string(rule_id));
    if (found == rules_.end()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleUnknown,
                                          "promotion target rule unknown");
    }
    ReactiveRule &rule = found->second;
    if (rule.status != ReactiveRuleStatus::Candidate) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleStateInvalid,
                                          "promotion requires a Candidate rule");
    }
    const auto evidence = evidence_.find(rule.rule_id);
    if (evidence == evidence_.end() || evidence->second.false_trigger_count != 0 ||
        evidence->second.passed_count < options_.min_test_support) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::PromotionEvidenceMissing,
                                          "promotion evidence gate not satisfied");
    }
    rule.status = ReactiveRuleStatus::Runtime;
    JsonValue::Object payload;
    payload.emplace_back("passed_count",
                         JsonValue(static_cast<std::int64_t>(evidence->second.passed_count)));
    payload.emplace_back("state", JsonValue(rule.state));
    payload.emplace_back("support_count", JsonValue(static_cast<std::int64_t>(rule.support_count)));
    emit(PolicyEventType::RulePromoted, rule.rule_id,
         canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    return Result<void>();
}

Result<void> ReactivePolicyRuntime::demote_rule(std::string_view rule_id, std::string_view reason) {
    const auto found = rules_.find(std::string(rule_id));
    if (found == rules_.end()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleUnknown,
                                          "demotion target rule unknown");
    }
    if (reason != "conflict" && reason != "host-explicit") {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleSchemaInvalid,
                                          "demotion reason outside the frozen closed set");
    }
    ReactiveRule &rule = found->second;
    if (rule.status != ReactiveRuleStatus::Runtime) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleStateInvalid,
                                          "demotion requires a Runtime rule");
    }
    rule.status = ReactiveRuleStatus::Candidate;
    JsonValue::Object payload;
    payload.emplace_back("from_status", JsonValue("Runtime"));
    payload.emplace_back("reason", JsonValue(std::string(reason)));
    payload.emplace_back("to_status", JsonValue("Candidate"));
    emit(PolicyEventType::RuleDemoted, rule.rule_id,
         canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    return Result<void>();
}

Result<void> ReactivePolicyRuntime::retire_rule(std::string_view rule_id) {
    const auto found = rules_.find(std::string(rule_id));
    if (found == rules_.end()) {
        return make_temporal_policy_error(TemporalPolicyDomainCode::RuleUnknown,
                                          "retire target rule unknown");
    }
    ReactiveRule &rule = found->second;
    if (rule.status == ReactiveRuleStatus::Retired) {
        return Result<void>(); // terminal idempotent NoOp: zero events, zero change
    }
    const std::string from_status(kStatusNames[static_cast<std::size_t>(rule.status)]);
    rule.status = ReactiveRuleStatus::Retired;
    JsonValue::Object payload;
    payload.emplace_back("from_status", JsonValue(from_status));
    payload.emplace_back("reason", JsonValue("host-explicit"));
    payload.emplace_back("to_status", JsonValue("Retired"));
    emit(PolicyEventType::RuleDemoted, rule.rule_id,
         canonical_json_string(JsonValue(std::move(payload))), max_tick_);
    return Result<void>();
}

const TemporalHistory *
ReactivePolicyRuntime::temporal_history(std::string_view entity_key) const noexcept {
    const auto found = histories_.find(std::string(entity_key));
    return found == histories_.end() ? nullptr : &found->second;
}

// ---------------------------------------------------------------------------
// Read-only rebuild projection (§4.6, T1-G4).
// ---------------------------------------------------------------------------

Result<std::map<std::string, TemporalHistory>>
rebuild_temporal_histories(std::span<const PolicyEpisodeSample> episodes,
                           TemporalHistoryOptions options) {
    if (auto valid = options.validate(); !valid.has_value()) {
        return Result<std::map<std::string, TemporalHistory>>(valid.error());
    }
    if (episodes.empty()) {
        return Result<std::map<std::string, TemporalHistory>>(make_temporal_policy_error(
            TemporalPolicyDomainCode::EpisodeSamplesInvalid, "rebuild episode batch is empty"));
    }
    std::map<std::string, TemporalHistory> rebuilt;
    std::map<std::string, std::uint64_t> sequences;
    for (const auto &sample : episodes) {
        if (auto valid = sample.world.validate(); !valid.has_value()) {
            return Result<std::map<std::string, TemporalHistory>>(valid.error());
        }
        for (const auto &entity : sample.world.entities) {
            TemporalHistoryEntry entry;
            entry.sequence = ++sequences[entity.entity_key]; // matches the runtime accumulator
            entry.sampled_at = sample.tick.tick_time;
            entry.position_x = entity.position_x;
            entry.position_y = entity.position_y;
            entry.motion = entity.motion;
            entry.motion_phase = entity.motion_phase;
            entry.source_ref = entity.source_ref;
            entry.source_digest = ""; // T1 frozen semantics (§4.1 review B2)
            auto position = rebuilt.emplace(entity.entity_key, TemporalHistory{options});
            auto appended = position.first->second.append(std::move(entry));
            if (!appended.has_value()) {
                return Result<std::map<std::string, TemporalHistory>>(appended.error());
            }
        }
    }
    return rebuilt;
}

} // namespace mira
