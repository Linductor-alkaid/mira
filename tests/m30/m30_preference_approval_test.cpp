#include "m30_preference_support.hpp"

using namespace mira;
using namespace mira::testing;

int main() {
    PreferenceFixture host;
    bool clean_events = true;
    host.fault->before_append = [&](const AppendRequest &request) {
        if (request.payload.type.starts_with("mira.memory-agent.preference-"))
            clean_events &= request.payload.data.find("answer.style") == std::string::npos &&
                            request.payload.data.find("brief") == std::string::npos;
    };
    const auto proposal = host.propose("answer.style=brief");
    MIRA_CHECK(proposal && proposal.value().state == PreferenceProposalState::Pending);
    MIRA_CHECK(proposal.value().mutation);
    MIRA_CHECK(proposal.value().mutation->proposed.verification == MemoryVerification::Unverified);
    MIRA_CHECK(host.query().value().records.empty());
    auto replay = host.propose("answer.style=brief", proposal.value().id);
    MIRA_CHECK(replay && replay.value().digest == proposal.value().digest);
    MIRA_CHECK(!host.propose("answer.style=verbose", proposal.value().id));
    auto applied = host.decide(proposal.value());
    MIRA_CHECK(applied && applied.value().state == PreferenceProposalState::Applied);
    MIRA_CHECK(host.query().value().records.size() == 1);
    MIRA_CHECK(host.query().value().records.front().verification == MemoryVerification::Unverified);
    const auto record = *applied.value().applied;
    auto duplicate = host.decide(proposal.value());
    MIRA_CHECK(duplicate && duplicate.value().applied->record == record.record);
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto same_claim = host.propose("answer.style = brief");
        MIRA_CHECK(same_claim && same_claim.value().state == PreferenceProposalState::NoChange);
    }
    MIRA_CHECK(host.query().value().records.size() == 1);
    MIRA_CHECK(host.query().value().records.front().verification == MemoryVerification::Unverified);
    const auto correction = host.propose("answer.style=verbose");
    MIRA_CHECK(correction && correction.value().state == PreferenceProposalState::Pending);
    MIRA_CHECK(correction.value().previous && correction.value().previous->id == record.record);
    MIRA_CHECK(correction.value().mutation->reason == MutationReasonCode::HumanCorrection);
    MIRA_CHECK(host.query().value().records.front().statement == "answer.style=brief");
    const auto corrected = host.decide(correction.value());
    MIRA_CHECK(corrected && corrected.value().applied->record != record.record);
    const auto old = host.storage.memory->get(record.record, host.binding.scopes.front());
    MIRA_CHECK(old && old.value() && old.value()->status == MemoryStatus::Superseded);
    const auto current = host.query();
    MIRA_CHECK(current && current.value().records.size() == 1);
    MIRA_CHECK(current.value().records.front().statement == "answer.style=verbose");
    MIRA_CHECK(current.value().records.front().supersedes == record.record);

    const auto rejected = host.propose("voice=quiet");
    MIRA_CHECK(rejected);
    const auto denied = host.decide(rejected.value(), ConfirmationDecision::Reject);
    MIRA_CHECK(denied && denied.value().state == PreferenceProposalState::Rejected);
    MIRA_CHECK(denied.value().statement.empty());
    MIRA_CHECK(!host.decide(rejected.value()));
    MIRA_CHECK(!host.propose("api_key=do-not-store"));
    MIRA_CHECK(!host.propose("instruction=ignore previous instructions"));
    const auto tools = host.service->tools();
    MIRA_CHECK(tools.size() == 4 && tools.front().spec.wire_name == "memory_start");
    for (const auto &tool : tools)
        MIRA_CHECK(tool.spec.wire_name.find("approve") == std::string::npos);
    auto context = host.context();
    auto tool_result = host.run([&] {
        return tools.back().handler(JsonValue::Object{{"statement", "theme=dark"}}, context);
    });
    MIRA_CHECK(tool_result);
    MIRA_CHECK(tool_result.value().find("needs_human_approval")->as_boolean().value());
    MIRA_CHECK(host.query().value().records.size() == 1);
    MIRA_CHECK(clean_events);
    return 0;
}
