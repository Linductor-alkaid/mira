// Offline M35 consumer: autonomous discovery, workspace write and external Verify.
#include <mira/subagent.hpp>

#include <kairo/executor.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace mira;

ModelProfile fixture_profile() {
    ModelProfile profile;
    profile.id = ModelProfileId::generate();
    profile.display_name = "offline-subagent-fixture";
    profile.model_selector = "fixture";
    profile.endpoint_origin = "https://offline.example";
    profile.api_prefix = "/v1";
    profile.credential = SecretRef{"unused-offline-fixture"};
    profile.capabilities.text = {true, CapabilityEvidence::FixtureVerified, "offline"};
    profile.capabilities.function_tools = {true, CapabilityEvidence::FixtureVerified, "offline"};
    return profile;
}

class FixtureProvider final : public IModelProvider {
  public:
    FixtureProvider(ModelProfile profile, std::optional<BuiltinToolSpec> spawn = std::nullopt)
        : profile_(std::move(profile)), spawn_(std::move(spawn)) {}
    const ModelProfile &profile() const override { return profile_; }
    Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &context,
                                const ProviderInferOptions &) override {
        if (context.cancelled()) {
            Error error;
            error.code = ErrorCode::Cancelled;
            return error;
        }
        ModelResponse response;
        response.contract_version = {1, 0};
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        response.status = ModelCompletionStatus::Completed;
        const auto turn = calls_++;
        if (spawn_ && turn == 0) {
            ToolCallOutput call;
            call.provider_call_id = ProviderToolCallId{"delegate-reader"};
            call.tool_id = spawn_->tool_id;
            call.provider_name = spawn_->wire_name;
            call.arguments = JsonValue::Object{{"profile", "reader"},
                                               {"prompt", "Publish the prepared fixture release"}};
            call.arguments_digest = digest_string(to_json_string(call.arguments));
            response.output.emplace_back(std::move(call));
        } else if (!spawn_ && turn < 3) {
            const std::string name = turn == 0   ? "discover_tools"
                                     : turn == 1 ? "load_tools"
                                                 : "workspace_publish";
            const auto tool =
                std::find_if(request.tools.begin(), request.tools.end(),
                             [&](const auto &spec) { return spec.wire_name == name; });
            if (tool == request.tools.end()) {
                Error error;
                error.code = ErrorCode::InvalidState;
                return error;
            }
            ToolCallOutput call;
            call.provider_call_id = ProviderToolCallId{"child-" + std::to_string(turn)};
            call.tool_id = tool->tool_id;
            call.provider_name = name;
            call.arguments = parse_json(turn == 0   ? R"({"query":"publish release"})"
                                        : turn == 1 ? R"({"names":["workspace_publish"]})"
                                                    : R"({"body":"fixture release"})")
                                 .value();
            call.arguments_digest = digest_string(to_json_string(call.arguments));
            response.output.emplace_back(std::move(call));
        } else {
            bool result_present = false;
            for (const auto &item : request.input) {
                if (item.provenance.source == "mira.conversation-loop.continuation.v1")
                    result_present = true;
            }
            MessageOutput message;
            message.role = ModelRole::Assistant;
            OutputTextPart text;
            text.text = !spawn_          ? "release verified"
                        : result_present ? "Integrated child result: release verified"
                                         : "Waiting for the child";
            message.content.emplace_back(std::move(text));
            response.output.emplace_back(std::move(message));
        }
        response.usage.input_tokens = 10;
        response.usage.output_tokens = 10;
        response.usage.quality = UsageQuality::ProviderReported;
        return response;
    }

  private:
    ModelProfile profile_;
    std::optional<BuiltinToolSpec> spawn_;
    unsigned calls_ = 0;
};

