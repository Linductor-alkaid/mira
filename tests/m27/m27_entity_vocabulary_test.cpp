// M27 (DEC-041 first stage) entity-vocabulary IVA gate matrix - WS-G1
// (milestone plan §6; vocabulary freeze: docs/design/entity_vocabulary_design.md
// §2/§3, DEC-046). Cases:
//
//   - §2.4 layer (a): is_workflow_event_type accepts exactly the 20 frozen
//     CamelCase type names and rejects names outside the closed set;
//   - §2.4 layer (b): the 20 frozen mira.workflow.<kebab>.v1 schema names are
//     produced exactly by each event's serialization artifact (the "schema"
//     member of the to_event_payload data JSON), one-to-one with the type
//     names; the nine mira.policy.*.v1 names match policy_event_schema_name;
//   - §2.1: ElementRef evidence with a nil observation_id or an invalid space
//     is rejected on the projection input path (VocabularyViolation);
//   - §2.2: empty state_id/app_id inputs are rejected (VocabularyViolation);
//   - §2.3: the authority parser parse_tool_reference (workflow module) drives
//     the accept/reject smoke while the core keeps the frozen registry-level
//     check only (non-empty, <= 256 bytes) - core does not replicate syntax;
//   - §3 reverse face: MemoryScope.subject_id / TemporalPolicy entity_key are
//     not in the catalog and the projection input closed set has no
//     entity_key-shaped alternative (compile-face + header scan).

#include "m27_support.hpp"

#include <mira/json.hpp>
#include <mira/temporal_policy.hpp>
#include <mira/tool_reference.hpp>
#include <mira/workflow_events.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace mira::m27 {
namespace {

// ---------------------------------------------------------------------------
// Frozen §2.4 catalog: type name (closed set a) <-> wire schema name (list b).
// Single frozen list per the vocabulary design; keep in table order.
// ---------------------------------------------------------------------------

struct WorkflowCatalogEntry final {
    std::string_view type;
    std::string_view schema;
};

inline const WorkflowCatalogEntry kWorkflowCatalog[] = {
    {"WorkflowRunStarted", "mira.workflow.run-started.v1"},
    {"WorkflowStepStarted", "mira.workflow.step-started.v1"},
    {"WorkflowStepSettled", "mira.workflow.step-settled.v1"},
    {"WorkflowRunSettled", "mira.workflow.run-settled.v1"},
    {"WorkflowPatchProposed", "mira.workflow.patch-proposed.v1"},
    {"WorkflowPatchApplied", "mira.workflow.patch-applied.v1"},
    {"WorkflowPatchRejected", "mira.workflow.patch-rejected.v1"},
    {"WorkflowPolicySwitched", "mira.workflow.policy-switched.v1"},
    {"WorkflowDecisionRaised", "mira.workflow.decision-raised.v1"},
    {"WorkflowDecisionResolved", "mira.workflow.decision-resolved.v1"},
    {"WorkflowPublishProposed", "mira.workflow.publish-proposed.v1"},
    {"WorkflowPublishApplied", "mira.workflow.publish-applied.v1"},
    {"WorkflowPublishRejected", "mira.workflow.publish-rejected.v1"},
    {"WorkflowNavigationPlanned", "mira.workflow.navigation-planned.v1"},
    {"WorkflowNavigationObserved", "mira.workflow.navigation-observed.v1"},
    {"WorkflowEpisodeRecorded", "mira.workflow.episode-recorded.v1"},
    {"WorkflowLessonRecorded", "mira.workflow.lesson-recorded.v1"},
    {"WorkflowRecoveryAttempted", "mira.workflow.recovery-attempted.v1"},
    {"WorkflowToolCompatDegraded", "mira.workflow.tool-compat-degraded.v1"},
    {"WorkflowProceduresSynced", "mira.workflow.procedures-synced.v1"},
};

// Serialization artifact for catalog position index: default-constructed
// events are legal payloads; only the schema member is asserted here.
[[nodiscard]] EventPayload catalog_payload(std::size_t index) {
    switch (index) {
    case 0:
        return to_event_payload(WorkflowRunStartedEvent{});
    case 1:
        return to_event_payload(WorkflowStepStartedEvent{});
    case 2:
        return to_event_payload(WorkflowStepSettledEvent{});
    case 3:
        return to_event_payload(WorkflowRunSettledEvent{});
    case 4:
        return to_event_payload(WorkflowPatchProposedEvent{});
    case 5:
        return to_event_payload(WorkflowPatchAppliedEvent{});
    case 6:
        return to_event_payload(WorkflowPatchRejectedEvent{});
    case 7:
        return to_event_payload(WorkflowPolicySwitchedEvent{});
    case 8:
        return to_event_payload(WorkflowDecisionRaisedEvent{});
    case 9:
        return to_event_payload(WorkflowDecisionResolvedEvent{});
    case 10:
        return to_event_payload(WorkflowPublishProposedEvent{});
    case 11:
        return to_event_payload(WorkflowPublishAppliedEvent{});
    case 12:
        return to_event_payload(WorkflowPublishRejectedEvent{});
    case 13:
        return to_event_payload(WorkflowNavigationPlannedEvent{});
    case 14:
        return to_event_payload(WorkflowNavigationObservedEvent{});
    case 15:
        return to_event_payload(WorkflowEpisodeRecordedEvent{});
    case 16:
        return to_event_payload(WorkflowLessonRecordedEvent{});
    case 17:
        return to_event_payload(WorkflowRecoveryAttemptedEvent{});
    case 18:
        return to_event_payload(WorkflowToolCompatDegradedEvent{});
    default:
        return to_event_payload(WorkflowProceduresSyncedEvent{});
    }
}

// Frozen §2.4 mira.policy.*.v1 subset (nine; names per the M26 §4.4 event
// table, referenced back by the vocabulary design).
struct PolicyCatalogEntry final {
    PolicyEventType type;
    std::string_view schema;
};

inline const PolicyCatalogEntry kPolicyCatalog[] = {
    {PolicyEventType::PolicyActivated, "mira.policy.policy-activated.v1"},
    {PolicyEventType::PolicyDeactivated, "mira.policy.policy-deactivated.v1"},
    {PolicyEventType::RuleCandidateInduced, "mira.policy.rule-candidate-induced.v1"},
    {PolicyEventType::RuleTestingResulted, "mira.policy.rule-testing-resulted.v1"},
    {PolicyEventType::RulePromoted, "mira.policy.rule-promoted.v1"},
    {PolicyEventType::RuleDemoted, "mira.policy.rule-demoted.v1"},
    {PolicyEventType::RuleConflicted, "mira.policy.rule-conflicted.v1"},
    {PolicyEventType::RuleTriggered, "mira.policy.rule-triggered.v1"},
    {PolicyEventType::PolicyEscalatedToAgent, "mira.policy.policy-escalated-to-agent.v1"},
};

} // namespace

