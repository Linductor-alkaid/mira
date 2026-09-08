#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/workflow_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// App Model contract, confidence bookkeeping and the navigation planner
// (DEC-027/028, stage E): the UI state graph as a versioned public contract,
// deterministic confidence updates, and a pure-function planner over the
// graph. No perception happens here: UI state recognition is host-supplied
// (DEC-011 boundary).
// ---------------------------------------------------------------------------

// Deterministic App Model decode/validation error codes (DEC-027 §1).
enum class AppModelError : std::int32_t {
    UnknownField = 1,     // payload object carries a member outside its closed key set
    VersionMismatch = 2,  // unsupported schema_version
    LimitExceeded = 3,    // RULE-08 document/state/transition/string limits
    InvalidReference = 4, // transition from/to does not name a declared state
    DuplicateId = 5,      // repeated state or transition id
    InvalidShape = 6,     // empty ids, bad numbers, malformed action/guard/source
};

[[nodiscard]] Error make_app_model_error(AppModelError code, std::string detail);

// Deterministic navigation error codes (DEC-028 §1).
enum class NavigationError : std::int32_t {
    UnknownFromState = 1,
    UnknownToState = 2,
    NoPath = 3,
    BudgetExceeded = 4,
};

[[nodiscard]] Error make_navigation_error(NavigationError code, std::string detail);

// RULE-08 limits enforced while decoding and validating an App Model document.
struct AppModelLimits final {
    std::size_t max_document_bytes = 512 * 1024;
    std::size_t max_states = 1024;
    std::size_t max_transitions = 4096;
    std::size_t max_string_bytes = 8 * 1024;
    std::size_t max_summary_bytes = 2048;
    std::size_t max_arguments_bytes = 64 * 1024;
};

extern const AppModelLimits kDefaultAppModelLimits;

// Provenance of an App Model fact (DEC-027 §2). A closed set: projections
// rebuilt from events must be able to validate sources. Source is a labeling,
// never an authorization (RULE-09).
enum class AppModelSource : std::uint8_t { Host, Agent, Trajectory };

[[nodiscard]] std::string app_model_source_name(AppModelSource source);
[[nodiscard]] Result<AppModelSource> parse_app_model_source(std::string_view name);

// Confidence bookkeeping for one state or transition (DEC-027 §2). All
// updates are pure functions: the clock is a caller-supplied parameter and
// equal inputs yield equal outputs, so the projection is rebuildable from
// events (W-03).
struct ConfidenceRecord final {
    double confidence = 0.5; // [0,1]
    std::uint64_t observed_at_ms = 0;
    std::uint64_t last_verified_ms = 0;
    std::uint32_t verified_count = 0;
    std::uint32_t failure_count = 0;
    AppModelSource source = AppModelSource::Host;
    [[nodiscard]] bool operator==(const ConfidenceRecord &) const noexcept = default;
};

// Records one observed outcome (Laplace-smoothed ratio: with zero evidence a
// record starts and stays at 0.5; every observation moves it monotonically
// toward 1.0 on success or toward 0.0 on failure given a fixed counter pair).
[[nodiscard]] ConfidenceRecord note_transition_outcome(ConfidenceRecord record, bool success,
                                                       std::uint64_t now_ms);

// Exponential decay toward zero since the last verification (DEC-027 §2):
// confidence *= 0.5^(dt/half_life). Monotonically non-increasing, a no-op for
// dt <= 0 or half_life == 0, and never touches the counters or timestamps.
[[nodiscard]] ConfidenceRecord apply_confidence_decay(ConfidenceRecord record, std::uint64_t now_ms,
                                                      std::uint64_t half_life_ms);

// Exploration flag (DEC-027 §2): low confidence marks the fact for agent
// re-exploration; it never triggers behavior on its own.
[[nodiscard]] bool needs_exploration(const ConfidenceRecord &record, double threshold);

// Multi-dimensional transition costs (DEC-028 §1). Estimates, not
// measurements: no latency or optimality claim holds before target-platform
// calibration (RULE-10).
struct NavigationCosts final {
    double latency_ms = 0.0;          // >= 0
    double failure_probability = 0.0; // [0,1]
    double risk = 0.0;                // [0,1]
    double energy = 0.0;              // [0,1]
    double model_cost = 0.0;          // >= 0
    bool agent_required = false;
    bool vision_required = false;
    [[nodiscard]] bool operator==(const NavigationCosts &) const noexcept = default;
};

// One UI state node (DEC-027 §1): a hierarchical descriptor (page + modal
// state + navigation context) rather than a bare page name.
struct AppModelState final {
    std::string id;
    std::string page;
    std::optional<std::string> modal;
    std::optional<std::string> context;
    std::string summary;
    ConfidenceRecord confidence;
};

