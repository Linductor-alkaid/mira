#include "support/m36_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int scopes_and_work_revisions_are_isolated() {
    MemoryAgentFixture fixture;
    fixture.start();
    auto first = fixture.binding("volume task");
    auto second = fixture.binding("network task");
    MIRA_CHECK(fixture.service->bind(first));
    MIRA_CHECK(fixture.service->bind(second));
    MIRA_CHECK(fixture.seed(first, "volume=30"));
    MIRA_CHECK(fixture.seed(second, "network=offline"));
    MemoryAgentRequest request;
    request.task = first.work.task_id;
    request.query = "volume";
    auto handle = fixture.service->submit(request, memory_context(first.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &result = handle.value().result.get();
    MIRA_CHECK(result && result.value().candidates.size() == 1);
    const auto supplier = fixture.service->context_supplier();
    auto snapshot = supplier(memory_request(first.work), memory_context(first.work));
    MIRA_CHECK(snapshot && snapshot.value().current());
    MIRA_CHECK(snapshot.value().candidates.size() == 1);
    auto other = supplier(memory_request(second.work), memory_context(second.work));
    MIRA_CHECK(other && other.value().candidates.empty());
    MIRA_CHECK(fixture.service->revise(first.work.task_id, 1, 2));
    MIRA_CHECK(!snapshot.value().current());
    MIRA_CHECK(!fixture.service->result(handle.value().operation));
    auto foreign = first;
    foreign.work.task_id = TaskId::generate();
    foreign.scopes.front().tenant_id = TenantId::generate().to_string();
    MIRA_CHECK(!fixture.service->bind(foreign));
    MIRA_CHECK(memory_user_scope(first.principal).subject_id !=
               first.principal.user_id.to_string());
    return 0;
}

int terminal_event_recording_uses_existing_write_and_approval_gates() {
    MemoryAgentFixture fixture;
    fixture.start(
        [](const MemoryAgentInput &, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            proposal.memories = {{MemoryKind::Preference, {"tone=concise", {0}}},
                                 {MemoryKind::EnvironmentFact, {"invented=verified", {0}}}};
            return proposal;
        });
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.fact(binding, "volume", "30"));
    MIRA_CHECK(fixture.service->retire(binding.work.task_id));
    MemoryAgentRequest request;
    request.mode = MemoryAgentMode::Record;
    request.task = binding.work.task_id;
    request.write_scope = binding.scopes.front();
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &result = handle.value().result.get();
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(result.value().consolidation.pending_approval.size() == 1);
    MIRA_CHECK(result.value().consolidation.pending_approval.front().proposed.verification ==
               MemoryVerification::Unverified);
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::RejectedInvalid) == 1);
    auto snapshot = fixture.service->context_supplier()(memory_request(binding.work),
                                                        memory_context(binding.work));
    MIRA_CHECK(!snapshot);
    MemoryQuery query;
    query.scopes = binding.scopes;
    query.text = "volume";
    auto recalled = fixture.memory->query(query);
    MIRA_CHECK(recalled && recalled.value().records.size() == 1);
    MIRA_CHECK(recalled.value().records.front().verification == MemoryVerification::Verified);
    return 0;
}

int compression_requires_real_citations_and_matching_protocol_prefix() {
    MemoryAgentFixture fixture;
    fixture.start(
        [](const MemoryAgentInput &, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            proposal.summary = {{"old decision: volume stays below 30", {0, 1}}};
            return proposal;
        });
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Compress;
    request.history = {
        history_text("volume below 30"), history_text("acknowledged", ModelRole::Assistant),
        history_text("new correction: 20"), history_text("latest answer", ModelRole::Assistant)};
    request.protected_items = 2;
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(handle.value().result.get());
    MIRA_CHECK(handle.value().result.get().value().projection_artifact.has_value());
    const auto artifact = *handle.value().result.get().value().projection_artifact;
    FileArtifactStore artifacts(fixture.root / "artifacts");
    MIRA_CHECK(artifacts.open(artifact));
    const auto transform = fixture.service->history_transformer(binding.work.task_id);
    auto compacted = transform(request.history);
    MIRA_CHECK(compacted && compacted.value().size() == 3);
    MIRA_CHECK(compacted.value().front().authority == Sensitivity::Public);
    MIRA_CHECK(prompt_digest(std::vector<ModelInputItem>{compacted.value().back()}) ==
               prompt_digest(std::vector<ModelInputItem>{request.history.back()}));
    auto changed = request.history;
    changed.front() = history_text("different source");
    auto untouched = transform(changed);
    MIRA_CHECK(untouched && untouched.value().size() == changed.size());
    MIRA_CHECK(fixture.service->revise(binding.work.task_id, 1, 1));
    MIRA_CHECK(transform(request.history).value().size() == request.history.size());
    MIRA_CHECK(fixture.service->erase_scope(binding.scopes.front()));
    MIRA_CHECK(!artifacts.open(artifact));
    MIRA_CHECK(fixture.service->close());
    fixture.start();
    MIRA_CHECK(!fixture.service->bind(binding));
    MIRA_CHECK(!artifacts.open(artifact));
    return 0;
}
} // namespace
int main() {
    if (scopes_and_work_revisions_are_isolated())
        return 1;
    if (terminal_event_recording_uses_existing_write_and_approval_gates())
        return 1;
    return compression_requires_real_citations_and_matching_protocol_prefix();
}
