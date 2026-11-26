#include "support/m36_support.hpp"
#include "support/test.hpp"
#include <mira/adapters/simulator/simulator_environment.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

int real_memory_loop_runs_with_isolated_identity_and_validates_citations() {
    MemoryAgentFixture fixture;
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://memory.test"));
    const auto source_id = EventId::generate();
    auto provider = std::make_shared<RecordingProvider>(
        profile,
        std::vector{
            text_response(
                R"({"summary":[{"text":"volume must remain below 30","citations":[0]}]})"),
            text_response(R"({"memories":[{"kind":"preference","text":"Prefer a brief reply",
                             "subject":"tone","value":"brief","citations":[0]},
                             {"kind":"invented_kind","text":"bad","subject":"","value":"","citations":[0]},
                             {"kind":"preference","text":"bad","subject":"","value":"","citations":[1]}]})"),
            text_response("```json\nThis is a reflected prompt, not a proposal.\n```")});
    MemoryAgentModelConfig config;
    config.model = *profile;
    class Exclusive final : public IModelProvider {
      public:
        explicit Exclusive(std::shared_ptr<RecordingProvider> provider)
            : provider_(std::move(provider)) {}
        const ModelProfile &profile() const override { return provider_->profile(); }
        Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &context,
                                    const ProviderInferOptions &options) override {
            return provider_->infer(request, context, options);
        }

      private:
        std::shared_ptr<RecordingProvider> provider_;
    };
    config.create_provider =
        [provider](const OperationContext &) -> Result<std::unique_ptr<IModelProvider>> {
        return std::unique_ptr<IModelProvider>{std::make_unique<Exclusive>(provider)};
    };
    fixture.start(make_model_memory_agent(fixture.executor, config));
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Compress;
    request.history = {history_text("never exceed volume 30"), history_text("recent input")};
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(handle.value().result.get());
    const auto calls = provider->requests();
    MIRA_CHECK(calls.size() == 1 && calls.front().task_id != binding.work.task_id);
    MIRA_CHECK(calls.front().tools.empty());
    MIRA_CHECK(calls.front().output_contract.mode == OutputMode::StrictJsonSchema);
    MIRA_CHECK(calls.front().prompt_provenance.context_selection_digest != Hash{});
    MIRA_CHECK(provider->request_text(0).find("memory specialist") != std::string::npos);
    // DEC-108: the compress prompt requires speaker attribution so compressed
    // context keeps who said what.
    MIRA_CHECK(provider->request_text(0).find("attribute quoted speech to its speaker") !=
               std::string::npos);
    const auto digest = digest_string("internal event digest").to_string();
    MIRA_CHECK(fixture.events->append(
        {source_id,
         fixture.runtime,
         binding.work.session_id,
         binding.work.task_id,
         {"test_user_message", to_json_string(JsonValue::Object{{"text", "Please be brief"},
                                                                {"event_id", source_id.to_string()},
                                                                {"digest", digest}})}}));
    request.mode = MemoryAgentMode::Record;
    request.history.clear();
    request.write_scope = binding.scopes.front();
    auto recording = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(recording && recording.value().result.wait_for(3s) == std::future_status::ready);
    const auto report = recording.value().result.get();
    MIRA_CHECK(report && !report.value().deterministic_record_only &&
               report.value().rejected_record_rows == 2 &&
               report.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(report.value().record_pages.size() == 1);
    const auto &rejected = report.value().record_pages.front().rejected;
    MIRA_CHECK(rejected.size() == 2 && rejected[0].row == 1 &&
               rejected[0].reason == MemoryAgentRecordRejectReason::UnknownKind &&
               rejected[1].row == 2 &&
               rejected[1].reason == MemoryAgentRecordRejectReason::InvalidCitation);
    MemoryQuery claim_query;
    claim_query.scopes = binding.scopes;
    claim_query.claim_key = "tone";
    const auto stored = fixture.memory->query(claim_query);
    MIRA_CHECK(stored && stored.value().records.size() == 1 &&
               memory_claim_key(stored.value().records.front()).value == "brief");
    const auto prompt = provider->request_text(1);
    MIRA_CHECK(prompt.find("Please be brief") != std::string::npos &&
               prompt.find("Valid kind names:") != std::string::npos &&
               prompt.find(digest) == std::string::npos &&
               prompt.find(source_id.to_string()) == std::string::npos);
    MemoryAgentInput input;
    input.mode = MemoryAgentMode::Record;
    input.work = binding.work;
    const auto invalid =
        make_model_memory_agent(fixture.executor, config)(input, memory_context(binding.work));
    MIRA_CHECK(!invalid && invalid.error().code == ErrorCode::InvalidModelOutput);
    return 0;
}

