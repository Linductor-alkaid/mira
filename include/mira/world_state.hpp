#pragma once

// Mira Session World State projection core (DEC-041 first stage; milestone
// plan docs/plans/m27-world-state-projection-core.md §4 frozen contract,
// 2026-09-28, including the §9 third-revision B1/B2 corrections).
//
// Deterministic, rebuildable session projection over the entity vocabulary
// (DEC-046; docs/design/entity_vocabulary_design.md): believed foreground app,
// believed page assumption (Believed/Stale/Unknown), bounded live-entity
// table, bounded recent-change ring, the mira.worldstate.v1 wire schema
// (DEC-002) and the mira.world_state error domain.
//
// Frozen invariants (milestone §4; violations are contract errors):
//   - pure-function update: current projection + input record + explicit time
//     -> new projection; nothing here reads the system clock (§4.1/§4.6);
//   - the six input records (five updates + the explicit expiry instruction)
//     plus the eight named operators below are the only write paths (§4.2/§4.3);
//   - WorldStateOptions is strategy, not state: the value type does not embed
//     it and the wire schema has no options key; every operator takes it as an
//     explicit const& and owns the capacity/summary invariants (§4.1);
//   - operator failures are strongly consistent: the projected input value is
//     left untouched and only an error is returned (§4.3);
//   - ConflictMarked has exactly one trigger: navigation from_state
//     reconciliation (projected page pair != claimed departure pair, §4.3
//     B1 revision); screen recognition and successful navigation are normal
//     belief updates recorded as PageAssumed/NavigationObserved entries;
//   - error-code folding (§4.5 B2 revision): validate()/from_json() nesting
//     failures - including vocabulary-class value illegality and
//     change_sequence regression - always fold to StateInvalid;
//     VocabularyViolation is produced only on the operator input path;
//   - zero Executor registration surface: every entry point is a bounded
//     synchronous function driven by the caller (§4.6); projection state is
//     not an authorization source (RULE-09; no capability/permission outlet).

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/observation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Error domain (§4.5 frozen face; DEC-002 stable public values: the domain
// string, the explicit int32 code values and the stable names are frozen).
// ---------------------------------------------------------------------------

enum class WorldStateDomainCode : std::int32_t {
    OptionsInvalid = 1,      // WorldStateOptions::validate failure (any capacity 0)
    StateInvalid = 2,        // WorldState::validate failure (structural invariants,
                             //   including all nested folds - §4.1 folding rule)
    RecordInvalid = 3,       // input record structurally illegal (numeric out of
                             //   range, disposition/outcome outside closed set,
                             //   element count over capacity)
    VocabularyViolation = 4, // out-of-vocabulary reference (operator input path
                             //   only): nil observation_id, invalid space, empty
                             //   state_id/app_id, empty or >256-byte tool_ref;
                             //   validate()/from_json() never produce this code -
                             //   nested vocabulary-class value failures fold to
                             //   StateInvalid (§4.1 folding rule)
    PayloadTooLarge = 5,     // summary exceeds max_change_summary_bytes
                             //   (rejected, never truncated)
    SchemaUnsupported = 6,   // from_json: major not inside the {1,x} support set
    PayloadMalformed = 7,    // from_json: parse failure, unknown member, missing
                             //   member, member of the wrong JSON type
    StaleTargetMissing = 8,  // apply_verification with an Unknown target page
};

[[nodiscard]] std::string_view world_state_domain_code_name(WorldStateDomainCode code);

// domain = "mira.world_state"; domain_code = static_cast<std::int32_t>(code);
// deterministic ErrorCode assignment along the make_context_error precedent.
[[nodiscard]] Error make_world_state_error(WorldStateDomainCode code, std::string detail);

// ---------------------------------------------------------------------------
// Options and projection value types (§4.1).
// ---------------------------------------------------------------------------

struct WorldStateOptions final {
    std::size_t max_entities = 128;               // RULE-08 documented default
    std::size_t max_recent_changes = 64;          // RULE-08 documented default
    std::size_t max_change_summary_bytes = 2048;  // kWorkflowEventMaxSummaryBytes aligned
    std::chrono::milliseconds stale_after{30000}; // staleness bound; provisional
                                                  //   default (§4.1 note; owner
                                                  //   Mira Maintainers, latest
                                                  //   freeze = consumption milestone)
    [[nodiscard]] Result<void> validate() const;  // any capacity 0 -> OptionsInvalid
};

enum class WorldBeliefStatus : std::uint8_t { Believed, Stale, Unknown };

// Foreground-app belief (entity vocabulary §2.1 AppContext domain).
struct WorldForegroundApp final {
    WorldBeliefStatus status = WorldBeliefStatus::Unknown;
    std::string package_name; // must be empty exactly when status == Unknown
    std::string activity_name;
    bool sensitive = false;
    Timestamp observed_at{}; // caller-supplied deterministic time
    [[nodiscard]] Result<void> validate() const;
    // Fails with StateInvalid (§4.1 folding rule) unless
    // status == Unknown <=> package_name is empty.
};

// Page-state assumption (entity vocabulary §2.2 (app_id, state_id) pair).
struct WorldPageAssumption final {
    WorldBeliefStatus status = WorldBeliefStatus::Unknown;
    std::string app_id;      // empty exactly when status == Unknown
    std::string state_id;    // same; non-empty with a non-empty app_id otherwise
    double confidence = 0.0; // [0,1]; always 0 when Unknown
    Timestamp recognized_at{};
    [[nodiscard]] Result<void> validate() const;
    // Fails with StateInvalid (§4.1 folding rule) unless status == Unknown <=>
    // app_id and state_id are empty and confidence == 0, and non-Unknown
    // statuses carry a non-empty pair with confidence inside [0,1].
};

// Live-entity entry (entity vocabulary §2.1 ElementRef; evidence, not identity).
struct WorldEntity final {
    ElementRef element;      // seven-field identity (observation.hpp:345-353)
    double confidence = 0.0; // [0,1]; 1.0 from the observation path
                             //   (provisional default, §4.1 note)
    bool stale = false;
    Timestamp last_seen_at{};
    Sha256Digest source_event_digest{}; // M27 frozen semantics: canonical JSON
                                        //   digest of the input record that
                                        //   produced/refreshed this entry (§4.3
                                        //   note; envelope form arrives with the
                                        //   consumption milestone, schema unchanged)
    [[nodiscard]] Result<void> validate() const;
    // Fails with StateInvalid (§4.1 folding rule, validate never distinguishes
    // vocabulary semantics - §4.5) when element.observation_id is nil, when
    // element.space is invalid (nil), when element.source is outside the
    // ElementSource closed set, or when confidence is outside [0,1].
};

enum class WorldChangeKind : std::uint8_t {
    PageAssumed,
    EntityObserved,
    EntityStaled,
    NavigationObserved,
    ToolSettled,
    VerificationSettled,
    ConflictMarked,
    EntityEvicted,
};

// Recent-change ring entry.
struct WorldChange final {
    WorldChangeKind kind = WorldChangeKind::PageAssumed;
    std::string summary;                // operator-generated, bounded by
                                        //   max_change_summary_bytes; content
                                        //   originates from pre-sanitized
                                        //   caller inputs (never truncated)
    Sha256Digest source_event_digest{}; // M27 frozen semantics as above
    Timestamp changed_at{};
    std::uint64_t change_sequence = 0; // strictly monotonic within a projection
                                       //   instance; regression -> StateInvalid
                                       //   via validate() (B2 revision: the six
                                       //   input records never carry this field,
                                       //   so the operator write path cannot
                                       //   regress it; RecordInvalid is
                                       //   unreachable for this trigger)
};

struct WorldState final {
    SchemaVersion schema_version{1, 0};
    WorldForegroundApp foreground_app;
    WorldPageAssumption current_page;
    std::vector<WorldEntity> entities;
    std::vector<WorldChange> recent_changes;

    [[nodiscard]] Result<void> validate() const;
    // Structural value invariants only (§4.1): nested validates, belief/kind
    // enum ranges and strictly increasing change_sequence. Capacity and
    // summary bounds are operator invariants, not value invariants. Every
    // nested failure folds to StateInvalid (§4.1/§4.5 folding rule).

    [[nodiscard]] std::string to_json() const; // mira.worldstate.v1 canonical JSON
    [[nodiscard]] static Result<WorldState> from_json(std::string_view text);
    // DEC-002 version policy (§4.4): {1,x} read; older majors ({0,x}) and
    // unknown newer majors ({2,x} and beyond) -> SchemaUnsupported; JSON parse
    // failure, unknown member, missing member or a member of the wrong JSON
    // type -> PayloadMalformed; structurally legal member with an illegal
    // value (including every nested fold) -> StateInvalid.

    [[nodiscard]] Sha256Digest digest() const; // canonical JSON digest; the wire
                                               //   options-free form makes it
                                               //   byte-stable across processes
};

// ---------------------------------------------------------------------------
// Input records - the only update entry point (§4.2 closed set).
// ---------------------------------------------------------------------------

struct WorldObservationInput final {
    ObservationId observation_id;
    EnvironmentEpoch environment_epoch = 0;
    AppContext app;                   // empty package_name = no foreground supply
    std::vector<ElementRef> elements; // size <= options.max_entities
    Timestamp observed_at{};
};

struct WorldScreenStateInput final { // host recognition (DEC-027 §3
                                     //   ScreenStateSnapshot shape note)
    std::string app_id;              // non-empty (vocabulary §2.2)
    std::string state_id;            // vocabulary §2.2; empty -> violation
    double confidence = 0.0;         // [0,1]
    Timestamp recognized_at{};
};

struct WorldNavigationInput final { // mira.workflow.navigation-observed.v1
                                    //   shape note (same-type, not the
                                    //   workflow contract type)
    std::string app_id;             // navigation app domain (vocabulary
                                    //   §2.2 pair head; empty -> violation)
    std::string from_state;         // vocabulary §2.2
    std::string to_state;           // vocabulary §2.2
    std::string transition_id;
    bool success = false;
    double confidence = 0.0; // [0,1]
    Timestamp observed_at{};
};

struct WorldToolSettledInput final {    // tool settlement (vocabulary §2.3)
    std::string tool_ref;               // toolref: v1 canonical shape (core
                                        //   performs the frozen registry-level
                                        //   check only: non-empty and <= 256
                                        //   bytes; syntax authority stays in
                                        //   the workflow module's
                                        //   parse_tool_reference)
    std::string disposition;            // closed set completed|skipped|failed|stale
    Sha256Digest source_event_digest{}; // carried by the input record and
                                        //   covered by the record digest below
    Timestamp settled_at{};
};

struct WorldVerificationInput final { // verification settlement (first-stage
                                      //   scope = page assumption only)
    std::string outcome;              // closed set confirmed|refuted|inconclusive
    double confidence = 0.0;          // [0,1]; new confidence for
                                      //   confirmed/refuted
    Timestamp verified_at{};
};

struct WorldExpiryInput final { // explicit expiry instruction (the
                                //   replayable form of expire_stale)
    Timestamp now{};            // staleness decision instant (caller
                                //   deterministic time source)
};

using WorldStateInput =
    std::variant<WorldObservationInput, WorldScreenStateInput, WorldNavigationInput,
                 WorldToolSettledInput, WorldVerificationInput, WorldExpiryInput>;

// ---------------------------------------------------------------------------
// Update operators (§4.3 frozen set: pure functions, exactly these eight).
// Every Result-returning operator validates options first (OptionsInvalid),
// then the input record; time comes only from inputs - never from a clock.
// Each entry's source_event_digest is the canonical JSON digest of the input
// record that produced it (§4.3 M27 frozen semantics).
// ---------------------------------------------------------------------------

// Foreground belief from the observation AppContext (non-empty package ->
// Believed with package/activity/sensitive/observed_at overwritten
// field-by-field on every observation - a same-content later observation
// still refreshes observed_at; empty -> reset to Unknown); per-element
// seven-field lookup refreshes (confidence -> 1.0, last_seen_at/stale/source
// digest refreshed) or inserts (confidence 1.0) entities; insert overflow
// runs the frozen eviction order first. Ring: one PageAssumed entry when the
// foreground belief content changed (the comparison ignores observed_at), one
// EntityObserved entry when entities were added or refreshed (counts in the
// summary).
[[nodiscard]] Result<WorldState> apply_world_observation(WorldState state,
                                                         const WorldObservationInput &input,
                                                         const WorldStateOptions &options);

// Host recognition is authoritative new evidence: current_page becomes Believed
// with the input pair and recognized_at = input.recognized_at, recorded as one
// PageAssumed entry. Never records a conflict (the sole ConflictMarked trigger
// is the navigation from_state reconciliation).
[[nodiscard]] Result<WorldState> apply_screen_state(WorldState state,
                                                    const WorldScreenStateInput &input,
                                                    const WorldStateOptions &options);

// from_state reconciliation (the only ConflictMarked trigger): when the
// projected page is not Unknown and its (app_id, state_id) pair differs from
// (input.app_id, input.from_state), one ConflictMarked entry is appended (the
// summary carries the projected and the claimed pair); reconciliation never
// changes the transition semantics. success -> Believed with the input pair and
// recognized_at = input.observed_at; failure -> Stale with confidence and
// recognized_at untouched (staleness keeps counting from the original
// recognition), or no state change when currently Unknown. Either way one
// NavigationObserved entry is appended.
[[nodiscard]] Result<WorldState> apply_navigation(WorldState state,
                                                  const WorldNavigationInput &input,
                                                  const WorldStateOptions &options);

// Appends one ToolSettled entry only (v1 scope: no foreground/page/entity
// change). tool_ref gets the frozen registry-level check (non-empty, <= 256
// bytes, vocabulary §2.3) -> VocabularyViolation; disposition outside
// completed|skipped|failed|stale -> RecordInvalid.
[[nodiscard]] Result<WorldState> apply_tool_settled(WorldState state,
                                                    const WorldToolSettledInput &input,
                                                    const WorldStateOptions &options);

// Page-assumption settlement. Unknown target -> StaleTargetMissing (explicit
// failure, never a silent insert). confirmed -> Believed, refuted -> Stale,
// both with confidence = input.confidence and recognized_at = input.verified_at
// (a freshly settled assumption must not be killed by the existing staleness
// bound); inconclusive changes nothing. All three append one
// VerificationSettled entry.
[[nodiscard]] Result<WorldState> apply_verification(WorldState state,
                                                    const WorldVerificationInput &input,
                                                    const WorldStateOptions &options);

// Entities whose last_seen_at (page: recognized_at) is more than
// options.stale_after behind input.now (Timestamp::wall difference) and that
// are not Stale yet: become Stale with confidence halved (already-Stale
// entries never decay twice; an Unknown page holds no belief and is never
// expired). One EntityStaled entry per staled entity, then one for the page.
[[nodiscard]] Result<WorldState> apply_expiry(WorldState state, const WorldExpiryInput &input,
                                              const WorldStateOptions &options);

// Named implementation of the frozen eviction order and audit convenience
// entry: while entities.size() > options.max_entities, evict
// stale-first -> earliest last_seen_at -> lowest confidence -> ElementRef
// canonical JSON lexicographic ascending; one EntityEvicted entry per
// eviction. No-op within capacity. NOT part of the rebuild recipe (§4.3):
// insert-overflow eviction happens inside apply_world_observation with the
// same frozen order; a standalone prune call is outside the WS-G3 equivalence
// assertion. Total function (returns a value, cannot fail); its fixed-width
// eviction summaries are the one place the summary bound is not enforced.
[[nodiscard]] WorldState prune_entities(WorldState state, const WorldStateOptions &options);

// Replay rebuild (§4.3): validate options, then fold the input sequence in
// order through the operators above (five update records through their
// apply_*, WorldExpiryInput through apply_expiry; insert-overflow eviction
// happens inside the observation operator). Empty input yields the legal
// default WorldState. With the same options, the result is byte-identical
// (digest()) to the equivalent incremental advance (WS-G3).
[[nodiscard]] Result<WorldState> rebuild_world_state(std::span<const WorldStateInput> inputs,
                                                     const WorldStateOptions &options);

} // namespace mira
