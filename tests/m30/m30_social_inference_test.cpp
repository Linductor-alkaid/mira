#include "support/m36_support.hpp"
#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_provider.hpp>
#include <mira/social_mention_inference.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

class MentionProvider final : public IModelProvider {
  public:
    [[nodiscard]] const ModelProfile &profile() const override { return config; }
    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &,
                                              const ProviderInferOptions &) override {
        captured = request;
        if (on_infer)
            on_infer();
        ModelResponse response;
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        response.status = ModelCompletionStatus::Completed;
        response.output.emplace_back(
            MessageOutput{ModelRole::Assistant, {OutputTextPart{answer, std::nullopt}}});
        return response;
    }
    ModelProfile config = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://mentions.test");
    std::string answer;
    std::optional<ModelRequest> captured;
    std::function<void()> on_infer;
};

class SocialFixture final {
  public:
    SocialFixture() {
        SqliteSocialDirectoryOptions options;
        options.path = storage.root / "social-inference.db";
        auto opened = SqliteSocialDirectory::open(storage.executor, options);
        if (!opened)
            std::abort();
        directory = std::move(opened).value();
    }
    Result<SocialMessageEvidence> record(const SocialAccountKey &account, std::string text,
                                         std::string type = std::string{kSocialMessageTextEvent}) {
        AppendRequest event;
        event.event_id = EventId::generate();
        event.session_id = session;
        event.runtime_id = storage.runtime;
        event.payload = {std::move(type), std::move(text), EventClass::State};
        auto appended = storage.events->append(event);
        if (!appended)
            return appended.error();
        return directory->index_message(
            environment, account, {session, appended.value().session_sequence, event.event_id},
            *storage.events);
    }
    OperationContext operation() const {
        auto value = make_control_context();
        value.session = session;
        return value;
    }
    Result<SocialInferredMentions> infer(MentionProvider &provider,
                                         const SocialMessageLocator &message,
                                         const OperationContext &context) {
        return storage.executor
            .submit_auto([&] {
                return infer_social_message_mentions(*directory, *storage.events, provider,
                                                     environment, message, context);
            })
            .get();
    }

    MemoryAgentFixture storage;
    SocialEnvironmentKey environment{storage.principal.tenant_id, Id128::generate()};
    SessionId session = SessionId::generate();
    SocialAccountKey qq{"qq", Id128::generate()};
    SocialAccountKey bilibili{"bilibili", Id128::generate()};
    std::unique_ptr<SqliteSocialDirectory> directory;
};

int resolves_candidate_names_and_preceding_speakers() {
    SocialFixture fixture;
    const auto first = fixture.record(fixture.qq, "我昨天测了这个。 ");
    const auto current = fixture.record(fixture.bilibili, "他给陈工发了结果，小许也抄送了。");
    MIRA_CHECK(first && current);
    MIRA_CHECK(fixture.directory->activate(first.value().speaker, {2, {"陈工"}}));
    MIRA_CHECK(fixture.directory->activate(current.value().speaker, {2, {"陈工"}}));

    MentionProvider provider;
    provider.answer =
        R"({"mentions":[{"surface":"他","prior_row":0},{"surface":"陈工","prior_row":-1},{"surface":"小许","prior_row":-1}]})";
    const auto result = fixture.infer(provider, current.value().message, fixture.operation());
    MIRA_CHECK(result && result.value().speaker == current.value());
    MIRA_CHECK(result.value().mentions.size() == 3 && provider.captured);
    const auto &reference = result.value().mentions[0];
    MIRA_CHECK(reference.surface == "他" &&
               reference.inferred_prior_message == first.value().message);
    const auto *prior = std::get_if<SocialMessageEvidence>(&reference.candidate);
    MIRA_CHECK(prior && *prior == first.value());
    const auto *ambiguous = std::get_if<SocialAliasResult>(&result.value().mentions[1].candidate);
    MIRA_CHECK(ambiguous && ambiguous->resolution == SocialAliasResolution::Ambiguous);
    const auto *missing = std::get_if<SocialAliasResult>(&result.value().mentions[2].candidate);
    MIRA_CHECK(missing && missing->resolution == SocialAliasResolution::Missing);
    const auto &request = *provider.captured;
    MIRA_CHECK(validate_model_request(request));
    MIRA_CHECK(request.tools.empty() && request.tool_choice.mode == ToolChoiceMode::None);
    MIRA_CHECK(request.data_policy.store == false && !request.data_policy.allow_uploads);
    MIRA_CHECK(request.output_contract.mode == OutputMode::StrictJsonSchema &&
               !request.output_contract.schema_id.is_nil());
    const auto &text = std::get<TextPart>(request.input.at(1).content.front());
    const auto transcript = parse_json(text.text);
    MIRA_CHECK(text.sensitivity == Sensitivity::Sensitive && transcript &&
               transcript.value().as_array()->size() == 2);
    MIRA_CHECK(text.text.find(fixture.qq.account.to_string()) == std::string::npos &&
               text.text.find(first.value().speaker.object.to_string()) == std::string::npos);
    return 0;
}

