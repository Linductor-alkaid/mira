#include "m30_preference_support.hpp"

#include <algorithm>

using namespace mira;
using namespace mira::testing;

namespace {
int malformed_applied_results_fail_closed() {
    for (int mode = 0; mode < 12; ++mode) {
        PreferenceFixture host;
        AppendRequest applied;
        host.fault->before_append = [&](const AppendRequest &event) {
            if (event.payload.type == "mira.memory-agent.preference-applied.v1")
                applied = event;
        };
        const auto pending = host.propose("tone=brief");
        MIRA_CHECK(pending && host.decide(pending.value()));
        MIRA_CHECK(!applied.event_id.is_nil());
        MIRA_CHECK(host.service->close());
        auto parsed = parse_json(applied.payload.data);
        MIRA_CHECK(parsed);
        auto &json = parsed.value();
        auto *result = json.find("result");
        MIRA_CHECK(result && result->is_object());
        if (mode == 0) {
            auto fields = *json.as_object();
            std::erase_if(fields, [](const auto &field) { return field.first == "result"; });
            json = JsonValue{std::move(fields)};
        } else if (mode == 1) {
            *result = JsonValue{};
        } else if (mode <= 4) {
            const auto name = mode == 2 ? "record" : mode == 3 ? "type" : "version";
            auto fields = *result->as_object();
            std::erase_if(fields, [name](const auto &field) { return field.first == name; });
            *result = JsonValue{std::move(fields)};
        } else if (mode == 5) {
            *result->find("version") = JsonValue{"0"};
        } else if (mode == 6) {
            *result->find("version") = JsonValue{"-1"};
        } else if (mode == 7) {
            *result->find("version") = JsonValue{1};
        } else if (mode == 8) {
            *result->find("replayed") = JsonValue{"true"};
        } else if (mode == 9) {
            *result->find("record") = JsonValue{MemoryId{}.to_string()};
        } else if (mode == 10) {
            *result->find("type") = JsonValue{"unknown-mutation"};
        } else {
            *result->find("version") = JsonValue{"18446744073709551616"};
        }
        applied.event_id = EventId::generate();
        applied.payload.data = to_json_string(json);
        MIRA_CHECK(host.storage.events->append(applied));
        MemoryAgentService recovered(host.storage.executor, host.storage.memory,
                                     host.storage.events, host.storage.runtime,
                                     MemoryAgentProcessor{}, host.config);
        const auto restored = recovered.initialize();
        MIRA_CHECK(!restored && restored.error().code == ErrorCode::DataLoss);
        MIRA_CHECK(recovered.close());
    }
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(malformed_applied_results_fail_closed() == 0);
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
