#pragma once

// Mira Temporal Policy Stage T1 (DEC-037; milestone plan
// docs/plans/m26-temporal-policy-stage-t1.md §4 frozen contract, 2026-09-24).
//
// Deterministic conditional-policy minimal closed loop, pure Core: bounded
// observation ring (TemporalHistory), current-tick world view inputs
// (TrackedEntity/PolicyWorldView), the ReactiveRule schema v1 wire contract
// (mira.policy.rule.v1, DEC-002) and the ReactivePolicyRuntime evaluation +
// host-explicit induction/adopt/test/promote/demote/retire lifecycle with the
// nine-versioned-event T1 subset.
//
// Frozen invariants (milestone §4; violations are contract errors):
//   - the runtime never reads the system clock; every Timestamp comes from
//     the caller-supplied deterministic dataset clock (§4.5);
//   - step/induce/adopt/test/promote/demote/retire are bounded synchronous
//     functions - no thread, timer or Executor registration is created here
//     (§4.5); orchestration belongs to the host;
//   - rule sets, activation state, per-entity history accumulators and the
//     latest testing evidence are owned solely by the runtime instance; no
//     global or static mutable state (§4.4 Q2);
//   - failures are returned through Result/Error in the mira.temporal_policy
//     domain; nothing throws across the boundary;
//   - reactive rule triggers are not authorization (RULE-09; DEC-004
//     PolicyEngine boundary is unchanged).

#include <mira/core_contracts.hpp>
#include <mira/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Error domain (milestone §4.4 "错误域冻结面"; DEC-002 stable public values:
// the domain string, the explicit int32 code values and the stable names are
// frozen - renaming or renumbering any of them is a breaking change).
// ---------------------------------------------------------------------------

enum class TemporalPolicyDomainCode : std::int32_t {
    OptionsInvalid = 1,               // options/history capacity validate failure
    BoundsExceeded = 2,               // rule-set/per-state bounds (RULE-08)
    EntityInvalid = 3,                // TrackedEntity validation failure
    WorldViewInvalid = 4,             // PolicyWorldView validation failure
    TickNotMonotonic = 5,             // tick regression or repeat
    RuleSchemaInvalid = 6,            // rule schema/closed enums/provenance missing
    RuleUnknown = 7,                  // unknown rule_id
    RuleStateInvalid = 8,             // illegal lifecycle transition
    PromotionEvidenceMissing = 9,     // promotion evidence gate fail-closed
    EpisodeSamplesInvalid = 10,       // empty/out-of-bound batch input
    HistorySequenceNotAdvancing = 11, // history sequence not strictly advancing
    HistoryCapacityInvalid = 12,      // capacity == 0
};

[[nodiscard]] std::string temporal_policy_domain() noexcept; // "mira.temporal_policy"

// Stable member-name string for a domain code ("OptionsInvalid", ...).
[[nodiscard]] std::string temporal_policy_domain_code_name(TemporalPolicyDomainCode code);

// domain = temporal_policy_domain(); domain_code = static_cast<std::int32_t>(code);
// deterministic ErrorCode assignment (InvalidArgument for the schema/input
// family, InvalidState for the state/bounds family).
[[nodiscard]] Error make_temporal_policy_error(TemporalPolicyDomainCode code,
                                               std::string safe_message, bool retryable = false);

// ---------------------------------------------------------------------------
// TemporalHistory - bounded observation ring (RULE-07/RULE-08, §4.1).
// ---------------------------------------------------------------------------

struct TemporalHistoryOptions final {
    std::size_t capacity = 64;                   // RULE-08 documented default
    [[nodiscard]] Result<void> validate() const; // capacity > 0, else HistoryCapacityInvalid
};

struct TemporalHistoryEntry final {
    std::uint64_t sequence = 0; // strictly advancing per entity
    Timestamp sampled_at{};     // dataset clock; the runtime never reads the system clock
    double position_x = 0.0;
    double position_y = 0.0;
    std::string motion;        // versioned action label; empty = no motion
    double motion_phase = 0.0; // 0..1
    std::string source_ref;    // stable observation reference (payload never copied)
    std::string source_digest; // T1 frozen semantics: always empty (§4.1 review B2);
                               // populated only when T2+ input contracts carry digests
};

// Value-type component shared by the runtime accumulators and the rebuild
// projection. Append-only with FIFO eviction at capacity; digest() is a
// canonical JSON digest over the current entries (byte-identical across
// processes for equal content, T1-G4).
class TemporalHistory final {
  public:
    explicit TemporalHistory(TemporalHistoryOptions options = {});

