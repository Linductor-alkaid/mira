#include "support/subagent_test_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int cumulative_and_capacity_admission() {
    for (int mode = 0; mode < 6; ++mode) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto parent_probe = std::make_shared<SubagentProbe>();
        auto child_probe = std::make_shared<SubagentProbe>();
        parent_probe->blocked = true;
        child_probe->blocked = true;
        fixture.gateway_->register_provider(
            std::make_shared<SubagentProbeProvider>(*fixture.profile_, parent_probe));
        SubagentConfig config;
        if (mode == 0)
            config.max_active = 1;
        if (mode == 1) {
            config.max_children = 1;
            config.max_active = 1;
        }
        if (mode == 2)
            config.max_pending_results = 1;
        if (mode == 3)
            config.max_model_requests = 4;
        if (mode == 4)
            config.max_input_tokens = 4 * 16'384;
        if (mode == 5)
            config.max_output_tokens = 4 * 128;
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_,
                                  config);
        MIRA_CHECK(owner.register_profile(subagent_profile(child_probe)));
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        MIRA_CHECK(started && parent_probe->wait_entered());
        const auto caller = subagent_caller(fixture);
        auto wrong = caller;
        wrong.task = TaskId::generate();
        MIRA_CHECK(!owner.spawn("reader", "foreign", wrong));
        MIRA_CHECK(!owner.spawn("missing", "task", caller));
        MIRA_CHECK(!owner.spawn("reader", std::string(17 * 1024, 'a'), caller));
        const auto first = owner.spawn("reader", "one", caller);
        MIRA_CHECK(first && child_probe->wait_entered());
        const auto second = owner.spawn("reader", "two", caller);
        MIRA_CHECK(!second && second.error().code == ErrorCode::ResourceExhausted);
        MIRA_CHECK(child_probe->recorded().size() == 1);
        owner.request_cancel();
        MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
        MIRA_CHECK(started.value().get());
        MIRA_CHECK(owner.close());
    }
    return 0;
}

int child_tool_and_profile_boundaries() {
    for (int mode = 0; mode < 6; ++mode) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        auto registry = std::make_shared<BuiltinToolRegistry>();
        BuiltinToolSpec echo = fixture.echo_spec();
        echo.has_side_effects = mode == 1;
        if (mode == 2)
            echo.wire_name = "spawn_agent";
        std::atomic<int> executions{0};
        MIRA_CHECK(registry->register_tool(
            echo, [&](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
                executions.fetch_add(1);
                return JsonValue{JsonValue::Object{}};
            }));
        auto profile = subagent_profile(probe, registry);
        profile.allowed_tools = {mode == 3 ? ToolId::generate() : echo.tool_id};
        if (mode == 4)
            profile.allowed_tools.push_back(echo.tool_id);
        if (mode == 5)
            profile.model.id =
                ModelProfileId::generate(); // Factory still binds the original profile.
        probe->script = {tool_call_response(echo, R"({"message":"read"})"),
                         text_response("finding")};
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
        MIRA_CHECK(owner.register_profile(std::move(profile)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        MIRA_CHECK(tools.size() == 3);
        MIRA_CHECK(to_json_string(tools[0].parameters_schema.root).find("reader") !=
                   std::string::npos);
        MIRA_CHECK(!owner.register_profile(subagent_profile(probe)));
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"read"})"),
             text_response("done"), text_response("done")});
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        MIRA_CHECK(started && started.value().wait_for(3s) == std::future_status::ready);
        MIRA_CHECK(started.value().get());
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        MIRA_CHECK(handle);
        const auto snapshot = owner.get(*handle);
        MIRA_CHECK(snapshot);
        if (mode == 0) {
            MIRA_CHECK(snapshot.value().status == SubagentStatus::Answered &&
                       executions.load() == 1);
            const auto requests = probe->recorded();
            MIRA_CHECK(requests.size() == 2 && requests[0].tools.size() == 1);
            MIRA_CHECK(requests[0].tools[0].tool_id == echo.tool_id);
        } else {
            MIRA_CHECK(snapshot.value().status == SubagentStatus::Failed && executions.load() == 0);
            MIRA_CHECK(probe->recorded().empty());
        }
        MIRA_CHECK(owner.close());
    }
    return 0;
}

int input_quota_and_artifact_failure_are_visible() {
    for (const bool input_quota : {false, true}) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        auto artifacts = std::make_shared<MemoryArtifactStore>(input_quota ? 1024 : 1);
        auto profile = subagent_profile(probe);
        if (input_quota)
            profile.max_input_tokens_per_request = 1;
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log, artifacts,
                                  fixture.runtime_);
        MIRA_CHECK(owner.register_profile(std::move(profile)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"read"})"),
             text_response("handled"), text_response("handled")});
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        MIRA_CHECK(started && started.value().wait_for(3s) == std::future_status::ready);
        MIRA_CHECK(started.value().get());
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        MIRA_CHECK(handle && owner.get(*handle).value().status == SubagentStatus::Failed);
        if (input_quota)
            MIRA_CHECK(probe->recorded().empty());
        MIRA_CHECK(owner.close());
    }
    return 0;
}

int structured_output_validation_and_explicit_supply() {
    for (const bool valid : {false, true}) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        probe->script = {text_response(valid ? R"({"answer":42})" : R"({"answer":"wrong"})")};
        auto profile = subagent_profile(probe);
        profile.output_schema = JsonSchema{
            parse_json(
                R"({"type":"object","properties":{"answer":{"type":"integer"}},"required":["answer"],"additionalProperties":false})")
                .value()};
        const auto factory = profile.create_resources;
        profile.create_resources =
            [factory](const AgentLoopSpec &spec,
                      const OperationContext &context) -> Result<SubagentResources> {
            auto resources = factory(spec, context);
            resources.value().input_supplier = [](const AgentLoopSpec &) {
                ModelInputItem item;
                item.role = ModelRole::User;
                item.content.emplace_back(TextPart{"explicit snapshot baseline"});
                return Result<std::vector<ModelInputItem>>{std::vector<ModelInputItem>{item}};
            };
            return resources;
        };
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
        MIRA_CHECK(owner.register_profile(std::move(profile)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"read"})"),
             text_response("done"), text_response("done")});
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        MIRA_CHECK(started && started.value().wait_for(3s) == std::future_status::ready);
        MIRA_CHECK(started.value().get());
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        MIRA_CHECK(handle);
        const auto snapshot = owner.get(*handle);
        MIRA_CHECK(snapshot && (snapshot.value().status == SubagentStatus::Answered) == valid);
        MIRA_CHECK(snapshot.value().structured.has_value() == valid);
        const auto requests = probe->recorded();
        MIRA_CHECK(requests.size() == 1);
        MIRA_CHECK(
            to_json_string(model_request_to_json(requests[0])).find("explicit snapshot baseline") !=
            std::string::npos);
        MIRA_CHECK(owner.close());
    }
    return 0;
}
} // namespace

int main() {
    for (const auto test : {cumulative_and_capacity_admission, child_tool_and_profile_boundaries,
                            input_quota_and_artifact_failure_are_visible,
                            structured_output_validation_and_explicit_supply}) {
        if (const auto result = test(); result != 0)
            return result;
    }
    return 0;
}
