#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_upload.hpp>

#include <algorithm>

#include <stdexcept>

namespace {
using namespace mira;
using namespace mira::testing;

bool add_catalog(ConversationFixture &fixture, std::size_t count = 174) {
    while (fixture.registry_->size() < count) {
        BuiltinToolSpec spec;
        spec.wire_name = "catalog_" + std::to_string(fixture.registry_->size());
        spec.description = "Read-only fixture tool";
        spec.parameters_schema = JsonSchema{parse_json(R"({"type":"object"})").value()};
        if (!fixture.registry_->register_tool(spec,
                                              [](const JsonValue &, const OperationContext &) {
                                                  return Result<JsonValue>{JsonValue::Object{}};
                                              })) {
            return false;
        }
    }
    return true;
}

int dynamic_selection_and_history() {
    ConversationFixture fixture;
    MIRA_CHECK(add_catalog(fixture));
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), R"({"message":"hello"})"), text_response("done")});
    auto loop = fixture.make_loop({});
    std::size_t calls = 0;
    bool context_ok = true;
    loop.set_tool_selector([&](const AgentLoopSpec &spec,
                               const ModelRequest &request) -> Result<std::vector<ToolId>> {
        ++calls;
        context_ok = context_ok && spec.session_id == fixture.spec_.session_id &&
                     request.task_id == spec.task_id && request.tools.size() == 174 &&
                     request.input.size() >= 2 && request.generation.max_output_tokens.has_value();
        if (calls == 1) {
            return std::vector<ToolId>{fixture.echo_spec().tool_id};
        }
        context_ok = context_ok && request.input.size() == 4;
        fixture.profile_->capabilities.limits.max_tools_per_request = 0;
        return std::vector<ToolId>{};
    });
    auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(calls == 2 && context_ok && fixture.registry_->size() == 174);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2 && requests[0].tools.size() == 1 && requests[1].tools.empty());
    MIRA_CHECK(fixture.seen_arguments().size() == 1);
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::count(types.begin(), types.end(), "ModelToolsSelected") == 2);
    EventQuery query;
    query.session_id = fixture.spec_.session_id;
    const auto page = fixture.events_->read(query);
    MIRA_CHECK(page);
    std::size_t selection_events = 0;
    for (const auto &event : page.value().events) {
        if (event.payload.type != "ModelToolsSelected")
            continue;
        const auto payload = parse_json(event.payload.data);
        MIRA_CHECK(payload);
        const auto *detail = payload.value().find("detail");
        MIRA_CHECK(detail && detail->find("available_count")->as_integer() == 174);
        const auto &exposed = requests[selection_events++].tools;
        MIRA_CHECK(*detail->find("tool_snapshot_digest")->as_string() ==
                   tool_snapshot_digest(exposed).to_string());
        MIRA_CHECK(detail->find("selected_tool_ids")->as_array()->size() == exposed.size());
    }
    MIRA_CHECK(selection_events == 2);
    NullArtifactSource artifacts;
    for (const auto dialect :
         {ProtocolDialect::OpenAIResponsesV1, ProtocolDialect::OpenAIChatCompletionsV1,
          ProtocolDialect::AnthropicMessagesV1}) {
        auto profile = make_profile(dialect, "https://api.test");
        profile.capabilities.limits.max_tools_per_request = 0;
        ResponsesV1Mapper responses;
        ChatCompletionsV1Mapper chat;
        AnthropicMessagesV1Mapper messages;
        const IDialectMapper &mapper = dialect == ProtocolDialect::OpenAIResponsesV1
                                           ? static_cast<const IDialectMapper &>(responses)
                                       : dialect == ProtocolDialect::OpenAIChatCompletionsV1
                                           ? static_cast<const IDialectMapper &>(chat)
                                           : static_cast<const IDialectMapper &>(messages);
        MIRA_CHECK(mapper.encode_request(requests[1], profile, false, artifacts));
        MIRA_CHECK(mapper.encode_request(requests[1], profile, true, artifacts));
    }
    return 0;
}

