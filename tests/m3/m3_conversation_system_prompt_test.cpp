#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mira;
using namespace mira::testing;

constexpr auto kHostText =
    "Replying with text does not reach anyone; speech happens only through the send tools.\n"
    "Tool results arrive on the next turn. Terminal text must be exactly 无事.";

ConversationLoopConfig host_config(bool explicit_digest = false) {
    ConversationLoopConfig config;
    config.system_prompt = ConversationSystemPrompt{kHostText, "private-host.speech.v2"};
    if (explicit_digest) {
        config.system_prompt->template_digest = digest_string("host-template-resource-v2");
    }
    return config;
}

bool matches_host(const ModelRequest &request, const ConversationLoopConfig &config) {
    const auto &prompt = *config.system_prompt;
    const auto &system = request.input.front();
    const auto *text = std::get_if<TextPart>(&system.content.front());
    return system.role == ModelRole::System && system.authority == Sensitivity::Internal &&
           system.provenance.source == prompt.template_id && system.content.size() == 1 && text &&
           text->text == prompt.text && text->sensitivity == Sensitivity::Internal &&
           request.prompt_provenance.system_template_digest ==
               prompt.template_digest.value_or(digest_string(prompt.text)) &&
           std::count_if(request.input.begin(), request.input.end(),
                         [](const auto &item) { return item.role == ModelRole::System; }) == 1;
}

int legacy_default_is_unchanged() {
    for (const bool tools : {false, true}) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("answer")});
        auto loop = fixture.make_loop({}, tools);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
        const auto requests = fixture.provider_->requests();
        const auto &request = requests.front();
        std::string expected =
            "You are Mira, a conversational agent. Answer the user's request directly with "
            "a final text answer. You may call the exposed tools when they help; tool results "
            "arrive on your next turn. A text reply without a tool call ends the conversation.";
        if (tools) {
            expected += " Keep using tools until you can answer, then reply with text only.";
        }
        MIRA_CHECK(request.input.size() == 2);
        MIRA_CHECK(request.input.front().provenance.source == "mira.conversation-loop.system.v1");
        MIRA_CHECK(std::get<TextPart>(request.input.front().content.front()).text == expected);
        MIRA_CHECK(request.prompt_provenance.system_template_digest ==
                   digest_string("mira.conversation-loop.system.v1"));
        const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
        MIRA_CHECK(std::count(types.begin(), types.end(), "ConversationSystemPromptSelected") == 0);
    }
    return 0;
}

