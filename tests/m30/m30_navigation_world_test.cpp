#include "support/m12_support.hpp"
#include "support/m36_support.hpp"

#include <thread>

namespace {
using namespace mira;
using namespace mira::testing;

NavigationWorldFacts binding_for(const AppModel &model, ScreenDevice &device, int &recognitions) {
    NavigationWorldFacts binding;
    binding.tenant = TenantId::generate();
    binding.environment = Id128::generate();
    binding.application = Id128::generate();
    binding.app_id = model.app_id;
    binding.screen_state = {Id128::generate(), "screen_state"};
    binding.observation_request.required.structure = true;
    binding.authorized = [](const OperationContext &) { return true; };
    binding.lookup = [](const WorldFactQuery &,
                        const OperationContext &) -> Result<WorldFactLookup> {
        return WorldFactLookup{};
    };
    binding.recognize = [&device,
                         &recognitions](const Observation &observed,
                                        const OperationContext &) -> Result<ScreenStateSnapshot> {
        if (!observed.structure)
            return ScreenStateSnapshot{};
        ++recognitions;
        return ScreenStateSnapshot{device.current(), 0};
    };
    return binding;
}

Result<WorkflowRunResult> navigate(WorkflowRuntime &workflow, std::string id,
                                   const OperationContext &context = drive_context()) {
    auto definition = base_definition(std::move(id));
    definition.steps = {navigate_step("composer")};
    auto created = workflow.create_run(definition, JsonValue::Object{}, WorkflowPolicy::Strict);
    if (!created)
        return created.error();
    return workflow.execute_run(created.value().run_id, context);
}

bool write_fact(MemoryAgentFixture &fixture, const MemoryRecord &record) {
    return fixture.executor
        .submit_auto([&] {
            MemoryCandidate candidate;
            candidate.proposed = record;
            candidate.evidence = record.provenance;
            const auto written = MemoryConsolidator{}.consolidate_candidates(
                *fixture.memory, {candidate}, record.scope, Timestamp::now());
            return written && written.value().count_of(CandidateDisposition::Applied) == 1;
        })
        .get();
}

int stale_and_replaced_version_reobserve() {
    WorkflowFixture fixture;
    MemoryAgentFixture knowledge;
    auto counted = std::make_shared<ObservationCountingEnvironment>(fixture.environment_);
    WorkflowRuntime workflow(fixture.executor_, *fixture.runtime_, fixture.session_id_, counted);
    workflow.set_tool_registry(fixture.registry_);
    workflow.set_event_store(fixture.events_);
    ScreenDevice device("chat_view");
    MIRA_CHECK(register_registration(*fixture.registry_, device.action("tap_input", "composer")));
    const auto original = device_model();
    int recognitions = 0;
    auto binding = binding_for(original, device, recognitions);
    const auto scope = world_application_model_scope(
        binding.tenant, binding.environment, binding.application, app_model_digest(original));
    MIRA_CHECK(scope);
    std::vector<MemoryScope> reads;
    binding.lookup = [&](const WorldFactQuery &query, const OperationContext &) {
        reads.push_back(query.scope);
        return knowledge.memory->lookup_world_fact(query);
    };
    const auto now = std::chrono::system_clock::now();
    const auto stale = make_world_observation(scope.value(), binding.screen_state, "home",
                                              EventId::generate(), {now - 2h, now - 1h}, now - 2h);
    MIRA_CHECK(stale && write_fact(knowledge, stale.value()));
    std::string cheap_state = "home";
    MIRA_CHECK(workflow.set_navigation_context(original, [&] {
        const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        return ScreenStateSnapshot{cheap_state, static_cast<std::uint64_t>(wall)};
    }));
    MIRA_CHECK(workflow.set_navigation_world_facts(binding));
    const auto first = navigate(workflow, "m30-nav-stale");
    MIRA_CHECK(first && first.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counted->observations() == 2 && recognitions == 2 && device.dispatches() == 1);
    MIRA_CHECK(reads.size() == 1 && reads.front() == scope.value());

    const auto fresh = make_world_observation(scope.value(), binding.screen_state, "chat_view",
                                              EventId::generate(), {now - 1min, now + 1h}, now);
    MIRA_CHECK(fresh && write_fact(knowledge, fresh.value()));
    device.set_current("chat_view");
    cheap_state = "chat_view";
    const auto corroborated = navigate(workflow, "m30-nav-current");
    MIRA_CHECK(corroborated && corroborated.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counted->observations() == 3 && recognitions == 3 && device.dispatches() == 2);
    MIRA_CHECK(reads.size() == 2 && reads.back() == scope.value());

    device.set_current("chat_view");
    binding.authorized = [driver = std::this_thread::get_id()](const OperationContext &) {
        return std::this_thread::get_id() == driver;
    };
    MIRA_CHECK(workflow.set_navigation_world_facts(binding));
    const auto rejected = navigate(workflow, "m30-nav-worker-revoked");
    MIRA_CHECK(rejected && rejected.value().state == WorkflowRunState::Failed);
    MIRA_CHECK(counted->observations() == 3 && recognitions == 3 && device.dispatches() == 2);
    const auto after_rejection = workflow.app_model_snapshot();
    MIRA_CHECK(after_rejection && after_rejection->transitions[2].confidence.verified_count == 2 &&
               after_rejection->transitions[2].confidence.failure_count == 0);
    binding.authorized = [](const OperationContext &) { return true; };
    MIRA_CHECK(workflow.set_navigation_world_facts(binding));

    auto replacement = original;
    replacement.summary = "Reinstalled after an application update";
    MIRA_CHECK(workflow.set_app_model(replacement));
    device.set_current("chat_view");
    const auto next = navigate(workflow, "m30-nav-version");
    MIRA_CHECK(next && next.value().state == WorkflowRunState::Completed);
    MIRA_CHECK(counted->observations() == 5 && recognitions == 5 && device.dispatches() == 3);
    const auto updated_scope = world_application_model_scope(
        binding.tenant, binding.environment, binding.application, app_model_digest(replacement));
    MIRA_CHECK(updated_scope && reads.size() == 4 && reads.back() == updated_scope.value() &&
               reads.back() != scope.value());
    return 0;
}

int cancellation_revocation_and_replacement_gate_actions() {
    WorkflowFixture fixture;
    auto workflow = fixture.make_workflow();
    ScreenDevice device("chat_view");
    const auto model = device_model();
    int recognitions = 0;
    int action_entries = 0;
    auto replacement = model;
    replacement.summary = "Replacement reached during edge execution";
    auto action = device.action("tap_input", "composer");
    action.handler = [&](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
        ++action_entries;
        auto replaced = workflow->set_app_model(replacement);
        if (!replaced)
            return replaced.error();
        device.set_current("composer");
        return JsonValue{"moved"};
    };
    MIRA_CHECK(register_registration(*fixture.registry_, std::move(action)));
    MIRA_CHECK(workflow->set_navigation_context(model, device.provider()));

    auto binding = binding_for(model, device, recognitions);
    const auto recognize = binding.recognize;
    std::atomic<bool> requested{false};
    auto context = drive_context();
    context.cancellation_requested = [&] { return requested.load(); };
    binding.recognize = [&](const Observation &observed, const OperationContext &operation) {
        requested.store(true);
        return recognize(observed, operation);
    };
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto cancelled = navigate(*workflow, "m30-nav-cancel", context);
    MIRA_CHECK(cancelled && requested.load() && action_entries == 0);

    binding.recognize = [](const Observation &,
                           const OperationContext &) -> Result<ScreenStateSnapshot> {
        return ScreenStateSnapshot{};
    };
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto missing = navigate(*workflow, "m30-nav-missing");
    MIRA_CHECK(missing && missing.value().state == WorkflowRunState::Failed && action_entries == 0);

    std::atomic<bool> allowed{true};
    binding.authorized = [&](const OperationContext &) { return allowed.load(); };
    binding.recognize = [&](const Observation &observed, const OperationContext &operation) {
        allowed.store(false);
        return recognize(observed, operation);
    };
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto revoked = navigate(*workflow, "m30-nav-revoked");
    MIRA_CHECK(revoked && revoked.value().state == WorkflowRunState::Failed && action_entries == 0);

    binding.authorized = [](const OperationContext &) { return true; };
    binding.recognize = [&](const Observation &observed,
                            const OperationContext &operation) -> Result<ScreenStateSnapshot> {
        auto changed = workflow->set_app_model(replacement);
        if (!changed)
            return changed.error();
        return recognize(observed, operation);
    };
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto changed = navigate(*workflow, "m30-nav-replace-before-edge");
    MIRA_CHECK(changed && changed.value().state == WorkflowRunState::Failed && action_entries == 0);

    MIRA_CHECK(workflow->set_app_model(model));
    binding.recognize = recognize;
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto in_flight = navigate(*workflow, "m30-nav-replace-during-edge");
    MIRA_CHECK(in_flight && in_flight.value().state == WorkflowRunState::Failed &&
               action_entries == 1);
    const auto snapshot = workflow->app_model_snapshot();
    MIRA_CHECK(snapshot && app_model_digest(*snapshot) == app_model_digest(replacement));

    device.set_current("chat_view");
    bool takeover_settled = false;
    binding.recognize = [&](const Observation &observed,
                            const OperationContext &operation) -> Result<ScreenStateSnapshot> {
        const auto request = fixture.runtime_->request_human_takeover(fixture.session_id_);
        if (!request)
            return request.error();
        const auto outcome = request.value().outcome(2s);
        if (!outcome)
            return outcome.error();
        takeover_settled = outcome.value().status == SettlementStatus::Applied;
        return recognize(observed, operation);
    };
    MIRA_CHECK(workflow->set_navigation_world_facts(binding));
    const auto takeover = navigate(*workflow, "m30-nav-human-takeover");
    MIRA_CHECK(takeover && takeover_settled && takeover.value().state == WorkflowRunState::Paused &&
               action_entries == 1);
    const auto released = fixture.runtime_->release_human_takeover(fixture.session_id_);
    MIRA_CHECK(released && released.value().outcome(2s));
    return 0;
}

} // namespace

int main() {
    if (const int result = stale_and_replaced_version_reobserve(); result != 0)
        return result;
    return cancellation_revocation_and_replacement_gate_actions();
}
