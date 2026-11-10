#include "m30_support.hpp"

using namespace mira;
using namespace mira::testing;

int main() {
    auto principal = m28::make_principal();
    const auto user = memory_user_scope(principal);
    const auto session = SessionId::generate();
    const auto scoped_session = memory_session_scope(principal, session);
    const auto application = memory_application_scope(principal, "internal.app.editor");
    MIRA_CHECK(user == memory_user_scope(principal));
    MIRA_CHECK(scoped_session == memory_session_scope(principal, session));
    MIRA_CHECK(application == memory_application_scope(principal, "internal.app.editor"));
    MIRA_CHECK(user.subject_id != principal.user_id.to_string());
    MIRA_CHECK(digest_from_hex(user.subject_id) && digest_from_hex(application.subject_id));
    principal.host_id = HostInstanceId::generate();
    ++principal.identity_version;
    MIRA_CHECK(user == memory_user_scope(principal));
    principal.tenant_id = TenantId::generate();
    MIRA_CHECK(user != memory_user_scope(principal));
    MIRA_CHECK(application != memory_application_scope(principal, "internal.app.editor"));

    MemoryAgentFixture host;
    host.start();
    auto oversized = host.binding();
    oversized.user_constraints.resize(65, "constraint");
    MIRA_CHECK(!host.service->bind(oversized));
    auto binding = host.binding();
    auto staged = host.service->stage_input(binding);
    MIRA_CHECK(staged);
    MIRA_CHECK(!host.service->context_supplier()(memory_request(binding.work),
                                                 memory_context(binding.work)));
    auto next = binding;
    next.user_constraints = {"new constraint"};
    const auto revised = host.service->stage_input(next);
    MIRA_CHECK(revised && revised.value() > staged.value());
    MIRA_CHECK(!host.service->acknowledge_input(binding.work.task_id, staged.value()));
    MIRA_CHECK(host.service->acknowledge_input(binding.work.task_id, revised.value()));
    MIRA_CHECK(host.service->context_supplier()(memory_request(binding.work),
                                                memory_context(binding.work)));
    auto foreign = binding;
    foreign.principal.user_id = UserId::generate();
    MIRA_CHECK(!host.service->stage_input(foreign));
    auto available = std::make_shared<std::atomic<bool>>(true);
    binding.request_current = [available] { return available->load(); };
    staged = host.service->stage_input(binding);
    MIRA_CHECK(staged && host.service->acknowledge_input(binding.work.task_id, staged.value()));
    const auto snapshot = host.service->context_supplier()(memory_request(binding.work),
                                                           memory_context(binding.work));
    MIRA_CHECK(snapshot && snapshot.value().current());
    available->store(false);
    MIRA_CHECK(!snapshot.value().current() && !snapshot.value().refreshable());
    MIRA_CHECK(!host.service->context_supplier()(memory_request(binding.work),
                                                 memory_context(binding.work)));
    binding.request_current = []() -> bool { throw std::runtime_error("private probe error"); };
    staged = host.service->stage_input(binding);
    MIRA_CHECK(staged && host.service->acknowledge_input(binding.work.task_id, staged.value()));
    MIRA_CHECK(!host.service->context_supplier()(memory_request(binding.work),
                                                 memory_context(binding.work)));
    foreign = binding;
    foreign.scopes.push_back(memory_user_scope(binding.principal));
    MIRA_CHECK(!host.service->stage_input(foreign));
    foreign = binding;
    foreign.work.session_id = SessionId::generate();
    MIRA_CHECK(!host.service->stage_input(foreign));
    MIRA_CHECK(host.service->retire(binding.work.task_id));
    MIRA_CHECK(!host.service->stage_input(binding));
    return 0;
}
