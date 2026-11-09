#include "support/subagent_test_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int child_result_wakes_parent_and_preserves_history() {
    ConversationFixture fixture;
    auto log = std::make_shared<SubagentTestLog>();
    auto artifacts = std::make_shared<MemoryArtifactStore>();
    auto probe = std::make_shared<SubagentProbe>();
    probe->blocked = true;
    SubagentCoordinator owner(fixture.executor_, fixture.spec_, log, artifacts, fixture.runtime_);
    MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
    const auto tools = register_subagent_tools(owner, *fixture.registry_);
    MIRA_CHECK(tools.size() == 3);
    fixture.use_provider(
        {tool_call_response(tools[0], R"({"profile":"reader","prompt":"private child prompt"})"),
         text_response("waiting for the child"), text_response("final integrated answer")});
    auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
    MIRA_CHECK(started);
    MIRA_CHECK(probe->wait_entered());
    MIRA_CHECK(log->wait("SubagentParentWaiting"));
    MIRA_CHECK(started.value().wait_for(0ms) != std::future_status::ready);
    probe->release();
    MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
    const auto result = started.value().get();
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "final integrated answer");
    MIRA_CHECK(result.value().total_turns == 3);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(requests[2].input.size() == 6);
    MIRA_CHECK(std::holds_alternative<ToolCallPart>(requests[2].input[2].content.front()));
    MIRA_CHECK(std::holds_alternative<ToolResultPart>(requests[2].input[3].content.front()));
    MIRA_CHECK(requests[2].input[4].role == ModelRole::Assistant);
    MIRA_CHECK(requests[2].input[5].role == ModelRole::User);
    MIRA_CHECK(requests[2].input[5].provenance.source == "mira.conversation-loop.continuation.v1");
    MIRA_CHECK(fixture.provider_->request_text(2).find("child finding") != std::string::npos);
    const auto child_requests = probe->recorded();
    MIRA_CHECK(child_requests.size() == 1 && child_requests[0].tools.empty());
    MIRA_CHECK(child_requests[0].task_id != fixture.spec_.task_id);
    MIRA_CHECK(child_requests[0].input.front().provenance.source == "test.child.system.v1");
    MIRA_CHECK(log->count("SubagentResultClaimed") == 1);
    const auto handle = spawned_handle(*log, fixture.spec_.session_id);
    MIRA_CHECK(handle);
    const auto snapshot = owner.get(*handle);
    MIRA_CHECK(snapshot && snapshot.value().claimed && snapshot.value().result_artifact);
    MIRA_CHECK(snapshot.value().status == SubagentStatus::Answered);
    const auto artifact = artifacts->open(*snapshot.value().result_artifact);
    MIRA_CHECK(artifact && artifact.value().size() == std::string("child finding").size());
    EventQuery query;
    query.session_id = fixture.spec_.session_id;
    const auto page = log->read(query);
    MIRA_CHECK(page);
    // DEC-104: the parent's AssistantSpeech events carry the model-produced
    // tool call arguments (spawn prompt included, classified Internal); no
    // other event type may quote them, and the child's own answer text never
    // reaches the parent session's events at all.
    bool spawn_prompt_in_speech = false;
    for (const auto &event : page.value().events) {
        if (event.payload.type == "AssistantSpeech") {
            if (event.payload.data.find("private child prompt") != std::string::npos) {
                spawn_prompt_in_speech = true;
            }
        } else {
            MIRA_CHECK(event.payload.data.find("private child prompt") == std::string::npos);
        }
        MIRA_CHECK(event.payload.data.find("child finding") == std::string::npos);
    }
    MIRA_CHECK(spawn_prompt_in_speech);
    MIRA_CHECK(owner.close());
    MIRA_CHECK(owner.close());
    return 0;
}

int multiple_children_progress_without_default_pool_workers() {
    ConversationFixture fixture;
    auto log = std::make_shared<SubagentTestLog>();
    auto artifacts = std::make_shared<MemoryArtifactStore>();
    auto probe = std::make_shared<SubagentProbe>();
    SubagentCoordinator owner(fixture.executor_, fixture.spec_, log, artifacts, fixture.runtime_);
    MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
    const auto tools = register_subagent_tools(owner, *fixture.registry_);
    auto response = tool_call_response(tools[0], R"({"profile":"reader","prompt":"a"})", "a");
    auto second = tool_call_response(tools[0], R"({"profile":"reader","prompt":"b"})", "b");
    response.output.push_back(second.output.front());
    fixture.use_provider(
        {response, text_response("joined"), text_response("joined"), text_response("joined")});
    // Both default workers are occupied: managed I/O children and parent still progress.
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::promise<void> first_entered;
    std::promise<void> other_entered;
    auto first_ready = first_entered.get_future();
    auto other_ready = other_entered.get_future();
    auto first = fixture.executor_.submit_auto([gate, &first_entered] {
        first_entered.set_value();
        gate.wait();
    });
    auto other = fixture.executor_.submit_auto([gate, &other_entered] {
        other_entered.set_value();
        gate.wait();
    });
    const bool busy = first_ready.wait_for(3s) == std::future_status::ready &&
                      other_ready.wait_for(3s) == std::future_status::ready;
    const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
    const auto ready = started && started.value().wait_for(3s) == std::future_status::ready;
    release.set_value();
    first.get();
    other.get();
    MIRA_CHECK(busy && ready);
    const auto result = started.value().get();
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(probe->recorded().size() == 2);
    MIRA_CHECK(log->count("SubagentSettled") == 2 && log->count("SubagentResultClaimed") == 2);
    MIRA_CHECK(owner.close());
    return 0;
}

int child_failure_and_late_success_are_structured() {
    for (int mode = 0; mode < 3; ++mode) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        probe->throws = mode == 0;
        probe->fails = mode == 1;
        if (mode == 2)
            probe->script = {text_response(std::string(20'000, 'x'))};
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
        MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"test"})"),
             text_response("handled failure"), text_response("handled failure")});
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        MIRA_CHECK(started && started.value().wait_for(3s) == std::future_status::ready);
        MIRA_CHECK(started.value().get());
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        MIRA_CHECK(handle);
        const auto snapshot = owner.get(*handle);
        MIRA_CHECK(snapshot && snapshot.value().status == SubagentStatus::Failed);
        MIRA_CHECK(snapshot.value().answer.empty());
        MIRA_CHECK(snapshot.value().safe_summary.find("private") == std::string::npos);
        bool saw_failure = false;
        for (const auto &request : fixture.provider_->requests()) {
            const auto text = to_json_string(model_request_to_json(request));
            saw_failure = saw_failure || text.find("subagent-result") != std::string::npos;
        }
        MIRA_CHECK(saw_failure);
        MIRA_CHECK(owner.close());
    }
    return 0;
}
} // namespace

int main() {
    for (const auto test : {child_result_wakes_parent_and_preserves_history,
                            multiple_children_progress_without_default_pool_workers,
                            child_failure_and_late_success_are_structured}) {
        if (const auto result = test(); result != 0)
            return result;
    }
    return 0;
}
