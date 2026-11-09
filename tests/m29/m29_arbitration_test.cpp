#include "m28/m28_support.hpp"

#include <stdexcept>

using namespace mira;
using namespace mira::m28;

int main() {
    CoordinatorFixture fixture;
    MIRA_CHECK(fixture.start());
    const auto principal = make_principal();
    int resolver_calls = 0;
    fixture.resolver = [&](const InputEnvelope &,
                           const PrincipalState &) -> std::optional<RouteSuggestion> {
        ++resolver_calls;
        throw std::runtime_error("private resolver data");
    };
    auto first = fixture.envelope("one", "prepare report", principal);
    first.requested_intent = RouteIntent::NewTask;
    const auto one = fixture.submit(first);
    MIRA_CHECK(one && one.value().task_id && resolver_calls == 0);
    auto second = fixture.envelope("two", "compare prices", principal);
    second.requested_intent = RouteIntent::NewTask;
    const auto two = fixture.submit(second);
    MIRA_CHECK(two && two.value().task_id && two.value().task_id != one.value().task_id);
    auto correction = fixture.envelope("correction", "use the latest figures", principal);
    correction.reply_to = "one";
    const auto corrected = fixture.submit(correction);
    MIRA_CHECK(corrected && corrected.value().task_id == one.value().task_id &&
               resolver_calls == 0);
    auto explicit_target = fixture.envelope("target", "include appendix", principal);
    explicit_target.target_task = two.value().task_id;
    explicit_target.requested_intent = RouteIntent::SupplementTask;
    const auto targeted = fixture.submit(explicit_target);
    MIRA_CHECK(targeted && targeted.value().task_id == two.value().task_id);
    explicit_target.input_id = "conflict";
    explicit_target.reply_to = "one";
    const auto conflict = fixture.submit(explicit_target);
    MIRA_CHECK(conflict && conflict.value().stage == InputStage::Rejected);
    correction.input_id = "unknown";
    correction.reply_to = "lost-reference";
    const auto unknown = fixture.submit(correction);
    MIRA_CHECK(unknown && unknown.value().stage == InputStage::Rejected);

    fixture.resolver = {};
    const auto clarified = fixture.submit(fixture.envelope("ambiguous", "stop that", principal));
    MIRA_CHECK(clarified && clarified.value().clarification && !clarified.value().error);
    MIRA_CHECK(clarified.value().candidates ==
               std::vector<TaskId>({*one.value().task_id, *two.value().task_id}));
    const auto repeat = fixture.submit(fixture.envelope("ambiguous", "stop that", principal));
    MIRA_CHECK(repeat && repeat.value().duplicate &&
               repeat.value().candidates == clarified.value().candidates);

    // Knowing an opaque id is not permission to read the original receipt.
    const auto foreign =
        fixture.submit(fixture.envelope("one", "prepare report", make_principal()));
    MIRA_CHECK(foreign && foreign.value().stage == InputStage::Rejected);
    MIRA_CHECK(!foreign.value().task_id && foreign.value().ingress_id.is_nil());
    auto revoked = principal;
    revoked.grants.clear();
    const auto denied = fixture.submit(fixture.envelope("one", "prepare report", revoked));
    MIRA_CHECK(denied && denied.value().stage == InputStage::Rejected && !denied.value().task_id);
    const auto original = fixture.coordinator_->input_record("one");
    MIRA_CHECK(original && original.value().stage == InputStage::Routed);

    fixture.resolver = [&](const InputEnvelope &,
                           const PrincipalState &) -> std::optional<RouteSuggestion> {
        ++resolver_calls;
        throw std::runtime_error("must not run for emergency stop");
    };
    auto stop = fixture.envelope("stop", "stop report", principal);
    stop.target_task = one.value().task_id;
    stop.requested_intent = RouteIntent::CancelTask;
    const auto stopped = fixture.submit(stop);
    MIRA_CHECK(stopped && stopped.value().stage == InputStage::Routed && resolver_calls == 0);
    stop.input_id = "stop-again";
    MIRA_CHECK(fixture.submit(stop).value().stage == InputStage::Routed);
    MIRA_CHECK(fixture.runtime_->task_snapshot(*one.value().task_id).value().state ==
               TaskState::Cancelled);
    auto resume = fixture.envelope("resume", "now use the corrected totals", principal);
    resume.target_task = one.value().task_id;
    const auto resumed = fixture.submit(resume);
    MIRA_CHECK(resumed && resumed.value().task_id != one.value().task_id);
    const auto state = fixture.coordinator_->principal_state(principal);
    MIRA_CHECK(state);
    const auto *work = find_task(state.value(), *resumed.value().task_id);
    MIRA_CHECK(work && work->resumed_from == one.value().task_id);
    MIRA_CHECK(work->context_block.find("now use the corrected totals") != std::string::npos);

    auto control_reply =
        fixture.envelope("control-reply", "continue the stopped report", principal);
    control_reply.reply_to = "stop";
    const auto follow_up = fixture.submit(control_reply);
    MIRA_CHECK(follow_up && follow_up.value().stage == InputStage::Routed);
    const auto after_control = fixture.coordinator_->principal_state(principal);
    const auto *linked = find_task(after_control.value(), *follow_up.value().task_id);
    MIRA_CHECK(linked && linked->resumed_from == one.value().task_id);

    auto unicode = fixture.envelope("unicode-bound", std::string(4095, 'a') + "报告", principal);
    unicode.requested_intent = RouteIntent::NewTask;
    const auto bounded_text = fixture.submit(unicode);
    MIRA_CHECK(bounded_text && bounded_text.value().task_id);
    const auto unicode_state = fixture.coordinator_->principal_state(principal);
    const auto *unicode_work = find_task(unicode_state.value(), *bounded_text.value().task_id);
    MIRA_CHECK(unicode_work && unicode_work->goal == std::string(4095, 'a'));

    // Task capacity is checked before opening a second environment/session.
    CoordinatorConfig config;
    config.max_tasks_per_principal = 1;
    CoordinatorFixture bounded(config);
    MIRA_CHECK(bounded.start());
    int environments = 0;
    bounded.coordinator_->set_environment_provider([&]() -> Result<std::shared_ptr<IEnvironment>> {
        ++environments;
        return std::shared_ptr<IEnvironment>(bounded.environment_);
    });
    MIRA_CHECK(bounded.submit(first).value().stage == InputStage::Routed);
    MIRA_CHECK(bounded.submit(second).value().stage == InputStage::Rejected);
    MIRA_CHECK(environments == 1);
    MIRA_CHECK(bounded.coordinator_->principal_state(principal).value().sessions.size() == 1);
    // Independent host ingress callers race on the same id. They must see
    // one admission/route, not create two runtime work items. This Executor
    // is explicitly owned by the test; context preparation uses host's pool.
    CoordinatorFixture concurrent;
    MIRA_CHECK(concurrent.start());
    kairo::Executor ingress;
    kairo::ExecutorConfig ingress_config;
    ingress_config.min_threads = 2;
    ingress_config.max_threads = 2;
    ingress_config.queue_capacity = 4;
    MIRA_CHECK(ingress.initialize(ingress_config));
    std::promise<void> release;
    const auto gate = release.get_future().share();
    auto send = [&] {
        gate.wait();
        return concurrent.submit(concurrent.envelope("racing-id", "one work item", principal));
    };
    auto a = ingress.submit_auto(send);
    auto b = ingress.submit_auto(send);
    release.set_value();
    const auto result_a = a.get();
    const auto result_b = b.get();
    (void)ingress.shutdown(true);
    MIRA_CHECK(result_a && result_b);
    MIRA_CHECK(result_a.value().duplicate != result_b.value().duplicate);
    MIRA_CHECK(result_a.value().ingress_id == result_b.value().ingress_id);
    MIRA_CHECK(result_a.value().task_id == result_b.value().task_id);
    MIRA_CHECK(concurrent.coordinator_->principal_state(principal).value().tasks.size() == 1);

    MIRA_CHECK(fixture.coordinator_->close());
    MIRA_CHECK(!fixture.coordinator_->register_sink(*two.value().task_id,
                                                    std::make_shared<RecordingSink>()));
    MIRA_CHECK(!fixture.coordinator_->initialize());
    return 0;
}
