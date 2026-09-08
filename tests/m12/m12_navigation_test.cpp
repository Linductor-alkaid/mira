// M12-03/M12-04 (runtime half): Navigate admission and execution (DEC-028
// §3). Without a navigation context the M9 admission rejection stays; with
// one, Navigate steps resolve, dispatch edges through the tool channel,
// verify arrival against the host screen reading, write confidence back and
// evaluate screen_state predicates.

#include "support/m12_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace {

using namespace mira;
using namespace mira::testing;

// An edge action that blocks until released; on release it moves the device.
// Used to pin pause/cancel convergence inside a navigation step.
class GatedEdgeAction final {
  public:
    GatedEdgeAction(ScreenDevice &device, std::string to) : device_(device), to_(std::move(to)) {}

    [[nodiscard]] BuiltinToolRegistration registration(const std::string &wire) {
        BuiltinToolRegistration registration;
        registration.spec.wire_name = wire;
        registration.spec.description = "m12 gated edge action";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {},
            "additionalProperties": false
        })json")
                                                             .value()};
        registration.spec.has_side_effects = true;
        registration.handler = [this](const JsonValue &,
                                      const OperationContext &context) -> Result<JsonValue> {
            ++entries_;
            // Poll like the m9 GatedTool: the cancellation probe flips from
            // another thread without notifying anything here.
            while (!released_.load(std::memory_order_relaxed)) {
                if (context.cancelled()) {
                    Error error;
                    error.code = ErrorCode::Cancelled;
                    error.domain = "mira.test";
                    error.safe_message = "gated edge released by cancellation";
                    return error;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            device_.set_current(to_);
            return JsonValue{"moved"};
        };
        return registration;
    }

    void release() { released_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] int entries() const { return entries_.load(); }

  private:
    ScreenDevice &device_;
    std::string to_;
    std::atomic<bool> released_{false};
    std::atomic<int> entries_{0};
};

[[nodiscard]] int install_device(WorkflowRuntime &workflow, ScreenDevice &device,
                                 const AppModel &model = device_model()) {
    return workflow.set_navigation_context(model, device.provider()).has_value() ? 0 : 1;
}

