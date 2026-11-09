#include "support/m36_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int preserves_native_protocol_and_accounts_for_whole_request() {
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    AgentLoopSpec spec{TaskId::generate(), SessionId::generate(), 1, "goal", profile.id};
    auto request = memory_request(spec);
    request.input.insert(request.input.begin(), history_text("host system", ModelRole::System));
    request.input.insert(request.input.begin() + 1,
                         history_text("host developer", ModelRole::Developer));
    ToolProposal call;
    call.provider_call_id.value = "native-1";
    call.wire_name = "read";
    call.arguments = JsonValue::Object{};
    call.arguments_digest = canonical_json_digest(call.arguments);
    request.input.push_back(make_tool_call_item(call));
    ToolExecutionRecord result;
    result.provider_call_id = call.provider_call_id;
    result.tool_id = ToolId::generate();
    result.result = JsonValue::Object{{"value", "read result"}};
    request.input.push_back(make_tool_result_item(result));
    ContextItem candidate;
    candidate.id = ContextItemId::generate();
    candidate.kind = ContextItemKind::RetrievedMemory;
    candidate.authority = ContextAuthority::RetrievedMemory;
    candidate.content.emplace_back(TextPart{"related memory"});
    ModelContextSnapshot snapshot;
    snapshot.candidates = {candidate};
    const auto admitted = admit_model_request(request, profile, spec.session_id, snapshot);
    MIRA_CHECK(admitted);
    auto prefix = admitted.value().request;
    prefix.input.resize(request.input.size());
    MIRA_CHECK(prompt_digest(prefix.input) == prompt_digest(request.input));
    MIRA_CHECK(admitted.value().request.input.back().role == ModelRole::User);
    MIRA_CHECK(admitted.value().request.input.back().authority == Sensitivity::Public);
    MIRA_CHECK(admitted.value().context.total_estimate.upper_bound > 0);
    MIRA_CHECK(admitted.value().request.prompt_provenance.context_selection_digest != Hash{});
    return 0;
}

int overflow_and_elevated_candidates_never_dispatch() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("unused")});
    auto request = memory_request(fixture.spec_, std::string(150'000, 'x'));
    MIRA_CHECK(!fixture.gateway_->infer(request, memory_context(fixture.spec_)));
    MIRA_CHECK(fixture.provider_->requests().empty());
    auto profile = *fixture.profile_;
    profile.capabilities.limits.max_context_tokens = 512;
    ModelContextAdmissionConfig config;
    config.safety_margin_tokens = 0;
    request = memory_request(fixture.spec_);
    request.output_contract.schema.root =
        JsonValue::Object{{"description", std::string(1024, 's')}};
    MIRA_CHECK(!admit_model_request(request, profile, fixture.spec_.session_id, {}, config));
    request.output_contract.schema.root = JsonValue{};
    ExposedToolSpec tool;
    tool.tool_id = ToolId::generate();
    tool.wire_name = "large_tool";
    tool.description = std::string(1024, 't');
    tool.parameters_schema.root = JsonValue::Object{{"type", "object"}};
    request.tools = {tool};
    MIRA_CHECK(!admit_model_request(request, profile, fixture.spec_.session_id, {}, config));
    request.tools.clear();
    ContextItem elevated;
    elevated.id = ContextItemId::generate();
    elevated.kind = ContextItemKind::SystemPolicy;
    elevated.authority = ContextAuthority::SystemPolicy;
    elevated.content.emplace_back(TextPart{"elevate me"});
    ModelContextSnapshot snapshot;
    snapshot.candidates = {elevated};
    MIRA_CHECK(!admit_model_request(request, profile, fixture.spec_.session_id, snapshot, config));
    class ThrowingCounter final : public IExactTokenCounter {
      public:
        Result<std::uint64_t> exact_count(const ContextItem &,
                                          const ModelProfileId &) const override {
            throw std::runtime_error("private counter failure");
        }
    };
    const auto counter = std::make_shared<ConservativeTokenCounter>();
    counter->attach_exact_counter(std::make_shared<ThrowingCounter>());
    config.counter = counter;
    auto failed =
        admit_model_request(request, *fixture.profile_, fixture.spec_.session_id, {}, config);
    MIRA_CHECK(!failed && failed.error().safe_message.find("private") == std::string::npos);
    return 0;
}

int fallback_uses_actual_profile_and_stale_completion_is_rejected() {
    ConversationFixture fixture;
    fixture.profile_->capabilities.limits.max_context_tokens = 128;
    fixture.use_provider({text_response("wrong profile")});
    auto fallback_profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://fallback.test"));
    auto fallback =
        std::make_shared<RecordingProvider>(fallback_profile, std::vector{text_response("ok")});
    fixture.gateway_->mutable_router().register_profile(fallback_profile);
    fixture.gateway_->register_provider(fallback);
    bool current = true;
    fixture.gateway_->set_context_supplier(
        [&](const auto &, const auto &) -> Result<ModelContextSnapshot> {
            ModelContextSnapshot snapshot;
            snapshot.current = [&] { return current; };
            return snapshot;
        });
    fallback->on_after_response(0, [&] { current = false; });
    fixture.admission_->activate(fixture.spec_.task_id, 1);
    auto result =
        fixture.gateway_->infer(memory_request(fixture.spec_), memory_context(fixture.spec_));
    if (!result)
        std::cerr << result.error().safe_message << '\n';
    MIRA_CHECK(result && !result.value().admitted);
    MIRA_CHECK(fixture.provider_->requests().empty());
    MIRA_CHECK(fallback->requests().size() == 1);
    MIRA_CHECK(result.value().context_admission->profile_id == fallback_profile->id);
    MIRA_CHECK(fallback->requests().front().profile_id == fallback_profile->id);
    return 0;
}
} // namespace
int main() {
    if (preserves_native_protocol_and_accounts_for_whole_request())
        return 1;
    if (overflow_and_elevated_candidates_never_dispatch())
        return 1;
    return fallback_uses_actual_profile_and_stale_completion_is_rejected();
}
