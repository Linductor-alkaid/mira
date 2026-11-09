#include "support/subagent_test_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

ModelResponse call(const std::string &name, const std::string &args) {
    BuiltinToolSpec spec;
    spec.wire_name = name;
    return tool_call_response(spec, args);
}

struct Scenario final {
    ConversationFixture fixture;
    std::shared_ptr<SubagentTestLog> log = std::make_shared<SubagentTestLog>();
    std::shared_ptr<SubagentProbe> probe = std::make_shared<SubagentProbe>();
    std::vector<SubagentTool> catalog;
    SubagentToolAuthorizer authorize;
    SubagentToolSearch search;
    SubagentProfile profile = subagent_profile(probe);
    std::unique_ptr<SubagentCoordinator> owner;
    SubagentSnapshot snapshot;
    std::atomic<int> executions{0};
    std::atomic<int> verifications{0};
    std::atomic<bool> published{false};
    std::atomic<bool> interrupted{false};

    Scenario() {
        log->acknowledge_durability = true;
        profile.tool_access = SubagentToolAccess::Discover;
        profile.conversation.max_turns = 10;
        probe->resolve_tool_names = true;
        SubagentTool tool;
        tool.registration.spec.wire_name = "publish_release";
        tool.registration.spec.description = "Publish the release in the assigned workspace.";
        tool.registration.spec.has_side_effects = true;
        tool.registration.spec.parameters_schema = JsonSchema{parse_json(R"({"type":"object",
            "properties":{"body":{"type":"string"}},"required":["body"],
            "additionalProperties":false})")
                                                                  .value()};
        tool.registration.handler = [this](const JsonValue &, const OperationContext &) {
            ++executions;
            published.store(true);
            return Result<JsonValue>{JsonValue{JsonValue::Object{{"receipt", "child-private"}}}};
        };
        tool.verify = [this](const JsonValue &, const Result<JsonValue> &,
                             const OperationContext &) -> Result<SubagentEffectStatus> {
            ++verifications;
            return published.load() ? SubagentEffectStatus::Applied
                                    : SubagentEffectStatus::NotApplied;
        };
        catalog.push_back(std::move(tool));
        probe->script = {call("discover_tools", R"({"query":"publish this release"})"),
                         call("load_tools", R"({"names":["publish_release"]})"),
                         call("publish_release", R"({"body":"private-content"})"),
                         text_response("release completed")};
    }
    std::optional<std::shared_future<Result<ConversationLoopResult>>> future;
    bool start() {
        const auto factory = profile.create_resources;
        profile.create_resources =
            [this, factory](const AgentLoopSpec &spec,
                            const OperationContext &ctx) -> Result<SubagentResources> {
            auto resources = factory(spec, ctx);
            if (!resources)
                return resources;
            resources.value().tool_catalog = catalog;
            resources.value().authorize_tool = authorize;
            resources.value().search_tools = search;
            return resources;
        };
        owner = std::make_unique<SubagentCoordinator>(fixture.executor_, fixture.spec_, log,
                                                      std::make_shared<MemoryArtifactStore>(),
                                                      fixture.runtime_);
        if (!owner->register_profile(profile))
            return false;
        const auto tools = register_subagent_tools(*owner, *fixture.registry_);
        if (tools.empty() || !tools.front().has_side_effects)
            return false;
        fixture.use_provider(
            {tool_call_response(
                 tools[0],
                 R"({"profile":"reader","prompt":"publish the prepared release in my workspace"})"),
             text_response("waiting"), text_response("finished")});
        auto context = conversation_context();
        context.cancellation_requested = [this] { return interrupted.load(); };
        auto started = owner->start_parent(subagent_parent(fixture), context);
        if (!started)
            return false;
        future = started.value();
        return true;
    }
    bool finish() {
        if (!future || future->wait_for(3s) != std::future_status::ready || !future->get())
            return false;
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        if (!handle)
            return false;
        auto result = owner->get(*handle);
        if (!result)
            return false;
        snapshot = result.value();
        return static_cast<bool>(owner->close());
    }
    bool run() { return start() && finish(); }
};

