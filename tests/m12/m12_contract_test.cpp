// M12-01/M12-02 (contract half): the App Model contract (DEC-027) and the
// confidence pure functions. JSON round trips are lossless, structural
// violations fail closed with deterministic codes, digests are content
// addressed, and every confidence update is a pure function of its inputs.

#include "support/m12_support.hpp"

#include <algorithm>
#include <array>

namespace {

using namespace mira;
using namespace mira::testing;

int json_round_trip_is_lossless() {
    const AppModel model = device_model();
    const JsonValue json = app_model_to_json(model);
    auto decoded = app_model_from_json(json);
    MIRA_CHECK(decoded.has_value());
    // Canonical forms are identical: the document survives byte-for-byte.
    MIRA_CHECK(app_model_to_json(decoded.value()) == json);
    auto reparsed = parse_app_model(to_json_string(json));
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(app_model_to_json(reparsed.value()) == json);
    return 0;
}

int structural_violations_fail_closed() {
    const AppModel model = device_model();

    // Unknown field.
    auto with_unknown = app_model_from_json(app_model_to_json(model));
    MIRA_CHECK(with_unknown.has_value());
    auto extended = app_model_to_json(with_unknown.value());
    extended.set("mystery", JsonValue{1});
    auto rejected = app_model_from_json(extended);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(AppModelError::UnknownField));

    // Dangling transition reference.
    auto dangling = model;
    dangling.transitions.push_back(ui_transition("t-broken", "home", "nowhere", "open_chats"));
    MIRA_CHECK(!validate_app_model(dangling).has_value());
    auto dangling_decoded = app_model_from_json(app_model_to_json(dangling));
    MIRA_CHECK(!dangling_decoded.has_value());
    MIRA_CHECK(dangling_decoded.error().domain_code ==
               static_cast<std::int32_t>(AppModelError::InvalidReference));

    // Duplicate state and transition ids.
    auto duplicate_state = model;
    duplicate_state.states.push_back(ui_state("home", "Launcher"));
    MIRA_CHECK(app_model_from_json(app_model_to_json(duplicate_state)).error().domain_code ==
               static_cast<std::int32_t>(AppModelError::DuplicateId));
    auto duplicate_edge = model;
    duplicate_edge.transitions.push_back(
        ui_transition("t-open-chats", "home", "chat_list", "open_chats"));
    MIRA_CHECK(app_model_from_json(app_model_to_json(duplicate_edge)).error().domain_code ==
               static_cast<std::int32_t>(AppModelError::DuplicateId));

    // Version mismatch.
    auto future = model;
    future.schema_version = SchemaVersion{2, 0};
    MIRA_CHECK(app_model_from_json(app_model_to_json(future)).error().domain_code ==
               static_cast<std::int32_t>(AppModelError::VersionMismatch));

    // Limits (RULE-08): tighten below the document's content.
    AppModelLimits limits;
    limits.max_states = 2;
    MIRA_CHECK(app_model_from_json(app_model_to_json(model), limits).error().domain_code ==
               static_cast<std::int32_t>(AppModelError::LimitExceeded));

    // Malformed action: reserved "tool" member missing.
    auto actionless = model;
    actionless.transitions[0].action = JsonValue{JsonValue::Object{}};
    MIRA_CHECK(!validate_app_model(actionless).has_value());

    // Empty ids and out-of-range costs fail closed through validate (same
    // semantics as decoding by construction).
    auto empty_id = model;
    empty_id.states[0].id.clear();
    MIRA_CHECK(!validate_app_model(empty_id).has_value());
    auto bad_cost = model;
    bad_cost.transitions[0].costs.failure_probability = 1.5;
    MIRA_CHECK(!validate_app_model(bad_cost).has_value());
    return 0;
}

