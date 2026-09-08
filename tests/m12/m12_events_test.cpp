// M12-05: the two navigation events (DEC-028 §4). Payloads round trip, fail
// closed on unknown fields and schema mismatches, the closed event set
// accepts the new types, DryRun emits only WorkflowNavigationPlanned, and
// offline replay rebuilds from the events without side effects.

#include "support/m12_support.hpp"

#include <mira/replay.hpp>

#include <algorithm>

namespace {

using namespace mira;
using namespace mira::testing;

int navigation_payloads_round_trip() {
    const WorkflowRunId run_id = WorkflowRunId::generate();
    const StepId step_id = StepId::generate();

    WorkflowNavigationPlannedEvent planned;
    planned.run_id = run_id;
    planned.step_id = step_id;
    planned.from_state = "home";
    planned.to_state = "composer";
    planned.edge_count = 2;
    planned.plan_digest = app_model_digest(device_model());
    planned.total_cost = 70.9;
    planned.guards_blocked = 1;
    planned.guards_unevaluable = 0;
    const auto planned_payload = to_event_payload(planned);
    MIRA_CHECK(planned_payload.type == "WorkflowNavigationPlanned");
    MIRA_CHECK(planned_payload.classification == EventClass::State);
    auto reparsed = parse_workflow_navigation_planned(planned_payload);
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(reparsed.value().run_id == run_id);
    MIRA_CHECK(reparsed.value().step_id == step_id);
    MIRA_CHECK(reparsed.value().from_state == "home");
    MIRA_CHECK(reparsed.value().to_state == "composer");
    MIRA_CHECK(reparsed.value().edge_count == 2);
    MIRA_CHECK(reparsed.value().plan_digest == planned.plan_digest);
    MIRA_CHECK(reparsed.value().total_cost == 70.9);
    MIRA_CHECK(reparsed.value().guards_blocked == 1);
    MIRA_CHECK(reparsed.value().guards_unevaluable == 0);

    WorkflowNavigationObservedEvent observed;
    observed.run_id = run_id;
    observed.step_id = step_id;
    observed.transition_id = "t-open-chats";
    observed.from_state = "home";
    observed.to_state = "chat_list";
    observed.success = true;
    observed.confidence = 2.0 / 3.0;
    const auto observed_payload = to_event_payload(observed);
    MIRA_CHECK(observed_payload.type == "WorkflowNavigationObserved");
    MIRA_CHECK(observed_payload.classification == EventClass::State);
    auto reobserved = parse_workflow_navigation_observed(observed_payload);
    MIRA_CHECK(reobserved.has_value());
    MIRA_CHECK(reobserved.value().transition_id == "t-open-chats");
    MIRA_CHECK(reobserved.value().success);
    MIRA_CHECK(reobserved.value().confidence == 2.0 / 3.0);
    return 0;
}

int navigation_payloads_fail_closed() {
    const WorkflowRunId run_id = WorkflowRunId::generate();
    WorkflowNavigationPlannedEvent planned;
    planned.run_id = run_id;
    planned.step_id = StepId::generate();
    planned.from_state = "home";
    planned.to_state = "composer";
    planned.edge_count = 1;
    planned.plan_digest = app_model_digest(device_model());
    planned.total_cost = 1.0;

    // Unknown field.
    auto with_unknown = parse_json(to_event_payload(planned).data).value();
    with_unknown.set("mystery", JsonValue{1});
    EventPayload extended = to_event_payload(planned);
    extended.data = to_json_string(with_unknown);
    MIRA_CHECK(!parse_workflow_navigation_planned(extended).has_value());

    // Schema mismatch.
    EventPayload mismatched = to_event_payload(planned);
    auto schema_swapped = parse_json(mismatched.data).value();
    schema_swapped.set("schema", JsonValue{"mira.workflow.navigation-planned.v2"});
    mismatched.data = to_json_string(schema_swapped);
    MIRA_CHECK(!parse_workflow_navigation_planned(mismatched).has_value());

    // Malformed members: negative counts, unbounded ids, out-of-range
    // confidence.
    for (const auto &[key, value] : std::array<std::pair<const char *, JsonValue>, 4>{
             std::pair{"edge_count", JsonValue{static_cast<std::int64_t>(-1)}},
             std::pair{"from_state", JsonValue{""}},
             std::pair{"total_cost", JsonValue{-0.5}},
             std::pair{"guards_blocked", JsonValue{"two"}},
         }) {
        auto mutated = parse_json(to_event_payload(planned).data).value();
        mutated.set(key, value);
        EventPayload payload = to_event_payload(planned);
        payload.data = to_json_string(mutated);
        MIRA_CHECK(!parse_workflow_navigation_planned(payload).has_value());
    }

    WorkflowNavigationObservedEvent observed;
    observed.run_id = run_id;
    observed.step_id = StepId::generate();
    observed.transition_id = "t-open-chats";
    observed.from_state = "home";
    observed.to_state = "chat_list";
    observed.success = false;
    observed.confidence = 1.5; // out of [0,1]
    MIRA_CHECK(!parse_workflow_navigation_observed(to_event_payload(observed)).has_value());

    // The closed event set knows both types.
    MIRA_CHECK(is_workflow_event_type("WorkflowNavigationPlanned"));
    MIRA_CHECK(is_workflow_event_type("WorkflowNavigationObserved"));
    MIRA_CHECK(!is_workflow_event_type("WorkflowNavigationGuessed"));
    return 0;
}

int dry_run_emits_only_planned_and_dispatching_emits_both() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chat", "chat_view")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("deep_link", "chat_view")));