int device_loop_uses_same_selection() {
    ConversationFixture fixture;
    MIRA_CHECK(add_catalog(fixture));
    fixture.use_provider({tool_call_response(fixture.echo_spec(), R"({"message":"hello"})"),
                          text_response(R"({"action":"done","reason":"verified"})")});
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>(
        adapters::simulator::SimulatorSetup::single_display());
    AgentLoop loop(environment, *fixture.gateway_);
    loop.set_tool_registry(fixture.registry_);
    fixture.admission_->activate(fixture.spec_.task_id, fixture.spec_.task_epoch);
    std::size_t calls = 0;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &request) -> Result<std::vector<ToolId>> {
            ++calls;
            if (request.tools.size() != 174 || request.input.empty()) {
                throw std::runtime_error("bad fixture");
            }
            return std::vector<ToolId>{fixture.echo_spec().tool_id};
        });
    ModelDoneVerifier verifier;
    const auto result = loop.run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result && result.value().outcome == LoopOutcome::Completed && calls == 2);
    MIRA_CHECK(fixture.seen_arguments().size() == 1);
    MIRA_CHECK(fixture.provider_->requests()[0].tools.size() == 1);
    return 0;
}

int invalid_selection_fails_closed() {
    for (int mode = 0; mode < 5; ++mode) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("must not run")});
        auto loop = fixture.make_loop({});
        loop.set_tool_selector(
            [&](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
                const auto id = fixture.echo_spec().tool_id;
                switch (mode) {
                case 0:
                    return std::vector<ToolId>{ToolId::generate()};
                case 1:
                    return std::vector<ToolId>{id, id};
                case 2:
                    throw std::runtime_error("secret exception text");
                case 3:
                    return make_model_error(ModelDomainCode::TransportFailed, "secret error text");
                default:
                    return std::vector<ToolId>(129, id);
                }
            });
        const auto result = loop.run(fixture.spec_, plain_loop_context());
        MIRA_CHECK(!result || result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(fixture.provider_->requests().empty());
        const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
        MIRA_CHECK(std::count(types.begin(), types.end(), "ModelToolSelectionFailed") == 1);
        const auto summary = result ? result.value().safe_summary : result.error().safe_message;
        MIRA_CHECK(summary.find("secret") == std::string::npos);
    }
    return 0;
}

int unselected_tool_cannot_execute() {
    ConversationFixture fixture;
    MIRA_CHECK(add_catalog(fixture));
    fixture.use_provider({tool_call_response(fixture.echo_spec(), R"({"message":"blocked"})")});
    auto loop = fixture.make_loop({});
    loop.set_tool_selector(
        [](const AgentLoopSpec &, const ModelRequest &request) -> Result<std::vector<ToolId>> {
            return std::vector<ToolId>{request.tools.front().tool_id};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(!result || result.value().outcome != ConversationOutcome::Answered);
    MIRA_CHECK(fixture.seen_arguments().empty());
    return 0;
}

int defaults_limits_order_and_cancel() {
    for (const auto count : {64U, 65U, 128U, 129U, 174U}) {
        ConversationFixture fixture;
        MIRA_CHECK(add_catalog(fixture, count));
        fixture.profile_->capabilities.limits.max_tools_per_request = count == 128 ? 128 : 64;
        fixture.use_provider({text_response("done")});
        auto loop = fixture.make_loop({});
        const auto result = loop.run(fixture.spec_, plain_loop_context());
        const bool accepted = count == 64 || count == 128;
        MIRA_CHECK((result && result.value().outcome == ConversationOutcome::Answered) == accepted);
        MIRA_CHECK(fixture.provider_->requests().size() == (accepted ? 1 : 0));
        if (!accepted) {
            const auto message = result ? result.value().safe_summary : result.error().safe_message;
            MIRA_CHECK(message.find(std::to_string(count)) != std::string::npos);
            MIRA_CHECK(message.find(count > 128 ? "128" : "64") != std::string::npos);
        }
    }
    ConversationFixture fixture;
    MIRA_CHECK(add_catalog(fixture));
    fixture.use_provider({text_response("done")});
    auto loop = fixture.make_loop({});
    const auto catalog = fixture.registry_->exposed_tools();
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
            return std::vector<ToolId>{catalog[2].tool_id, catalog[0].tool_id};
        });
    auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    const auto exposed = fixture.provider_->requests()[0].tools;
    MIRA_CHECK(exposed.size() == 2 && exposed[0].tool_id == catalog[2].tool_id &&
               exposed[1].tool_id == catalog[0].tool_id);
    std::size_t calls = 0;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
            ++calls;
            return std::vector<ToolId>{};
        });
    auto context = plain_loop_context();
    context.cancellation_requested = [] { return true; };
    result = loop.run(fixture.spec_, context);
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Cancelled && calls == 0);
    return 0;
}