int digests_are_content_addressed() {
    const AppModel model = device_model();
    MIRA_CHECK(app_model_digest(model) == app_model_digest(device_model()));
    auto evolved = model;
    evolved.transitions[0].confidence =
        note_transition_outcome(evolved.transitions[0].confidence, true, 42'000);
    MIRA_CHECK(app_model_digest(evolved) != app_model_digest(model));
    return 0;
}

int source_is_a_closed_set() {
    MIRA_CHECK(app_model_source_name(AppModelSource::Host) == "host");
    MIRA_CHECK(app_model_source_name(AppModelSource::Agent) == "agent");
    MIRA_CHECK(app_model_source_name(AppModelSource::Trajectory) == "trajectory");
    for (const auto name : {"", "user", "model", "HOST"}) {
        MIRA_CHECK(!parse_app_model_source(name).has_value());
    }
    // Unknown sources fail closed at decode time too.
    auto text = to_json_string(app_model_to_json(device_model()));
    const auto position = text.find("\"source\":\"host\"");
    MIRA_CHECK(position != std::string::npos);
    text.replace(position, 15, "\"source\":\"user\"");
    MIRA_CHECK(!parse_app_model(text).has_value());
    return 0;
}

int confidence_updates_are_pure_and_deterministic() {
    ConfidenceRecord record; // 0.5 at rest
    MIRA_CHECK(record.confidence == 0.5);

    const auto first_success = note_transition_outcome(record, true, 1'000);
    MIRA_CHECK(first_success.verified_count == 1);
    MIRA_CHECK(first_success.failure_count == 0);
    MIRA_CHECK(first_success.confidence == 2.0 / 3.0);
    MIRA_CHECK(first_success.observed_at_ms == 1'000);
    MIRA_CHECK(first_success.last_verified_ms == 1'000);

    const auto first_failure = note_transition_outcome(record, false, 2'000);
    MIRA_CHECK(first_failure.failure_count == 1);
    MIRA_CHECK(first_failure.verified_count == 0);
    MIRA_CHECK(first_failure.confidence == 1.0 / 3.0);
    MIRA_CHECK(first_failure.observed_at_ms == 2'000);
    // Failures never advance the verification timestamp.
    MIRA_CHECK(first_failure.last_verified_ms == 0);

    // Monotonicity along both directions.
    auto rising = record;
    for (int index = 0; index < 5; ++index) {
        const double before = rising.confidence;
        rising = note_transition_outcome(rising, true, 3'000);
        MIRA_CHECK(rising.confidence >= before);
    }
    auto falling = rising;
    for (int index = 0; index < 5; ++index) {
        const double before = falling.confidence;
        falling = note_transition_outcome(falling, false, 4'000);
        MIRA_CHECK(falling.confidence <= before);
    }

    // Purity: same inputs, same outputs (repeated calls agree exactly).
    MIRA_CHECK(note_transition_outcome(record, true, 5'000).confidence ==
               note_transition_outcome(record, true, 9'000).confidence);

    // Decay: exponential toward zero since last_verified, monotonic in dt.
    ConfidenceRecord verified = note_transition_outcome(record, true, 10'000);
    const auto decayed = apply_confidence_decay(verified, 110'000, 100'000); // one half-life
    MIRA_CHECK(decayed.confidence == verified.confidence * 0.5);
    const auto decayed_more = apply_confidence_decay(verified, 210'000, 100'000);
    MIRA_CHECK(decayed_more.confidence < decayed.confidence);
    // dt <= 0 and a zero half-life are no-ops; counters and timestamps stay.
    MIRA_CHECK(apply_confidence_decay(verified, 10'000, 100'000).confidence == verified.confidence);
    MIRA_CHECK(apply_confidence_decay(verified, 999'999, 0).confidence == verified.confidence);
    const auto untouched = apply_confidence_decay(verified, 999'999, 1);
    MIRA_CHECK(untouched.verified_count == verified.verified_count);
    MIRA_CHECK(untouched.last_verified_ms == verified.last_verified_ms);

    // Exploration thresholding.
    MIRA_CHECK(needs_exploration(decayed, 0.4));
    MIRA_CHECK(!needs_exploration(decayed, 0.3));
    MIRA_CHECK(needs_exploration(ConfidenceRecord{}, 0.6));
    return 0;
}

// --- Planner -----------------------------------------------------------------

const JsonValue kEmptyContext = JsonValue{JsonValue::Object{}};

int planner_picks_paths_by_weights() {
    const AppModel model = device_model();

    // Default weights: home->chat_view via the risky deep link costs
    // 20 + 0.9 < 100 + 200, so the shortcut wins.
    auto direct =
        plan_navigation(model, "home", "chat_view", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(direct.has_value());
    MIRA_CHECK(direct.value().transition_ids == std::vector<std::string>{"t-deep-link"});
    MIRA_CHECK(direct.value().total_cost == 20.9);

    // Raising the failure weight makes the safe route cheaper (the shortcut
    // costs 20 + w*0.9, the safe walk 100 + 200).
    NavigationCostProfile cautious;
    cautious.failure = 1000.0;
    auto safe = plan_navigation(model, "home", "chat_view", cautious, kEmptyContext);
    MIRA_CHECK(safe.has_value());
    MIRA_CHECK(safe.value().transition_ids ==
               std::vector<std::string>({"t-open-chats", "t-open-chat"}));

    // The full walk exists and reports its state sequence.
    auto walk = plan_navigation(model, "home", "composer", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(walk.has_value());
    MIRA_CHECK(walk.value().state_sequence ==
               std::vector<std::string>({"home", "chat_view", "composer"}));
    return 0;
}

int planner_is_deterministic_with_lexicographic_ties() {
    AppModel model;
    model.app_id = "tie";
    model.name = "tie";
    model.states = {ui_state("a", "A"), ui_state("b", "B")};
    model.transitions = {ui_transition("t-slow", "a", "b", "act", 10.0),
                         ui_transition("t-fast", "a", "b", "act", 5.0)};
    // Equal costs: the smaller edge id wins.
    model.transitions[0].costs.latency_ms = 5.0;
    auto planned = plan_navigation(model, "a", "b", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(planned.has_value());
    MIRA_CHECK(planned.value().transition_ids == std::vector<std::string>{"t-fast"});

    // Repeated calls agree exactly (digest included).
    auto again = plan_navigation(model, "a", "b", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(again.value().plan_digest == planned.value().plan_digest);
    MIRA_CHECK(again.value().transition_ids == planned.value().transition_ids);
    return 0;
}

int planner_guards_block_and_disclose() {
    AppModel model = device_model();

    // Guard not satisfied: edge unusable, counted as blocked.
    WorkflowPredicate deny;
    deny.signal = "run_parameter:dark_mode";
    deny.op = WorkflowPredicateOp::Eq;
    deny.value = JsonValue{true};
    model.transitions[3].guard = deny; // the shortcut
    JsonValue context(JsonValue::Object{});
    context.set("run_parameter:dark_mode", false);
    auto blocked = plan_navigation(model, "home", "chat_view", NavigationCostProfile{}, context);
    MIRA_CHECK(blocked.has_value());
    MIRA_CHECK(blocked.value().transition_ids ==
               std::vector<std::string>({"t-open-chats", "t-open-chat"}));
    MIRA_CHECK(blocked.value().guards_blocked == 1);

    // Guard not evaluable (signal absent, eq comparison): unusable but
    // counted separately from blocked.
    WorkflowPredicate opaque;
    opaque.signal = "screen_state:lock_screen";
    opaque.op = WorkflowPredicateOp::Eq;
    opaque.value = JsonValue{true};
    model.transitions[0].guard = opaque;
    model.transitions[1].guard = opaque;
    auto unevaluable = plan_navigation(model, "home", "composer", NavigationCostProfile{}, context);
    MIRA_CHECK(!unevaluable.has_value());
    MIRA_CHECK(unevaluable.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::NoPath));
    // The message discloses both counters: disconnected graph and guard-only
    // failures stay distinguishable (RULE-10).
    MIRA_CHECK(unevaluable.error().safe_message.find("blocked=1") != std::string::npos);
    MIRA_CHECK(unevaluable.error().safe_message.find("unevaluable=1") != std::string::npos);
    return 0;
}

int planner_excludes_agent_edges_by_default() {
    AppModel model = device_model();
    model.transitions[0].costs.agent_required = true;
    model.transitions[3].costs.agent_required = true;

    auto planned =
        plan_navigation(model, "home", "composer", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(!planned.has_value()); // all exits from home need the agent

    NavigationPlanOptions options;
    options.allow_agent_edges = true;
    auto host =
        plan_navigation(model, "home", "composer", NavigationCostProfile{}, kEmptyContext, options);
    MIRA_CHECK(host.has_value());
    return 0;
}

int planner_enforces_budgets_and_endpoints() {
    const AppModel model = device_model();

    // Unknown endpoints.
    auto from = plan_navigation(model, "nowhere", "home", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(from.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::UnknownFromState));
    auto to = plan_navigation(model, "home", "nowhere", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(to.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::UnknownToState));

    // from == to: the empty plan is legal and deterministic.
    auto empty = plan_navigation(model, "home", "home", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(empty.has_value());
    MIRA_CHECK(empty.value().transition_ids.empty());
    MIRA_CHECK(empty.value().state_sequence == std::vector<std::string>{"home"});

    // Edge-evaluation budget (RULE-08).
    NavigationPlanOptions tiny_evaluations;
    tiny_evaluations.max_edge_evaluations = 1;
    auto exhausted = plan_navigation(model, "home", "composer", NavigationCostProfile{},
                                     kEmptyContext, tiny_evaluations);
    MIRA_CHECK(exhausted.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::BudgetExceeded));

    // Path-length budget.
    NavigationPlanOptions tiny_path;
    tiny_path.max_path_edges = 1;
    auto too_long = plan_navigation(model, "home", "composer", NavigationCostProfile{},
                                    kEmptyContext, tiny_path);
    MIRA_CHECK(too_long.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::BudgetExceeded));

    // A genuinely disconnected target is NoPath, not budget exhaustion.
    auto island = model;
    island.states.push_back(ui_state("settings", "Settings"));
    auto unreachable =
        plan_navigation(island, "home", "settings", NavigationCostProfile{}, kEmptyContext);
    MIRA_CHECK(unreachable.error().domain_code ==
               static_cast<std::int32_t>(NavigationError::NoPath));

    // Invalid weight profiles fail closed.
    NavigationCostProfile negative;
    negative.latency = -1.0;
    auto invalid = plan_navigation(model, "home", "composer", negative, kEmptyContext);
    MIRA_CHECK(!invalid.has_value());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {json_round_trip_is_lossless, structural_violations_fail_closed,
         digests_are_content_addressed, source_is_a_closed_set,
         confidence_updates_are_pure_and_deterministic, planner_picks_paths_by_weights,
         planner_is_deterministic_with_lexicographic_ties, planner_guards_block_and_disclose,
         planner_excludes_agent_edges_by_default, planner_enforces_budgets_and_endpoints});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
