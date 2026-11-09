#include "m29_support.hpp"
#include "support/action_dispatch_support.hpp"
#include "support/conversation_loop_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::testing;

namespace {
int correction_during_inference() {
    for (int mode : {0, 1, 2, 3}) {
        CoordinatorFixture host;
        MIRA_CHECK(host.start());
        const auto principal = make_principal();
        const auto input = host.submit(host.envelope("goal", "original task", principal));
        MIRA_CHECK(input && input.value().task_id);
        ConversationFixture model;
        model.spec_.task_id = *input.value().task_id;
        model.spec_.session_id = *input.value().session_id;
        model.spec_.task_epoch = 0;
        const bool device = mode == 1 || mode == 2;
        const bool repair = mode == 2;
        auto script = std::vector{
            text_response(device ? R"({"action":"tap","x":0.5,"y":0.5,"reason":"obsolete"})"
                                 : "obsolete answer"),
            text_response(device ? R"({"action":"done","reason":"corrected goal"})"
                                 : "corrected answer")};
        if (repair)
            script.insert(script.begin(), text_response("malformed"));
        model.use_provider(script);
        ConversationLoopConfig config;
        if (mode == 3)
            config.max_recoveries = 0;
        auto conversation = model.make_loop(config, false);
        AgentLoop agent(host.environment_, *model.gateway_);
        wire_scripted_action_dispatch(agent, model.spec_.session_id);
        const std::shared_ptr<IUserInputSink> sink =
            device ? std::shared_ptr<IUserInputSink>{std::make_shared<AgentLoopInputSink>(agent)}
                   : std::shared_ptr<IUserInputSink>{
                         std::make_shared<ConversationLoopInputSink>(conversation)};
        MIRA_CHECK(host.coordinator_->register_sink(model.spec_.task_id, sink));
        bool corrected = false;
        model.provider_->on_after_response(repair ? 1 : 0, [&] {
            auto envelope = host.envelope("correction", "use the corrected goal", principal);
            envelope.target_task = model.spec_.task_id;
            const auto result = host.submit(envelope);
            corrected = result && result.value().stage == InputStage::Routed;
        });
        auto context = plain_loop_context();
        context.task = model.spec_.task_id;
        context.session = model.spec_.session_id;
        if (device) {
            ModelDoneVerifier verifier;
            const auto result = agent.run(model.spec_, context, verifier);
            MIRA_CHECK(result && result.value().outcome == LoopOutcome::Completed);
            MIRA_CHECK(host.environment_->executed_event_count() == 0);
        } else {
            const auto result = conversation.run(model.spec_, context);
            MIRA_CHECK(result);
            if (mode == 3) {
                MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
                MIRA_CHECK(result.value().answer.empty());
            } else
                MIRA_CHECK(result.value().answer == "corrected answer");
        }
        MIRA_CHECK(corrected);
        const auto calls = model.provider_->requests();
        MIRA_CHECK(calls.size() == (mode == 3 ? 1U : repair ? 3U : 2U));
        if (mode != 3)
            MIRA_CHECK(
                model.provider_->request_text(calls.size() - 1).find("use the corrected goal") !=
                std::string::npos);
    }
    return 0;
}

int correction_skips_unstarted_tools_and_preserves_native_results() {
    for (bool device : {false, true}) {
        ConversationFixture fixture;
        ConversationLoop conversation = fixture.make_loop({}, false);
        auto environment = std::make_shared<test::FakeEnvironment>();
        AgentLoop agent(environment, *fixture.gateway_);
        AgentLoopInputSink device_sink(agent);
        ConversationLoopInputSink conversation_sink(conversation);
        auto registry = std::make_shared<BuiltinToolRegistry>();
        BuiltinToolSpec tool;
        tool.wire_name = "bounded_work";
        tool.description = "One bounded scripted operation.";
        tool.parameters_schema = JsonSchema{JsonValue::Object{{"type", "object"}}};
        int executions = 0;
        bool delivered = false;
        MIRA_CHECK(registry->register_tool(
            tool, [&](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
                ++executions;
                InputEnvelope input;
                input.input_id = "new-requirement";
                input.content = "stop the remaining old operations";
                delivered = (device ? device_sink.deliver(input, fixture.spec_.task_id)
                                    : conversation_sink.deliver(input, fixture.spec_.task_id))
                                .has_value();
                return JsonValue{JsonValue::Object{{"completed", true}}};
            }));
        auto batch = tool_call_response(tool, "{}", "first-call");
        const auto second = tool_call_response(tool, "{}", "second-call");
        batch.output.push_back(second.output.front());
        fixture.use_provider(
            {batch, text_response(device ? R"({"action":"done","reason":"updated"})" : "updated")});
        agent.set_tool_registry(registry);
        conversation.set_tool_registry(registry);
        if (device) {
            ModelDoneVerifier verifier;
            const auto result = agent.run(fixture.spec_, plain_loop_context(), verifier);
            MIRA_CHECK(result && result.value().outcome == LoopOutcome::Completed);
        } else {
            const auto result = conversation.run(fixture.spec_, plain_loop_context());
            MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
            MIRA_CHECK(result.value().tool_executions == 1);
        }
        MIRA_CHECK(delivered && executions == 1);
        MIRA_CHECK(fixture.provider_->requests().size() == 2);
        const auto requests = fixture.provider_->requests();
        const auto &request = requests.back();
        std::size_t results = 0;
        for (const auto &item : request.input)
            for (const auto &part : item.content)
                if (std::holds_alternative<ToolResultPart>(part))
                    ++results;
        // AgentLoop renders records into the primary envelope; conversation
        // keeps native calls/results. Both must preserve the skipped receipt.
        if (!device)
            MIRA_CHECK(results == 2);
        MIRA_CHECK(fixture.provider_->request_text(1).find("stop the remaining old operations") !=
                   std::string::npos);
    }
    return 0;
}
class InterruptingEnvironment final : public IEnvironment {
  public:
    EnvironmentCapabilities capabilities() const override { return inner.capabilities(); }
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override {
        ++observations;
        return inner.observe(request, context);
    }
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override {
        auto result = inner.execute(input, context);
        if (on_execute)
            on_execute();
        return result;
    }
    Result<void> interrupt(const OperationContext &context) override {
        return inner.interrupt(context);
    }
    test::FakeEnvironment inner;
    int observations = 0;
    std::function<void()> on_execute;
};

int admitted_action_verifies_before_processing_correction() {
    CoordinatorFixture host;
    MIRA_CHECK(host.start());
    const auto principal = make_principal();
    const auto input = host.submit(host.envelope("action-goal", "original goal", principal));
    MIRA_CHECK(input);
    ConversationFixture model;
    model.spec_.task_id = *input.value().task_id;
    model.spec_.session_id = *input.value().session_id;
    model.spec_.task_epoch = 0;
    model.use_provider({text_response(R"({"action":"tap","x":0.5,"y":0.5,"reason":"original"})"),
                        text_response(R"({"action":"done","reason":"corrected"})")});
    auto environment = std::make_shared<InterruptingEnvironment>();
    AgentLoop loop(environment, *model.gateway_);
    model.admission_->activate(model.spec_.task_id, model.spec_.task_epoch);
    wire_scripted_action_dispatch(loop, model.spec_.session_id);
    MIRA_CHECK(host.coordinator_->register_sink(model.spec_.task_id,
                                                std::make_shared<AgentLoopInputSink>(loop)));
    bool delivered = false;
    environment->on_execute = [&] {
        auto correction = host.envelope("action-correction", "the goal has changed", principal);
        correction.target_task = model.spec_.task_id;
        const auto result = host.submit(correction);
        delivered = result && result.value().stage == InputStage::Routed;
    };
    class Satisfied final : public ILoopVerifier {
      public:
        Verdict verify(const Observation &, const DecisionCandidate &) override {
            return Verdict::Satisfied;
        }
    } verifier;
    const auto result = loop.run(model.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result && result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(delivered && environment->inner.executed_event_count() == 1);
    MIRA_CHECK(environment->observations == 4);
    MIRA_CHECK(model.provider_->requests().size() == 2);
    MIRA_CHECK(model.provider_->request_text(1).find("the goal has changed") != std::string::npos);
    return 0;
}
int runtime_control_invalidates_late_completion() {
    for (bool takeover : {false, true}) {
        for (bool device : {false, true}) {
            CoordinatorFixture host;
            MIRA_CHECK(host.start());
            const auto principal = make_principal();
            const auto input =
                host.submit(host.envelope("control-goal", "original goal", principal));
            MIRA_CHECK(input);
            ConversationFixture model;
            model.spec_.task_id = *input.value().task_id;
            model.spec_.session_id = *input.value().session_id;
            model.spec_.task_epoch = 0;
            model.use_provider({text_response(
                device ? R"({"action":"tap","x":0.5,"y":0.5,"reason":"late"})" : "late answer")});
            auto conversation = model.make_loop({}, false);
            AgentLoop agent(host.environment_, *model.gateway_);
            bool controlled = false;
            model.provider_->on_after_response(0, [&] {
                const auto command =
                    takeover ? host.runtime_->request_human_takeover(model.spec_.session_id)
                             : host.coordinator_->cancel_work(model.spec_.task_id);
                if (command) {
                    const auto outcome = command.value().outcome(std::chrono::seconds(2));
                    controlled = outcome && outcome.value().status == SettlementStatus::Applied;
                }
            });
            auto context = plain_loop_context();
            context.cancellation_requested = [&] {
                const auto current = host.runtime_->task_snapshot(model.spec_.task_id);
                return !current || current.value().epoch != 0 || is_terminal(current.value().state);
            };
            if (device) {
                ModelDoneVerifier verifier;
                const auto result = agent.run(model.spec_, context, verifier);
                MIRA_CHECK(result && result.value().outcome == LoopOutcome::Cancelled);
            } else {
                const auto result = conversation.run(model.spec_, context);
                MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Cancelled);
                MIRA_CHECK(result.value().answer.empty());
            }
            MIRA_CHECK(controlled && host.environment_->executed_event_count() == 0);
            MIRA_CHECK(model.provider_->requests().size() == 1);
        }
    }
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(correction_during_inference() == 0);
    MIRA_CHECK(runtime_control_invalidates_late_completion() == 0);
    MIRA_CHECK(admitted_action_verifies_before_processing_correction() == 0);
    MIRA_CHECK(correction_skips_unstarted_tools_and_preserves_native_results() == 0);
    return 0;
}