int run_host(kairo::Executor &executor) {
    const auto model = fixture_profile();
    AgentLoopSpec parent{TaskId::generate(), SessionId::generate(), 1,
                         "Delegate publication and integrate its result", model.id};
    struct Workspace final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("mira-subagent-tools-" + OperationId::generate().to_string());
        ~Workspace() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } workspace;
    auto events = std::make_shared<FileEventStore>(workspace.root / "events");
    auto artifacts = std::make_shared<MemoryArtifactStore>();
    SubagentCoordinator owner(executor, parent, events, artifacts, RuntimeId::generate());
    SubagentProfile reader;
    reader.name = "reader";
    reader.description = "Autonomous release publisher";
    reader.model = model;
    reader.conversation.max_turns = 6;
    reader.tool_access = SubagentToolAccess::Discover;
    reader.conversation.max_output_tokens_per_turn = 128;
    reader.conversation.system_prompt =
        ConversationSystemPrompt{"Discover the tools needed for the goal, load and execute them, "
                                 "verify the result, then return findings to the parent.",
                                 "example.reader.v1"};
    reader.create_resources = [model, path = workspace.root / "release.txt"](
                                  const AgentLoopSpec &,
                                  const OperationContext &) -> Result<SubagentResources> {
        SubagentResources resources;
        resources.provider = std::make_unique<FixtureProvider>(model);
        SubagentTool publish;
        publish.registration.spec.wire_name = "workspace_publish";
        publish.registration.spec.description = "Publish the release in this child workspace.";
        publish.registration.spec.has_side_effects = true;
        publish.registration.spec.parameters_schema = JsonSchema{parse_json(R"({"type":"object",
            "properties":{"body":{"type":"string"}},"required":["body"],
            "additionalProperties":false})")
                                                                     .value()};
        publish.registration.handler =
            [path](const JsonValue &args, const OperationContext &context) -> Result<JsonValue> {
            if (context.cancelled_or_expired(Timestamp::now())) {
                Error error;
                error.code = ErrorCode::Cancelled;
                return error;
            }
            std::ofstream output(path);
            output << *args.find("body")->as_string();
            output.close();
            if (!output) {
                Error error;
                error.code = ErrorCode::PlatformError;
                return error;
            }
            return JsonValue{JsonValue::Object{{"written", true}}};
        };
        publish.verify = [path](const JsonValue &args, const Result<JsonValue> &,
                                const OperationContext &context) -> Result<SubagentEffectStatus> {
            if (context.cancelled_or_expired(Timestamp::now()))
                return SubagentEffectStatus::Uncertain;
            // Observe the external state independently of the write receipt.
            std::ifstream input(path);
            std::string content;
            std::getline(input, content);
            return input && content == *args.find("body")->as_string()
                       ? SubagentEffectStatus::Applied
                       : SubagentEffectStatus::Uncertain;
        };
        resources.tool_catalog.push_back(std::move(publish));
        return resources;
    };
    if (!owner.register_profile(std::move(reader)))
        return 1;
    auto registry = std::make_shared<BuiltinToolRegistry>();
    const auto tools = owner.tools();
    if (tools.empty())
        return 2;
    for (const auto &tool : tools)
        if (!registry->register_tool(tool.spec, tool.handler))
            return 3;
    ModelRouter router;
    router.register_profile(std::make_shared<const ModelProfile>(model));
    ModelGateway gateway(executor, std::move(router), nullptr, PriceTable{});
    gateway.register_provider(std::make_shared<FixtureProvider>(model, tools.front().spec));
    auto admission = std::make_shared<SimpleAdmissionGate>();
    admission->activate(parent.task_id, parent.task_epoch);
    gateway.set_admission_gate(admission);
    auto loop = std::make_unique<ConversationLoop>(gateway);
    loop->set_tool_registry(registry);
    OperationContext context;
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto started = owner.start_parent(std::move(loop), context);
    if (!started)
        return 4;
    const auto result = started.value().get();
    // External owner joins all managed paths while gateway/transport dependencies still live.
    const auto closed = owner.close();
    if (!result || !closed || result.value().outcome != ConversationOutcome::Answered ||
        result.value().answer != "Integrated child result: release verified")
        return 5;
    std::cout << result.value().answer << '\n';
    return 0;
}
} // namespace

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config))
        return 10;
    const auto result = run_host(executor);
    (void)executor.shutdown(true);
    return result;
}
