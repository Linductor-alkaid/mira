#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/memory_recording_batch.hpp>
#include <mira/social_mention_inference.hpp>

#include <array>
#include <exception>
#include <future>
#include <iostream>
#include <map>

namespace {
using namespace mira;
using namespace mira::testing;

class RecordingBatchWorker final : public kairo::IBlockingIoWorker {
  public:
    RecordingBatchWorker(const MemoryRecordingOwners &owners,
                         std::span<const MemoryRecordingCandidate> candidates,
                         const OperationContext &operation,
                         std::promise<Result<MemoryRecordingBatchResult>> &completion)
        : owners_(owners), candidates_(candidates), operation_(operation), completion_(completion) {
    }

    void run(kairo::StopToken stop) override {
        try {
            const auto inherited = operation_.cancellation_requested;
            operation_.cancellation_requested = [stop, inherited] {
                return stop.stop_requested() || (inherited && inherited());
            };
            completion_.set_value(record_memory_batch(owners_, candidates_, operation_));
        } catch (...) {
            completion_.set_exception(std::current_exception());
        }
    }

    void wakeup() noexcept override {} // Called owners observe the shared operation context.

  private:
    const MemoryRecordingOwners &owners_;
    std::span<const MemoryRecordingCandidate> candidates_;
    OperationContext operation_;
    std::promise<Result<MemoryRecordingBatchResult>> &completion_;
};

Result<MemoryRecordingBatchResult>
run_recording_batch(kairo::Executor &executor, const MemoryRecordingOwners &owners,
                    std::span<const MemoryRecordingCandidate> candidates,
                    const OperationContext &operation) {
    std::promise<Result<MemoryRecordingBatchResult>> completion;
    auto outcome = completion.get_future();
    kairo::BlockingWorkerSpec spec;
    spec.name = "recording-batch-test-" + Id128::generate().to_string();
    spec.config.thread_name = "mira-rec-batch";
    spec.worker = std::make_unique<RecordingBatchWorker>(owners, candidates, operation, completion);
    auto worker = executor.start_worker(std::move(spec));
    if (!worker.started()) {
        Error error;
        error.code = ErrorCode::Unavailable;
        error.safe_message = worker.start_result().message;
        return error;
    }
    outcome.wait();
    worker.stop();
    return outcome.get();
}

int routes_a_mixed_batch_without_bypassing_owners() {
    MemoryAgentFixture fixture;
    ConversationFixture conversation;
    auto work = fixture.binding();
    conversation.spec_ = work.work;
    conversation.spec_.profile_id = conversation.profile_->id;
    conversation.runtime_ = fixture.runtime;
    constexpr std::string_view commitment = "I will check the draft";
    constexpr std::string_view recurring = "I will send a weekly summary";
    conversation.use_provider(
        {text_response("I will check the draft. I will send a weekly summary.")});
    auto operation = memory_context(work.work);
    auto loop = conversation.make_loop(ConversationLoopConfig{}, false);
    loop.set_event_store(fixture.events, fixture.runtime, operation.session);
    MIRA_CHECK(loop.enqueue_user_message("Use a concise tone"));
    const auto response = loop.run(conversation.spec_, operation);
    MIRA_CHECK(response && response.value().outcome == ConversationOutcome::Answered);
    EventSliceQuery query;
    query.session_id = operation.session;
    query.event_type = "UserMessageInjected";
    const auto user = fixture.events->read_slice(query);
    query.event_type = "ConversationSettled";
    const auto settlement = fixture.events->read_slice(query);
    MIRA_CHECK(user && user.value().events.size() == 1 && settlement &&
               settlement.value().events.size() == 1);
    const auto &user_event = user.value().events.front();
    const auto &settled = settlement.value().events.front();

    const SocialEnvironmentKey room{fixture.principal.tenant_id, Id128::generate()};
    SqliteSocialDirectoryOptions social_options;
    social_options.path = fixture.root / "social.db";
    auto social = SqliteSocialDirectory::open(fixture.executor, social_options, fixture.memory);
    MIRA_CHECK(social);
    const SocialAccountKey sender{"qq", Id128::generate()};
    const SocialAccountKey other{"bilibili", Id128::generate()};
    const auto indexed = [&](const SocialAccountKey &account,
                             std::string message) -> Result<SocialMessageEvidence> {
        const auto id = EventId::generate();
        const auto event = fixture.events->append(
            {id,
             fixture.runtime,
             operation.session,
             operation.task,
             {std::string{kSocialMessageTextEvent}, std::move(message), EventClass::State}});
        if (!event)
            return event.error();
        return social.value()->index_message(
            room, account, {operation.session, event.value().session_sequence, id},
            *fixture.events);
    };
    const auto other_speaker = indexed(other, "The notes are ready.");
    const auto message = indexed(sender, "I work on Backend and mentor Chen.");
    MIRA_CHECK(other_speaker && message &&
               social.value()->activate(other_speaker.value().speaker, {2, {"Chen"}}));

    const GoalOwner goal_owner{fixture.principal.tenant_id, fixture.principal.user_id};
    SqliteGoalStoreOptions goal_options;
    goal_options.path = fixture.root / "goals.db";
    auto goals = SqliteGoalStore::open_authorized(
        fixture.executor, goal_options,
        [&](const GoalStoreAccess &access) { return access.owner == goal_owner; });
    MIRA_CHECK(goals);
    MemoryAgentConfig memory_config;
    memory_config.durable_record_pages = true;
    fixture.start(
        [&](const auto &, const auto &) -> Result<MemoryAgentProposal> {
            return fixture.executor.submit_auto([] { return MemoryAgentProposal{}; }).get();
        },
        memory_config);
    MIRA_CHECK(fixture.service->bind(work));
    MIRA_CHECK(fixture.service->retire(work.work.task_id));
    AgentPersonaCatalog catalog;
    const AgentPersonaKey persona_key{fixture.principal.tenant_id, Id128::generate()};
    MIRA_CHECK(catalog.activate(persona_key, {1, "Speak clearly."}));
    std::map<Id128, AgentPersonaSuggestion> review_queue;
    MemoryRecordingOwners owners{*fixture.events};
    owners.bind_persona =
        [&](const OperationContext &context) -> Result<std::optional<AgentPersonaBinding>> {
        if (context.task != operation.task || context.session != operation.session)
            return std::optional<AgentPersonaBinding>{};
        return std::optional{
            AgentPersonaBinding{catalog.read(persona_key).value(), [] { return true; }}};
    };
    owners.stage_persona = [&](const AgentPersonaSuggestion &suggestion,
                               const OperationContext &) -> Result<AgentPersonaSuggestionReceipt> {
        const auto added = review_queue.emplace(suggestion.id, suggestion).second;
        return AgentPersonaSuggestionReceipt{suggestion.id, !added};
    };
    owners.event_memory = fixture.service.get();
    owners.social = social.value().get();
    bool allow_social = true;
    owners.authorize_social = [&](const auto &target, const auto &source, const auto &) {
        return allow_social && target == room && source == message.value().message;
    };
    owners.goals = goals.value().get();
    owners.authorize_goal = [&](const GoalCommitmentBinding &binding, const auto &context) {
        return binding.owner == goal_owner && binding.runtime == fixture.runtime &&
               context.task == operation.task && context.session == operation.session;
    };
    const GoalCommitmentBinding goal_binding{fixture.runtime, goal_owner};
    const GoalCommitmentSource goal_source{settled.session_sequence, settled.event_id};
    const auto due =
        std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) +
        1h;
    const std::array<MemoryRecordingCandidate, 8> candidates{
        PersonaRecordingCandidate{
            {fixture.runtime, user_event.session_sequence, user_event.event_id},
            "Use a concise tone"},
        SocialAttributeRecordingCandidate{room, message.value().message, {{"team", "invented"}}},
        EventRecordingCandidate{work.scopes.front(), 8},
        SocialAttributeRecordingCandidate{room, message.value().message, {{"team", "Backend"}}},
        SocialRelationshipRecordingCandidate{
            room, message.value().message, {{"mentors", "Chen", "mentor Chen", true}}},
        GoalRecordingCandidate{goal_binding, goal_source, {{{std::string(commitment), due, {}}}}},
        SkillHintRecordingCandidate{"demo.skill", {}, "Use the published procedure"},
        RecurringPlanRecordingCandidate{
            goal_binding, goal_source, {std::string(recurring), due, 168h}}};
    const auto run = [&](std::span<const MemoryRecordingCandidate> input) {
        return run_recording_batch(fixture.executor, owners, input, operation);
    };
    const auto result = run(candidates);
    MIRA_CHECK(result && !result.value().interruption &&
               result.value().outcomes.size() == candidates.size());
    const auto attempts = result.value().report.attempts();
    MIRA_CHECK(attempts[0].counts.pending_approval == 1 && review_queue.size() == 1);
    MIRA_CHECK(attempts[1].error == ErrorCode::InvalidArgument && !attempts[2].error &&
               attempts[2].counts.applied == 1);
    MIRA_CHECK(attempts[3].counts.applied == 1 && attempts[4].counts.applied == 1);
    MIRA_CHECK(attempts[5].counts.applied == 1 &&
               attempts[6].error == ErrorCode::UnsupportedCapability &&
               attempts[7].counts.applied == 1);
    const auto &raw_goal =
        std::get<Result<std::vector<GoalCreationReceipt>>>(result.value().outcomes[5]);
    MIRA_CHECK(raw_goal && raw_goal.value().size() == 1);
    MIRA_CHECK(catalog.read(persona_key).value().revision().version == 1);

