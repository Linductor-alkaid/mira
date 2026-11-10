#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/context_contracts.hpp>

#include <algorithm>
#include <string>

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
int replay_wire_roles_reach_the_wire_and_system_roles_fail_closed() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("unused")});
    auto request = memory_request(fixture.spec_);
    const auto admitted_candidate =
        [&](const ContextItem &candidate) -> Result<AdmittedModelRequest> {
        ModelContextSnapshot snapshot;
        snapshot.candidates = {candidate};
        return admit_model_request(request, *fixture.profile_, fixture.spec_.session_id, snapshot);
    };

    // DEC-108: an assistant-tagged replay item reaches the wire as a native
    // Assistant message while keeping the Public trust tier of retrieved
    // memory (RULE-09: never System/Developer).
    ContextItem assistant_replay;
    assistant_replay.id = ContextItemId::generate();
    assistant_replay.kind = ContextItemKind::RetrievedMemory;
    assistant_replay.authority = ContextAuthority::RetrievedMemory;
    assistant_replay.wire_role = ModelRole::Assistant;
    assistant_replay.content.emplace_back(TextPart{"assistant replay: atlas-2 is the backup"});
    const auto assistant_admitted = admitted_candidate(assistant_replay);
    MIRA_CHECK(assistant_admitted);
    const auto assistant_on_wire =
        std::find_if(assistant_admitted.value().request.input.begin(),
                     assistant_admitted.value().request.input.end(),
                     [](const ModelInputItem &item) {
                         const auto *text = std::get_if<TextPart>(&item.content.front());
                         return text != nullptr &&
                                text->text.find("atlas-2 is the backup") != std::string::npos;
                     });
    MIRA_CHECK(assistant_on_wire != assistant_admitted.value().request.input.end());
    MIRA_CHECK(assistant_on_wire->role == ModelRole::Assistant);
    MIRA_CHECK(assistant_on_wire->authority == Sensitivity::Public);
    MIRA_CHECK(assistant_replay.validate().has_value());

    // A user-tagged replay item maps to User, like untagged memory.
    ContextItem user_replay = assistant_replay;
    user_replay.id = ContextItemId::generate();
    user_replay.wire_role = ModelRole::User;
    const auto user_admitted = admitted_candidate(user_replay);
    MIRA_CHECK(user_admitted);
    const auto user_on_wire = std::find_if(user_admitted.value().request.input.begin(),
                                           user_admitted.value().request.input.end(),
                                           [](const ModelInputItem &item) {
                                               const auto *text =
                                                   std::get_if<TextPart>(&item.content.front());
                                               return text != nullptr &&
                                                      text->text.find("atlas-2 is the backup") !=
                                                          std::string::npos;
                                           });
    MIRA_CHECK(user_on_wire != user_admitted.value().request.input.end());
    MIRA_CHECK(user_on_wire->role == ModelRole::User);

    // Untrusted recall can never borrow System authority: validate() rejects
    // the item and admission returns invalid without dispatching anything.
    ContextItem system_replay = assistant_replay;
    system_replay.id = ContextItemId::generate();
    system_replay.wire_role = ModelRole::System;
    MIRA_CHECK(!system_replay.validate().has_value());
    const auto rejected = admitted_candidate(system_replay);
    MIRA_CHECK(!rejected);
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::InvalidItem));
    MIRA_CHECK(fixture.provider_->requests().empty());
    ContextItem developer_replay = assistant_replay;
    developer_replay.id = ContextItemId::generate();
    developer_replay.wire_role = ModelRole::Developer;
    MIRA_CHECK(!developer_replay.validate().has_value());

    // wire_role is reserved for retrieved-memory items.
    ContextItem mislabelled = assistant_replay;
    mislabelled.id = ContextItemId::generate();
    mislabelled.kind = ContextItemKind::RecentAction;
    MIRA_CHECK(!mislabelled.validate().has_value());

    // The item JSON codec round-trips wire roles and rejects unknown values.
    const auto encoded = context_item_to_json(assistant_replay);
    const auto decoded = context_item_from_json(encoded);
    MIRA_CHECK(decoded.has_value());
    MIRA_CHECK(decoded.value().wire_role == ModelRole::Assistant);
    MIRA_CHECK(decoded.value().kind == assistant_replay.kind);
    MIRA_CHECK(context_item_from_json(context_item_to_json(user_replay)).value().wire_role ==
               ModelRole::User);
    JsonValue forged = encoded;
    forged.set("wire_role", JsonValue{std::string{"system"}});
    MIRA_CHECK(!context_item_from_json(forged).has_value());
    forged.set("wire_role", JsonValue{std::string{"developer"}});
    MIRA_CHECK(!context_item_from_json(forged).has_value());
    return 0;
}
} // namespace
int main() {
    if (preserves_native_protocol_and_accounts_for_whole_request())
        return 1;
    if (overflow_and_elevated_candidates_never_dispatch())
        return 1;
    if (replay_wire_roles_reach_the_wire_and_system_roles_fail_closed())
        return 1;
    return fallback_uses_actual_profile_and_stale_completion_is_rejected();
}
