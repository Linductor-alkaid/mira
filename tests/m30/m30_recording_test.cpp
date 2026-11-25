#include "m30_support.hpp"
#include "support/conversation_loop_support.hpp"

#include <algorithm>

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

int ordinary_conversation_settlement_records_an_episode() {
    MemoryAgentConfig config;
    config.durable_record_pages = true;
    InteractionMemoryFixture host({}, config);
    const auto input = host.coordinator->submit_input(host.input("conversation", "What is 6 x 7?"));
    MIRA_CHECK(input && input.value().task_id && input.value().session_id);
    const auto binding = host.binding(input.value());
    const auto task = binding.work.task_id;
    ConversationFixture conversation;
    conversation.spec_ = binding.work;
    conversation.spec_.profile_id = conversation.profile_->id;
    conversation.use_provider({text_response("The result is 42.")});
    auto loop = conversation.make_loop(ConversationLoopConfig{}, false);
    loop.set_event_store(host.storage.events, host.storage.runtime, binding.work.session_id);
    MIRA_CHECK(
        host.coordinator->register_sink(task, std::make_shared<ConversationLoopInputSink>(loop)));
    MIRA_CHECK(host.finish(task));
    const auto answered = loop.run(conversation.spec_, conversation_context());
    MIRA_CHECK(answered && answered.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(host.coordinator->record_answer("conversation", "Answered"));
    MIRA_CHECK(host.storage.fact(binding, "volume", "30"));
    MIRA_CHECK(host.coordinator->record_settlement("conversation", TaskState::Completed));
    const auto result = host.finish(task);
    MIRA_CHECK(result && result.value().consolidation.entries.size() == 2 &&
               result.value().consolidation.count_of(CandidateDisposition::Applied) == 2);
    MIRA_CHECK(result.value().consolidation.entries.front().kind == MemoryKind::Episode &&
               result.value().consolidation.entries.back().kind == MemoryKind::EnvironmentFact);

    EventSliceQuery query;
    query.session_id = binding.work.session_id;
    query.task_id = task;
    query.event_type = "ConversationSettled";
    const auto evidence = host.storage.events->read_slice(query);
    MIRA_CHECK(evidence && evidence.value().events.size() == 1);
    const auto &event = evidence.value().events.front();
    MemoryQuery episodes;
    episodes.scopes = binding.scopes;
    episodes.kinds = std::vector{MemoryKind::Episode};
    const auto saved = host.storage.memory->query(episodes);
    MIRA_CHECK(saved && saved.value().records.size() == 1);
    const auto &record = saved.value().records.front();
    MIRA_CHECK(record.statement ==
                   "episode " + event.event_id.to_string() + "=conversation answered" &&
               record.provenance == std::vector{event.event_id} &&
               record.verification == MemoryVerification::Observed &&
               record.recorded_at == event.timestamp.wall);

    const auto receipt_hash = digest_string(
        "mira.memory-agent.record-partition.v1:" + binding.principal.tenant_id.to_string() + ':' +
        binding.principal.user_id.to_string() + ':' + binding.work.session_id.to_string() + ':' +
        task.to_string() + ':' + memory_erasure_scope_digest(binding.scopes.front()).to_string());
    Id128::Bytes partition_bytes{};
    std::copy_n(receipt_hash.bytes.begin(), partition_bytes.size(), partition_bytes.begin());
    const auto receipts = host.storage.events->read({SessionId{Id128{partition_bytes}}, {}, 2});
    MIRA_CHECK(receipts && receipts.value().events.size() == 1);
    const auto payload = parse_json(receipts.value().events.front().payload.data);
    MIRA_CHECK(payload);
    const auto *by_kind = payload.value().find("by_kind");
    MIRA_CHECK(by_kind && by_kind->find("episode") && by_kind->find("environment-fact"));
    for (const auto *kind : {"episode", "environment-fact"}) {
        const auto *counts = by_kind->find(kind);
        const auto *applied = counts->find("applied");
        const auto *rejected = counts->find("rejected");
        MIRA_CHECK(applied && applied->as_integer() == 1 && rejected &&
                   rejected->as_integer() == 0);
    }
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

int durable_record_resumes_each_scope_at_its_own_snapshot() {
    MemoryAgentFixture fixture;
    auto binding = fixture.binding();
    const auto session_scope = binding.scopes.front();
    const auto user_scope = memory_user_scope(binding.principal);
    binding.scopes.push_back(user_scope);
    MIRA_CHECK(fixture.fact(binding, "first", "a"));
    MIRA_CHECK(fixture.fact(binding, "second", "b"));
    MemoryAgentConfig config;
    config.max_events = 1;
    config.durable_record_pages = true;
    const auto bind = [&] {
        return fixture.service->bind(binding) && fixture.service->retire(binding.work.task_id);
    };
    const auto record = [&](const MemoryScope &scope,
                            std::size_t pages = 1) -> Result<MemoryAgentResult> {
        MemoryAgentRequest request;
        request.task = binding.work.task_id;
        request.mode = MemoryAgentMode::Record;
        request.write_scope = scope;
        request.record_page_limit = pages;
        auto job = fixture.service->submit(std::move(request), memory_context(binding.work));
        if (!job)
            return job.error();
        return job.value().result.get();
    };
    const auto count = [&](const MemoryScope &scope) {
        MemoryQuery query;
        query.scopes = {scope};
        return fixture.memory->query(query).value().records.size();
    };

    fixture.start({}, config);
    MIRA_CHECK(bind());
    const auto first = record(session_scope);
    MIRA_CHECK(first && first.value().more_pending && first.value().through_event_sequence == 1);
    MIRA_CHECK(count(session_scope) == 1 && count(user_scope) == 0);
    MIRA_CHECK(fixture.fact(binding, "late", "c"));
    const auto user = record(user_scope, 8);
    MIRA_CHECK(user && !user.value().more_pending &&
               user.value().consolidation.count_of(CandidateDisposition::Applied) == 3);
    MIRA_CHECK(count(session_scope) == 1 && count(user_scope) == 3);
    MIRA_CHECK(fixture.service->close());
    fixture.service.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    MIRA_CHECK(fixture.memory->close());
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    auto reopened = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(reopened);
    fixture.memory = std::shared_ptr<SqliteMemoryStore>{std::move(reopened).value()};
    fixture.runtime = RuntimeId::generate();
    fixture.start({}, config);
    MIRA_CHECK(bind());

    const auto resumed = record(session_scope, 8);
    MIRA_CHECK(resumed && !resumed.value().more_pending &&
               resumed.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(count(session_scope) == 2);
    const auto later = record(session_scope, 8);
    MIRA_CHECK(later && !later.value().more_pending &&
               later.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    const auto replay = record(user_scope, 8);
    MIRA_CHECK(replay && replay.value().consolidation.candidates_examined == 0);
    MIRA_CHECK(count(session_scope) == 3 && count(user_scope) == 3);
    MIRA_CHECK(fixture.service->erase_scope(session_scope));
    MIRA_CHECK(fixture.service->close());
    fixture.service.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    fixture.start({}, config);
    const auto erased = fixture.service->bind(binding);
    MIRA_CHECK(!erased && erased.error().code == ErrorCode::PermissionDenied);
    return 0;
}

int durable_record_preserves_an_unapproved_page() {
    MemoryAgentFixture fixture;
    auto binding = fixture.binding();
    binding.scopes = {memory_user_scope(binding.principal)};
    MIRA_CHECK(fixture.fact(binding, "first", "a"));
    MIRA_CHECK(fixture.fact(binding, "second", "b"));
    MemoryAgentConfig config;
    config.max_events = 1;
    config.durable_record_pages = true;
    config.consolidation.approval_required_kinds = {MemoryKind::Preference};
    fixture.start({}, config);
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Record;
    request.write_scope = binding.scopes.front();
    request.record_page_limit = 8;
    const auto first = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(first);
    const auto &held = first.value().result.get();
    MIRA_CHECK(held && held.value().more_pending && held.value().through_event_sequence == 0 &&
               held.value().consolidation.pending_approval.size() == 1);
    MIRA_CHECK(MemoryConsolidator::apply_pending(
        *fixture.memory, held.value().consolidation.pending_approval.front()));
    const auto retry = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(retry);
    const auto &advanced = retry.value().result.get();
    MIRA_CHECK(advanced && advanced.value().more_pending &&
               advanced.value().through_event_sequence == 1 &&
               advanced.value().consolidation.pending_approval.size() == 1);
    return 0;
}

} // namespace

int main() {
    if (const auto result = settlement_records_memory(); result != 0)
        return result;
    if (const auto result = settlement_drains_the_fixed_event_snapshot(); result != 0)
        return result;
    if (const auto result = ordinary_conversation_settlement_records_an_episode(); result != 0)
        return result;
    if (const auto result = record_page_cap_and_failed_page_resume(); result != 0)
        return result;
    if (const auto result = durable_record_resumes_each_scope_at_its_own_snapshot(); result != 0)
        return result;
    return durable_record_preserves_an_unapproved_page();
}
