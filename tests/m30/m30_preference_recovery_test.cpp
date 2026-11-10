#include "m30_preference_support.hpp"
using namespace mira;
using namespace mira::testing;

int main() {
    {
        PreferenceFixture weak;
        weak.fault->weak_type = "mira.memory-agent.preference-proposed.v1";
        const auto id = MutationId::generate();
        MIRA_CHECK(!weak.propose("tone=brief", id));
        MIRA_CHECK(!weak.preview(id));
        MIRA_CHECK(weak.reopen());
        const auto recovered = weak.preview(id);
        MIRA_CHECK(recovered && recovered.value().state == PreferenceProposalState::Pending);
        MIRA_CHECK(weak.decide(recovered.value()));
    }
    for (const std::string failure : {"mira.memory-agent.preference-proposed.v1",
                                      "mira.memory-agent.preference-approval-intent.v1",
                                      "mira.memory-agent.preference-applied.v1"}) {
        for (bool after_write : {false, true}) {
            PreferenceFixture host;
            const auto id = MutationId::generate();
            host.fault->fail_type = failure;
            host.fault->after_write = after_write;
            auto proposal = host.propose("answer.style=brief", id);
            if (failure == "mira.memory-agent.preference-proposed.v1") {
                MIRA_CHECK(!proposal);
                MIRA_CHECK(!host.propose("tone=quiet"));
                MIRA_CHECK(host.reopen());
                proposal = host.propose("answer.style=brief", id);
                MIRA_CHECK(proposal);
            } else {
                MIRA_CHECK(proposal);
                const auto failed = host.decide(proposal.value());
                MIRA_CHECK(!failed && failed.error().code == ErrorCode::DataLoss);
                MIRA_CHECK(!host.preview(proposal.value().id));
                MIRA_CHECK(host.reopen());
            }
            const auto restored = host.preview(id);
            MIRA_CHECK(restored);
            auto confirmed = host.decide(restored.value());
            MIRA_CHECK(confirmed && confirmed.value().state == PreferenceProposalState::Applied);
            const auto records = host.query();
            MIRA_CHECK(records && records.value().records.size() == 1);
            MIRA_CHECK(records.value().records.front().statement == "answer.style=brief");
            const auto stored = confirmed.value().applied->record;
            MIRA_CHECK(host.reopen());
            const auto repeat = host.decide(confirmed.value());
            MIRA_CHECK(repeat && repeat.value().applied->record == stored);
            MIRA_CHECK(host.query().value().records.size() == 1);
        }
    }
    PreferenceFixture host;
    const auto proposal = host.propose("tone=quiet");
    MIRA_CHECK(proposal);
    MIRA_CHECK(host.decide(proposal.value(), ConfirmationDecision::Reject));
    MIRA_CHECK(host.reopen());
    const auto rejected = host.preview(proposal.value().id);
    MIRA_CHECK(rejected && rejected.value().state == PreferenceProposalState::Rejected);
    MIRA_CHECK(rejected.value().statement.empty());
    const auto still_pending = host.propose("theme=dark");
    MIRA_CHECK(still_pending);
    MIRA_CHECK(host.run([&] { return host.service->erase_scope(host.binding.scopes.front()); }));
    MIRA_CHECK(host.service->close());
    host.service = std::make_unique<MemoryAgentService>(host.storage.executor, host.storage.memory,
                                                        host.storage.events, host.storage.runtime,
                                                        MemoryAgentProcessor{}, host.config);
    MIRA_CHECK(host.service->initialize());
    MIRA_CHECK(!host.service->bind(host.binding));
    const auto erased = host.preview(still_pending.value().id);
    MIRA_CHECK(erased && erased.value().state == PreferenceProposalState::Erased &&
               erased.value().statement.empty());
    return 0;
}
