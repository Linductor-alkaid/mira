#include "m30_support.hpp"
#include "support/conversation_loop_support.hpp"

#include <mira/memory_recording_report.hpp>

#include <algorithm>

using namespace mira;
using namespace mira::testing;

namespace {

int collects_six_recording_systems() {
    MemoryRecordingReport report;
    AgentPersonaSuggestionReceipt persona{Id128::generate(), true};
    report.collect(Result{persona});

    MemoryAgentResult record;
    record.mode = MemoryAgentMode::Record;
    record.more_pending = true;
    record.rejected_record_rows = 1;
    record.consolidation.candidates_truncated = 2;
    ConsolidationEntry same_claim;
    same_claim.reason_code = "duplicate-noop";
    ConsolidationEntry awaiting_approval;
    awaiting_approval.disposition = CandidateDisposition::PendingApproval;
    record.consolidation.entries = {same_claim, awaiting_approval};
    report.collect(Result{record});

    Error denied;
    denied.code = ErrorCode::PermissionDenied;
    report.collect(Result<SocialMessageAttributeReport>{denied});

    ConsolidationReport world;
    world.entries.emplace_back();
    report.collect_world_fact(Result{world});

    GoalCreationReceipt first_goal;
    GoalCreationReceipt repeated_goal;
    repeated_goal.idempotent_replay = true;
    report.collect(Result{std::vector{first_goal, repeated_goal}});
    RecurringPlanCreationReceipt repeated_plan;
    repeated_plan.idempotent_replay = true;
    report.collect(Result{repeated_plan});

    WorkflowRuntime::SkillHintRecordReport skill;
    ConsolidationEntry rejected_hint;
    rejected_hint.disposition = CandidateDisposition::RejectedForbidden;
    skill.consolidation.entries.push_back(rejected_hint);
    report.collect(Result{skill});
    Error unavailable;
    unavailable.code = ErrorCode::Unavailable;
    unavailable.retryable = true;
    report.collect(Result<WorkflowRuntime::SkillHintRecordReport>{unavailable});

    const auto attempts = report.attempts();
    MIRA_CHECK(attempts.size() == 8);
    MIRA_CHECK(attempts[0].system == MemoryRecordingSystem::Persona &&
               attempts[0].counts.pending_approval == 1 && attempts[0].counts.replayed == 1 &&
               attempts[0].counts.applied == 0);
    MIRA_CHECK(attempts[1].system == MemoryRecordingSystem::Event && attempts[1].more_pending &&
               attempts[1].counts.unchanged == 1 && attempts[1].counts.pending_approval == 1 &&
               attempts[1].counts.rejected == 1 && attempts[1].counts.truncated == 2);
    MIRA_CHECK(attempts[2].system == MemoryRecordingSystem::Social &&
               attempts[2].error == ErrorCode::PermissionDenied && !attempts[2].retryable &&
               attempts[2].counts.rejected == 0);
    MIRA_CHECK(attempts[3].system == MemoryRecordingSystem::World &&
               attempts[3].counts.applied == 1 && !attempts[3].error);
    MIRA_CHECK(attempts[4].system == MemoryRecordingSystem::Goal &&
               attempts[4].counts.applied == 1 && attempts[4].counts.replayed == 1 &&
               attempts[4].counts.unchanged == 1);
    MIRA_CHECK(attempts[5].system == MemoryRecordingSystem::Goal &&
               attempts[5].counts.applied == 0 && attempts[5].counts.replayed == 1);
    MIRA_CHECK(attempts[6].system == MemoryRecordingSystem::Skill &&
               attempts[6].counts.rejected == 1 && !attempts[6].error);
    MIRA_CHECK(attempts[7].system == MemoryRecordingSystem::Skill &&
               attempts[7].error == ErrorCode::Unavailable && attempts[7].retryable);
    const auto goals = report.totals(MemoryRecordingSystem::Goal);
    MIRA_CHECK(goals.applied == 1 && goals.unchanged == 2 && goals.replayed == 2);
    const auto skills = report.totals(MemoryRecordingSystem::Skill);
    MIRA_CHECK(skills.rejected == 1 && skills.applied == 0);
    return 0;
}

SessionId record_receipt_partition(const MemoryWorkBinding &binding) {
    const auto hash = digest_string(
        "mira.memory-agent.record-partition.v1:" + binding.principal.tenant_id.to_string() + ':' +
        binding.principal.user_id.to_string() + ':' + binding.work.session_id.to_string() + ':' +
        binding.work.task_id.to_string() + ':' +
        memory_erasure_scope_digest(binding.scopes.front()).to_string());
    Id128::Bytes bytes{};
    std::copy_n(hash.bytes.begin(), bytes.size(), bytes.begin());
    return SessionId{Id128{bytes}};
}

int settlement_records_memory() {
    InteractionMemoryFixture host(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.mode == MemoryAgentMode::Record && !input.events.empty()) {
                proposal.memories = {{MemoryKind::Preference, {"tone=brief", {0}}},
                                     {MemoryKind::RecoveryLesson, {"model guessed lesson", {0}}},
                                     {MemoryKind::SocialAttribute, {"model guessed name", {0}}}};
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
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::RejectedInvalid) == 2);
    MIRA_CHECK(result.value().consolidation.entries.back().reason_code ==
               "social-assertion-requires-host-source");
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
    MIRA_CHECK(result.value().deterministic_record_only);
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