int child_discovers_executes_and_verifies_without_parent_tool_list() {
    Scenario s;
    // A catalog larger than the static whitelist cap does not consume the
    // model's request budget: only the selected native schemas are exposed.
    for (int i = 0; i < 130; ++i) {
        auto entry = s.catalog.front();
        entry.registration.spec.tool_id = ToolId::generate();
        entry.registration.spec.wire_name = "unrelated_" + std::to_string(i);
        s.catalog.push_back(std::move(entry));
    }
    s.probe->script.insert(s.probe->script.begin() + 1, call("discover_tools", R"({"offset":16})"));
    bool policy_checked = false;
    s.authorize = [&](const AgentLoopSpec &child, const BuiltinToolSpec &tool,
                      const JsonValue &args, const OperationContext &ctx) -> Result<void> {
        policy_checked = child.goal.find("publish") != std::string::npos && tool.has_side_effects &&
                         args.find("body") && ctx.session == child.session_id &&
                         ctx.operation != OperationId{};
        return {};
    };
    MIRA_CHECK(s.run());
    MIRA_CHECK(s.snapshot.status == SubagentStatus::Answered && policy_checked);
    MIRA_CHECK(s.executions == 1 && s.verifications == 1 && s.published);
    MIRA_CHECK(s.snapshot.verified_effects == 1 && !s.snapshot.effects_uncertain);
    MIRA_CHECK(s.log->count("SubagentEffectDispatchStarted") == 1);
    MIRA_CHECK(s.log->count("SubagentEffectVerified") == 1);
    const auto requests = s.probe->recorded();
    MIRA_CHECK(requests.size() == 5 && requests[0].tools.size() == 2);
    MIRA_CHECK(requests[2].tools.size() == 2 && requests[3].tools.size() == 3);
    const auto child_text = to_json_string(model_request_to_json(requests.back()));
    MIRA_CHECK(child_text.find("next_offset") != std::string::npos);
    MIRA_CHECK(child_text.find("child-private") != std::string::npos);
    for (const auto &request : s.fixture.provider_->requests()) {
        const auto text = to_json_string(model_request_to_json(request));
        MIRA_CHECK(text.find("publish_release") == std::string::npos);
        MIRA_CHECK(text.find("unrelated_") == std::string::npos);
        MIRA_CHECK(text.find("child-private") == std::string::npos);
        MIRA_CHECK(text.find("private-content") == std::string::npos);
    }
    EventQuery query;
    query.session_id = s.snapshot.handle.session_id;
    const auto events = s.log->read(query);
    MIRA_CHECK(events);
    std::string operation;
    // DEC-104: the child's AssistantSpeech events carry the model-produced
    // publish_release arguments ("private-content", classified Internal);
    // no other event type may quote them, and the tool result content
    // ("child-private") never enters the event store at all.
    bool arguments_in_speech = false;
    for (const auto &event : events.value().events) {
        if (event.payload.type == "AssistantSpeech") {
            if (event.payload.data.find("private-content") != std::string::npos) {
                arguments_in_speech = true;
            }
        } else {
            MIRA_CHECK(event.payload.data.find("private-content") == std::string::npos);
        }
        MIRA_CHECK(event.payload.data.find("child-private") == std::string::npos);
        if (event.payload.type.starts_with("SubagentEffect")) {
            const auto data = parse_json(event.payload.data).value();
            const auto id = *data.find("operation_id")->as_string();
            MIRA_CHECK(operation.empty() || operation == id);
            operation = id;
            MIRA_CHECK(data.find("arguments_digest") && data.find("spec_digest"));
        }
    }
    MIRA_CHECK(arguments_in_speech);
    MIRA_CHECK(!operation.empty());
    return 0;
}