    // sequence must be strictly greater than every sequence already appended
    // (HistorySequenceNotAdvancing otherwise); at capacity the oldest entry
    // is evicted FIFO.
    [[nodiscard]] Result<void> append(TemporalHistoryEntry entry);

    [[nodiscard]] std::span<const TemporalHistoryEntry> entries() const noexcept;
    [[nodiscard]] const TemporalHistoryEntry *latest() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] Sha256Digest digest() const;

  private:
    std::size_t capacity_;
    std::vector<TemporalHistoryEntry> entries_;
};

// ---------------------------------------------------------------------------
// Input contracts: current-tick entity snapshot and closed scalar world view
// (§4.2, T1-local; DEC-041 WorldState alignment happens in a later stage).
// ---------------------------------------------------------------------------

struct TrackedEntity final {
    std::string entity_key; // deterministic stable identifier
    double position_x = 0.0;
    double position_y = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    std::string motion;                          // empty = no motion
    double motion_phase = 0.0;                   // 0..1
    double motion_confidence = 0.0;              // 0..1
    std::string source_ref;                      // current-tick observation reference
    [[nodiscard]] Result<void> validate() const; // EntityInvalid
};

struct PolicyFact final { // closed scalar world fact (e.g. distance = 2.7)
    std::string key;
    double value = 0.0;
};

struct PolicyWorldView final {                   // current-tick deterministic world view
    std::vector<TrackedEntity> entities;         // order is the deterministic scan order
    std::vector<PolicyFact> facts;               // keys must be unique
    [[nodiscard]] Result<void> validate() const; // WorldViewInvalid / nested EntityInvalid
};

struct PolicyTickContext final {
    std::uint64_t tick = 0; // strictly monotonic per runtime instance
    Timestamp tick_time{};  // dataset deterministic clock
};

// ---------------------------------------------------------------------------
// ReactiveRule schema v1 (§4.3): closed predicate condition language, fixed
// order transitions, provenance-carrying (RULE-07), versioned wire schema
// mira.policy.rule.v1 (DEC-002).
// ---------------------------------------------------------------------------

enum class ReactiveRuleStatus : std::uint8_t { Candidate, Testing, Runtime, Retired };

struct RuleCondition final {
    enum class Field : std::uint8_t { Motion, MotionPhase, MotionConfidence, Fact };
    enum class Compare : std::uint8_t { Eq, Lt, Le, Gt, Ge };
    Field field = Field::Motion;
    Compare compare = Compare::Eq;
    std::string fact_key;      // Field::Fact only
    std::string text_value;    // string operand of Eq (Motion)
    double number_value = 0.0; // numeric compare operand
    // Motion/MotionPhase/MotionConfidence evaluate on the anchor entity;
    // Fact evaluates on the world facts (§4.4 step semantics).
};

struct PolicyAction final { // structured action label (T1: zero side effects)
    std::string kind;       // discrete action label (e.g. "dodge")
    std::string parameter;  // optional parameter (e.g. "left"); empty = none
    friend bool operator==(const PolicyAction &, const PolicyAction &) noexcept = default;
};

struct RuleTransition final {
    std::vector<RuleCondition> when;      // non-empty conjunction
    std::vector<PolicyAction> do_actions; // non-empty
};

struct ReactiveRule final {
    // "rr-" + first 16 hex chars of the semantic-content digest (state +
    // transitions); identical content derives an identical id across runs -
    // the basis of idempotent adoption. priority is NOT semantic content.
    std::string rule_id;
    SchemaVersion schema_version{1, 0};
    std::string state;          // host activation gate is per state
    std::uint32_t priority = 0; // match order primary key (ascending)
    ReactiveRuleStatus status = ReactiveRuleStatus::Candidate;
    std::vector<RuleTransition> transitions; // non-empty, bounded (RULE-08)
    std::vector<std::string> source_refs;    // sorted, deduplicated (RULE-07)
    std::uint32_t support_count = 0;         // repeated-verification evidence (RULE-10)

    // RuleSchemaInvalid on: unknown enums, empty when/do, condition or
    // transition counts over the schema bounds, missing source references.
    [[nodiscard]] Result<void> validate() const;

    [[nodiscard]] std::string to_json() const; // mira.policy.rule.v1 canonical JSON
    // DEC-002 version policy: {1,x} reads, older majors rejected, unknown
    // newer majors rejected.
    [[nodiscard]] static Result<ReactiveRule> from_json(std::string_view text);

    // Canonical digest over the semantic content (state + transitions).
    [[nodiscard]] Sha256Digest digest() const;
};