class CountingRemoteFiles final : public IRemoteFileStore {
  public:
    Result<RemoteFileRef> upload(const ArtifactRef &, const std::string &, const ModelDataPolicy &,
                                 const OperationContext &) override {
        ++uploads;
        return make_model_error(ModelDomainCode::TransportFailed, "must not upload");
    }
    Result<void> retire(const RemoteFileRef &, std::chrono::seconds) override { return {}; }
    std::vector<RemoteFileAudit> audit() const override { return {}; }
    std::size_t uploads = 0;
};

int mapper_and_provider_limits() {
    ConversationFixture fixture;
    MIRA_CHECK(add_catalog(fixture, 129));
    fixture.use_provider({text_response("done")});
    auto loop = fixture.make_loop({});
    loop.set_tool_selector(
        [](const AgentLoopSpec &, const ModelRequest &request) -> Result<std::vector<ToolId>> {
            return std::vector<ToolId>{request.tools.front().tool_id};
        });
    MIRA_CHECK(loop.run(fixture.spec_, plain_loop_context()));
    auto request = fixture.provider_->requests()[0];
    NullArtifactSource artifacts;
    for (const auto dialect :
         {ProtocolDialect::OpenAIResponsesV1, ProtocolDialect::OpenAIChatCompletionsV1,
          ProtocolDialect::AnthropicMessagesV1}) {
        auto profile = make_profile(dialect, "https://api.test");
        ResponsesV1Mapper responses;
        ChatCompletionsV1Mapper chat;
        AnthropicMessagesV1Mapper messages;
        const IDialectMapper &mapper = dialect == ProtocolDialect::OpenAIResponsesV1
                                           ? static_cast<const IDialectMapper &>(responses)
                                       : dialect == ProtocolDialect::OpenAIChatCompletionsV1
                                           ? static_cast<const IDialectMapper &>(chat)
                                           : static_cast<const IDialectMapper &>(messages);
        for (const auto limit : {0U, 64U, 128U}) {
            profile.capabilities.limits.max_tools_per_request = limit;
            MIRA_CHECK(profile.validate());
            request.tools = fixture.registry_->exposed_tools();
            request.tools.resize(limit);
            MIRA_CHECK(validate_model_request(request));
            MIRA_CHECK(mapper.encode_request(request, profile, false, artifacts));
            MIRA_CHECK(mapper.encode_request(request, profile, true, artifacts));
            request.tools = fixture.registry_->exposed_tools();
            request.tools.resize(limit + 1);
            const auto encoded = mapper.encode_request(request, profile, false, artifacts);
            MIRA_CHECK(!encoded);
            MIRA_CHECK(encoded.error().safe_message.find(std::to_string(limit + 1)) !=
                       std::string::npos);
            MIRA_CHECK(!mapper.encode_request(request, profile, true, artifacts));
            auto secrets = std::make_shared<MapSecretResolver>();
            auto transport = std::make_shared<MockHttpTransport>(secrets);
            auto remote = std::make_shared<CountingRemoteFiles>();
            OpenAiCompatibleProvider provider(std::make_shared<ModelProfile>(profile), transport,
                                              nullptr, nullptr, remote);
            auto upload_request = request;
            upload_request.data_policy.allow_uploads = true;
            ImagePart image;
            image.source.id = ArtifactId::generate();
            image.media_type = "image/png";
            upload_request.input[1].content.emplace_back(image);
            MIRA_CHECK(!provider.infer(upload_request, plain_loop_context(), {}));
            MIRA_CHECK(remote->uploads == 0);
            MIRA_CHECK(!provider.infer(request, plain_loop_context(), {}));
            MIRA_CHECK(transport->recorded().empty());
        }
        profile.capabilities.limits.max_tools_per_request = 129;
        MIRA_CHECK(!profile.validate());
        request.tools.clear();
        MIRA_CHECK(!mapper.encode_request(request, profile, false, artifacts));
    }
    return 0;
}