int effect_denial_failure_and_uncertainty() {
    for (int mode = 0; mode < 9; ++mode) {
        Scenario s;
        if (mode == 0 || mode == 8)
            s.authorize = [mode](const AgentLoopSpec &, const BuiltinToolSpec &, const JsonValue &,
                                 const OperationContext &) -> Result<void> {
                if (mode == 8)
                    throw std::runtime_error("private policy failure");
                Error denied;
                denied.code = ErrorCode::PermissionDenied;
                return denied;
            };
        if (mode == 1)
            s.catalog[0].verify = {};
        if (mode == 2)
            s.catalog[0].registration.handler = [&](const JsonValue &,
                                                    const OperationContext &) -> Result<JsonValue> {
                ++s.executions;
                throw std::runtime_error("lost external receipt");
            };
        if (mode == 2 || mode == 3 || mode == 4)
            s.catalog[0].verify = [&,
                                   mode](const JsonValue &, const Result<JsonValue> &,
                                         const OperationContext &) -> Result<SubagentEffectStatus> {
                ++s.verifications;
                if (mode == 3)
                    throw std::runtime_error("private verify failure");
                return mode == 4 ? SubagentEffectStatus::NotApplied
                                 : SubagentEffectStatus::Uncertain;
            };
        if (mode == 5)
            s.log->fail_on("SubagentEffectDispatchStarted");
        if (mode == 6)
            s.log->fail_on("SubagentEffectVerified");
        if (mode == 7) {
            s.catalog[0].registration.handler = [&](const JsonValue &,
                                                    const OperationContext &) -> Result<JsonValue> {
                ++s.executions;
                s.published.store(true);
                return Error{};
            };
        }
        MIRA_CHECK(s.run());
        const bool denied = mode == 0 || mode == 8;
        const bool continued = denied || mode == 4;
        MIRA_CHECK(s.snapshot.status ==
                   (continued ? SubagentStatus::Answered : SubagentStatus::Failed));
        MIRA_CHECK(s.executions == (denied || mode == 1 || mode == 5 ? 0 : 1));
        MIRA_CHECK(s.snapshot.effects_uncertain == (mode == 2 || mode == 3 || mode == 6));
        MIRA_CHECK(s.snapshot.verified_effects == (mode == 6 || mode == 7 ? 1U : 0U));
        if (denied)
            MIRA_CHECK(s.log->count("SubagentToolDenied") == 1 && s.verifications == 0);
        if (mode == 2 || mode == 3)
            MIRA_CHECK(s.probe->recorded().size() == 3); // no reconciliation retry/model step
    }
    return 0;
}

int discovery_and_selection_boundaries() {
    for (int mode = 0; mode < 10; ++mode) {
        Scenario s;
        switch (mode) {
        case 0:
            s.catalog.push_back(s.catalog[0]);
            break;
        case 1:
            s.catalog[0].registration.spec.wire_name = "spawn_agent";
            break;
        case 2:
            s.probe->script = {call("publish_release", R"({"body":"hidden"})")};
            break;
        case 3:
            s.probe->script[1] = call("load_tools", R"({"names":["missing"]})");
            break;
        case 4:
            s.probe->script[1] =
                call("load_tools", R"({"names":["publish_release","publish_release"]})");
            break;
        case 5:
            s.search = [](const std::string &, const OperationContext &) {
                return Result<std::vector<ToolId>>{std::vector<ToolId>{ToolId::generate()}};
            };
            s.probe->script = {s.probe->script.front(), text_response("search unavailable")};
            break;
        case 6:
            s.search = [id = s.catalog[0].registration.spec.tool_id](const std::string &,
                                                                     const OperationContext &) {
                return Result<std::vector<ToolId>>{{id}};
            };
            break;
        case 7:
            s.probe->script = {call("load_tools", R"({"names":["publish_release"]})"),
                               call("load_tools", R"({"names":[]})"),
                               call("publish_release", R"({"body":"unloaded"})")};
            break;
        case 8:
            s.profile.max_input_tokens_per_request = 1;
            break;
        default:
            s.catalog[0].registration.spec.description = std::string(2049, 'x');
        }
        MIRA_CHECK(s.run());
        MIRA_CHECK(s.executions == (mode == 6 ? 1 : 0));
        if (mode != 5 && mode != 6)
            MIRA_CHECK(s.snapshot.status == SubagentStatus::Failed);
        else
            MIRA_CHECK(s.snapshot.status == SubagentStatus::Answered);
    }
    return 0;
}
int side_effect_cancellation_deadline_takeover_and_shutdown() {
    for (int mode = 0; mode < 5; ++mode) {
        Scenario s;
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::mutex gate_mutex;
        std::condition_variable gate_cv;
        bool release = false;
        if (mode == 2)
            s.profile.timeout = 1000ms;
        s.catalog[0].registration.handler = [&](const JsonValue &,
                                                const OperationContext &ctx) -> Result<JsonValue> {
            ++s.executions;
            s.published.store(true);
            entered.set_value();
            std::unique_lock lock(gate_mutex);
            while (!release && (mode == 4 || !ctx.cancelled_or_expired(Timestamp::now())))
                gate_cv.wait_for(lock, 5ms);
            if (mode != 4) {
                Error cancelled;
                cancelled.code =
                    ctx.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded;
                return cancelled;
            }
            return JsonValue{JsonValue::Object{}};
        };
        s.catalog[0].verify = [&, mode](const JsonValue &, const Result<JsonValue> &,
                                        const OperationContext &) -> Result<SubagentEffectStatus> {
            ++s.verifications;
            return mode == 4 ? SubagentEffectStatus::Applied : SubagentEffectStatus::Uncertain;
        };
        MIRA_CHECK(s.start());
        const bool dispatched = entered_future.wait_for(3s) == std::future_status::ready;
        bool control_ok = true;
        if (dispatched) {
            if (mode == 0 || mode == 4) {
                const auto handle = spawned_handle(*s.log, s.fixture.spec_.session_id);
                control_ok = handle && s.owner->cancel(*handle);
            } else if (mode == 1)
                s.owner->request_human_takeover();
            else if (mode == 3)
                (void)s.fixture.executor_.shutdown(true);
            if (mode == 4) {
                const auto closed = s.owner->close(1ms);
                control_ok =
                    control_ok && !closed && closed.error().code == ErrorCode::DeadlineExceeded;
            }
        }
        if (!dispatched || mode == 4) {
            const std::lock_guard lock(gate_mutex);
            release = true;
            gate_cv.notify_all();
        }
        const bool completed = s.finish();
        if (!dispatched || !control_ok || !completed)
            std::cerr << "lifecycle mode=" << mode << " dispatched=" << dispatched
                      << " controlled=" << control_ok << " completed=" << completed << '\n';
        MIRA_CHECK(dispatched && control_ok && completed);
        MIRA_CHECK(s.executions == 1 && s.verifications == 1);
        MIRA_CHECK(s.snapshot.status ==
                   (mode == 2 ? SubagentStatus::TimedOut : SubagentStatus::Cancelled));
        MIRA_CHECK(s.snapshot.effects_uncertain == (mode != 4));
        MIRA_CHECK(s.snapshot.verified_effects == (mode == 4 ? 1U : 0U));
        MIRA_CHECK(s.probe->recorded().size() == 3);
        MIRA_CHECK(s.owner->close());
    }
    return 0;
}