    allow_social = false;
    const auto repeated = run(candidates);
    MIRA_CHECK(repeated && repeated.value().outcomes.size() == candidates.size());
    const auto retry = repeated.value().report.attempts();
    MIRA_CHECK(retry[0].counts.replayed == 1 && review_queue.size() == 1);
    MIRA_CHECK(retry[2].counts.applied == 0 && retry[3].error == ErrorCode::PermissionDenied);
    MIRA_CHECK(retry[5].counts.replayed == 1 && retry[7].counts.replayed == 1);
    const auto &replayed_goal =
        std::get<Result<std::vector<GoalCreationReceipt>>>(repeated.value().outcomes[5]);
    MIRA_CHECK(replayed_goal &&
               replayed_goal.value().front().record.key == raw_goal.value().front().record.key);
    return 0;
}

int interruption_preserves_the_processed_prefix() {
    MemoryAgentFixture fixture;
    auto operation = memory_context(fixture.binding().work);
    MemoryRecordingOwners owners{*fixture.events};
    bool cancelled = false;
    int persona_calls = 0;
    owners.bind_persona = [&](const auto &) -> Result<std::optional<AgentPersonaBinding>> {
        ++persona_calls;
        cancelled = true;
        return std::optional<AgentPersonaBinding>{};
    };
    owners.stage_persona = [](const auto &, const auto &) -> Result<AgentPersonaSuggestionReceipt> {
        Error error;
        error.code = ErrorCode::InvalidState;
        return error;
    };
    operation.cancellation_requested = [&] { return cancelled; };
    const MemoryRecordingCandidate persona =
        PersonaRecordingCandidate{{fixture.runtime, 1, EventId::generate()}, "A short note"};
    const auto run = [&](std::span<const MemoryRecordingCandidate> input) {
        return fixture.executor
            .submit_auto([&] { return record_memory_batch(owners, input, operation); })
            .get();
    };
    const std::vector oversized(kMaxMemoryRecordingBatchSize + 1, persona);
    const auto over_limit = run(oversized);
    MIRA_CHECK(!over_limit && over_limit.error().code == ErrorCode::ResourceExhausted &&
               persona_calls == 0);
    const std::array<MemoryRecordingCandidate, 3> candidates{EventRecordingCandidate{}, persona,
                                                             persona};
    const auto interrupted = run(candidates);
    MIRA_CHECK(interrupted && interrupted.value().interruption == ErrorCode::Cancelled &&
               interrupted.value().outcomes.size() == 2 && persona_calls == 1);
    const auto attempts = interrupted.value().report.attempts();
    MIRA_CHECK(attempts[0].error == ErrorCode::UnsupportedCapability &&
               attempts[1].error == ErrorCode::PermissionDenied);
    cancelled = false;
    operation.deadline = operation.started_at.monotonic;
    const auto expired = run(candidates);
    MIRA_CHECK(expired && expired.value().interruption == ErrorCode::DeadlineExceeded &&
               expired.value().outcomes.empty() && persona_calls == 1);
    return 0;
}

} // namespace

int main() try {
    if (const auto result = routes_a_mixed_batch_without_bypassing_owners(); result != 0)
        return result;
    return interruption_preserves_the_processed_prefix();
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "recording batch test raised an unknown exception\n";
    return 1;
}