// ---------------------------------------------------------------------------
// Runtime options, events and lifecycle (§4.4).
// ---------------------------------------------------------------------------

struct PolicyRuntimeOptions final {
    std::size_t max_rules = 256; // RULE-08
    std::size_t max_rules_per_state = 32;
    std::size_t max_conditions_per_transition = 8;
    std::size_t max_transitions_per_rule = 8;
    std::uint32_t min_support = 3;      // induction evidence floor (RULE-10)
    std::uint32_t min_test_support = 2; // promotion pass floor
    // Sole construction source of the per-entity TemporalHistory accumulators;
    // rebuild_temporal_histories must be called with the same value for the
    // T1-G4 byte-identical guarantee (review B-A).
    TemporalHistoryOptions history_options{};
    [[nodiscard]] Result<void> validate() const; // zeroed bounds -> OptionsInvalid;
                                                 // folds history_options.validate()
};

enum class PolicyEventType : std::uint8_t {
    PolicyActivated,
    PolicyDeactivated,
    RuleCandidateInduced,
    RuleTestingResulted,
    RulePromoted,
    RuleDemoted,
    RuleConflicted,
    RuleTriggered,
    PolicyEscalatedToAgent,
}; // T1 nine-event subset; PolicyTransitioned belongs to Stage T5.

// Exact wire schema names of the nine frozen event types (§4.4 event table).
[[nodiscard]] std::string_view policy_event_schema_name(PolicyEventType type);

// Frame fields: type/tick/rule_id. rule_id is carried by single-rule events
// (RuleCandidateInduced/RuleTestingResulted/RulePromoted/RuleDemoted/
// RuleTriggered); PolicyActivated/PolicyDeactivated/PolicyEscalatedToAgent/
// RuleConflicted leave it empty. Frame tick is the largest tick the instance
// has seen: the current tick inside step, the last successful step tick for
// lifecycle events, 0 before the first successful step. Payloads are the
// canonical JSON of their frozen key sets and never repeat the frame fields.
struct PolicyEvent final {
    PolicyEventType type{};
    std::uint64_t tick = 0;
    std::string rule_id;
    std::string payload_json;
};

// Host-supplied synchronous inline callback. Contract: must not block and
// must not throw (T1 has no observer isolation layer - it arrives with the
// T2+ runtime integration surface).
class IPolicyEventSink {
  public:
    virtual ~IPolicyEventSink() = default;
    virtual void on_policy_event(const PolicyEvent &event) = 0;
};

struct PolicyEpisodeSample final { // induction/testing input (caller-owned, read-only)
    PolicyTickContext tick{};
    PolicyWorldView world{};
    std::string active_state;                 // activated policy state at this tick
    std::optional<PolicyAction> agent_action; // action taken by the (scripted) agent
    bool agent_handled = true;                // false = escalated/unhandled: never induces
};

struct RuleTestingReport final { // per-rule results; events already emitted
    struct RuleResult final {
        std::string rule_id;
        std::uint32_t passed_count = 0;        // triggered and action matched the record
        std::uint32_t false_trigger_count = 0; // triggered but action mismatched/unhandled
        std::uint32_t sample_count = 0;        // denominator: triggering samples only
    };
    std::vector<RuleResult> results;           // sorted by rule_id lexicographically
    [[nodiscard]] Sha256Digest digest() const; // cross-process stable
};

// Backend-neutral minimal surface (Stage T5 adds HSM/BT backends here).
class IPolicyRuntime {
  public:
    virtual ~IPolicyRuntime() = default;
    [[nodiscard]] virtual Result<void> activate(std::string_view state) = 0;
    [[nodiscard]] virtual Result<void> deactivate(std::string_view state) = 0;
    [[nodiscard]] virtual bool is_active(std::string_view state) const = 0;
    [[nodiscard]] virtual Result<std::vector<PolicyAction>> step(const PolicyWorldView &world,
                                                                 const PolicyTickContext &tick) = 0;
};

// Reference backend. Sole owner of the rule set (lifecycle states and latest
// testing evidence), the activation state set and the per-entity
// TemporalHistory accumulators. All operations are bounded synchronous
// functions driven by the caller; the runtime creates no thread, timer or
// Executor registration (§4.5).
class ReactivePolicyRuntime final : public IPolicyRuntime {
  public:
    explicit ReactivePolicyRuntime(PolicyRuntimeOptions options = {},
                                   IPolicyEventSink *sink = nullptr);