// One UI transition edge (DEC-027 §1): an action (the reserved "tool" member
// convention shared with ToolCall steps), an optional guard over the v1
// predicate signal set, costs and confidence.
struct AppModelTransition final {
    std::string id;
    std::string from_state;
    std::string to_state;
    JsonValue action; // object carrying the reserved "tool" string member
    std::optional<WorkflowPredicate> guard;
    NavigationCosts costs;
    ConfidenceRecord confidence;
};

// The App Model document (DEC-027 §1): versioned, content-addressed, a
// rebuildable projection (W-03). v1 is a single namespace per document; the
// multi-device/multi-environment question stays open (DEC-014 §17).
struct AppModel final {
    SchemaVersion schema_version{1, 0};
    std::string app_id;
    std::string name;
    std::string summary;
    std::vector<AppModelState> states;
    std::vector<AppModelTransition> transitions;
};

// Serialization and strict decoding; unknown fields, unsupported versions,
// dangling references, duplicate ids and limit violations fail closed. Round
// trips are lossless.
[[nodiscard]] JsonValue app_model_to_json(const AppModel &model);
[[nodiscard]] Result<AppModel>
app_model_from_json(const JsonValue &json, const AppModelLimits &limits = kDefaultAppModelLimits);
[[nodiscard]] Result<AppModel>
parse_app_model(std::string_view text, const AppModelLimits &limits = kDefaultAppModelLimits);

// Structural validation of an in-memory model with the same semantics as
// decoding (DEC-027 §1).
[[nodiscard]] Result<void>
validate_app_model(const AppModel &model, const AppModelLimits &limits = kDefaultAppModelLimits);

// Content-addressed identity: canonical JSON digest of the model.
[[nodiscard]] Sha256Digest app_model_digest(const AppModel &model);

// ---------------------------------------------------------------------------
// Navigation planner (DEC-028 §1, pure)
// ---------------------------------------------------------------------------

// Weight set for the linear cost composition (DEC-028 §1). Weights are
// configuration, never hardcoded policy; defaults are provisional until
// calibrated on a target platform (RULE-10).
struct NavigationCostProfile final {
    double latency = 1.0;
    double failure = 1.0;
    double model_cost = 1.0;
    double risk = 1.0;
    double energy = 1.0;

    // All weights must be finite and non-negative.
    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] double navigation_edge_cost(const NavigationCosts &costs,
                                          const NavigationCostProfile &profile);

struct NavigationPlanOptions final {
    // agent_required edges stay unusable for workflow navigation by default;
    // the flag is the host-side exploration escape hatch (DEC-028 §1).
    bool allow_agent_edges = false;
    std::size_t max_edge_evaluations = 4096; // RULE-08 search budget
    std::size_t max_path_edges = 64;         // RULE-08 path length cap
};

// One navigation plan (DEC-028 §1): the ordered transition ids, the state
// walk (from plus every edge target), the composed cost, guard exclusion
// counters (blocked vs unevaluable are reported separately for honesty) and
// a content digest over the transition id sequence.
struct NavigationPlan final {
    std::string from_state;
    std::string to_state;
    std::vector<std::string> transition_ids;
    std::vector<std::string> state_sequence;
    double total_cost = 0.0;
    std::size_t guards_blocked = 0;
    std::size_t guards_unevaluable = 0;
    Sha256Digest plan_digest{};
};

// Deterministic Dijkstra over the model (DEC-028 §1): costs are the weighted
// linear composition; equal-cost paths break ties by comparing edge
// sequences element-wise as (edge_cost, edge_id, to_state); guards evaluate
// against `predicate_context` (same signal reference set as run predicates)
// with NotSatisfied counted as blocked and NotEvaluable counted as
// unevaluable - both make the edge unusable. from == to yields an empty
// plan. Unknown endpoints, unreachable targets and exhausted budgets fail
// closed with the deterministic NavigationError codes.
[[nodiscard]] Result<NavigationPlan>
plan_navigation(const AppModel &model, const std::string &from_state, const std::string &to_state,
                const NavigationCostProfile &profile, const JsonValue &predicate_context,
                const NavigationPlanOptions &options = NavigationPlanOptions{});

// ---------------------------------------------------------------------------
// Host-supplied UI state recognition (DEC-027 §3)
// ---------------------------------------------------------------------------

// One host-side relocalization result. The host decides how the state was
// recognized (accessibility tree, OCR, VLM, a human); Mira Core only
// consumes the name.
struct ScreenStateSnapshot final {
    std::string state_id;
    std::uint64_t observed_at_ms = 0;
};

// The host boundary for screen state (DEC-027 §3): called synchronously on
// drive or caller threads, so it must be cheap and non-blocking. Returning
// nullopt means "no current reading" and every dependent decision fails
// closed.
using ScreenStateProvider = std::function<std::optional<ScreenStateSnapshot>()>;

} // namespace mira
