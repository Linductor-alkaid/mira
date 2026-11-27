#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/context_contracts.hpp>
#include <mira/model_dialect.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace {
using namespace mira;
using namespace mira::testing;

int preserves_native_protocol_and_accounts_for_whole_request() {
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    AgentLoopSpec spec{TaskId::generate(), SessionId::generate(), 1, "goal", profile.id};
    auto request = memory_request(spec);
    request.generation.max_output_tokens = 64;
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
    ContextItem old_user = candidate;
    old_user.id = ContextItemId::generate();
    old_user.sequence = 1;
    old_user.wire_role = ModelRole::User;
    old_user.content = {TextPart{"prior user"}};
    ContextItem old_assistant = old_user;
    old_assistant.id = ContextItemId::generate();
    old_assistant.sequence = 2;
    old_assistant.wire_role = ModelRole::Assistant;
    old_assistant.content = {TextPart{"prior assistant"}};
    snapshot.candidates = {old_user, old_assistant, candidate};
    snapshot.prepend_history = true;
    const auto prepended = admit_model_request(request, profile, spec.session_id, snapshot);
    MIRA_CHECK(prepended);
    auto input = prepended.value().request.input;
    MIRA_CHECK(input.size() == request.input.size() + 3);
    const auto original_user =
        std::find_if(request.input.begin(), request.input.end(),
                     [](const auto &item) { return item.role == ModelRole::User; });
    const auto offset = std::distance(request.input.begin(), original_user);
    const auto replay = input.begin() + offset;
    MIRA_CHECK(replay->role == ModelRole::User && (replay + 1)->role == ModelRole::Assistant);
    input.erase(replay, replay + 2);
    input.pop_back();
    MIRA_CHECK(prompt_digest(input) == prompt_digest(request.input));

    ResponsesV1Mapper responses;
    ChatCompletionsV1Mapper chat;
    AnthropicMessagesV1Mapper messages;
    NullArtifactSource artifacts;
    const std::array<const IDialectMapper *, 3> dialects{&responses, &chat, &messages};
    for (const auto *mapper : dialects) {
        auto wire_profile = profile;
        wire_profile.dialect = mapper->dialect();
        const auto wire =
            mapper->encode_request(prepended.value().request, wire_profile, false, artifacts);
        MIRA_CHECK(wire);
        const auto *turns = wire.value().find(mapper == &responses ? "input" : "messages");
        MIRA_CHECK(turns && turns->as_array() && turns->as_array()->size() >= 3);
        const auto serialized = to_json_string(*turns);
        const auto old_question = serialized.find("prior user");
        const auto old_answer = serialized.find("prior assistant");
        const auto tool_round = serialized.find("native-1");
        MIRA_CHECK(old_question < old_answer && old_answer < tool_round);
        MIRA_CHECK(tool_round != std::string::npos);
    }

    auto budget = profile;
    const ModelContextAdmissionConfig admission_config;
    budget.capabilities.limits.max_context_tokens =
        admitted.value().context.total_estimate.upper_bound +
        admission_config.safety_margin_tokens + *request.generation.max_output_tokens + 256;
    snapshot.candidates[0].content = {TextPart{std::string(8192, 'x')}};
    const auto reduced = admit_model_request(request, budget, spec.session_id, snapshot);
    MIRA_CHECK(reduced && reduced.value().request.input.size() == request.input.size() + 1);
    MIRA_CHECK(reduced.value().context.budget.dropped_items == 2);
    MIRA_CHECK(std::count_if(reduced.value().context.item_audit.begin(),
                             reduced.value().context.item_audit.end(), [](const auto &item) {
                                 return item.reason == "native_history_did_not_fit";
                             }) == 2);
    return 0;
}