// ---------------------------------------------------------------------------
// §2.4 layer (a): type-name closed set.
// ---------------------------------------------------------------------------

int workflow_type_closed_set_twenty() {
    int seen = 0;
    for (const auto &entry : kWorkflowCatalog) {
        MIRA_CHECK(is_workflow_event_type(entry.type));
        ++seen;
    }
    MIRA_CHECK(seen == 20);
    for (const std::string_view outsider :
         {"WorkflowRunStartedX", "workflow.run-started.v1", "", "run-started",
          "WorkflowRunStarted2", "workflow_run_started", "WorkflowUnknownEvent"}) {
        MIRA_CHECK(!is_workflow_event_type(outsider));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// §2.4 layer (b): schema-name list via the serialization artifacts.
// ---------------------------------------------------------------------------

int workflow_schema_catalog_twenty() {
    int seen = 0;
    for (std::size_t index = 0; index < sizeof(kWorkflowCatalog) / sizeof(kWorkflowCatalog[0]);
         ++index) {
        const auto &entry = kWorkflowCatalog[index];
        const EventPayload payload = catalog_payload(index);
        MIRA_CHECK(payload.type == entry.type);
        const auto parsed = parse_json(payload.data);
        MIRA_CHECK(expect_ok(parsed, "payload data json") == 0);
        const auto *schema = parsed.value().find("schema");
        MIRA_CHECK(schema != nullptr);
        MIRA_CHECK(schema->is_string());
        MIRA_CHECK(*schema->as_string() == entry.schema);
        MIRA_CHECK(is_workflow_event_type(payload.type));
        ++seen;
    }
    MIRA_CHECK(seen == 20);
    return 0;
}

// ---------------------------------------------------------------------------
// §2.4: the nine mira.policy.*.v1 names via policy_event_schema_name.
// ---------------------------------------------------------------------------

int policy_schema_nine_exact() {
    int seen = 0;
    for (const auto &entry : kPolicyCatalog) {
        MIRA_CHECK(policy_event_schema_name(entry.type) == entry.schema);
        ++seen;
    }
    MIRA_CHECK(seen == 9);
    return 0;
}

// ---------------------------------------------------------------------------
// §2.1: ElementRef evidence fail-closed on the projection input path.
// ---------------------------------------------------------------------------

int element_ref_fail_closed_on_input_path() {
    const WorldStateOptions options;
    const WorldState state;

    WorldObservationInput nil_input_id = make_observation(0x21, make_app("app.A", "act.B"), {}, 1);
    nil_input_id.observation_id = ObservationId{};
    MIRA_CHECK(expect_error(apply_world_observation(state, nil_input_id, options),
                            WorldStateDomainCode::VocabularyViolation,
                            "nil observation input id") == 0);

    const auto valid = make_observation(0x22, make_app("app.A", "act.B"), {make_element(5)}, 1);
    MIRA_CHECK(expect_ok(apply_world_observation(state, valid, options), "valid element") == 0);

    WorldObservationInput nil_element_id = valid;
    nil_element_id.elements.push_back(make_element(6));
    nil_element_id.elements.back().observation_id = ObservationId{};
    MIRA_CHECK(expect_error(apply_world_observation(state, nil_element_id, options),
                            WorldStateDomainCode::VocabularyViolation,
                            "nil element observation_id") == 0);

    WorldObservationInput nil_space = valid;
    nil_space.elements.front().space = CoordinateSpaceId{};
    MIRA_CHECK(expect_error(apply_world_observation(state, nil_space, options),
                            WorldStateDomainCode::VocabularyViolation, "nil element space") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// §2.2: (app_id, state_id) identity domain fail-closed on the input path.
// ---------------------------------------------------------------------------

int page_identity_empty_ids_rejected() {
    const WorldStateOptions options;
    const WorldState state;

    WorldScreenStateInput no_state;
    no_state.app_id = "app.A";
    no_state.state_id.clear();
    no_state.confidence = 0.5;
    no_state.recognized_at = at_ns(1);
    MIRA_CHECK(expect_error(apply_screen_state(state, no_state, options),
                            WorldStateDomainCode::VocabularyViolation, "empty state_id") == 0);

    WorldScreenStateInput no_app;
    no_app.app_id.clear();
    no_app.state_id = "state_home";
    no_app.confidence = 0.5;
    no_app.recognized_at = at_ns(1);
    MIRA_CHECK(expect_error(apply_screen_state(state, no_app, options),
                            WorldStateDomainCode::VocabularyViolation, "empty app_id") == 0);

    WorldNavigationInput no_nav_app;
    no_nav_app.app_id.clear();
    no_nav_app.from_state = "a";
    no_nav_app.to_state = "b";
    no_nav_app.observed_at = at_ns(1);
    MIRA_CHECK(expect_error(apply_navigation(state, no_nav_app, options),
                            WorldStateDomainCode::VocabularyViolation,
                            "empty navigation app_id") == 0);

    WorldNavigationInput empty_pair;
    empty_pair.app_id = "app.A";
    empty_pair.from_state.clear();
    empty_pair.to_state = "b";
    empty_pair.observed_at = at_ns(1);
    MIRA_CHECK(expect_error(apply_navigation(state, empty_pair, options),
                            WorldStateDomainCode::VocabularyViolation, "empty from_state") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// §2.3: authority parser smoke + core registry-level check + the split.
// ---------------------------------------------------------------------------

int tool_ref_authority_and_registry_split() {
    const WorldStateOptions options;
    const WorldState state;

    // Authority parser (workflow module) accept samples.
    MIRA_CHECK(parse_tool_reference("toolref:delta.render").has_value());
    const std::string pinned =
        std::string("toolref:epsilon.tool@") + digest_string("mira://m27/pin").to_string();
    MIRA_CHECK(parse_tool_reference(pinned).has_value());

    // Authority parser reject samples (syntax is NOT replicated in core).
    for (const std::string_view bad :
         {"delta.render", "toolref:", "toolref:UPPER.name", "toolref:a.b extra", "toolref:a b",
          "notref:a.b", "toolref:a.b@c", "toolref:a.b@ZZZZ"}) {
        MIRA_CHECK(!parse_tool_reference(bad).has_value());
    }
    const std::string over_length = "toolref:" + std::string(249, 'a'); // 257 bytes
    MIRA_CHECK(!parse_tool_reference(over_length).has_value());

    // Core registry-level check: empty and over-256-byte refs rejected.
    WorldToolSettledInput empty_ref = make_tool("", "completed", 1);
    MIRA_CHECK(expect_error(apply_tool_settled(state, empty_ref, options),
                            WorldStateDomainCode::VocabularyViolation, "empty tool_ref") == 0);
    WorldToolSettledInput over_ref = make_tool(over_length, "completed", 1);
    MIRA_CHECK(expect_error(apply_tool_settled(state, over_ref, options),
                            WorldStateDomainCode::VocabularyViolation, "over-long tool_ref") == 0);
    const std::string exact_256 = "toolref:" + std::string(248, 'a'); // 256 bytes
    MIRA_CHECK(expect_ok(apply_tool_settled(state, make_tool(exact_256, "completed", 1), options),
                         "256-byte tool_ref boundary") == 0);

    // Split of authority: a syntax-rejected but registry-legal reference still
    // passes the core check (syntax stays with the workflow-side parser), and
    // every authority-accepted reference passes the registry check.
    MIRA_CHECK(!parse_tool_reference("notref:a.b").has_value());
    MIRA_CHECK(
        expect_ok(apply_tool_settled(state, make_tool("notref:a.b", "completed", 1), options),
                  "registry-level accepts parser-rejected shape") == 0);
    MIRA_CHECK(expect_ok(apply_tool_settled(
                             state, make_tool("toolref:delta.render", "completed", 1), options),
                         "registry-level accepts authority-accepted ref") == 0);
    MIRA_CHECK(expect_ok(apply_tool_settled(state, make_tool(pinned, "completed", 1), options),
                         "registry-level accepts pinned ref") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// §3 reverse face: entity_key / subject_id stay outside the projection input
// closed set (compile face) and out of the public header (scan face).
// ---------------------------------------------------------------------------

static_assert(std::variant_size_v<WorldStateInput> == 6,
              "world state input closed set is exactly the six frozen records");
static_assert(
    std::is_same_v<std::variant_alternative_t<0, WorldStateInput>, WorldObservationInput>);
static_assert(
    std::is_same_v<std::variant_alternative_t<1, WorldStateInput>, WorldScreenStateInput>);
static_assert(std::is_same_v<std::variant_alternative_t<2, WorldStateInput>, WorldNavigationInput>);
static_assert(
    std::is_same_v<std::variant_alternative_t<3, WorldStateInput>, WorldToolSettledInput>);
static_assert(
    std::is_same_v<std::variant_alternative_t<4, WorldStateInput>, WorldVerificationInput>);
static_assert(std::is_same_v<std::variant_alternative_t<5, WorldStateInput>, WorldExpiryInput>);

int entity_key_subject_id_not_in_catalog() {
    return assert_header_absent({"entity_key", "subject_id"}, "vocabulary §3 reverse assertion");
}

} // namespace mira::m27

int main() {
    const struct {
        const char *name;
        int (*fn)();
    } cases[] = {
        {"workflow_type_closed_set_twenty", mira::m27::workflow_type_closed_set_twenty},
        {"workflow_schema_catalog_twenty", mira::m27::workflow_schema_catalog_twenty},
        {"policy_schema_nine_exact", mira::m27::policy_schema_nine_exact},
        {"element_ref_fail_closed_on_input_path", mira::m27::element_ref_fail_closed_on_input_path},
        {"page_identity_empty_ids_rejected", mira::m27::page_identity_empty_ids_rejected},
        {"tool_ref_authority_and_registry_split", mira::m27::tool_ref_authority_and_registry_split},
        {"entity_key_subject_id_not_in_catalog", mira::m27::entity_key_subject_id_not_in_catalog},
    };
    for (const auto &entry : cases) {
        if (const int code = entry.fn(); code != 0) {
            std::cerr << "m27_entity_vocabulary_test: case failed: " << entry.name << '\n';
            return code;
        }
    }
    std::cout << "m27_entity_vocabulary_test: OK (" << sizeof(cases) / sizeof(cases[0])
              << " cases)\n";
    return 0;
}