int uncertain_effect_stops_the_rest_of_the_same_batch() {
    Scenario s;
    s.catalog[0].verify = [](const JsonValue &, const Result<JsonValue> &,
                             const OperationContext &) -> Result<SubagentEffectStatus> {
        return SubagentEffectStatus::Uncertain;
    };
    auto second = call("publish_release", R"({"body":"second"})");
    std::get<ToolCallOutput>(second.output.front()).provider_call_id = ProviderToolCallId{"second"};
    s.probe->script[2].output.push_back(second.output.front());
    MIRA_CHECK(s.run());
    MIRA_CHECK(s.executions == 1 && s.snapshot.effects_uncertain);
    MIRA_CHECK(s.snapshot.status == SubagentStatus::Failed);
    return 0;
}

int cancellation_after_audit_before_dispatch_has_no_effect() {
    Scenario s;
    s.log->on_append = [&](const AppendRequest &request) {
        if (request.payload.type == "SubagentEffectDispatchStarted")
            s.interrupted.store(true);
    };
    MIRA_CHECK(s.run());
    MIRA_CHECK(s.snapshot.status == SubagentStatus::Cancelled);
    MIRA_CHECK(s.executions == 0 && s.verifications == 0);
    MIRA_CHECK(!s.snapshot.effects_uncertain && s.snapshot.verified_effects == 0);
    MIRA_CHECK(s.log->count("SubagentEffectNotApplied") == 1);
    return 0;
}

int weak_audit_acknowledgement_prevents_dispatch() {
    Scenario s;
    s.log->acknowledge_durability = false;
    MIRA_CHECK(s.run());
    MIRA_CHECK(s.snapshot.status == SubagentStatus::Failed);
    MIRA_CHECK(s.executions == 0 && s.verifications == 0 && !s.snapshot.effects_uncertain);
    return 0;
}
} // namespace

int main() {
    for (const auto test :
         {child_discovers_executes_and_verifies_without_parent_tool_list,
          effect_denial_failure_and_uncertainty, discovery_and_selection_boundaries,
          side_effect_cancellation_deadline_takeover_and_shutdown,
          weak_audit_acknowledgement_prevents_dispatch,
          uncertain_effect_stops_the_rest_of_the_same_batch,
          cancellation_after_audit_before_dispatch_has_no_effect})
        if (const auto result = test(); result != 0)
            return result;
    return 0;
}
