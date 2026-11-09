// Offline M34 consumer: real parent/child loops, no network or platform input.
#include <mira/subagent.hpp>

#include <kairo/executor.hpp>

#include <chrono>
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
        if (spawn_ && calls_++ == 0) {
            ToolCallOutput call;
            call.provider_call_id = ProviderToolCallId{"delegate-reader"};
            call.tool_id = spawn_->tool_id;
            call.provider_name = spawn_->wire_name;
            call.arguments =
                JsonValue::Object{{"profile", "reader"}, {"prompt", "Find the fixture result"}};
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
            text.text = !spawn_          ? "fixture finding: 42"
                        : result_present ? "Integrated child result: 42"
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
                         "Delegate the lookup and integrate its result", model.id};
    auto events = std::make_shared<MemoryEventStore>();
    auto artifacts = std::make_shared<MemoryArtifactStore>();
    SubagentCoordinator owner(executor, parent, events, artifacts, RuntimeId::generate());
    SubagentProfile reader;
    reader.name = "reader";
    reader.description = "Read-only fixture lookup";
    reader.model = model;
    reader.conversation.max_turns = 4;
    reader.conversation.max_output_tokens_per_turn = 128;
    reader.conversation.system_prompt = ConversationSystemPrompt{
        "You are a delegated reader. Return findings to the parent, never directly to the user.",
        "example.reader.v1"};
    reader.create_resources = [model](const AgentLoopSpec &,
                                      const OperationContext &) -> Result<SubagentResources> {
        SubagentResources resources;
        resources.provider = std::make_unique<FixtureProvider>(model);
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
        result.value().answer != "Integrated child result: 42")
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
