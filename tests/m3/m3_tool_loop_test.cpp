// AgentLoop tool closure and user-message tests (DEC-015, DEC-016, issue #8).
// The recording provider serves canonical ModelResponses so the assertions
// see exactly what the model saw, including tool exposure and fed-back results.

#include "support/harness_support.hpp"

#include "support/test.hpp"

#include <mira/agent_loop.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/conversation_log.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>
#include <mira/tool_executor.hpp>

#include <executor/executor.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::adapters::simulator;
using namespace mira::testing;

class ToolLoopFixture final {
  public:
    ToolLoopFixture() {
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 2;
        executor_config.max_threads = 2;
        executor_config.queue_capacity = 32;
        executor_.initialize(executor_config);
        environment_ = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                 ModelGatewayConfig{});
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);

        const auto registration = make_wait_tool();
        wait_spec_ = registration.spec;
        (void)registry_->register_tool(registration.spec, registration.handler);

        spec_.task_id = TaskId::generate();
        spec_.session_id = SessionId::generate();
        spec_.task_epoch = 1;
        spec_.profile_id = profile_->id;
        spec_.goal = "send the daily report";
    }

    ~ToolLoopFixture() { (void)executor_.shutdown(true); }

    void use_provider(std::vector<ModelResponse> script) {
        provider_ = std::make_shared<RecordingProvider>(profile_, std::move(script));
        gateway_->register_provider(provider_);
    }

    [[nodiscard]] std::unique_ptr<AgentLoop> make_loop(const AgentLoopConfig &config,
                                                       bool with_tools = true) {
        auto loop = std::make_unique<AgentLoop>(environment_, *gateway_, config);
        loop->set_event_store(events_, runtime_, spec_.session_id);
        if (with_tools) {
            loop->set_tool_registry(registry_);
        }
        admission_->activate(spec_.task_id, spec_.task_epoch);
        return loop;
    }

    executor::Executor executor_;
    std::shared_ptr<SimulatorEnvironment> environment_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<RecordingProvider> provider_;
    std::shared_ptr<BuiltinToolRegistry> registry_ = std::make_shared<BuiltinToolRegistry>();
    BuiltinToolSpec wait_spec_;
    std::shared_ptr<MemoryEventStore> events_ = std::make_shared<MemoryEventStore>();
    RuntimeId runtime_ = RuntimeId::generate();
    AgentLoopSpec spec_;

    [[nodiscard]] const BuiltinToolSpec &wait_spec() const { return wait_spec_; }
};

int tool_round_trips_through_the_loop() {
    ToolLoopFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 20})json"),
        text_response(R"json({"action":"done","reason":"report sent"})json"),
    });
    ModelDoneVerifier verifier;
    auto loop = fixture.make_loop(AgentLoopConfig{});
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(result.value().steps.size() == 2);
    MIRA_CHECK(result.value().steps.front().action_summary == "tool:wait");

    // The first request exposed exactly the registered tool.
    const auto first = fixture.provider_->requests().front();
    MIRA_CHECK(first.tools.size() == 1);
    MIRA_CHECK(first.tools.front().wire_name == "wait");
    MIRA_CHECK(first.tools.front().tool_id == fixture.wait_spec().tool_id);

    // The second request carried the bounded tool result back to the model.
    const auto second_text = fixture.provider_->request_text(1);
    MIRA_CHECK(second_text.find("Tool results from the previous turn") != std::string::npos);
    MIRA_CHECK(second_text.find("waited_ms") != std::string::npos);
    return 0;
}

int tool_proposals_without_registry_fail_closed() {
    ToolLoopFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 20})json"),
    });
    ModelDoneVerifier verifier;
    auto loop = fixture.make_loop(AgentLoopConfig{}, /*with_tools=*/false);
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    // Without a registry no tool is exposed, so the proposal is rejected at
    // resolution upstream and the loop settles Failed without dispatching.
    MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary.find("no usable decision") != std::string::npos);
    return 0;
}