int identifies_rejected_candidate_without_its_content() {
    const auto profile = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    const AgentLoopSpec spec{TaskId::generate(), SessionId::generate(), 1, "goal", profile.id};
    const auto request = memory_request(spec);
    ContextItem valid;
    valid.id = ContextItemId::generate();
    valid.kind = ContextItemKind::RetrievedMemory;
    valid.authority = ContextAuthority::RetrievedMemory;
    valid.content = {TextPart{"private candidate text"}};
    for (const auto reason :
         {"duplicate id", "invalid kind or authority", "pinned item", "invalid item"}) {
        auto bad = valid;
        if (std::string_view(reason) != "duplicate id")
            bad.id = ContextItemId::generate();
        if (std::string_view(reason) == "invalid kind or authority")
            bad.authority = ContextAuthority::SystemPolicy;
        if (std::string_view(reason) == "pinned item")
            bad.pinned = true;
        if (std::string_view(reason) == "invalid item")
            bad.id = {};
        ModelContextSnapshot snapshot;
        snapshot.candidates = {valid, bad};
        const auto result = admit_model_request(request, profile, spec.session_id, snapshot);
        MIRA_CHECK(!result &&
                   result.error().safe_message == std::string("context candidate 1: ") + reason);
    }
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

int reports_only_memory_used_by_the_accepted_attempt() {
    ConversationFixture fixture;
    fixture.profile_->capabilities.limits.max_context_tokens = 128;
    fixture.use_provider({text_response("unused primary")});
    auto fallback_profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://fallback.test"));
    fallback_profile->capabilities.limits.max_context_tokens = 2048;
    auto refused = text_response("refused");
    refused.status = ModelCompletionStatus::Refused;
    auto fallback = std::make_shared<RecordingProvider>(
        fallback_profile,
        std::vector{text_response("ok"), text_response("replay"), text_response("stale"), refused});
    fixture.gateway_->mutable_router().register_profile(fallback_profile);
    fixture.gateway_->register_provider(fallback);
    fixture.admission_->activate(fixture.spec_.task_id, fixture.spec_.task_epoch);

    ContextItem kept;
    kept.id = ContextItemId::generate();
    kept.kind = ContextItemKind::RetrievedMemory;
    kept.authority = ContextAuthority::RetrievedMemory;
    kept.content = {TextPart{"selected memory"}};
    auto duplicate = kept;
    duplicate.id = ContextItemId::generate();
    auto dropped = kept;
    dropped.id = ContextItemId::generate();
    dropped.content = {TextPart{std::string(8192, 'x')}};
    const auto scope = digest_string("test scope");
    const MemoryUseReference used{scope, MemoryId::generate(), 4};
    const MemoryUseReference unused{scope, MemoryId::generate(), 1};
    const std::vector expected{used};
    bool current = true;
    fixture.gateway_->set_context_supplier(
        [&](const auto &, const auto &) -> Result<ModelContextSnapshot> {
            ModelContextSnapshot snapshot;
            snapshot.candidates = {kept, duplicate, dropped};
            snapshot.memory_candidates = {
                {kept.id, {used}}, {duplicate.id, {used}}, {dropped.id, {unused}}};
            snapshot.current = [&] { return current; };
            return snapshot;
        });
    std::vector<MemoryUseReceipt> receipts;
    fixture.gateway_->set_memory_use_sink([&](const MemoryUseReceipt &receipt) -> Result<void> {
        receipts.push_back(receipt);
        if (receipts.size() == 2)
            throw std::runtime_error("test use sink failure");
        return {};
    });
    fallback->on_after_response(2, [&] { current = false; });
    const auto request = memory_request(fixture.spec_);
    const auto run = [&] {
        return fixture.gateway_->infer(request, memory_context(fixture.spec_));
    };
    const auto first = run();
    MIRA_CHECK(first && first.value().admitted && first.value().memory_use &&
               first.value().memory_use->records == expected && receipts.size() == 1);
    MIRA_CHECK(first.value().memory_use->request == request.request_id &&
               first.value().memory_use->attempt == fallback->requests().front().request_id &&
               first.value().memory_use->selection_digest ==
                   fallback->requests().front().prompt_provenance.context_selection_digest);
    MIRA_CHECK(first.value().context_admission->profile_id == fallback_profile->id &&
               first.value().context_admission->budget.dropped_items == 1);

    const auto replay = run();
    MIRA_CHECK(replay && replay.value().admitted && replay.value().memory_use &&
               replay.value().memory_use_report_error && receipts.size() == 2);
    MIRA_CHECK(receipts[0].request == receipts[1].request &&
               receipts[0].records == receipts[1].records &&
               receipts[0].attempt != receipts[1].attempt);

    const auto stale = run();
    MIRA_CHECK(stale && !stale.value().admitted && !stale.value().memory_use);
    current = true;
    const auto rejected = run();
    MIRA_CHECK(rejected && !rejected.value().memory_use && receipts.size() == 2);
    MIRA_CHECK(!run() && receipts.size() == 2);
    MIRA_CHECK(fixture.provider_->requests().empty());

    ModelContextSnapshot unlinked;
    unlinked.candidates = {kept};
    unlinked.memory_candidates = {{dropped.id, {unused}}};
    MIRA_CHECK(
        !admit_model_request(request, *fallback_profile, fixture.spec_.session_id, unlinked));
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
    const auto assistant_on_wire = std::find_if(
        assistant_admitted.value().request.input.begin(),
        assistant_admitted.value().request.input.end(), [](const ModelInputItem &item) {
            const auto *text = std::get_if<TextPart>(&item.content.front());
            return text != nullptr && text->text.find("atlas-2 is the backup") != std::string::npos;
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
    const auto user_on_wire = std::find_if(
        user_admitted.value().request.input.begin(), user_admitted.value().request.input.end(),
        [](const ModelInputItem &item) {
            const auto *text = std::get_if<TextPart>(&item.content.front());
            return text != nullptr && text->text.find("atlas-2 is the backup") != std::string::npos;
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
    if (identifies_rejected_candidate_without_its_content())
        return 1;
    if (overflow_and_elevated_candidates_never_dispatch())
        return 1;
    if (replay_wire_roles_reach_the_wire_and_system_roles_fail_closed())
        return 1;
    if (reports_only_memory_used_by_the_accepted_attempt())
        return 1;
    return fallback_uses_actual_profile_and_stale_completion_is_rejected();
}
