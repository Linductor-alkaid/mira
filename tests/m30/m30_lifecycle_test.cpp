#include "m30_support.hpp"

using namespace mira;
using namespace mira::testing;

int main() {
    InteractionMemoryFixture host;
    MIRA_CHECK(
        approve_preference(*host.storage.memory, memory_user_scope(host.principal), "volume=30"));
    auto other = host.principal;
    other.user_id = UserId::generate();
    MIRA_CHECK(approve_preference(*host.storage.memory, memory_user_scope(other), "volume=90"));
    auto foreign_tenant = host.principal;
    foreign_tenant.tenant_id = TenantId::generate();
    MIRA_CHECK(
        approve_preference(*host.storage.memory, memory_user_scope(foreign_tenant), "volume=80"));
    auto first_input = host.input("first");
    const auto first = host.coordinator->submit_input(first_input);
    MIRA_CHECK(first && first.value().stage == InputStage::Routed);
    const auto task = *first.value().task_id;
    auto status = host.coordinator->memory_status(host.principal, task);
    MIRA_CHECK(status && status.value().suspended && !status.value().job);
    MIRA_CHECK(!host.coordinator->memory_status(other, task));
    auto revoked = host.principal;
    revoked.grants.clear();
    MIRA_CHECK(!host.coordinator->memory_status(revoked, task));
    MIRA_CHECK(!host.coordinator->set_memory_service(host.service));
    const auto sink = std::make_shared<m28::RecordingSink>();
    MIRA_CHECK(host.coordinator->register_sink(task, sink));
    const auto recalled = host.finish(task);
    MIRA_CHECK(recalled && recalled.value().candidates.size() == 1);
    MIRA_CHECK(std::get<TextPart>(recalled.value().candidates.front().content.front()).text ==
               "volume=30");
    status = host.coordinator->memory_status(host.principal, task);
    MIRA_CHECK(status && !status.value().suspended && status.value().job);
    const auto operation = status.value().job->operation;
    const auto duplicate = host.coordinator->submit_input(first_input);
    MIRA_CHECK(duplicate && duplicate.value().duplicate);
    MIRA_CHECK(host.coordinator->memory_status(host.principal, task).value().job->operation ==
               operation);
    MIRA_CHECK(sink->deliveries.size() == 1);
    {
        ConversationFixture model;
        model.spec_ = host.spec(first.value());
        model.spec_.profile_id = model.profile_->id;
        model.use_provider({text_response("received scoped preference")});
        model.gateway_->set_context_supplier(host.service->context_supplier());
        auto loop = model.make_loop({}, false);
        MIRA_CHECK(loop.run(model.spec_, memory_context(model.spec_)));
        const auto text = model.provider_->request_text(0);
        MIRA_CHECK(text.find("volume=30") != std::string::npos);
        MIRA_CHECK(text.find("volume=90") == std::string::npos);
        MIRA_CHECK(text.find("volume=80") == std::string::npos);
        const auto requests = model.provider_->requests();
        MIRA_CHECK(requests.front().prompt_provenance.context_selection_digest != Hash{});
    }

    auto correction = host.input("correction", "volume=30");
    correction.target_task = task;
    const auto current_snapshot = host.service->context_supplier()(
        memory_request(host.spec(first.value())), memory_context(host.spec(first.value())));
    MIRA_CHECK(current_snapshot && current_snapshot.value().current());
    MIRA_CHECK(host.coordinator->submit_input(correction));
    MIRA_CHECK(!current_snapshot.value().current());
    MIRA_CHECK(host.finish(task));
    MIRA_CHECK(sink->deliveries.size() == 2);
    const auto first_scope = memory_session_scope(host.principal, *first.value().session_id);
    MIRA_CHECK(host.service->invalidate_scope(first_scope));
    MIRA_CHECK(host.storage.seed(host.binding(first.value()), "volume=31"));

    auto second_input = host.input("second-session");
    second_input.requested_intent = RouteIntent::NewTask;
    const auto second = host.coordinator->submit_input(second_input);
    MIRA_CHECK(second && second.value().task_id != first.value().task_id);
    MIRA_CHECK(second.value().session_id != first.value().session_id);
    MIRA_CHECK(host.coordinator->register_sink(*second.value().task_id,
                                               std::make_shared<m28::RecordingSink>()));
    MIRA_CHECK(host.finish(*second.value().task_id).value().candidates.size() == 1);
    auto unauthorized = host.input("unauthorized");
    unauthorized.principal.grants.clear();
    const auto denied = host.coordinator->submit_input(unauthorized);
    MIRA_CHECK(denied && denied.value().stage == InputStage::Rejected);

    const auto old_spec = host.spec(first.value());
    const auto before_takeover =
        host.service->context_supplier()(memory_request(old_spec), memory_context(old_spec));
    MIRA_CHECK(before_takeover && before_takeover.value().current());
    const auto takeover = host.runtime.request_human_takeover(*first.value().session_id);
    MIRA_CHECK(takeover && takeover.value().outcome(3s));
    MIRA_CHECK(!before_takeover.value().current() && !before_takeover.value().refreshable());
    MIRA_CHECK(
        !host.service->context_supplier()(memory_request(old_spec), memory_context(old_spec)));
    MIRA_CHECK(host.service->erase_scope(memory_user_scope(host.principal)));
    const auto erased = host.coordinator->memory_status(host.principal, *second.value().task_id);
    MIRA_CHECK(erased && erased.value().error && !erased.value().job);
    return 0;
}