int admission_stays_closed_without_context() {
    WorkflowFixture fixture;
    auto workflow = fixture.make_workflow();
    auto definition = base_definition("m12-nav");
    definition.steps = {navigate_step("composer")};

    // M9 regression: no context installed -> navigate-unresolvable.
    const auto strict =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(!strict.has_value());
    MIRA_CHECK(strict.error().code == ErrorCode::UnsupportedCapability);
    MIRA_CHECK(strict.error().safe_message.find("navigation context") != std::string::npos);

    // DryRun without a context keeps planning by shape (M9 semantics).
    const auto dry =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(dry.has_value());
    const auto driven = workflow->execute_run(dry.value().run_id, drive_context());
    MIRA_CHECK(driven.has_value());
    MIRA_CHECK(driven.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(driven.value().steps[0].safe_summary == "planned navigation");

    // The policy-switch gate stays closed without a context (M10 regression;
    // a Created run is not patch-admissible, and even on admissible states
    // the navigate gate keeps rejecting dispatching targets).
    const auto dry_two =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(dry_two.has_value());
    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::ExecutionPolicy;
    entry.op = WorkflowPatchOp::Set;
    entry.path = "policy";
    entry.value = JsonValue{"strict"};
    const auto switched =
        workflow->patch_run(dry_two.value().run_id, WorkflowPatchId::generate(), {entry});
    MIRA_CHECK(!switched.has_value());

    // Navigate arguments must name a string target, under any policy.
    auto shapeless = base_definition("m12-shape");
    auto step = navigate_step("composer");
    step.arguments = JsonValue{JsonValue::Object{}};
    shapeless.steps = {step};
    const auto invalid =
        workflow->create_run(shapeless, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(!invalid.has_value());
    MIRA_CHECK(invalid.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int navigation_executes_edges_and_arrives() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chat", "chat_view")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("deep_link", "chat_view")));
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    auto definition = base_definition("m12-run");
    definition.steps = {navigate_step("composer")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(result.value().steps[0].disposition == WorkflowStepDisposition::Completed);
    // Default weights route through the deep link shortcut: two dispatches.
    MIRA_CHECK(device.dispatches() == 2);
    MIRA_CHECK(device.current() == "composer");

    // Confidence write-back: every traversed edge got one verification.
    const auto snapshot = workflow->app_model_snapshot();
    MIRA_CHECK(snapshot.has_value());
    int verified = 0;
    for (const auto &transition : snapshot.value().transitions) {
        if (transition.confidence.verified_count == 1) {
            MIRA_CHECK(transition.confidence.confidence == 2.0 / 3.0);
            ++verified;
        }
    }
    MIRA_CHECK(verified == 2);

    // The installed model itself is not mutated: write-back lands on the
    // runtime projection only (the host reinstalls decayed content itself).
    MIRA_CHECK(device_model().transitions[0].confidence.verified_count == 0);
    return 0;
}

int arrival_mismatch_fails_closed_and_writes_back() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    // The second edge dispatches but never moves the screen: arrival is
    // unverified (RULE-05: no blind redispatch of the same edge).
    MIRA_CHECK(register_registration(*fixture.registry_, device.stuck_action("open_chat")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("deep_link", "chat_view")));
    auto workflow = fixture.make_workflow();
    // Suppress the shortcut so the stuck edge is the only route.
    auto model = device_model();
    model.transitions.pop_back();
    MIRA_CHECK(install_device(*workflow, device, model) == 0);

    auto definition = base_definition("m12-stuck");
    definition.steps = {navigate_step("chat_view")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.back().disposition == WorkflowStepDisposition::Failed);
    MIRA_CHECK(result.value().steps.back().safe_summary.find("did not arrive at 'chat_view'") !=
               std::string::npos);

    const auto snapshot = workflow->app_model_snapshot();
    const auto stuck =
        std::find_if(snapshot.value().transitions.begin(), snapshot.value().transitions.end(),
                     [](const AppModelTransition &edge) { return edge.id == "t-open-chat"; });
    MIRA_CHECK(stuck != snapshot.value().transitions.end());
    MIRA_CHECK(stuck->confidence.failure_count == 1);
    MIRA_CHECK(stuck->confidence.verified_count == 0);
    MIRA_CHECK(stuck->confidence.confidence == 1.0 / 3.0);
    return 0;
}

int missing_inputs_fail_closed() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    // No edge tools registered: every case below fails before a dispatch
    // could matter.
    auto workflow = fixture.make_workflow();

    auto definition = base_definition("m12-negative");
    definition.steps = {navigate_step("composer")};

    // Screen provider installed but returns nothing (host has no reading).
    MIRA_CHECK(
        workflow
            ->set_navigation_context(
                device_model(), []() -> std::optional<ScreenStateSnapshot> { return std::nullopt; })
            .has_value());
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.back().safe_summary.find("no current screen state") !=
               std::string::npos);

    // A target the model never declared fails deterministically.
    MIRA_CHECK(install_device(*workflow, device) == 0);
    auto unknown = base_definition("m12-unknown");
    unknown.steps = {navigate_step("mars")};
    const auto run =
        workflow->create_run(unknown, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(run.has_value());
    result = workflow->execute_run(run.value().run_id, drive_context());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.back().safe_summary.find("not declared") != std::string::npos);

    // An edge action naming an unregistered tool fails at dispatch.
    auto orphan = base_definition("m12-orphan");
    orphan.steps = {navigate_step("chat_list")};
    const auto orphaned =
        workflow->create_run(orphan, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(orphaned.has_value());
    result = workflow->execute_run(orphaned.value().run_id, drive_context());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().steps.back().safe_summary.find("not registered") !=
               std::string::npos);
    return 0;
}

int screen_state_predicates_evaluate() {
    WorkflowFixture fixture;
    ScreenDevice device("composer");
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    // eq against the current state resolves satisfied; eq against another
    // state's name is not evaluable and is disclosed, not claimed (DryRun).
    WorkflowPredicate here;
    here.signal = "screen_state:composer";
    here.op = WorkflowPredicateOp::Eq;
    here.value = JsonValue{true};
    WorkflowPredicate elsewhere;
    elsewhere.signal = "screen_state:home";
    elsewhere.op = WorkflowPredicateOp::Eq;
    elsewhere.value = JsonValue{true};

    auto definition = base_definition("m12-predicates");
    definition.steps = {verify_step(here), verify_step(elsewhere)};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(result.value().steps[0].verification == "satisfied");
    MIRA_CHECK(result.value().steps[1].verification == "not_evaluable");
    MIRA_CHECK(result.value().unevaluable_verifications >= 1);

    // exists answers presence directly: an absent state is not satisfied, so
    // a DryRun verification fails honestly.
    WorkflowPredicate absent;
    absent.signal = "screen_state:home";
    absent.op = WorkflowPredicateOp::Exists;
    auto presence = base_definition("m12-presence");
    presence.steps = {verify_step(absent)};
    const auto presence_run =
        workflow->create_run(presence, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(presence_run.has_value());
    const auto presence_result =
        workflow->execute_run(presence_run.value().run_id, drive_context());
    MIRA_CHECK(presence_result.value().state == WorkflowRunState::Failed);

    // The screen_state:current alias carries the state id.
    WorkflowPredicate alias;
    alias.signal = "screen_state:current";
    alias.op = WorkflowPredicateOp::Eq;
    alias.value = JsonValue{"composer"};
    auto aliased = base_definition("m12-alias");
    aliased.steps = {verify_step(alias)};
    const auto alias_run =
        workflow->create_run(aliased, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(alias_run.has_value());
    const auto alias_result = workflow->execute_run(alias_run.value().run_id, drive_context());
    MIRA_CHECK(alias_result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(alias_result.value().steps[0].verification == "satisfied");

    // Without a provider every screen_state predicate stays not evaluable
    // (M8/M9 regression): the run reports the disclosure, not a pass.
    auto bare = fixture.make_workflow();
    auto bare_definition = base_definition("m12-bare");
    bare_definition.steps = {verify_step(here)};
    const auto bare_run =
        bare->create_run(bare_definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(bare_run.has_value());
    const auto bare_result = bare->execute_run(bare_run.value().run_id, drive_context());
    MIRA_CHECK(bare_result.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(bare_result.value().steps[0].verification == "not_evaluable");
    MIRA_CHECK(bare_result.value().unevaluable_verifications >= 1);
    return 0;
}

int dry_run_plans_for_real_without_dispatch() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chat", "chat_view")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("deep_link", "chat_view")));
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    auto definition = base_definition("m12-dry");
    definition.steps = {navigate_step("composer")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(created.has_value());
    const auto result = workflow->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().state == WorkflowRunState::Completed);
    // Planning happened (the summary reports the real edge count)...
    MIRA_CHECK(result.value().steps[0].safe_summary.find("2 edge(s)") != std::string::npos);
    // ...but nothing dispatched and no confidence moved (RULE-10).
    MIRA_CHECK(device.dispatches() == 0);
    const auto snapshot = workflow->app_model_snapshot();
    MIRA_CHECK(snapshot.value().transitions[0].confidence.verified_count == 0);

    // A DryRun plan that cannot exist fails the step: the publish gate
    // therefore constrains navigation reachability (DEC-028 §3).
    auto island = device_model();
    island.states.push_back(ui_state("settings", "Settings"));
    MIRA_CHECK(workflow->set_app_model(island).has_value());
    auto unreachable = base_definition("m12-unreachable");
    unreachable.steps = {navigate_step("settings")};
    const auto lost =
        workflow->create_run(unreachable, JsonValue{JsonValue::Object{}}, WorkflowPolicy::DryRun);
    MIRA_CHECK(lost.has_value());
    const auto lost_result = workflow->execute_run(lost.value().run_id, drive_context());
    MIRA_CHECK(lost_result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(lost_result.value().steps.back().safe_summary.find("no path") != std::string::npos);
    return 0;
}

int policy_gate_relaxes_with_context() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    CountingTool failing;
    failing.failures_first = 1;
    MIRA_CHECK(register_registration(*fixture.registry_, failing.registration("flaky", "sent")));
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    // An Interactive run with navigate steps parks at a StepFailure decision
    // point (the patch-admissible wait state). Switching it to another
    // dispatching policy passes the navigate gate the M10 phase kept closed;
    // admission itself was only possible because a context is installed.
    auto definition = base_definition("m12-switch");
    definition.steps = {tool_step("flaky", std::nullopt), navigate_step("chat_list")};
    const auto created = workflow->create_run(definition, JsonValue{JsonValue::Object{}},
                                              WorkflowPolicy::Interactive);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());
    const auto waited = workflow->wait_run(created.value().run_id, std::chrono::seconds(5));
    MIRA_CHECK(waited.has_value());
    MIRA_CHECK(waited.value().state == WorkflowRunState::WaitingUser);

    WorkflowPatchEntry entry;
    entry.target = WorkflowPatchTarget::ExecutionPolicy;
    entry.op = WorkflowPatchOp::Set;
    entry.path = "policy";
    entry.value = JsonValue{"strict"};
    const auto switched =
        workflow->patch_run(created.value().run_id, WorkflowPatchId::generate(), {entry});
    MIRA_CHECK(switched.has_value());
    MIRA_CHECK(switched.value().view.policy == WorkflowPolicy::Strict);

    // The decision point stays the only exit from WaitingUser; cancel the
    // parked run instead of resolving it through this scenario.
    const auto cancelled = workflow->cancel_run(created.value().run_id);
    MIRA_CHECK(cancelled.has_value());
    return 0;
}

int edge_budget_counts_into_the_run_budget() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chat", "chat_view")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));

    // Suppress the shortcut; the only route takes two edges, and the run
    // budget admits exactly one of them (RULE-08).
    auto model = device_model();
    model.transitions.pop_back();

    WorkflowRuntimeConfig config;
    config.max_step_executions_per_run = 2; // 1 step attempt + 1 edge
    auto tight = fixture.make_workflow(config);
    MIRA_CHECK(tight->set_navigation_context(model, device.provider()).has_value());

    auto definition = base_definition("m12-budget");
    definition.steps = {navigate_step("composer")};
    const auto created =
        tight->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    const auto result = tight->execute_run(created.value().run_id, drive_context());
    MIRA_CHECK(result.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(result.value().safe_summary.find("step budget") != std::string::npos);
    MIRA_CHECK(device.dispatches() == 1);
    return 0;
}

int reinstalling_a_decayed_model_feeds_replanning() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chat", "chat_view")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("deep_link", "chat_view")));
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    auto definition = base_definition("m12-decay");
    definition.steps = {navigate_step("chat_view")};
    const auto first =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(workflow->execute_run(first.value().run_id, drive_context()).value().state ==
               WorkflowRunState::Completed);
    MIRA_CHECK(device.dispatches() == 1); // deep link

    // Host-side decay workflow: snapshot, decay, reinstall. Decay is relative
    // to each record's own last_verified stamp, so adding exactly one
    // half-life per record halves the shortcut's confidence precisely; the
    // runtime still routes through it (costs, not confidence, drive
    // planning) and the write-back continues from the reinstalled counters.
    auto decayed = workflow->app_model_snapshot().value();
    for (auto &transition : decayed.transitions) {
        transition.confidence = apply_confidence_decay(
            transition.confidence, transition.confidence.last_verified_ms + 500'000, 500'000);
    }
    MIRA_CHECK(workflow->set_app_model(decayed).has_value());
    MIRA_CHECK(workflow->app_model_snapshot().value().transitions[3].confidence.confidence ==
               (2.0 / 3.0) * 0.5);

    // The device still sits on chat_view: from == to would plan the empty
    // path. Walk back home first so the second run traverses the shortcut
    // again (the host owns the recognized state).
    device.set_current("home");
    const auto second =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(workflow->execute_run(second.value().run_id, drive_context()).value().state ==
               WorkflowRunState::Completed);
    const auto evolved = workflow->app_model_snapshot().value();
    const auto shortcut =
        std::find_if(evolved.transitions.begin(), evolved.transitions.end(),
                     [](const AppModelTransition &edge) { return edge.id == "t-deep-link"; });
    MIRA_CHECK(shortcut->confidence.verified_count == 2);
    MIRA_CHECK(needs_exploration(shortcut->confidence, 0.9));
    return 0;
}

int pause_converges_and_resume_replans() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("open_chats", "chat_list")));
    GatedEdgeAction gated(device, "chat_view");
    MIRA_CHECK(register_registration(*fixture.registry_, gated.registration("open_chat")));
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    auto workflow = fixture.make_workflow();

    // No shortcut: the only route blocks inside the gated second edge.
    auto model = device_model();
    model.transitions.pop_back();
    MIRA_CHECK(workflow->set_navigation_context(model, device.provider()).has_value());

    auto definition = base_definition("m12-pause");
    definition.steps = {navigate_step("composer")};
    const auto created =
        workflow->create_run(definition, JsonValue{JsonValue::Object{}}, WorkflowPolicy::Strict);
    MIRA_CHECK(created.has_value());
    MIRA_CHECK(workflow->start_run(created.value().run_id).has_value());

    // Wait until the drive is inside the gated edge, then pause: the edge
    // settles Cancelled, the step settles Stale and the run pauses at the
    // boundary (cursor stays put).
    while (gated.entries() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto paused = workflow->pause_run(created.value().run_id);
    MIRA_CHECK(paused.has_value());
    const auto waited = workflow->wait_run(created.value().run_id, std::chrono::seconds(5));
    MIRA_CHECK(waited.has_value());
    MIRA_CHECK(waited.value().state == WorkflowRunState::Paused);
    MIRA_CHECK(device.current() == "chat_list");
    MIRA_CHECK(waited.value().steps.back().disposition == WorkflowStepDisposition::Stale);

    // Resume: the step re-plans from the current reading and completes.
    gated.release();
    const auto resumed = workflow->resume_run(created.value().run_id);
    MIRA_CHECK(resumed.has_value());
    const auto finished = workflow->wait_run(created.value().run_id, std::chrono::seconds(5));
    MIRA_CHECK(finished.has_value());
    MIRA_CHECK(finished.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(device.current() == "composer");
    MIRA_CHECK(gated.entries() == 2); // retried after the stale settlement
    return 0;
}

int publish_gate_constrains_navigation_reachability() {
    WorkflowFixture fixture;
    ScreenDevice device("home");
    auto workflow = fixture.make_workflow();
    MIRA_CHECK(install_device(*workflow, device) == 0);

    // A draft navigating to a declared state passes the DryRun gate (real
    // planning under the installed context) and publishes.
    auto reachable = base_definition("m12-gate-good");
    reachable.steps = {navigate_step("composer")};
    const auto published =
        workflow->publish_validated(reachable, "m12", "navigation draft", std::nullopt);
    MIRA_CHECK(published.has_value());

    // A draft targeting an undeclared state cannot even plan in DryRun: the
    // gate fails and the library stays untouched (DEC-025 §3 rollback).
    auto orphan = base_definition("m12-gate-bad");
    orphan.steps = {navigate_step("mars")};
    const auto rejected =
        workflow->publish_validated(orphan, "m12", "navigation draft", std::nullopt);
    MIRA_CHECK(!rejected.has_value());
    const auto types = session_event_types(*fixture.events_, fixture.session_id_);
    MIRA_CHECK(std::find(types.begin(), types.end(), "WorkflowPublishRejected") != types.end());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {admission_stays_closed_without_context, navigation_executes_edges_and_arrives,
         arrival_mismatch_fails_closed_and_writes_back, missing_inputs_fail_closed,
         screen_state_predicates_evaluate, dry_run_plans_for_real_without_dispatch,
         policy_gate_relaxes_with_context, edge_budget_counts_into_the_run_budget,
         reinstalling_a_decayed_model_feeds_replanning, pause_converges_and_resume_replans,
         publish_gate_constrains_navigation_reachability});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    return 0;
}