    const auto receipts = host.storage.events->read({record_receipt_partition(binding), {}, 2});
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

int record_isolates_bad_citations_across_durable_pages() {
    MemoryAgentFixture fixture;
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.fact(binding, "first", "a"));
    MIRA_CHECK(fixture.fact(binding, "second", "b"));
    MemoryAgentConfig config;
    config.max_events = 1;
    config.durable_record_pages = true;
    fixture.start(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.events.empty())
                return proposal;
            proposal.memories = {{MemoryKind::Preference, {"invented-source=private-text", {1}}},
                                 {MemoryKind::Preference, {"tone=brief", {0}}}};
            return proposal;
        },
        config);
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Record;
    request.write_scope = binding.scopes.front();
    request.record_page_limit = 2;
    const auto context = memory_context(binding.work);
    MIRA_CHECK(fixture.service->bind(binding));
    const auto submitted = fixture.service->submit(request, context);
    MIRA_CHECK(submitted);
    const auto report = submitted.value().result.get();
    MIRA_CHECK(report && report.value().through_event_sequence == 2 &&
               report.value().rejected_record_rows == 2 && report.value().record_pages.size() == 2);
    MIRA_CHECK(report.value().consolidation.count_of(CandidateDisposition::Applied) == 4);
    for (std::size_t i = 0; i < report.value().record_pages.size(); ++i) {
        const auto &page = report.value().record_pages[i];
        MIRA_CHECK(page.after == i && page.through == i + 1 && page.rejected.size() == 1 &&
                   page.rejected.front().row == 0 &&
                   page.rejected.front().reason == MemoryAgentRecordRejectReason::InvalidCitation);
    }
    const auto tools = fixture.service->tools();
    const auto result_tool = std::ranges::find_if(
        tools, [](const auto &tool) { return tool.spec.wire_name == "memory_result"; });
    MIRA_CHECK(result_tool != tools.end());
    const auto status = result_tool->handler(
        JsonValue::Object{{"operation", submitted.value().operation.to_string()}}, context);
    MIRA_CHECK(status && status.value().find("rejected_model_rows") &&
               status.value().find("rejected_model_rows")->as_integer() == 2);
    const auto *counts = status.value().find("recording_counts");
    MIRA_CHECK(counts && counts->find("applied") && counts->find("applied")->as_integer() == 4 &&
               counts->find("rejected") && counts->find("rejected")->as_integer() == 2);
    const auto status_text = to_json_string(status.value());
    MIRA_CHECK(status_text.find("invalid-citation") != std::string::npos &&
               status_text.find("private-text") == std::string::npos);

    MemoryQuery query;
    query.scopes = binding.scopes;
    const auto stored = fixture.memory->query(query);
    MIRA_CHECK(stored && stored.value().records.size() == 3 &&
               std::ranges::none_of(stored.value().records, [](const auto &record) {
                   return record.statement.find("private-text") != std::string::npos;
               }));
    const auto receipts = fixture.events->read({record_receipt_partition(binding), {}, 3});
    MIRA_CHECK(receipts && receipts.value().events.size() == 2);
    for (const auto &event : receipts.value().events) {
        const auto payload = parse_json(event.payload.data);
        MIRA_CHECK(payload && payload.value().find("rejected_model_rows") &&
                   payload.value().find("rejected_model_rows")->as_integer() == 1);
        MIRA_CHECK(event.payload.data.find("private-text") == std::string::npos);
    }
    MIRA_CHECK(fixture.service->close());
    fixture.service.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    fixture.start({}, config);
    MIRA_CHECK(fixture.service->bind(binding));
    const auto replay = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(replay);
    const auto resumed = replay.value().result.get();
    MIRA_CHECK(resumed && resumed.value().through_event_sequence >= 2 &&
               resumed.value().consolidation.entries.empty());
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
    if (const auto result = collects_six_recording_systems(); result != 0)
        return result;
    if (const auto result = settlement_records_memory(); result != 0)
        return result;
    if (const auto result = settlement_drains_the_fixed_event_snapshot(); result != 0)
        return result;
    if (const auto result = ordinary_conversation_settlement_records_an_episode(); result != 0)
        return result;
    if (const auto result = record_isolates_bad_citations_across_durable_pages(); result != 0)
        return result;
    if (const auto result = record_page_cap_and_failed_page_resume(); result != 0)
        return result;
    if (const auto result = durable_record_resumes_each_scope_at_its_own_snapshot(); result != 0)
        return result;
    return durable_record_preserves_an_unapproved_page();
}
