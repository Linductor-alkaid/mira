#include "m30_preference_support.hpp"
using namespace mira;
using namespace mira::testing;

int main() {
    PreferenceFixture host;
    const auto pending = host.propose("tone=brief");
    MIRA_CHECK(pending);
    for (int mode = 0; mode < 7; ++mode) {
        auto principal = host.storage.principal;
        if (mode == 0)
            principal.user_id = UserId::generate();
        if (mode == 1)
            principal.tenant_id = TenantId::generate();
        if (mode == 2)
            principal.grants.clear();
        if (mode == 3)
            ++principal.identity_version;
        if (mode == 4)
            principal.grants.front().expires_at = std::chrono::system_clock::now() - 1s;
        if (mode == 5)
            principal.grants.front().resources = {"memory", "foreign-user-scope", ""};
        if (mode == 6)
            principal.auth_strength = AuthenticationStrength::Anonymous;
        const auto denied = host.run([&] {
            return host.service->preference_proposal(principal, pending.value().id, host.context());
        });
        MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
        MIRA_CHECK(!host.run([&] {
            return host.service->decide_preference(principal, pending.value().id,
                                                   pending.value().digest,
                                                   ConfirmationDecision::Approve, host.context());
        }));
    }
    MIRA_CHECK(!host.run([&] {
        return host.service->decide_preference(host.storage.principal, pending.value().id, {},
                                               ConfirmationDecision::Approve, host.context());
    }));
    host.storage.principal.grants.front().resources = {"memory",
                                                       host.binding.scopes.front().subject_id, ""};
    MIRA_CHECK(host.preview(pending.value().id));
    const auto current = host.decide(pending.value());
    MIRA_CHECK(current);
    MemoryAgentRequest recall;
    recall.task = host.binding.work.task_id;
    recall.query = "tone";
    const auto handle = host.run([&] { return host.service->submit(recall, host.context()); });
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(handle.value().result.get());
    const auto snapshot =
        host.service->context_supplier()(memory_request(host.binding.work), host.context());
    MIRA_CHECK(snapshot && snapshot.value().current() && !snapshot.value().candidates.empty());
    auto proposed = host.propose("tone=long");
    MIRA_CHECK(proposed);
    auto concurrent = host.propose("tone=short");
    MIRA_CHECK(concurrent && host.decide(concurrent.value()));
    MIRA_CHECK(!snapshot.value().current());
    auto stale = host.decide(proposed.value());
    MIRA_CHECK(!stale && stale.error().domain_code ==
                             static_cast<std::int32_t>(MemoryDomainCode::VersionConflict));
    MIRA_CHECK(host.preview(proposed.value().id).value().state == PreferenceProposalState::Pending);
    MIRA_CHECK(host.decide(proposed.value(), ConfirmationDecision::Reject));
    const auto invalidated = host.propose("volume=30");
    MIRA_CHECK(invalidated);
    MIRA_CHECK(
        host.run([&] { return host.service->invalidate_scope(host.binding.scopes.front()); }));
    const auto invalid = host.preview(invalidated.value().id);
    MIRA_CHECK(invalid && invalid.value().state == PreferenceProposalState::Stale);
    MIRA_CHECK(invalid.value().statement.empty() && !host.decide(invalidated.value()));
    const auto erased = host.propose("voice=quiet");
    MIRA_CHECK(erased);
    MIRA_CHECK(host.run([&] { return host.service->erase_scope(host.binding.scopes.front()); }));
    const auto after = host.preview(erased.value().id);
    MIRA_CHECK(after && after.value().state == PreferenceProposalState::Erased);
    MIRA_CHECK(after.value().statement.empty() && !host.decide(erased.value()));
    const auto records = host.query();
    MIRA_CHECK(records && records.value().records.empty());
    MIRA_CHECK(!host.propose("new=knowledge"));
    return 0;
}