    // IPolicyRuntime (activation is host-explicit; idempotent NoOps succeed
    // silently; empty state names are RuleSchemaInvalid; deactivation takes
    // effect immediately without waiting for rule consent, §9 design).
    [[nodiscard]] Result<void> activate(std::string_view state) override;
    [[nodiscard]] Result<void> deactivate(std::string_view state) override;
    [[nodiscard]] bool is_active(std::string_view state) const override;
    [[nodiscard]] Result<std::vector<PolicyAction>> step(const PolicyWorldView &world,
                                                         const PolicyTickContext &tick) override;

    // Pure computation: groups handled, anchor-carrying samples by exact
    // signature and emits the canonical self-covering candidate form for
    // signatures reaching min_support. Emits no events; repeated induction
    // over the same log is byte-identical (T1-G4).
    [[nodiscard]] Result<std::vector<ReactiveRule>>
    induce_candidate_rules(std::span<const PolicyEpisodeSample> episodes);

    // Per-rule validate + bounds, then whole-batch insert as Candidate.
    // Duplicate rule_id (any status) is an idempotent skip; exceeding
    // max_rules/max_rules_per_state rejects the whole batch with
    // BoundsExceeded and zero changes (fail-closed). Emits one
    // RuleCandidateInduced per actually adopted rule at this adopt point.
    [[nodiscard]] Result<std::size_t> adopt_candidate_rules(std::span<const ReactiveRule> rules);

    // Isolated per-rule evaluation over every Candidate rule; per rule one
    // RuleTestingResulted event; the report row becomes the rule's latest
    // testing evidence.
    [[nodiscard]] Result<RuleTestingReport>
    test_candidate_rules(std::span<const PolicyEpisodeSample> episodes);

    // Candidate -> Runtime, gated on latest evidence: false_trigger_count == 0
    // and passed_count >= min_test_support, else PromotionEvidenceMissing.
    [[nodiscard]] Result<void> promote_rule(std::string_view rule_id);

    // Runtime -> Candidate for reason "conflict" | "host-explicit" (unknown
    // reason: RuleSchemaInvalid; non-Runtime origin: RuleStateInvalid).
    [[nodiscard]] Result<void> demote_rule(std::string_view rule_id, std::string_view reason);

    // Runtime|Candidate -> Retired (terminal). Re-retiring a Retired rule is
    // an idempotent NoOp with zero events and zero state change.
    [[nodiscard]] Result<void> retire_rule(std::string_view rule_id);

    // Additive read-only observation of the per-entity step() accumulator
    // (DEC-002 additive evolution of this v1 surface; milestone §9 record).
    // Returns nullptr when the entity has no accumulated history. Enables the
    // runtime-accumulator leg of the T1-G4 digest comparison.
    [[nodiscard]] const TemporalHistory *
    temporal_history(std::string_view entity_key) const noexcept;

  private:
    struct Match final {
        const ReactiveRule *rule;
        const RuleTransition *transition;
        const TrackedEntity *anchor;
    };

    [[nodiscard]] const ReactiveRule *find_rule(std::string_view rule_id) const noexcept;
    void emit(PolicyEventType type, std::string rule_id, std::string payload_json,
              std::uint64_t tick);
    [[nodiscard]] Result<void> append_history(const TrackedEntity &entity, Timestamp sampled_at);
    [[nodiscard]] std::vector<Match> collect_matches(const PolicyWorldView &world) const;
    [[nodiscard]] bool transition_matches(const RuleTransition &transition,
                                          const PolicyWorldView &world,
                                          const TrackedEntity **anchor) const;

    PolicyRuntimeOptions options_;
    IPolicyEventSink *sink_;
    std::map<std::string, ReactiveRule> rules_;                     // by rule_id (lexicographic)
    std::map<std::string, RuleTestingReport::RuleResult> evidence_; // latest per rule
    std::map<std::string, TemporalHistory> histories_;
    std::map<std::string, std::uint64_t> next_sequence_;
    std::set<std::string> active_states_;
    std::uint64_t max_tick_ = 0; // largest tick accepted by step()
    bool has_stepped_ = false;   // frame tick is 0 before the first successful step
};

// Read-only rebuild projection: replays the episode samples in order into
// per-entity histories built with `options`. With the same
// PolicyRuntimeOptions::history_options as the runtime instance, the result
// is byte-identical to the runtime accumulators (T1-G4).
[[nodiscard]] Result<std::map<std::string, TemporalHistory>>
rebuild_temporal_histories(std::span<const PolicyEpisodeSample> episodes,
                           TemporalHistoryOptions options = {});

} // namespace mira
