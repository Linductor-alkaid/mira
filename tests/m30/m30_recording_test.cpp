#include "m30_support.hpp"

using namespace mira;
using namespace mira::testing;

namespace {

int settlement_records_memory() {
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
    // DEC-115: the model-recorded preference writes automatically at its
    // source trust level beside the deterministic fact.
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::Applied) == 2);
    MIRA_CHECK(result.value().consolidation.pending_approval.empty());
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::RejectedInvalid) == 1);
    MemoryQuery query;
    query.scopes = host.binding(input.value()).scopes;
    const auto recorded = host.storage.memory->query(query);
    MIRA_CHECK(recorded && recorded.value().records.size() == 2);
    bool fact_stored = false;
    bool preference_stored = false;
    for (const auto &record : recorded.value().records) {
        if (record.statement == "volume=30") {
            fact_stored = record.verification == MemoryVerification::Verified;
        } else {
            preference_stored = record.verification == MemoryVerification::Unverified;
        }
    }
    MIRA_CHECK(fact_stored && preference_stored);
    const auto status = host.coordinator->memory_status(host.principal, task);
    MIRA_CHECK(status && status.value().terminal && status.value().job);
    MIRA_CHECK(host.coordinator->record_settlement("record", TaskState::Completed));
    MIRA_CHECK(host.coordinator->memory_status(host.principal, task).value().job->operation ==
               status.value().job->operation);
    MIRA_CHECK(!host.service->context_supplier()(memory_request(host.spec(input.value())),
                                                 memory_context(host.spec(input.value()))));
    return 0;
}

int settlement_drains_the_fixed_event_snapshot() {
    MemoryAgentConfig config;
    config.max_events = 1;
    InteractionMemoryFixture host({}, config);
    const auto input = host.coordinator->submit_input(host.input("multi-page"));
    MIRA_CHECK(input && input.value().task_id);
    const auto task = *input.value().task_id;
    MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
    MIRA_CHECK(host.finish(task));
    const auto binding = host.binding(input.value());
    MIRA_CHECK(host.storage.fact(binding, "volume", "30"));
    MIRA_CHECK(host.storage.fact(binding, "brightness", "70"));
    MIRA_CHECK(host.storage.fact(binding, "contrast", "mid"));
    MIRA_CHECK(host.coordinator->record_settlement("multi-page", TaskState::Completed));
    const auto result = host.finish(task);
    MIRA_CHECK(result && !result.value().more_pending);
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::Applied) == 3);
    MemoryQuery query;
    query.scopes = binding.scopes;
    MIRA_CHECK(host.storage.memory->query(query).value().records.size() == 3);
    return 0;
}

int record_page_cap_and_failed_page_resume() {
    MemoryAgentFixture fixture;
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.fact(binding, "first", "a"));
    MIRA_CHECK(fixture.fact(binding, "second", "b"));
    MIRA_CHECK(fixture.fact(binding, "third", "c"));
    MIRA_CHECK(fixture.fact(binding, "fourth", "d"));
    bool append_late = true;
    bool fail_fourth = true;
    MemoryAgentConfig config;
    config.max_events = 1;
    fixture.start(
        [&](const MemoryAgentInput &input,
            const OperationContext &) -> Result<MemoryAgentProposal> {
            if (append_late) {
                append_late = false;
                if (!fixture.fact(binding, "late", "e"))
                    return Error{};
            }
            if (fail_fourth && !input.events.empty() &&
                input.events.front().payload.data.find("\"fourth\"") != std::string::npos) {
                fail_fourth = false;
                Error failed;
                failed.code = ErrorCode::Unavailable;
                return failed;
            }
            return MemoryAgentProposal{};
        },
        config);
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.service->retire(binding.work.task_id));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Record;
    request.write_scope = binding.scopes.front();
    request.record_page_limit = 2;
    const auto read_fact = [&](const std::string &key) {
        MemoryQuery query;
        query.scopes = binding.scopes;
        query.claim_key = key;
        return fixture.memory->query(query);
    };
    const auto process = [&]() -> Result<MemoryAgentResult> {
        auto submitted = fixture.service->submit(request, memory_context(binding.work));
        if (!submitted)
            return submitted.error();
        return submitted.value().result.get();
    };

    const auto first = process();
    MIRA_CHECK(first && first.value().more_pending && first.value().through_event_sequence == 2);
    MIRA_CHECK(first.value().consolidation.count_of(CandidateDisposition::Applied) == 2);
    const auto partial = process();
    MIRA_CHECK(!partial && partial.error().code == ErrorCode::Unavailable);
    MIRA_CHECK(read_fact("third").value().records.size() == 1);
    MIRA_CHECK(read_fact("fourth").value().records.empty());
    const auto resumed = process();
    MIRA_CHECK(resumed && !resumed.value().more_pending &&
               resumed.value().through_event_sequence > first.value().through_event_sequence);
    MIRA_CHECK(resumed.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(read_fact("fourth").value().records.size() == 1);
    MIRA_CHECK(read_fact("late").value().records.empty());
    const auto later = process();
    MIRA_CHECK(later && !later.value().more_pending);
    MIRA_CHECK(later.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(read_fact("late").value().records.size() == 1);
    request.record_page_limit = kMaxRecordPagesPerJob + 1;
    const auto invalid = process();
    MIRA_CHECK(!invalid && invalid.error().code == ErrorCode::InvalidArgument);
    return 0;
}

} // namespace

int main() {
    if (const auto result = settlement_records_memory(); result != 0)
        return result;
    if (const auto result = settlement_drains_the_fixed_event_snapshot(); result != 0)
        return result;
    return record_page_cap_and_failed_page_resume();
}