    // DryRun: one Planned event, no Observed.
    auto dry_workflow = fixture.make_workflow();
    MIRA_CHECK(dry_workflow->set_navigation_context(device_model(), device.provider()).has_value());
    auto dry_definition = base_definition("m12-dry-events");
    dry_definition.steps = {navigate_step("composer")};
    const auto dry_run = dry_workflow->create_run(dry_definition, JsonValue{JsonValue::Object{}},
                                                  WorkflowPolicy::DryRun);
    MIRA_CHECK(dry_run.has_value());
    MIRA_CHECK(dry_workflow->execute_run(dry_run.value().run_id, drive_context()).value().state ==
               WorkflowRunState::Completed);
    const auto dry_types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::count(dry_types.begin(), dry_types.end(), "WorkflowNavigationPlanned") == 1);
    MIRA_CHECK(std::count(dry_types.begin(), dry_types.end(), "WorkflowNavigationObserved") == 0);

    // Dispatching: one Planned plus one Observed per traversed edge, carrying
    // the post-update confidence. The store is shared, so the DryRun phase's
    // events accumulate: totals grow by exactly the dispatched amounts.
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(workflow->set_navigation_context(device_model(), device.provider()).has_value());
    auto definition = base_definition("m12-events");
    definition.steps = {navigate_step("composer")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context()).value().state ==
               WorkflowRunState::Completed);
    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowNavigationPlanned") == 2);
    MIRA_CHECK(std::count(types.begin(), types.end(), "WorkflowNavigationObserved") == 2);

    // The observed payloads rebuild the confidence projection (W-03): both
    // edges report verified_count=1 confidence 2/3.
    EventQuery query;
    query.session_id = fixture.session_id_;
    const auto page = fixture.events_->read(query);
    MIRA_CHECK(page.has_value());
    int successes = 0;
    for (const auto &envelope : page.value().events) {
        if (envelope.payload.type != "WorkflowNavigationObserved") {
            continue;
        }
        auto event = parse_workflow_navigation_observed(envelope.payload);
        MIRA_CHECK(event.has_value());
        MIRA_CHECK(event.value().success);
        MIRA_CHECK(event.value().confidence == 2.0 / 3.0);
        ++successes;
    }
    MIRA_CHECK(successes == 2);
    return 0;
}

