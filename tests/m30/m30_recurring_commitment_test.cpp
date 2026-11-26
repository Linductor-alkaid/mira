#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/goal_commitment_recording.hpp>

#include <atomic>

namespace {
using namespace mira;
using namespace mira::testing;

struct AnswerEvidence final {
    GoalCommitmentBinding binding;
    GoalCommitmentSource source;
    OperationContext operation;
    EventId answer;
};

std::optional<AnswerEvidence> settle_answer(MemoryAgentFixture &fixture, std::string text) {
    ConversationFixture conversation;
    auto operation = conversation_context();
    operation.session = conversation.spec_.session_id;
    operation.task = conversation.spec_.task_id;
    operation.task_epoch = conversation.spec_.task_epoch;
    conversation.use_provider({text_response(std::move(text))});
    auto loop = conversation.make_loop(ConversationLoopConfig{}, false);
    loop.set_event_store(fixture.events, fixture.runtime, operation.session);
    const auto result = loop.run(conversation.spec_, operation);
    if (!result || result.value().outcome != ConversationOutcome::Answered)
        return {};
    operation.deadline.reset();
    EventSliceQuery query;
    query.session_id = operation.session;
    query.event_type = "ConversationSettled";
    const auto page = fixture.events->read_slice(query);
    if (!page || page.value().events.size() != 1)
        return {};
    const auto &event = page.value().events.front();
    const auto payload = parse_json(event.payload.data);
    const auto *detail = payload ? payload.value().find("detail") : nullptr;
    const auto *answer = detail ? detail->find("answer_event") : nullptr;
    const auto id =
        answer && answer->is_string() ? EventId::parse(*answer->as_string()) : std::nullopt;
    if (!id)
        return {};
    return AnswerEvidence{
        {fixture.runtime, {fixture.principal.tenant_id, fixture.principal.user_id}},
        {event.session_sequence, event.event_id},
        std::move(operation),
        *id};
}

int fixed_plan_retains_progress_and_authorization() {
    MemoryAgentFixture fixture;
    const std::string quote = "I will check the report every twelve hours";
    const auto evidence = settle_answer(fixture, quote + ".");
    MIRA_CHECK(evidence);
    std::atomic<bool> allow_plan{false};
    const auto authorize_store = [&](const GoalStoreAccess &access) {
        return access.owner == evidence->binding.owner &&
               access.resource == GoalStoreResource::RecurringPlan &&
               (access.operation != GoalStoreOperation::Create || allow_plan);
    };
    SqliteGoalStoreOptions options;
    options.path = fixture.root / "goals.db";
    auto opened = SqliteGoalStore::open_authorized(fixture.executor, options, authorize_store);
    MIRA_CHECK(opened);
    auto goals = std::move(opened).value();
    bool allow_source = true;
    const auto authorize_source = [&](const GoalCommitmentBinding &binding,
                                      const OperationContext &operation) {
        return allow_source && binding.runtime == evidence->binding.runtime &&
               binding.owner == evidence->binding.owner &&
               operation.session == evidence->operation.session &&
               operation.task == evidence->operation.task;
    };
    const auto record = [&](const GoalRecurringPlanCandidate &candidate) {
        return fixture.executor
            .submit_auto([&] {
                return record_conversation_recurring_plan(
                    *goals, *fixture.events, evidence->binding, evidence->source, candidate,
                    evidence->operation, authorize_source);
            })
            .get();
    };
    const auto first =
        std::chrono::time_point_cast<std::chrono::milliseconds>(Timestamp::now().wall) + 1h;
    const GoalRecurringPlanCandidate candidate{quote, first, 12h};
    const auto denied = record(candidate);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
    allow_plan = true;
    allow_source = false;
    const auto no_source = record(candidate);
    MIRA_CHECK(!no_source && no_source.error().code == ErrorCode::PermissionDenied);
    allow_source = true;
    auto forged = candidate;
    forged.quote = "I will send the report every twelve hours";
    MIRA_CHECK(!record(forged));
    MIRA_CHECK(goals->due_plans({evidence->binding.owner, first, 4, {}}).value().records.empty());

    const auto created = record(candidate);
    MIRA_CHECK(created && !created.value().idempotent_replay && created.value().record.definition);
    const auto &plan = created.value().record;
    MIRA_CHECK(plan.next_at == first && plan.definition->source_event == evidence->answer &&
               plan.definition->title == quote);
    const auto due = goals->due_plans({evidence->binding.owner, first, 4, {}});
    MIRA_CHECK(due && due.value().records.size() == 1 &&
               due.value().records.front().key == plan.key);
    MIRA_CHECK(goals->record_occurrence({plan.key, plan.revision, first}, first));
    MIRA_CHECK(goals->close());
    goals.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    auto reopened = SqliteGoalStore::open_authorized(fixture.executor, options, authorize_store);
    MIRA_CHECK(reopened);
    goals = std::move(reopened).value();
    const auto replay = record(candidate);
    MIRA_CHECK(replay && replay.value().idempotent_replay &&
               replay.value().record.key == plan.key &&
               replay.value().record.next_at == first + 12h);
    auto changed = candidate;
    changed.interval = 24h;
    const auto conflict = record(changed);
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(goals->cancel_plan(plan.key, plan.revision, {EventId::generate(), first}));
    MIRA_CHECK(!record(candidate));
    MIRA_CHECK(goals->erase_plan(plan.key));
    MIRA_CHECK(!record(candidate) &&
               goals->read_plan(plan.key).value()->state == RecurringPlanState::Erased);
    return 0;
}

int calendar_plan_coexists_with_one_time_goal() {
    MemoryAgentFixture fixture;
    const std::string title = "I will send a status note each morning";
    const std::string single_quote = "I will review the draft today";
    const auto evidence = settle_answer(fixture, single_quote + ". " + title + ".");
    MIRA_CHECK(evidence);
    SqliteGoalStoreOptions options;
    options.path = fixture.root / "goals.db";
    auto opened = SqliteGoalStore::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto goals = std::move(opened).value();
    const auto authorize = [&](const GoalCommitmentBinding &binding, const OperationContext &) {
        return binding.owner == evidence->binding.owner;
    };
    const GoalTime day =
        std::chrono::floor<std::chrono::days>(Timestamp::now().wall) + std::chrono::days{1};
    const GoalCalendarRule schedule{
        "Test/Eight", "fixture-v1", day - std::chrono::days{3}, day + std::chrono::days{7}, 8h, {},
        9h,           0x7f};
    const GoalRecurringPlanCandidate candidate{title, day, 0ms, schedule};
    const auto record = [&](const GoalRecurringPlanCandidate &request) {
        return fixture.executor
            .submit_auto([&] {
                return record_conversation_recurring_plan(*goals, *fixture.events,
                                                          evidence->binding, evidence->source,
                                                          request, evidence->operation, authorize);
            })
            .get();
    };
    auto invalid = candidate;
    invalid.interval = 1h;
    MIRA_CHECK(!record(invalid));
    const auto recurring = record(candidate);
    MIRA_CHECK(recurring && recurring.value().record.definition &&
               recurring.value().record.definition->calendar == schedule &&
               recurring.value().record.next_at == day + 1h);
    const auto once = fixture.executor
                          .submit_auto([&] {
                              return record_conversation_commitment(
                                  *goals, *fixture.events, evidence->binding, evidence->source,
                                  {single_quote, day, {}}, evidence->operation, authorize);
                          })
                          .get();
    MIRA_CHECK(once && once.value().record.definition &&
               once.value().record.definition->source_event == evidence->answer &&
               once.value().record.key != recurring.value().record.key);
    return 0;
}
} // namespace

int main() {
    if (const int result = fixed_plan_retains_progress_and_authorization(); result != 0)
        return result;
    return calendar_plan_coexists_with_one_time_goal();
}