int host_prompt_with_dynamic_tools() {
    for (const auto mode : {ConversationLimitMode::Finite, ConversationLimitMode::Continuous}) {
        for (const bool explicit_digest : {false, true}) {
            ConversationFixture fixture;
            fixture.use_provider(
                {tool_call_response(fixture.echo_spec(), R"({"message":"speech"})"),
                 text_response("无事")});
            auto config = host_config(explicit_digest);
            config.limit_mode = mode;
            auto loop = fixture.make_loop(config);
            std::size_t selections = 0;
            bool valid = true;
            std::vector<ModelRequestId> assembled_ids;
            loop.set_tool_selector([&](const AgentLoopSpec &,
                                       const ModelRequest &request) -> Result<std::vector<ToolId>> {
                ++selections;
                assembled_ids.push_back(request.request_id);
                valid = valid && matches_host(request, config) && request.tools.size() == 1;
                // The handler's result changes the next turn's available set.
                if (fixture.seen_arguments().empty()) {
                    return std::vector<ToolId>{fixture.echo_spec().tool_id};
                }
                valid = valid && request.input.size() == 4 &&
                        std::holds_alternative<ToolCallPart>(request.input[2].content.front()) &&
                        std::holds_alternative<ToolResultPart>(request.input[3].content.front());
                return std::vector<ToolId>{};
            });
            const auto result = loop.run(fixture.spec_, conversation_context());
            MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
            MIRA_CHECK(result.value().answer == "无事" && result.value().tool_executions == 1);
            MIRA_CHECK(selections == 2 && valid);
            const auto requests = fixture.provider_->requests();
            MIRA_CHECK(requests.size() == 2 && requests[0].tools.size() == 1 &&
                       requests[1].tools.empty());
            for (const auto &request : requests) {
                MIRA_CHECK(matches_host(request, config));
                const auto decoded = model_request_from_json(model_request_to_json(request));
                MIRA_CHECK(decoded && matches_host(decoded.value(), config));
                NullArtifactSource artifacts;
                ResponsesV1Mapper responses;
                ChatCompletionsV1Mapper chat;
                AnthropicMessagesV1Mapper anthropic;
                for (const auto *mapper : {static_cast<const IDialectMapper *>(&responses),
                                           static_cast<const IDialectMapper *>(&chat),
                                           static_cast<const IDialectMapper *>(&anthropic)}) {
                    auto profile = make_profile(mapper->dialect(), "https://api.test");
                    for (const bool stream : {false, true}) {
                        const auto encoded =
                            mapper->encode_request(request, profile, stream, artifacts);
                        MIRA_CHECK(encoded);
                        const auto wire = to_json_string(encoded.value());
                        MIRA_CHECK(wire.find("speech happens only through the send tools") !=
                                   std::string::npos);
                        MIRA_CHECK(wire.find("then reply with text only") == std::string::npos);
                    }
                }
            }
            EventQuery query;
            query.session_id = fixture.spec_.session_id;
            const auto page = fixture.events_->read(query);
            MIRA_CHECK(page);
            std::size_t selected = 0;
            for (const auto &event : page.value().events) {
                MIRA_CHECK(event.payload.data.find("private-host") == std::string::npos);
                MIRA_CHECK(event.payload.data.find("speech happens") == std::string::npos);
                if (event.payload.type != "ConversationSystemPromptSelected") {
                    continue;
                }
                const auto payload = parse_json(event.payload.data);
                MIRA_CHECK(payload);
                const auto *detail = payload.value().find("detail");
                MIRA_CHECK(detail && selected < assembled_ids.size());
                MIRA_CHECK(*detail->find("request_id")->as_string() ==
                           assembled_ids[selected++].to_string());
                MIRA_CHECK(*detail->find("system_template_digest")->as_string() ==
                           requests[0].prompt_provenance.system_template_digest.to_string());
                MIRA_CHECK(*detail->find("rendered_system_digest")->as_string() ==
                           digest_string(kHostText).to_string());
            }
            MIRA_CHECK(selected == 2);
        }
    }
    return 0;
}

int invalid_config_rejected_before_callbacks() {
    for (int invalid = 0; invalid < 5; ++invalid) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("must not infer")});
        auto config = host_config();
        switch (invalid) {
        case 0:
            config.system_prompt->text.clear();
            break;
        case 1:
            config.system_prompt->text = " \t\r\n\f\v";
            break;
        case 2:
            config.system_prompt->template_id.clear();
            break;
        case 3:
            config.system_prompt->template_id = " \n\t";
            break;
        default:
            config.system_prompt->template_digest = Hash{};
            break;
        }
        auto loop = fixture.make_loop(config);
        std::size_t callbacks = 0;
        loop.set_model_input_supplier([&](const AgentLoopSpec &) {
            ++callbacks;
            return Result<std::vector<ModelInputItem>>{std::vector<ModelInputItem>{}};
        });
        loop.set_tool_selector([&](const AgentLoopSpec &, const ModelRequest &) {
            ++callbacks;
            return Result<std::vector<ToolId>>{std::vector<ToolId>{}};
        });
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(!result && result.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(callbacks == 0 && fixture.provider_->requests().empty());
        MIRA_CHECK(fixture.seen_arguments().empty());
        MIRA_CHECK(session_event_types(*fixture.events_, fixture.spec_.session_id).empty());
        MIRA_CHECK(result.error().safe_message.find("private-host") == std::string::npos);
    }
    return 0;
}