int retry_reuses_selection() {
    ConversationFixture fixture;
    auto retry = make_model_error(ModelDomainCode::RateLimited, "retry", true);
    fixture.use_flaky_provider({retry}, {text_response("done")});
    auto loop = fixture.make_loop({});
    std::size_t calls = 0;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
            ++calls;
            return std::vector<ToolId>{};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(calls == 1 && fixture.flaky_infer_calls() == 2);
    return 0;
}
int recovery_reselects() {
    ModelGatewayConfig gateway_config;
    gateway_config.retry_budget.max_attempts = 1;
    ConversationFixture fixture(ProtocolDialect::OpenAIResponsesV1, gateway_config);
    auto retry = make_model_error(ModelDomainCode::RateLimited, "recover", true);
    fixture.use_flaky_provider({retry}, {text_response("done")});
    auto loop = fixture.make_loop({});
    std::size_t calls = 0;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
            ++calls;
            return std::vector<ToolId>{};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(calls == 2 && result.value().recoveries == 1);
    return 0;
}

int continuous_selection_and_empty_registry() {
    ConversationFixture fixture;
    fixture.profile_->capabilities.limits.max_tools_per_request = 0;
    fixture.use_provider({text_response("done")});
    ConversationLoopConfig config;
    config.limit_mode = ConversationLimitMode::Continuous;
    auto loop = fixture.make_loop(config, false);
    std::size_t calls = 0;
    bool empty_catalog = false;
    loop.set_tool_selector(
        [&](const AgentLoopSpec &, const ModelRequest &request) -> Result<std::vector<ToolId>> {
            ++calls;
            empty_catalog = request.tools.empty();
            return std::vector<ToolId>{};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(calls == 1 && empty_catalog);
    MIRA_CHECK(fixture.provider_->requests()[0].tools.empty());
    return 0;
}

int device_selection_failure_is_visible() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("must not run")});
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>(
        adapters::simulator::SimulatorSetup::single_display());
    AgentLoop loop(environment, *fixture.gateway_);
    loop.set_tool_registry(fixture.registry_);
    loop.set_event_store(fixture.events_, fixture.runtime_, fixture.spec_.session_id);
    loop.set_tool_selector(
        [](const AgentLoopSpec &, const ModelRequest &) -> Result<std::vector<ToolId>> {
            return std::vector<ToolId>{ToolId::generate()};
        });
    fixture.admission_->activate(fixture.spec_.task_id, fixture.spec_.task_epoch);
    ModelDoneVerifier verifier;
    const auto result = loop.run(fixture.spec_, plain_loop_context(), verifier);
    MIRA_CHECK(result && result.value().outcome == LoopOutcome::Failed);
    MIRA_CHECK(fixture.provider_->requests().empty());
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::count(types.begin(), types.end(), "ModelToolSelectionFailed") == 1);
    return 0;
}

int route_limits() {
    auto small = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    auto large = std::make_shared<ModelProfile>(*small);
    large->id = ModelProfileId::generate();
    large->capabilities.limits.max_tools_per_request = 128;
    ModelRouter router;
    router.register_profile(small);
    router.register_profile(large);
    RouteQuery query;
    query.needs_tools = true;
    query.required_tools = 65;
    const auto route = router.route(query);
    MIRA_CHECK(route && route.value().selected_profile == large->id);
    MIRA_CHECK(route.value().rejections.size() == 1);
    MIRA_CHECK(route.value().rejections[0].reason.find("65") != std::string::npos);
    query.required_tools = 129;
    MIRA_CHECK(!router.route(query));
    small->capabilities.limits.max_tools_per_request = 0;
    ModelRouter disabled;
    disabled.register_profile(small);
    query.required_tools = 0;
    MIRA_CHECK(disabled.route(query)); // Native history may need tools with zero new exposure.
    query.needs_tools = false;
    MIRA_CHECK(disabled.route(query));
    query.required_tools = 1;
    MIRA_CHECK(!disabled.route(query));
    small->capabilities.limits.max_tools_per_request = 64;
    small->capabilities.function_tools.supported = false;
    MIRA_CHECK(!disabled.route(query)); // Count also requires protocol capability.
    query.required_tools = 0;
    query.needs_tools = true;
    MIRA_CHECK(!disabled.route(query)); // Native history requires protocol capability.
    return 0;
}

} // namespace

int main() {
    for (const auto test :
         {dynamic_selection_and_history, device_loop_uses_same_selection,
          invalid_selection_fails_closed, unselected_tool_cannot_execute,
          defaults_limits_order_and_cancel, mapper_and_provider_limits, retry_reuses_selection,
          recovery_reselects, route_limits, continuous_selection_and_empty_registry,
          device_selection_failure_is_visible}) {
        if (const auto code = test(); code != 0)
            return code;
    }
    return 0;
}
