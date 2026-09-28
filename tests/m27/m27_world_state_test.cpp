// M27 (DEC-041 first stage) world-state IVA gate matrix - WS-G2 (purity,
// validation, update operators), WS-G4 (capacity and eviction) and WS-G5
// (Unknown/staleness fail-closed matrix, read-only face). All expectations
// come from the milestone plan §4 frozen semantics
// (docs/plans/m27-world-state-projection-core.md), not from the
// implementation. WS-G3/WS-G6 live in m27_wire_rebuild.cpp.

#include "m27_cases.hpp"

#include <limits>
#include <string>
#include <vector>

namespace mira::m27 {
namespace {

[[nodiscard]] WorldPageAssumption believed_page(std::string app_id, std::string state_id,
                                                double confidence, std::int64_t second) {
    WorldPageAssumption page;
    page.status = WorldBeliefStatus::Believed;
    page.app_id = std::move(app_id);
    page.state_id = std::move(state_id);
    page.confidence = confidence;
    page.recognized_at = at_ns(second);
    return page;
}

[[nodiscard]] std::size_t count_kind(const WorldState &state, WorldChangeKind kind) {
    std::size_t seen = 0;
    for (const auto &change : state.recent_changes) {
        if (change.kind == kind) {
            ++seen;
        }
    }
    return seen;
}

} // namespace

// ---------------------------------------------------------------------------
// WS-G2: options validate rejections (§4.1: any capacity 0 -> OptionsInvalid).
// ---------------------------------------------------------------------------

int options_validate_rejections() {
    WorldStateOptions options;
    MIRA_CHECK(options.validate().has_value());

    WorldStateOptions zero_entities;
    zero_entities.max_entities = 0;
    MIRA_CHECK(expect_error(zero_entities.validate(), WorldStateDomainCode::OptionsInvalid,
                            "max_entities 0") == 0);

    WorldStateOptions zero_ring;
    zero_ring.max_recent_changes = 0;
    MIRA_CHECK(expect_error(zero_ring.validate(), WorldStateDomainCode::OptionsInvalid,
                            "max_recent_changes 0") == 0);

    WorldStateOptions zero_summary;
    zero_summary.max_change_summary_bytes = 0;
    MIRA_CHECK(expect_error(zero_summary.validate(), WorldStateDomainCode::OptionsInvalid,
                            "max_change_summary_bytes 0") == 0);

    // The operator path checks options before anything else.
    const auto input = make_observation(0x31, make_app("app.A", "act.B"), {make_element(1)}, 1);
    MIRA_CHECK(expect_error(apply_world_observation(WorldState{}, input, zero_entities),
                            WorldStateDomainCode::OptionsInvalid, "operator options gate") == 0);
    MIRA_CHECK(expect_error(apply_expiry(WorldState{}, make_expiry(1), zero_ring),
                            WorldStateDomainCode::OptionsInvalid, "expiry options gate") == 0);
    MIRA_CHECK(expect_error(rebuild_world_state({}, zero_summary),
                            WorldStateDomainCode::OptionsInvalid, "rebuild options gate") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: value-type validate rejections (§4.1; nested folds -> StateInvalid).
// ---------------------------------------------------------------------------

int value_validate_rejections() {
    WorldForegroundApp bad_unknown_with_package;
    bad_unknown_with_package.package_name = "app.A";
    MIRA_CHECK(expect_error(bad_unknown_with_package.validate(), WorldStateDomainCode::StateInvalid,
                            "Unknown foreground with package") == 0);

    WorldForegroundApp bad_believed_without_package;
    bad_believed_without_package.status = WorldBeliefStatus::Believed;
    MIRA_CHECK(expect_error(bad_believed_without_package.validate(),
                            WorldStateDomainCode::StateInvalid,
                            "Believed foreground without package") == 0);

    WorldPageAssumption bad_unknown_with_pair;
    bad_unknown_with_pair.app_id = "app.A";
    MIRA_CHECK(expect_error(bad_unknown_with_pair.validate(), WorldStateDomainCode::StateInvalid,
                            "Unknown page with pair") == 0);

    WorldPageAssumption bad_unknown_with_confidence;
    bad_unknown_with_confidence.confidence = 0.5;
    MIRA_CHECK(expect_error(bad_unknown_with_confidence.validate(),
                            WorldStateDomainCode::StateInvalid,
                            "Unknown page with confidence") == 0);

    WorldPageAssumption bad_missing_state;
    bad_missing_state.status = WorldBeliefStatus::Believed;
    bad_missing_state.app_id = "app.A";
    bad_missing_state.confidence = 0.5;
    MIRA_CHECK(expect_error(bad_missing_state.validate(), WorldStateDomainCode::StateInvalid,
                            "Believed page without state_id") == 0);

    WorldPageAssumption bad_confidence = believed_page("app.A", "s", 1.5, 1);
    MIRA_CHECK(expect_error(bad_confidence.validate(), WorldStateDomainCode::StateInvalid,
                            "page confidence above 1") == 0);

    WorldEntity nil_oid;
    MIRA_CHECK(nil_oid.element.observation_id.is_nil());
    MIRA_CHECK(expect_error(nil_oid.validate(), WorldStateDomainCode::StateInvalid,
                            "entity nil observation_id folds to StateInvalid") == 0);

    WorldEntity nil_space;
    nil_space.element = make_element(1);
    nil_space.element.space = CoordinateSpaceId{};
    MIRA_CHECK(expect_error(nil_space.validate(), WorldStateDomainCode::StateInvalid,
                            "entity nil space folds to StateInvalid") == 0);

    WorldEntity bad_source;
    bad_source.element = make_element(1);
    bad_source.element.source = static_cast<ElementSource>(99);
    MIRA_CHECK(expect_error(bad_source.validate(), WorldStateDomainCode::StateInvalid,
                            "entity source outside closed set") == 0);

    WorldEntity nan_confidence;
    nan_confidence.element = make_element(1);
    nan_confidence.confidence = std::numeric_limits<double>::quiet_NaN();
    MIRA_CHECK(expect_error(nan_confidence.validate(), WorldStateDomainCode::StateInvalid,
                            "entity NaN confidence") == 0);

    WorldState regression;
    regression.recent_changes.push_back(WorldChange{});
    regression.recent_changes.back().change_sequence = 5;
    regression.recent_changes.push_back(WorldChange{});
    regression.recent_changes.back().change_sequence = 5;
    MIRA_CHECK(expect_error(regression.validate(), WorldStateDomainCode::StateInvalid,
                            "change_sequence regression") == 0);

    WorldState bad_kind;
    bad_kind.recent_changes.push_back(WorldChange{});
    bad_kind.recent_changes.back().kind = static_cast<WorldChangeKind>(99);
    MIRA_CHECK(expect_error(bad_kind.validate(), WorldStateDomainCode::StateInvalid,
                            "change kind outside closed set") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_world_observation matrix (§4.3 row 1).
// ---------------------------------------------------------------------------

int observation_operator_matrix() {
    const WorldStateOptions options;
    const auto first = make_observation(0x41, make_app("app.A", "act.B", true),
                                        {make_element(1), make_element(2)}, 1);

    auto state = apply_world_observation(WorldState{}, first, options);
    MIRA_CHECK(expect_ok(state, "first observation") == 0);
    MIRA_CHECK(state.value().foreground_app.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(state.value().foreground_app.package_name == "app.A");
    MIRA_CHECK(state.value().foreground_app.activity_name == "act.B");
    MIRA_CHECK(state.value().foreground_app.sensitive);
    MIRA_CHECK(same_instant(state.value().foreground_app.observed_at, at_ns(1)));
    MIRA_CHECK(state.value().entities.size() == 2);
    for (const auto &entity : state.value().entities) {
        MIRA_CHECK(entity.confidence == 1.0);
        MIRA_CHECK(!entity.stale);
        MIRA_CHECK(same_instant(entity.last_seen_at, at_ns(1)));
    }
    MIRA_CHECK(count_kind(state.value(), WorldChangeKind::PageAssumed) == 1);
    MIRA_CHECK(count_kind(state.value(), WorldChangeKind::EntityObserved) == 1);

    // Repeat observation of the same foreground at a later time: the frozen
    // semantics overwrite the four belief fields field-by-field (observed_at
    // included) and the ring gains only the EntityObserved entry (no fresh
    // PageAssumed - the belief content did not change).
    const auto repeat =
        make_observation(0x42, make_app("app.A", "act.B", true), {make_element(2)}, 9);
    auto advanced = apply_world_observation(std::move(state).value(), repeat, options);
    MIRA_CHECK(expect_ok(advanced, "repeat observation") == 0);
    MIRA_CHECK(same_instant(advanced.value().foreground_app.observed_at, at_ns(9)));
    MIRA_CHECK(count_kind(advanced.value(), WorldChangeKind::PageAssumed) == 1);
    MIRA_CHECK(advanced.value().recent_changes.back().kind == WorldChangeKind::EntityObserved);

    // Empty package resets the foreground to Unknown and records it.
    const auto gone = make_observation(0x43, make_app("", ""), {}, 10);
    auto reset = apply_world_observation(std::move(advanced).value(), gone, options);
    MIRA_CHECK(expect_ok(reset, "foreground reset") == 0);
    MIRA_CHECK(reset.value().foreground_app.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(reset.value().foreground_app.package_name.empty());
    MIRA_CHECK(count_kind(reset.value(), WorldChangeKind::PageAssumed) == 2);

    // Refresh semantics: a stale low-confidence entry is restored to 1.0.
    WorldState stale_state;
    stale_state.entities.push_back(make_entity(7, true, 0.25, 1));
    const auto refresh = make_observation(0x44, make_app("", ""), {make_element(7)}, 11);
    auto revived = apply_world_observation(std::move(stale_state), refresh, options);
    MIRA_CHECK(expect_ok(revived, "entity refresh") == 0);
    MIRA_CHECK(revived.value().entities.size() == 1);
    MIRA_CHECK(revived.value().entities.front().confidence == 1.0);
    MIRA_CHECK(!revived.value().entities.front().stale);
    MIRA_CHECK(same_instant(revived.value().entities.front().last_seen_at, at_ns(11)));

    // Element count over capacity -> RecordInvalid.
    WorldStateOptions tiny;
    tiny.max_entities = 1;
    const auto crowd =
        make_observation(0x45, make_app("app.A", "act.B"), {make_element(1), make_element(2)}, 1);
    MIRA_CHECK(expect_error(apply_world_observation(WorldState{}, crowd, tiny),
                            WorldStateDomainCode::RecordInvalid, "elements over capacity") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_screen_state matrix (authoritative recognition, zero conflict).
// ---------------------------------------------------------------------------

int screen_state_operator_zero_conflict() {
    const WorldStateOptions options;

    auto state = apply_screen_state(WorldState{}, make_screen("app.A", "home", 0.9, 1), options);
    MIRA_CHECK(expect_ok(state, "first recognition") == 0);
    MIRA_CHECK(state.value().current_page.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(state.value().current_page.app_id == "app.A");
    MIRA_CHECK(state.value().current_page.state_id == "home");
    MIRA_CHECK(state.value().current_page.confidence == 0.9);
    MIRA_CHECK(same_instant(state.value().current_page.recognized_at, at_ns(1)));
    MIRA_CHECK(count_kind(state.value(), WorldChangeKind::PageAssumed) == 1);
    MIRA_CHECK(count_kind(state.value(), WorldChangeKind::ConflictMarked) == 0);

    // Page switch A -> B is a normal update: PageAssumed again, still no
    // ConflictMarked (the sole conflict trigger is the navigation from_state
    // reconciliation).
    auto switched =
        apply_screen_state(std::move(state).value(), make_screen("app.B", "list", 0.8, 2), options);
    MIRA_CHECK(expect_ok(switched, "page switch") == 0);
    MIRA_CHECK(switched.value().current_page.app_id == "app.B");
    MIRA_CHECK(count_kind(switched.value(), WorldChangeKind::PageAssumed) == 2);
    MIRA_CHECK(count_kind(switched.value(), WorldChangeKind::ConflictMarked) == 0);

    MIRA_CHECK(
        expect_error(apply_screen_state(WorldState{}, make_screen("app.A", "", 0.5, 1), options),
                     WorldStateDomainCode::VocabularyViolation, "empty state_id") == 0);
    MIRA_CHECK(expect_error(
                   apply_screen_state(WorldState{}, make_screen("app.A", "home", 1.5, 1), options),
                   WorldStateDomainCode::RecordInvalid, "confidence above 1") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_navigation matrix (§4.3 row 3, incl. the B1 conflict rule).
// ---------------------------------------------------------------------------

int navigation_operator_matrix() {
    const WorldStateOptions options;

    // Unknown departure + success + app_id -> Believed from the input record.
    auto from_unknown = apply_navigation(
        WorldState{}, make_navigation("app.A", "anywhere", "detail", "t1", true, 0.7, 1), options);
    MIRA_CHECK(expect_ok(from_unknown, "unknown departure success") == 0);
    MIRA_CHECK(from_unknown.value().current_page.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(from_unknown.value().current_page.app_id == "app.A");
    MIRA_CHECK(from_unknown.value().current_page.state_id == "detail");
    MIRA_CHECK(from_unknown.value().current_page.confidence == 0.7);
    MIRA_CHECK(same_instant(from_unknown.value().current_page.recognized_at, at_ns(1)));
    MIRA_CHECK(count_kind(from_unknown.value(), WorldChangeKind::NavigationObserved) == 1);
    MIRA_CHECK(count_kind(from_unknown.value(), WorldChangeKind::ConflictMarked) == 0);

    // Unknown departure + failure -> zero page change, entry still recorded.
    auto failed_unknown = apply_navigation(
        WorldState{}, make_navigation("app.A", "x", "y", "t2", false, 0.5, 1), options);
    MIRA_CHECK(expect_ok(failed_unknown, "unknown departure failure") == 0);
    MIRA_CHECK(failed_unknown.value().current_page.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(count_kind(failed_unknown.value(), WorldChangeKind::NavigationObserved) == 1);

    // Believed page + failure -> Stale; confidence and recognized_at keep
    // counting from the original recognition.
    WorldState believed;
    believed.current_page = believed_page("app.A", "home", 0.9, 1);
    auto failed =
        apply_navigation(std::move(believed),
                         make_navigation("app.A", "home", "detail", "t3", false, 0.1, 50), options);
    MIRA_CHECK(expect_ok(failed, "believed failure") == 0);
    MIRA_CHECK(failed.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(failed.value().current_page.confidence == 0.9);
    MIRA_CHECK(same_instant(failed.value().current_page.recognized_at, at_ns(1)));
    MIRA_CHECK(count_kind(failed.value(), WorldChangeKind::NavigationObserved) == 1);

    // from_state mismatch -> ConflictMarked (sole trigger) AND the successful
    // transition proceeds normally.
    WorldState mismatch;
    mismatch.current_page = believed_page("app.A", "home", 0.9, 1);
    auto conflicted =
        apply_navigation(std::move(mismatch),
                         make_navigation("app.A", "other", "detail", "t4", true, 0.6, 2), options);
    MIRA_CHECK(expect_ok(conflicted, "conflict and transition") == 0);
    MIRA_CHECK(count_kind(conflicted.value(), WorldChangeKind::ConflictMarked) == 1);
    MIRA_CHECK(count_kind(conflicted.value(), WorldChangeKind::NavigationObserved) == 1);
    MIRA_CHECK(conflicted.value().recent_changes.size() == 2);
    MIRA_CHECK(conflicted.value().recent_changes.front().kind == WorldChangeKind::ConflictMarked);
    MIRA_CHECK(conflicted.value().recent_changes.front().summary.find("home") != std::string::npos);
    MIRA_CHECK(conflicted.value().recent_changes.front().summary.find("other") !=
               std::string::npos);
    MIRA_CHECK(conflicted.value().current_page.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(conflicted.value().current_page.state_id == "detail");
    MIRA_CHECK(same_instant(conflicted.value().current_page.recognized_at, at_ns(2)));

    // Matching from_state -> normal A -> B transition, zero ConflictMarked.
    WorldState aligned;
    aligned.current_page = believed_page("app.A", "home", 0.9, 1);
    auto clean =
        apply_navigation(std::move(aligned),
                         make_navigation("app.A", "home", "detail", "t5", true, 0.6, 2), options);
    MIRA_CHECK(expect_ok(clean, "aligned navigation") == 0);
    MIRA_CHECK(count_kind(clean.value(), WorldChangeKind::ConflictMarked) == 0);
    MIRA_CHECK(clean.value().current_page.state_id == "detail");

    // Vocabulary and record rejections.
    MIRA_CHECK(
        expect_error(apply_navigation(WorldState{},
                                      make_navigation("", "a", "b", "t", false, 0.5, 1), options),
                     WorldStateDomainCode::VocabularyViolation, "empty app_id") == 0);
    MIRA_CHECK(expect_error(apply_navigation(WorldState{},
                                             make_navigation("app.A", "a", "b", "t", false, 1.5, 1),
                                             options),
                            WorldStateDomainCode::RecordInvalid, "confidence above 1") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_tool_settled matrix (registry-level check, entry-only v1 scope).
// ---------------------------------------------------------------------------

int tool_settled_operator_matrix() {
    const WorldStateOptions options;
    WorldState state;
    state.current_page = believed_page("app.A", "home", 0.9, 1);
    state.entities.push_back(make_entity(1, false, 1.0, 1));

    const WorldBeliefStatus page_status_before = state.current_page.status;
    const double page_confidence_before = state.current_page.confidence;
    const std::size_t entities_before = state.entities.size();

    auto settled = apply_tool_settled(std::move(state),
                                      make_tool("toolref:delta.render", "completed", 5), options);
    MIRA_CHECK(expect_ok(settled, "tool settled") == 0);
    MIRA_CHECK(settled.value().recent_changes.size() == 1);
    MIRA_CHECK(settled.value().recent_changes.front().kind == WorldChangeKind::ToolSettled);
    MIRA_CHECK(same_instant(settled.value().recent_changes.front().changed_at, at_ns(5)));
    // v1 scope: the settlement touches only the ring - foreground/page/entity
    // beliefs are untouched.
    MIRA_CHECK(settled.value().current_page.status == page_status_before);
    MIRA_CHECK(settled.value().current_page.confidence == page_confidence_before);
    MIRA_CHECK(settled.value().entities.size() == entities_before);

    MIRA_CHECK(
        expect_error(apply_tool_settled(WorldState{}, make_tool("", "completed", 1), options),
                     WorldStateDomainCode::VocabularyViolation, "empty tool_ref") == 0);
    MIRA_CHECK(
        expect_error(apply_tool_settled(WorldState{},
                                        make_tool(std::string(257, 'x'), "completed", 1), options),
                     WorldStateDomainCode::VocabularyViolation, "over-long tool_ref") == 0);
    MIRA_CHECK(
        expect_error(apply_tool_settled(WorldState{},
                                        make_tool("toolref:delta.render", "aborted", 1), options),
                     WorldStateDomainCode::RecordInvalid, "disposition outside closed set") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_verification matrix (three outcomes + StaleTargetMissing).
// ---------------------------------------------------------------------------

int verification_operator_matrix() {
    const WorldStateOptions options;

    // Unknown target -> explicit StaleTargetMissing (never a silent insert).
    MIRA_CHECK(expect_error(apply_verification(WorldState{}, make_verification("confirmed", 0.9, 1),
                                               options),
                            WorldStateDomainCode::StaleTargetMissing,
                            "unknown verification target") == 0);

    WorldState state;
    state.current_page = believed_page("app.A", "home", 0.4, 1);

    // confirmed -> Believed with the new confidence and recognized_at.
    auto confirmed = apply_verification(state, make_verification("confirmed", 0.95, 7), options);
    MIRA_CHECK(expect_ok(confirmed, "confirmed") == 0);
    MIRA_CHECK(confirmed.value().current_page.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(confirmed.value().current_page.confidence == 0.95);
    MIRA_CHECK(same_instant(confirmed.value().current_page.recognized_at, at_ns(7)));
    MIRA_CHECK(count_kind(confirmed.value(), WorldChangeKind::VerificationSettled) == 1);

    // refuted -> Stale, confidence and recognized_at refreshed.
    auto refuted = apply_verification(std::move(confirmed).value(),
                                      make_verification("refuted", 0.8, 8), options);
    MIRA_CHECK(expect_ok(refuted, "refuted") == 0);
    MIRA_CHECK(refuted.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(refuted.value().current_page.confidence == 0.8);
    MIRA_CHECK(same_instant(refuted.value().current_page.recognized_at, at_ns(8)));

    // inconclusive -> status, confidence and recognized_at all unchanged.
    const WorldBeliefStatus status_before = refuted.value().current_page.status;
    const double confidence_before = refuted.value().current_page.confidence;
    const Timestamp recognized_before = refuted.value().current_page.recognized_at;
    auto inconclusive = apply_verification(std::move(refuted).value(),
                                           make_verification("inconclusive", 0.1, 9), options);
    MIRA_CHECK(expect_ok(inconclusive, "inconclusive") == 0);
    MIRA_CHECK(inconclusive.value().current_page.status == status_before);
    MIRA_CHECK(inconclusive.value().current_page.confidence == confidence_before);
    MIRA_CHECK(same_instant(inconclusive.value().current_page.recognized_at, recognized_before));
    // All three outcomes append the settlement entry: the accumulated ring
    // carries one per settled verification (confirmed + refuted + inconclusive).
    MIRA_CHECK(count_kind(inconclusive.value(), WorldChangeKind::VerificationSettled) == 3);
    MIRA_CHECK(inconclusive.value().recent_changes.back().kind ==
               WorldChangeKind::VerificationSettled);

    MIRA_CHECK(expect_error(apply_verification(state, make_verification("maybe", 0.5, 1), options),
                            WorldStateDomainCode::RecordInvalid,
                            "outcome outside closed set") == 0);
    MIRA_CHECK(
        expect_error(apply_verification(state, make_verification("confirmed", 1.5, 1), options),
                     WorldStateDomainCode::RecordInvalid, "verification confidence above 1") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: apply_expiry matrix (staleness bound, halving, no double decay).
// ---------------------------------------------------------------------------

int expiry_operator_matrix() {
    const WorldStateOptions options;

    WorldState state;
    state.entities.push_back(make_entity(1, false, 0.5, 1));   // age 100s > 30s
    state.entities.push_back(make_entity(2, false, 1.0, 100)); // fresh
    state.current_page = believed_page("app.A", "home", 0.5, 1);

    auto expired = apply_expiry(std::move(state), make_expiry(101), options);
    MIRA_CHECK(expect_ok(expired, "expiry") == 0);
    MIRA_CHECK(expired.value().entities.front().stale);
    MIRA_CHECK(expired.value().entities.front().confidence == 0.25);
    MIRA_CHECK(!expired.value().entities.back().stale);
    MIRA_CHECK(expired.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(expired.value().current_page.confidence == 0.25);
    MIRA_CHECK(same_instant(expired.value().current_page.recognized_at, at_ns(1)));
    // One entry per staled entity (e1) plus one for the page; the fresh
    // entity produces none.
    MIRA_CHECK(count_kind(expired.value(), WorldChangeKind::EntityStaled) == 2);

    // Second expiry at the same instant: already-Stale entries never decay
    // twice and produce no further entries.
    const std::size_t ring_before = expired.value().recent_changes.size();
    auto again = apply_expiry(std::move(expired).value(), make_expiry(101), options);
    MIRA_CHECK(expect_ok(again, "second expiry") == 0);
    MIRA_CHECK(again.value().entities.front().confidence == 0.25);
    MIRA_CHECK(again.value().current_page.confidence == 0.25);
    MIRA_CHECK(again.value().recent_changes.size() == ring_before);

    // Boundary: age exactly equal to stale_after does not stale (strictly
    // more-than), one nanosecond more does.
    WorldState edge;
    edge.entities.push_back(make_entity(3, false, 1.0, 0));
    auto at_bound = apply_expiry(std::move(edge), make_expiry(30, 0), options);
    MIRA_CHECK(expect_ok(at_bound, "expiry at bound") == 0);
    MIRA_CHECK(!at_bound.value().entities.front().stale);
    MIRA_CHECK(at_bound.value().recent_changes.empty());
    auto past_bound = apply_expiry(std::move(at_bound).value(), make_expiry(30, 1), options);
    MIRA_CHECK(expect_ok(past_bound, "expiry past bound") == 0);
    MIRA_CHECK(past_bound.value().entities.front().stale);
    MIRA_CHECK(count_kind(past_bound.value(), WorldChangeKind::EntityStaled) == 1);

    // Unknown page holds no belief and is never expired.
    WorldState unknown_page;
    unknown_page.entities.push_back(make_entity(4, false, 1.0, 0));
    auto unknown = apply_expiry(std::move(unknown_page), make_expiry(1000), options);
    MIRA_CHECK(expect_ok(unknown, "unknown page expiry") == 0);
    MIRA_CHECK(unknown.value().current_page.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(unknown.value().current_page.confidence == 0.0);
    MIRA_CHECK(is_epoch(unknown.value().current_page.recognized_at));

    // No over-bound entry -> zero change, zero entry.
    WorldState fresh;
    fresh.entities.push_back(make_entity(5, false, 1.0, 1000));
    const auto digest_before = fresh.digest();
    auto quiet = apply_expiry(std::move(fresh), make_expiry(1000), options);
    MIRA_CHECK(expect_ok(quiet, "quiet expiry") == 0);
    MIRA_CHECK(quiet.value().digest() == digest_before);
    MIRA_CHECK(quiet.value().recent_changes.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G2: purity (no system clock observable) and strong-consistency failures.
// ---------------------------------------------------------------------------

int purity_clock_independence_and_strong_consistency() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();

    const auto first = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(first, "rebuild one") == 0);
    const Timestamp before_now = Timestamp::now();
    const auto second = rebuild_world_state(dataset, options);
    const Timestamp after_now = Timestamp::now();
    MIRA_CHECK(expect_ok(second, "rebuild two") == 0);
    MIRA_CHECK(first.value().digest() == second.value().digest());
    // The host wall clock advanced between the two replays while the
    // projection digest stayed byte-identical: projection time comes only
    // from the input records (§4.6). The cross-process face of this
    // assertion is the pinned dataset anchor (WS-G3).
    MIRA_CHECK(after_now.wall >= before_now.wall);

    // Strong consistency: an operator failure leaves the caller's value
    // untouched - the pre-state stays valid and reusable.
    WorldState state = first.value();
    const auto before = state.digest();
    const std::size_t entities_before = state.entities.size();

    WorldVerificationInput bad_outcome;
    bad_outcome.outcome = "maybe";
    bad_outcome.confidence = 0.5;
    bad_outcome.verified_at = at_ns(1);
    MIRA_CHECK(expect_error(apply_verification(state, bad_outcome, options),
                            WorldStateDomainCode::RecordInvalid, "strong consistency") == 0);
    MIRA_CHECK(state.digest() == before);
    MIRA_CHECK(state.entities.size() == entities_before);
    MIRA_CHECK(state.validate().has_value());

    const auto huge =
        make_observation(0x51, make_app(std::string(2100, 'x'), "act"), {make_element(1)}, 1);
    MIRA_CHECK(expect_error(apply_world_observation(state, huge, options),
                            WorldStateDomainCode::PayloadTooLarge,
                            "summary bound strong consistency") == 0);
    MIRA_CHECK(state.digest() == before);
    MIRA_CHECK(state.validate().has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G4: the frozen eviction order, cell by cell (§4.4 four-key total order,
// via the named prune_entities implementation).
// ---------------------------------------------------------------------------

int eviction_order_grid() {
    WorldStateOptions options;
    options.max_entities = 3;

    // (a) stale beats everything, even the newest entry.
    WorldState stale_first;
    stale_first.entities = {make_entity(1, false, 1.0, 5), make_entity(2, true, 1.0, 9),
                            make_entity(3, false, 1.0, 2), make_entity(4, false, 1.0, 3)};
    auto pruned = prune_entities(std::move(stale_first), options);
    MIRA_CHECK(pruned.entities.size() == 3);
    MIRA_CHECK(tag_of(pruned.entities.front()) == tagged_oid(1).to_string());
    MIRA_CHECK(count_kind(pruned, WorldChangeKind::EntityEvicted) == 1);

    // (b) same staleness -> earliest last_seen_at goes.
    WorldState earliest;
    earliest.entities = {make_entity(1, false, 1.0, 4), make_entity(2, false, 1.0, 1),
                         make_entity(3, false, 1.0, 3), make_entity(4, false, 1.0, 2)};
    pruned = prune_entities(std::move(earliest), options);
    MIRA_CHECK(pruned.entities.size() == 3);
    for (const auto &entity : pruned.entities) {
        MIRA_CHECK(tag_of(entity) != tagged_oid(2).to_string());
    }

    // (c) same staleness and time -> lowest confidence goes.
    WorldState weakest;
    weakest.entities = {make_entity(1, false, 0.5, 2), make_entity(2, false, 0.8, 2),
                        make_entity(3, false, 1.0, 2), make_entity(4, false, 0.9, 2)};
    pruned = prune_entities(std::move(weakest), options);
    MIRA_CHECK(pruned.entities.size() == 3);
    for (const auto &entity : pruned.entities) {
        MIRA_CHECK(tag_of(entity) != tagged_oid(1).to_string());
    }

    // (d) full key tie -> ElementRef canonical JSON lexicographic ascending.
    // All four entries share the identical seven-field element identity except
    // stable_hint, so the hint settles the order: the smallest key goes.
    WorldState tied;
    tied.entities = {
        make_entity(9, false, 1.0, 2, "node_d"), make_entity(9, false, 1.0, 2, "node_b"),
        make_entity(9, false, 1.0, 2, "node_c"), make_entity(9, false, 1.0, 2, "node_a")};
    pruned = prune_entities(std::move(tied), options);
    MIRA_CHECK(pruned.entities.size() == 3);
    for (const auto &entity : pruned.entities) {
        MIRA_CHECK(entity.element.stable_hint.hint != "node_a");
    }

    // Stability: two fully identical entries - exactly one of them goes and
    // the other survives (deterministic pick, no ambiguity).
    WorldState duplicates;
    duplicates.entities = {make_entity(1, false, 1.0, 2), make_entity(1, false, 1.0, 2),
                           make_entity(2, false, 1.0, 3), make_entity(3, false, 1.0, 4)};
    pruned = prune_entities(std::move(duplicates), options);
    MIRA_CHECK(pruned.entities.size() == 3);
    MIRA_CHECK(count_kind(pruned, WorldChangeKind::EntityEvicted) == 1);
    MIRA_CHECK(tag_of(pruned.entities.front()) == tagged_oid(1).to_string());
    MIRA_CHECK(tag_of(pruned.entities.at(1)) == tagged_oid(2).to_string());
    MIRA_CHECK(tag_of(pruned.entities.at(2)) == tagged_oid(3).to_string());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G4: insert overflow eviction happens inside the observation operator and
// is replayable from the input sequence (EntityEvicted entries included).
// ---------------------------------------------------------------------------

int insert_overflow_eviction_replayable() {
    const auto options = dataset_options();
    const AppContext launcher = make_app("com.example.launcher", "MainActivity");
    const std::vector<WorldStateInput> prefix = {
        make_observation(0x11, launcher, {make_element(1), make_element(2)}, 1),
        make_observation(0x12, launcher, {make_element(2), make_element(3)}, 2),
    };

    auto incremental = fold_all(prefix, options);
    MIRA_CHECK(expect_ok(incremental, "incremental fold") == 0);
    MIRA_CHECK(incremental.value().entities.size() == options.max_entities);
    for (const auto &entity : incremental.value().entities) {
        MIRA_CHECK(tag_of(entity) != tagged_oid(1).to_string()); // e1 evicted
    }
    MIRA_CHECK(count_kind(incremental.value(), WorldChangeKind::EntityEvicted) == 1);
    MIRA_CHECK(count_kind(incremental.value(), WorldChangeKind::EntityObserved) == 2);

    // The very same projection rebuilds byte-identically from the input
    // sequence: the eviction is part of the recipe, not caller-side state.
    auto rebuilt = rebuild_world_state(prefix, options);
    MIRA_CHECK(expect_ok(rebuilt, "rebuild") == 0);
    MIRA_CHECK(rebuilt.value().digest() == incremental.value().digest());
    MIRA_CHECK(count_kind(rebuilt.value(), WorldChangeKind::EntityEvicted) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G4: the recent-change ring evicts FIFO at max_recent_changes.
// ---------------------------------------------------------------------------

int ring_fifo_capacity() {
    WorldStateOptions options;
    options.max_recent_changes = 3;

    WorldState state;
    for (std::int64_t second = 1; second <= 5; ++second) {
        auto advanced = apply_tool_settled(
            std::move(state),
            make_tool("toolref:tool." + std::to_string(second), "completed", second), options);
        MIRA_CHECK(expect_ok(advanced, "tool settle push") == 0);
        state = std::move(advanced).value();
    }
    MIRA_CHECK(state.recent_changes.size() == 3);
    for (std::size_t index = 0; index < state.recent_changes.size(); ++index) {
        MIRA_CHECK(state.recent_changes[index].kind == WorldChangeKind::ToolSettled);
        if (index > 0) {
            MIRA_CHECK(state.recent_changes[index].change_sequence >
                       state.recent_changes[index - 1].change_sequence);
        }
    }
    MIRA_CHECK(state.recent_changes.back().summary.find("tool.5") != std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G4: summary over max_change_summary_bytes -> PayloadTooLarge, zero change
// (rejected, never truncated).
// ---------------------------------------------------------------------------

int summary_bound_rejected_zero_change() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();
    auto state = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(state, "populated projection") == 0);
    const auto before = state.value().digest();

    // summary = "foreground " + package + "/" + activity; 11 + 2036 + 1 + 1 =
    // 2049 > 2048 -> rejected.
    const auto over = make_observation(0x61, make_app(std::string(2036, 'x'), "a"), {}, 1);
    const auto rejected = apply_world_observation(state.value(), over, options);
    MIRA_CHECK(
        expect_error(rejected, WorldStateDomainCode::PayloadTooLarge, "summary 2049 bytes") == 0);
    MIRA_CHECK(state.value().digest() == before);

    // Boundary: exactly 2048 bytes is accepted (the bound is <=).
    auto at_bound = apply_world_observation(
        WorldState{}, make_observation(0x62, make_app(std::string(2035, 'x'), "a"), {}, 1),
        options);
    MIRA_CHECK(expect_ok(at_bound, "summary exactly 2048 bytes") == 0);
    MIRA_CHECK(at_bound.value().foreground_app.status == WorldBeliefStatus::Believed);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G4: options are part of the rebuild recipe - the same input sequence
// under different options yields a different (internally consistent)
// projection; nothing in the public face compares digests across options.
// ---------------------------------------------------------------------------

int options_enter_rebuild_recipe() {
    const auto dataset = frozen_dataset();
    const auto wide = dataset_options();
    WorldStateOptions narrow = dataset_options();
    narrow.max_recent_changes = 2;

    const auto wide_state = rebuild_world_state(dataset, wide);
    const auto narrow_state = rebuild_world_state(dataset, narrow);
    MIRA_CHECK(expect_ok(wide_state, "wide rebuild") == 0);
    MIRA_CHECK(expect_ok(narrow_state, "narrow rebuild") == 0);
    MIRA_CHECK(wide_state.value().recent_changes.size() == 12);
    MIRA_CHECK(narrow_state.value().recent_changes.size() == 2);
    MIRA_CHECK(wide_state.value().digest() != narrow_state.value().digest());

    // Each projection is internally consistent under its own options only.
    const auto wide_incremental = fold_all(dataset, wide);
    const auto narrow_incremental = fold_all(dataset, narrow);
    MIRA_CHECK(expect_ok(wide_incremental, "wide incremental") == 0);
    MIRA_CHECK(expect_ok(narrow_incremental, "narrow incremental") == 0);
    MIRA_CHECK(wide_state.value().digest() == wide_incremental.value().digest());
    MIRA_CHECK(narrow_state.value().digest() == narrow_incremental.value().digest());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G5: the default projection is the typed "no supply != guess" state.
// ---------------------------------------------------------------------------

int default_state_unknown_and_legal() {
    const WorldState state;
    MIRA_CHECK(state.validate().has_value());
    MIRA_CHECK(state.foreground_app.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(state.foreground_app.package_name.empty());
    MIRA_CHECK(state.current_page.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(state.current_page.app_id.empty());
    MIRA_CHECK(state.current_page.state_id.empty());
    MIRA_CHECK(state.current_page.confidence == 0.0);
    MIRA_CHECK(state.entities.empty());
    MIRA_CHECK(state.recent_changes.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G5: without recognition/navigation supply the page stays Unknown at
// confidence 0 - observations, tools and expiry never invent a page.
// ---------------------------------------------------------------------------

int no_supply_stays_unknown_zero_confidence() {
    const auto options = dataset_options();
    const auto dataset = frozen_dataset();
    auto with_supply = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(with_supply, "dataset rebuild") == 0);
    MIRA_CHECK(with_supply.value().current_page.status != WorldBeliefStatus::Unknown);

    // The same supplies minus the page-evidence records (screen, navigation,
    // verification): the page is and stays Unknown with zero confidence. The
    // verification record is dropped with them - settling an Unknown page is
    // the explicit StaleTargetMissing failure, never a silent insert.
    std::vector<WorldStateInput> pageless;
    for (const auto &record : dataset) {
        if (std::get_if<WorldScreenStateInput>(&record) == nullptr &&
            std::get_if<WorldNavigationInput>(&record) == nullptr &&
            std::get_if<WorldVerificationInput>(&record) == nullptr) {
            pageless.push_back(record);
        }
    }
    auto pageless_state = rebuild_world_state(pageless, options);
    MIRA_CHECK(expect_ok(pageless_state, "pageless rebuild") == 0);
    MIRA_CHECK(pageless_state.value().current_page.status == WorldBeliefStatus::Unknown);
    MIRA_CHECK(pageless_state.value().current_page.confidence == 0.0);
    MIRA_CHECK(pageless_state.value().current_page.app_id.empty());
    MIRA_CHECK(!pageless_state.value().entities.empty());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G5: Stale is never auto-promoted - only fresh evidence (recognition,
// successful navigation, verification) may restore Believed; entry-only and
// expiry updates never upgrade a stale projection.
// ---------------------------------------------------------------------------

int stale_never_promoted_without_new_evidence() {
    const WorldStateOptions options;

    // Expire a believed page into Stale (0.8 -> 0.4).
    WorldState state;
    state.current_page = believed_page("app.A", "home", 0.8, 1);
    auto staled = apply_expiry(std::move(state), make_expiry(100), options);
    MIRA_CHECK(expect_ok(staled, "page expiry") == 0);
    MIRA_CHECK(staled.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(staled.value().current_page.confidence == 0.4);

    // Tool settlement (entry-only) leaves the page Stale.
    auto tool_only = apply_tool_settled(
        std::move(staled).value(), make_tool("toolref:delta.render", "completed", 101), options);
    MIRA_CHECK(expect_ok(tool_only, "tool after stale") == 0);
    MIRA_CHECK(tool_only.value().current_page.status == WorldBeliefStatus::Stale);

    // Another expiry leaves it Stale (already-Stale never re-decays, never
    // upgrades).
    auto expiry_again = apply_expiry(std::move(tool_only).value(), make_expiry(200), options);
    MIRA_CHECK(expect_ok(expiry_again, "expiry after stale") == 0);
    MIRA_CHECK(expiry_again.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(expiry_again.value().current_page.confidence == 0.4);

    // A failed navigation also does not upgrade.
    auto nav_failed = apply_navigation(
        std::move(expiry_again).value(),
        make_navigation("app.A", "home", "elsewhere", "t", false, 0.1, 201), options);
    MIRA_CHECK(expect_ok(nav_failed, "failed nav after stale") == 0);
    MIRA_CHECK(nav_failed.value().current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(nav_failed.value().current_page.confidence == 0.4);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G5: the public face has no authorization outlet - no security.hpp
// dependency and no capability/permission type surface (header face
// assertion; the architecture policy check runs in the script's gate set).
// ---------------------------------------------------------------------------

int read_only_face_no_authorization_outlet() {
    return assert_header_absent({"#include <mira/security.hpp>", "Capability", "Permission",
                                 "CapabilityGrant", "PermissionGrant"},
                                "read-only projection face");
}

} // namespace mira::m27

int main(int argc, char **argv) {
    const struct {
        const char *name;
        int (*fn)();
    } cases[] = {
        // WS-G2
        {"options_validate_rejections", mira::m27::options_validate_rejections},
        {"value_validate_rejections", mira::m27::value_validate_rejections},
        {"observation_operator_matrix", mira::m27::observation_operator_matrix},
        {"screen_state_operator_zero_conflict", mira::m27::screen_state_operator_zero_conflict},
        {"navigation_operator_matrix", mira::m27::navigation_operator_matrix},
        {"tool_settled_operator_matrix", mira::m27::tool_settled_operator_matrix},
        {"verification_operator_matrix", mira::m27::verification_operator_matrix},
        {"expiry_operator_matrix", mira::m27::expiry_operator_matrix},
        {"purity_clock_independence_and_strong_consistency",
         mira::m27::purity_clock_independence_and_strong_consistency},
        // WS-G4
        {"eviction_order_grid", mira::m27::eviction_order_grid},
        {"insert_overflow_eviction_replayable", mira::m27::insert_overflow_eviction_replayable},
        {"ring_fifo_capacity", mira::m27::ring_fifo_capacity},
        {"summary_bound_rejected_zero_change", mira::m27::summary_bound_rejected_zero_change},
        {"options_enter_rebuild_recipe", mira::m27::options_enter_rebuild_recipe},
        // WS-G5
        {"default_state_unknown_and_legal", mira::m27::default_state_unknown_and_legal},
        {"no_supply_stays_unknown_zero_confidence",
         mira::m27::no_supply_stays_unknown_zero_confidence},
        {"stale_never_promoted_without_new_evidence",
         mira::m27::stale_never_promoted_without_new_evidence},
        {"read_only_face_no_authorization_outlet",
         mira::m27::read_only_face_no_authorization_outlet},
        // WS-G3 (m27_wire_rebuild.cpp)
        {"frozen_dataset_digest_anchor", mira::m27::frozen_dataset_digest_anchor},
        {"rebuild_equals_incremental_advance", mira::m27::rebuild_equals_incremental_advance},
        {"prefix_replay_equivalence", mira::m27::prefix_replay_equivalence},
        {"prune_within_capacity_noop_and_outside_recipe",
         mira::m27::prune_within_capacity_noop_and_outside_recipe},
        {"cross_thread_pipeline_replay_byte_identical",
         mira::m27::cross_thread_pipeline_replay_byte_identical},
        // WS-G6 (m27_wire_rebuild.cpp)
        {"wire_round_trip_byte_identical", mira::m27::wire_round_trip_byte_identical},
        {"wire_version_policy_dec002", mira::m27::wire_version_policy_dec002},
        {"wire_fail_closed_reader", mira::m27::wire_fail_closed_reader},
        {"wire_key_sets_exactly_frozen", mira::m27::wire_key_sets_exactly_frozen},
        {"error_domain_eight_codes_triple_reachable",
         mira::m27::error_domain_eight_codes_triple_reachable},
    };
    // Optional case-name filter (harness convenience for triage; the default
    // run executes every case and stops at the first failure).
    std::string_view filter;
    if (argc > 1) {
        filter = argv[1];
    }
    for (const auto &entry : cases) {
        if (!filter.empty() && filter != entry.name) {
            continue;
        }
        if (const int code = entry.fn(); code != 0) {
            std::cerr << "m27_world_state_test: case failed: " << entry.name << '\n';
            return code;
        }
    }
    std::cout << "m27_world_state_test: OK (" << sizeof(cases) / sizeof(cases[0]) << " cases)\n";
    return 0;
}