int main_agent_delegates_and_continues_with_loaded_memory() {
    MemoryAgentFixture fixture;
    fixture.start();
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.seed(binding, "volume=30"));
    auto registry = std::make_shared<BuiltinToolRegistry>();
    const auto tools = fixture.service->tools();
    for (const auto &tool : tools)
        MIRA_CHECK(registry->register_tool(tool.spec, tool.handler));
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://frontend.test"));
    binding.work.profile_id = profile->id;
    auto provider = std::make_shared<RecordingProvider>(
        profile,
        std::vector{tool_call_response(tools.front().spec, R"({"mode":"recall","query":"volume"})"),
                    text_response("volume is 30"), text_response("volume is 30")});
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(fixture.executor, router, nullptr, PriceTable{});
    gateway.register_provider(provider);
    gateway.set_context_supplier(fixture.service->context_supplier());
    ConversationLoopConfig config;
    config.max_turns = 4;
    config.max_recoveries = 0;
    ConversationLoop loop(gateway, config);
    loop.set_tool_registry(registry);
    loop.set_history_transformer(fixture.service->history_transformer(binding.work.task_id));
    loop.set_continuation_source(fixture.service->continuation_source(binding.work.task_id));
    // The parent runs on the test owner's thread here; its child memory work
    // and SQLite each use managed I/O workers, even with one finite pool slot.
    auto result = loop.run(binding.work, memory_context(binding.work));
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "volume is 30");
    const auto calls = provider->requests();
    MIRA_CHECK(calls.size() >= 2);
    bool loaded = false;
    for (std::size_t i = 1; i < calls.size(); ++i)
        loaded |= provider->request_text(i).find("volume=30") != std::string::npos;
    MIRA_CHECK(loaded);
    auto foreign = memory_context(binding.work);
    foreign.task = TaskId::generate();
    const auto id =
        parse_json(
            to_json_string(JsonValue::Object{{"operation", OperationId::generate().to_string()}}))
            .value();
    MIRA_CHECK(!tools[1].handler(id, foreign));
    return 0;
}

int forged_sources_and_split_tool_groups_are_rejected() {
    MemoryAgentFixture fixture;
    fixture.start(
        [](const MemoryAgentInput &, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            proposal.summary = {{"invented", {99}}};
            return proposal;
        });
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.mode = MemoryAgentMode::Compress;
    request.task = binding.work.task_id;
    request.history = {history_text("old"), history_text("latest")};
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(!handle.value().result.get());
    ToolProposal call;
    call.provider_call_id.value = "split";
    call.wire_name = "read";
    call.arguments = JsonValue::Object{};
    call.arguments_digest = canonical_json_digest(call.arguments);
    ToolExecutionRecord receipt;
    receipt.provider_call_id = call.provider_call_id;
    receipt.tool_id = ToolId::generate();
    receipt.result = JsonValue::Object{};
    request.history = {make_tool_call_item(call), make_tool_result_item(receipt)};
    MIRA_CHECK(!fixture.service->submit(request, memory_context(binding.work)));
    return 0;
}

int inflight_correction_refreshes_both_loops_without_old_actions() {
    for (const int mode : {0, 1, 2}) {
        const bool device = mode != 0;
        const bool repair = mode == 2;
        MemoryAgentFixture fixture;
        fixture.start();
        auto binding = fixture.binding();
        auto profile = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://correction.test"));
        binding.work.profile_id = profile->id;
        MIRA_CHECK(fixture.service->bind(binding));
        auto script = std::vector{
            text_response(device ? R"({"action":"tap","x":0.5,"y":0.5,"reason":"old target"})"
                                 : "old answer"),
            text_response(device ? R"({"action":"done","reason":"fresh target verified"})"
                                 : "new answer")};
        if (repair)
            script.insert(script.begin(), text_response("malformed decision"));
        auto provider = std::make_shared<RecordingProvider>(profile, std::move(script));
        ModelRouter router;
        router.register_profile(profile);
        ModelGateway gateway(fixture.executor, router, nullptr, PriceTable{});
        gateway.register_provider(provider);
        gateway.set_context_supplier(fixture.service->context_supplier());
        auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>();
        AgentLoop agent(environment, gateway);
        ConversationLoop conversation(gateway);
        const auto downstream =
            device ? std::shared_ptr<IUserInputSink>{std::make_shared<AgentLoopInputSink>(agent)}
                   : std::shared_ptr<IUserInputSink>{
                         std::make_shared<ConversationLoopInputSink>(conversation)};
        auto sink = fixture.service->input_sink(binding.work.task_id, downstream);
        bool delivered = false;
        provider->on_after_response(repair ? 1 : 0, [&] {
            InputEnvelope correction;
            correction.principal = binding.principal;
            correction.input_id = "inflight-correction";
            correction.content = "use the corrected target";
            delivered = sink->deliver(correction, binding.work.task_id).has_value();
        });
        if (device) {
            ModelDoneVerifier verifier;
            auto result = agent.run(binding.work, memory_context(binding.work), verifier);
            if (!result || result.value().outcome != LoopOutcome::Completed)
                std::cerr << "correction mode " << mode << ": "
                          << (result ? result.value().safe_summary : result.error().safe_message)
                          << " calls=" << provider->requests().size() << '\n';
            MIRA_CHECK(result && result.value().outcome == LoopOutcome::Completed);
        } else {
            auto result = conversation.run(binding.work, memory_context(binding.work));
            MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
            MIRA_CHECK(result.value().answer == "new answer");
        }
        MIRA_CHECK(delivered && provider->requests().size() == (repair ? 3U : 2U));
        MIRA_CHECK(provider->request_text(repair ? 2 : 1).find("use the corrected target") !=
                   std::string::npos);
        MIRA_CHECK(environment->executed_inputs().empty());
        if (device)
            MIRA_CHECK(environment->observation_count() >= 3);
    }
    return 0;
}
} // namespace
int main() {
    if (real_memory_loop_runs_with_isolated_identity_and_validates_citations())
        return 1;
    if (main_agent_delegates_and_continues_with_loaded_memory())
        return 1;
    if (forged_sources_and_split_tool_groups_are_rejected())
        return 1;
    return inflight_correction_refreshes_both_loops_without_old_actions();
}