int rejects_fabricated_mentions_and_out_of_scope_sources() {
    SocialFixture fixture;
    const auto first = fixture.record(fixture.qq, "我检查过。");
    const auto current = fixture.record(fixture.bilibili, "他说陈工检查过。");
    MIRA_CHECK(first && current);
    MentionProvider provider;
    auto context = fixture.operation();
    context.session = SessionId::generate();
    const auto unauthorized = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(!unauthorized && unauthorized.error().code == ErrorCode::PermissionDenied &&
               !provider.captured);

    context = fixture.operation();
    provider.answer = R"({"mentions":[{"surface":"未提及","prior_row":0}]})";
    const auto invented = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(!invented && invented.error().code == ErrorCode::InvalidModelOutput);
    provider.answer = R"({"mentions":[{"surface":"他","prior_row":1}]})";
    const auto fabricated_row = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(!fabricated_row && fabricated_row.error().code == ErrorCode::InvalidModelOutput);

    provider.captured.reset();
    MemoryEventStore alternate;
    AppendRequest event;
    event.event_id = first.value().message.event;
    event.session_id = fixture.session;
    event.runtime_id = fixture.storage.runtime;
    event.payload = {std::string{kSocialMessageTextEvent}, "changed source", EventClass::State};
    MIRA_CHECK(alternate.append(event));
    event.event_id = current.value().message.event;
    event.payload.data = "他说陈工检查过。";
    MIRA_CHECK(alternate.append(event));
    auto substituted = fixture.storage.executor
                           .submit_auto([&] {
                               return infer_social_message_mentions(
                                   *fixture.directory, alternate, provider, fixture.environment,
                                   current.value().message, context);
                           })
                           .get();
    MIRA_CHECK(!substituted && substituted.error().code == ErrorCode::DataLoss &&
               !provider.captured);

    provider.config.default_data_policy.store = true;
    const auto retained = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(!retained && retained.error().code == ErrorCode::PermissionDenied &&
               !provider.captured);
    provider.config.default_data_policy.store = false;
    const auto forbidden =
        fixture.record(fixture.bilibili, default_forbidden_content_markers().front());
    MIRA_CHECK(forbidden);
    const auto blocked = fixture.infer(provider, forbidden.value().message, context);
    MIRA_CHECK(!blocked && blocked.error().code == ErrorCode::SafetyRejected && !provider.captured);
    const auto unrelated = fixture.record(fixture.bilibili, "other payload", "TaskFactVerified");
    MIRA_CHECK(unrelated);
    const auto wrong_type = fixture.infer(provider, unrelated.value().message, context);
    MIRA_CHECK(!wrong_type && wrong_type.error().code == ErrorCode::UnsupportedCapability &&
               !provider.captured);
    return 0;
}

int invalidates_erased_or_cancelled_evidence() {
    SocialFixture fixture;
    const auto first = fixture.record(fixture.qq, "我检查过。");
    const auto current = fixture.record(fixture.bilibili, "他说陈工检查过。");
    MIRA_CHECK(first && current);
    MentionProvider provider;
    provider.answer = R"({"mentions":[{"surface":"他","prior_row":0}]})";
    auto context = fixture.operation();
    bool cancelled = false;
    context.cancellation_requested = [&] { return cancelled; };
    provider.on_infer = [&] { cancelled = true; };
    const auto stopped = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled);

    bool erased = false;
    provider.on_infer = [&] {
        erased = fixture.directory->erase_object(first.value().speaker).has_value();
    };
    const auto stale = fixture.infer(provider, current.value().message, fixture.operation());
    MIRA_CHECK(erased && !stale && stale.error().code == ErrorCode::InvalidState);
    return 0;
}
} // namespace

int main() {
    if (const auto result = resolves_candidate_names_and_preceding_speakers(); result != 0)
        return result;
    if (const auto result = rejects_fabricated_mentions_and_out_of_scope_sources(); result != 0)
        return result;
    return invalidates_erased_or_cancelled_evidence();
}