int discovery_can_expose_a_new_tool_next_turn() {
    ConversationFixture fixture;
    auto send = fixture.echo_spec();
    send.tool_id = ToolId::generate();
    send.wire_name = "send_message";
    std::size_t sends = 0;
    MIRA_CHECK(fixture.registry_->register_tool(
        send, [&](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            ++sends;
            return JsonValue{JsonValue::Object{{"delivered", true}}};
        }));
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), R"({"message":"discover send_message"})"),
         tool_call_response(send, R"({"message":"hello"})"), text_response("无事")});
    const auto config = host_config();
    auto loop = fixture.make_loop(config);
    bool valid = true;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &request) -> Result<std::vector<ToolId>> {
            valid = valid && matches_host(request, config) && request.tools.size() == 2;
            if (fixture.seen_arguments().empty()) {
                return std::vector<ToolId>{fixture.echo_spec().tool_id};
            }
            if (sends == 0) {
                return std::vector<ToolId>{send.tool_id};
            }
            return std::vector<ToolId>{};
        });
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered && valid);
    MIRA_CHECK(result.value().tool_executions == 2 && sends == 1);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(requests[0].tools.size() == 1 &&
               requests[0].tools.front().tool_id == fixture.echo_spec().tool_id);
    MIRA_CHECK(requests[1].tools.size() == 1 && requests[1].tools.front().tool_id == send.tool_id);
    MIRA_CHECK(requests[2].tools.empty() && requests[2].input.size() == 6);
    return 0;
}

int recovery_retry_and_cancellation() {
    for (const bool gateway_retry : {false, true}) {
        ModelGatewayConfig gateway_config;
        gateway_config.retry_budget.max_attempts = gateway_retry ? 2 : 1;
        ConversationFixture fixture(ProtocolDialect::OpenAIResponsesV1, gateway_config);
        fixture.use_flaky_provider({make_model_error(ModelDomainCode::RateLimited, "retry", true)},
                                   {text_response("无事")});
        const auto config = host_config(true);
        auto loop = fixture.make_loop(config, false);
        std::size_t selections = 0;
        bool valid = true;
        loop.set_tool_selector([&](const AgentLoopSpec &, const ModelRequest &request) {
            ++selections;
            valid = valid && matches_host(request, config);
            return Result<std::vector<ToolId>>{std::vector<ToolId>{}};
        });
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered && valid);
        MIRA_CHECK(selections == (gateway_retry ? 1U : 2U));
        MIRA_CHECK(result.value().recoveries == (gateway_retry ? 0U : 1U));
    }
    for (const bool pre_cancel : {false, true}) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("must not infer")});
        auto loop = fixture.make_loop(host_config());
        bool cancelled = pre_cancel;
        std::size_t selections = 0;
        loop.set_tool_selector([&](const AgentLoopSpec &, const ModelRequest &) {
            ++selections;
            cancelled = true;
            return Result<std::vector<ToolId>>{std::vector<ToolId>{}};
        });
        const auto result =
            loop.run(fixture.spec_, conversation_context([&] { return cancelled; }));
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Cancelled);
        MIRA_CHECK(selections == (pre_cancel ? 0U : 1U));
        MIRA_CHECK(fixture.provider_->requests().empty() && fixture.seen_arguments().empty());
    }
    return 0;
}

int host_prompt_does_not_grant_unselected_tools() {
    ConversationFixture fixture;
    fixture.use_provider({tool_call_response(fixture.echo_spec(), R"({"message":"blocked"})")});
    auto loop = fixture.make_loop(host_config());
    loop.set_tool_selector([](const AgentLoopSpec &, const ModelRequest &) {
        return Result<std::vector<ToolId>>{std::vector<ToolId>{}};
    });
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(fixture.seen_arguments().empty());
    return 0;
}
} // namespace

int main() {
    for (const auto test :
         {legacy_default_is_unchanged, host_prompt_with_dynamic_tools,
          discovery_can_expose_a_new_tool_next_turn, invalid_config_rejected_before_callbacks,
          recovery_retry_and_cancellation, host_prompt_does_not_grant_unselected_tools}) {
        if (const auto code = test(); code != 0) {
            return code;
        }
    }
    return 0;
}