int failed_arrival_observes_with_failure_confidence() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.stuck_action("open_chat")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    auto workflow = fixture.make_workflow();
    auto model = device_model();
    model.transitions.pop_back(); // no shortcut: the stuck edge is the route
    MIRA_CHECK(workflow->set_navigation_context(model, device.provider()).has_value());

    auto definition = base_definition("m12-fail-events");
    definition.steps = {navigate_step("chat_view")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->execute_run(created.value().run_id, drive_context()).value().state ==
               WorkflowRunState::Failed);

    EventQuery query;
    query.session_id = fixture.session_id_;
    const auto page = fixture.events_->read(query);
    MIRA_CHECK(page.has_value());
    bool saw_failure = false;
    for (const auto &envelope : page.value().events) {
        if (envelope.payload.type != "WorkflowNavigationObserved") {
            continue;
        }
        auto event = parse_workflow_navigation_observed(envelope.payload);
        MIRA_CHECK(event.has_value());
        if (!event.value().success) {
            saw_failure = true;
            MIRA_CHECK(event.value().transition_id == "t-open-chat");
            MIRA_CHECK(event.value().confidence == 1.0 / 3.0);
        }
    }
    MIRA_CHECK(saw_failure);
    return 0;
}

int offline_replay_of_navigation_events_has_no_side_effects() {
    // W-08 assertion for the new payload family: events are rebuildable
    // facts and the replay environment never dispatches real input.
    MemoryEventStore store;

    WorkflowNavigationPlannedEvent planned;
    planned.run_id = WorkflowRunId::generate();
    planned.step_id = StepId::generate();
    planned.from_state = "home";
    planned.to_state = "composer";
    planned.edge_count = 2;
    planned.plan_digest = app_model_digest(device_model());
    planned.total_cost = 70.9;

    WorkflowNavigationObservedEvent observed;
    observed.run_id = planned.run_id;
    observed.step_id = planned.step_id;
    observed.transition_id = "t-deep-link";
    observed.from_state = "home";
    observed.to_state = "chat_view";
    observed.success = true;
    observed.confidence = 2.0 / 3.0;

    const SessionId session = SessionId::generate();
    for (const auto &payload : {to_event_payload(planned), to_event_payload(observed)}) {
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = RuntimeId::generate();
        request.session_id = session;
        request.payload = payload;
        MIRA_CHECK(store.append(request).has_value());
    }

    EventQuery query;
    query.session_id = session;
    const auto page = store.read(query);
    MIRA_CHECK(page.has_value());
    MIRA_CHECK(page.value().events.size() == 2);
    for (const auto &envelope : page.value().events) {
        MIRA_CHECK(is_workflow_event_type(envelope.payload.type));
    }
    auto replanned = parse_workflow_navigation_planned(page.value().events[0].payload);
    MIRA_CHECK(replanned.has_value());
    MIRA_CHECK(replanned.value().edge_count == 2);
    auto reobserved = parse_workflow_navigation_observed(page.value().events[1].payload);
    MIRA_CHECK(reobserved.has_value());
    MIRA_CHECK(reobserved.value().confidence == 2.0 / 3.0);

    // Replay holds no receipts for workflow input: a dispatch attempt
    // settles uncertain instead of executing.
    OfflineReplayEnvironment replay{{}, {}, EnvironmentCapabilities{}};
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    InputSequence sequence_input;
    sequence_input.events.push_back(InputEvent{.kind = "tap", .payload = "0.5,0.5"});
    const auto dispatch = replay.execute(sequence_input, context);
    MIRA_CHECK(!dispatch.has_value());
    MIRA_CHECK(dispatch.error().code == ErrorCode::ExecutionUncertain);
    MIRA_CHECK(!replay.interrupted());
    return 0;
}

} // namespace

int main() {
    const auto scenarios =
        std::to_array({navigation_payloads_round_trip, navigation_payloads_fail_closed,
                       dry_run_emits_only_planned_and_dispatching_emits_both,
                       failed_arrival_observes_with_failure_confidence,
                       offline_replay_of_navigation_events_has_no_side_effects});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
