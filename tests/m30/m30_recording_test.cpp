#include "m30_support.hpp"

using namespace mira;
using namespace mira::testing;

int main() {
    InteractionMemoryFixture host(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.mode == MemoryAgentMode::Record && !input.events.empty()) {
                proposal.memories = {{MemoryKind::Preference, {"tone=brief", {0}}},
                                     {MemoryKind::RecoveryLesson, {"model guessed lesson", {0}}}};
            }
            return proposal;
        });
    const auto input = host.coordinator->submit_input(host.input("record"));
    MIRA_CHECK(input && input.value().task_id);
    const auto task = *input.value().task_id;
    MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
    MIRA_CHECK(host.finish(task));
    MIRA_CHECK(host.storage.fact(host.binding(input.value()), "volume", "30"));
    const auto settled = host.coordinator->record_settlement("record", TaskState::Completed);
    MIRA_CHECK(settled && settled.value().stage == InputStage::TaskSettled);
    const auto result = host.finish(task);
    MIRA_CHECK(result && result.value().mode == MemoryAgentMode::Record);
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(result.value().consolidation.pending_approval.size() == 1);
    MIRA_CHECK(result.value().consolidation.pending_approval.front().proposed.verification ==
               MemoryVerification::Unverified);
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::RejectedInvalid) == 1);
    MemoryQuery query;
    query.scopes = host.binding(input.value()).scopes;
    const auto recorded = host.storage.memory->query(query);
    MIRA_CHECK(recorded && recorded.value().records.size() == 1);
    MIRA_CHECK(recorded.value().records.front().statement == "volume=30");
    MIRA_CHECK(recorded.value().records.front().verification == MemoryVerification::Verified);
    const auto status = host.coordinator->memory_status(host.principal, task);
    MIRA_CHECK(status && status.value().terminal && status.value().job);
    MIRA_CHECK(host.coordinator->record_settlement("record", TaskState::Completed));
    MIRA_CHECK(host.coordinator->memory_status(host.principal, task).value().job->operation ==
               status.value().job->operation);
    MIRA_CHECK(!host.service->context_supplier()(memory_request(host.spec(input.value())),
                                                 memory_context(host.spec(input.value()))));
    return 0;
}