int failed_tool_results_feed_back_to_the_model() {
    ToolLoopFixture fixture;
    // duration_ms above the schema maximum: a model-attributable failure that
    // must come back as a failed record instead of settling the loop.
    fixture.use_provider({
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 99999})json"),
        text_response(R"json({"action":"done","reason":"recovered"})json"),
    });
    ModelDoneVerifier verifier;
    auto loop = fixture.make_loop(AgentLoopConfig{});
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    const auto second_text = fixture.provider_->request_text(1);
    MIRA_CHECK(second_text.find("failed") != std::string::npos);
    MIRA_CHECK(second_text.find("schema validation") != std::string::npos);
    MIRA_CHECK(result.value().steps.front().action_summary == "tool:wait");
    return 0;
}

int tool_execution_budget_is_enforced() {
    ToolLoopFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 10})json", "call-1"),
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 10})json", "call-2"),
    });
    AgentLoopConfig config;
    config.max_tool_executions = 1;
    ModelDoneVerifier verifier;
    auto loop = fixture.make_loop(config);
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary.find("budget exhausted") != std::string::npos);
    return 0;
}

int user_messages_inject_at_step_boundaries() {
    ToolLoopFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.wait_spec(), R"json({"duration_ms": 10})json"),
        text_response(R"json({"action":"done","reason":"adjusted and sent"})json"),
    });
    // Pre-run message reaches request 0; the hook fires while the loop sits
    // inside the first model call, so its message only reaches request 1
    // (DEC-016: queue drains at step boundaries).
    AgentLoopConfig config;
    config.max_pending_user_messages = 2;
    auto loop = fixture.make_loop(config);
    MIRA_CHECK(loop->enqueue_user_message("send it over DingTalk"));
    fixture.provider_->on_after_response(
        0, [&loop] { (void)loop->enqueue_user_message("and skip the attachment"); });
    ModelDoneVerifier verifier;
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);

    const auto first_text = fixture.provider_->request_text(0);
    const auto second_text = fixture.provider_->request_text(1);
    MIRA_CHECK(first_text.find("User follow-up: send it over DingTalk") != std::string::npos);
    MIRA_CHECK(first_text.find("and skip the attachment") == std::string::npos);
    MIRA_CHECK(second_text.find("User follow-up: send it over DingTalk") != std::string::npos);
    MIRA_CHECK(second_text.find("User follow-up: and skip the attachment") != std::string::npos);

    // Both injections and the outcome are reconstructable from the store.
    const auto view = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(view);
    MIRA_CHECK(view.value().size() == 3);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[0].text == "send it over DingTalk");
    MIRA_CHECK(view.value()[1].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[1].text == "and skip the attachment");
    MIRA_CHECK(view.value()[2].kind == ConversationEntry::Kind::LoopOutcome);
    MIRA_CHECK(view.value()[2].text.find("Completed") != std::string::npos);

    // Empty messages and an over-full queue are rejected (RULE-08).
    MIRA_CHECK(!loop->enqueue_user_message(""));
    MIRA_CHECK(loop->enqueue_user_message("fills the queue"));
    MIRA_CHECK(loop->enqueue_user_message("fills the queue again"));
    MIRA_CHECK(!loop->enqueue_user_message("overflows"));
    return 0;
}

int conversation_events_without_tools() {
    ToolLoopFixture fixture;
    fixture.use_provider({
        text_response(R"json({"action":"done","reason":"goal achieved"})json"),
    });
    ModelDoneVerifier verifier;
    auto loop = fixture.make_loop(AgentLoopConfig{}, /*with_tools=*/false);
    const auto result = loop->run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    const auto view = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(view);
    MIRA_CHECK(view.value().size() == 1);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::LoopOutcome);
    return 0;
}

} // namespace

int main() {
    if (const int code = tool_round_trips_through_the_loop(); code != 0) {
        return code;
    }
    if (const int code = tool_proposals_without_registry_fail_closed(); code != 0) {
        return code;
    }
    if (const int code = failed_tool_results_feed_back_to_the_model(); code != 0) {
        return code;
    }
    if (const int code = tool_execution_budget_is_enforced(); code != 0) {
        return code;
    }
    if (const int code = user_messages_inject_at_step_boundaries(); code != 0) {
        return code;
    }
    return conversation_events_without_tools();
}
