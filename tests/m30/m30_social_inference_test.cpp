#include "support/m36_support.hpp"
#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_provider.hpp>
#include <mira/social_attribute_inference.hpp>
#include <mira/social_mention_inference.hpp>
#include <mira/social_profile.hpp>
#include <mira/social_relationship_inference.hpp>

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

class SourceReadHook final : public IEventStore {
  public:
    explicit SourceReadHook(IEventStore &store) : store_(store) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        return store_.append(request);
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return store_.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return store_.read(query); }
    Result<EventSlicePage> read_slice(const EventSliceQuery &query) const override {
        on_read();
        return store_.read_slice(query);
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return store_.recover(options);
    }
    Result<void> flush(Durability required) override { return store_.flush(required); }

    std::function<void()> on_read;

  private:
    IEventStore &store_;
};

class SocialFixture final {
  public:
    explicit SocialFixture(bool paired = false) {
        SqliteSocialDirectoryOptions options;
        options.path = storage.root / "social-inference.db";
        auto opened = SqliteSocialDirectory::open(storage.executor, options,
                                                  paired ? storage.memory : nullptr);
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
        R"({"mentions":[{"surface":"他","prior_row":0},{"surface":"陈工","prior_row":0},{"surface":"小许","prior_row":-1}]})";
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
    const auto &claims = result.value().claims.rows;
    MIRA_CHECK(claims.size() == 3 && claims[0].row == 0 &&
               claims[0].disposition == SocialClaimDisposition::Applied && claims[1].row == 1 &&
               claims[1].reason == "literal-alias" && claims[2].row == 2 &&
               claims[2].disposition == SocialClaimDisposition::Unresolved);
    const auto &request = *provider.captured;
    MIRA_CHECK(validate_model_request(request));
    MIRA_CHECK(request.tools.empty() && request.tool_choice.mode == ToolChoiceMode::None);
    MIRA_CHECK(request.data_policy.store == false && !request.data_policy.allow_uploads);
    MIRA_CHECK(request.output_contract.mode == OutputMode::StrictJsonSchema &&
               !request.output_contract.schema_id.is_nil());
    const auto &text = std::get<TextPart>(request.input.at(1).content.front());
    const auto transcript = parse_json(text.text);
    MIRA_CHECK(text.sensitivity == Sensitivity::Sensitive && transcript &&
               transcript.value().find("current") && transcript.value().find("prior") &&
               transcript.value().find("prior")->as_array()->size() == 1);
    MIRA_CHECK(text.text.find(fixture.qq.account.to_string()) == std::string::npos &&
               text.text.find(first.value().speaker.object.to_string()) == std::string::npos);
    return 0;
}

int literal_mentions_resolve_without_a_working_model() {
    SocialFixture fixture;
    const auto first = fixture.record(fixture.qq, "上午好");
    const auto current =
        fixture.record(fixture.bilibili, "小许给 CHEN   LI 发消息，问 Joanne 和吴工。小许");
    MIRA_CHECK(first && current);
    MIRA_CHECK(fixture.directory->activate(first.value().speaker,
                                           {2, {"小许", "许", "Chen Li", "Ann", "吴工"}}));
    MIRA_CHECK(fixture.directory->activate(current.value().speaker, {2, {"吴工"}}));
    const SocialObjectKey foreign{{fixture.environment.tenant, Id128::generate()},
                                  Id128::generate()};
    MIRA_CHECK(fixture.directory->activate(foreign, {1, {"小许"}}));
    const auto literal =
        fixture.storage.executor
            .submit_auto([&] {
                return infer_social_message_mentions(*fixture.directory, *fixture.storage.events,
                                                     nullptr, fixture.environment,
                                                     current.value().message, fixture.operation());
            })
            .get();
    MIRA_CHECK(literal && literal.value().mentions.size() == 3 && !literal.value().model_error);
    const auto &names = literal.value().mentions;
    MIRA_CHECK(names[0].surface == "小许" && names[1].surface == "CHEN   LI" &&
               names[2].surface == "吴工");
    const auto &unique = std::get<SocialAliasResult>(names[0].candidate);
    MIRA_CHECK(unique.resolution == SocialAliasResolution::Unique &&
               unique.matches.front().key == first.value().speaker);
    const auto &ambiguous = std::get<SocialAliasResult>(names[2].candidate);
    MIRA_CHECK(ambiguous.resolution == SocialAliasResolution::Ambiguous &&
               ambiguous.matches.size() == 2);
    MentionProvider provider;
    provider.answer = "not JSON";
    const auto fallback = fixture.infer(provider, current.value().message, fixture.operation());
    MIRA_CHECK(fallback && fallback.value().mentions.size() == 3 && fallback.value().model_error &&
               fallback.value().model_error->code == ErrorCode::InvalidModelOutput);
    auto expired = fixture.operation();
    expired.deadline = Timestamp::now().monotonic - std::chrono::milliseconds{1};
    const auto timed =
        fixture.directory->scan_message_aliases(fixture.environment, "小许", expired);
    MIRA_CHECK(!timed && timed.error().code == ErrorCode::DeadlineExceeded);
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
    provider.answer =
        R"({"mentions":[{"surface":"未提及","prior_row":0},{"surface":"他","prior_row":0}]})";
    const auto invented = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(invented && invented.value().mentions.size() == 1 &&
               invented.value().mentions.front().surface == "他" &&
               invented.value().rejected.size() == 1 &&
               invented.value().rejected.front().row == 0 &&
               invented.value().rejected.front().reason == "missing-source");
    provider.answer = R"({"mentions":[{"surface":"他","prior_row":1}]})";
    const auto fabricated_row = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(fabricated_row && fabricated_row.value().mentions.empty() &&
               fabricated_row.value().rejected.size() == 1 &&
               fabricated_row.value().rejected.front().reason == "invalid-prior-row");
    JsonValue::Array excess(9, JsonValue::Object{{"surface", "他"}, {"prior_row", 0}});
    provider.answer = to_json_string(JsonValue::Object{{"mentions", std::move(excess)}});
    const auto truncated = fixture.infer(provider, current.value().message, context);
    MIRA_CHECK(truncated && truncated.value().truncated && truncated.value().mentions.size() == 1 &&
               truncated.value().rejected.size() == 7 &&
               truncated.value().claims.truncated_rows == 1 &&
               truncated.value().claims.rows.size() == 8);

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

int records_grounded_attributes_for_the_indexed_speaker() {
    SocialFixture fixture(true);
    const auto first = fixture.record(fixture.qq, "我负责导航开发和接口文档，团队是夜航。");
    MIRA_CHECK(first);
    const auto linked = fixture.directory->observe_account(
        fixture.environment, fixture.bilibili, first.value().message.event, first.value().speaker);
    MIRA_CHECK(linked && linked.value().object == first.value().speaker);
    const auto unrelated =
        fixture.record(SocialAccountKey{"qq", Id128::generate()}, "我做设备运维。");
    MIRA_CHECK(unrelated && unrelated.value().speaker != first.value().speaker);
    MentionProvider provider;
    const std::vector<std::string> fields{"role", "team"};
    const auto infer = [&](const SocialMessageLocator &message, const OperationContext &context) {
        return fixture.storage.executor
            .submit_auto([&] {
                return infer_and_record_social_message_attributes(
                    *fixture.directory, *fixture.storage.events, provider, fixture.environment,
                    message, fields, context);
            })
            .get();
    };
    const auto recall = [&] {
        return fixture.directory->recall_account_attribute(fixture.environment, fixture.bilibili,
                                                           "role");
    };
    auto foreign = fixture.operation();
    foreign.session = SessionId::generate();
    const auto denied = infer(first.value().message, foreign);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied && !provider.captured);
    provider.answer = R"({"attributes":[{"field":"role","quote":"未出现"}]})";
    const auto fabricated = infer(first.value().message, fixture.operation());
    MIRA_CHECK(fabricated && fabricated.value().claims.rows.size() == 1 &&
               fabricated.value().claims.rows[0].reason == "missing-source");
    provider.answer = R"({"attributes":[{"field":"private_field","quote":"导航开发"}]})";
    const auto invalid_field = infer(first.value().message, fixture.operation());
    MIRA_CHECK(invalid_field && invalid_field.value().claims.rows.size() == 1 &&
               invalid_field.value().claims.rows[0].reason == "unknown-field");
    const std::vector<SocialMessageAttributeCandidate> invalid_batch{{"role", "导航开发"},
                                                                     {"team", "不存在"}};
    const auto invalid = fixture.storage.executor
                             .submit_auto([&] {
                                 return fixture.directory->record_inferred_message_attributes(
                                     fixture.environment, first.value().message,
                                     *fixture.storage.events, invalid_batch, fixture.operation());
                             })
                             .get();
    MIRA_CHECK(!invalid && recall().value()->assertions.records.empty());
    provider.answer =
        R"({"attributes":[{"field":"role","quote":"未出现"},{"field":"role","quote":"导航开发"},{"field":"team","quote":"夜航"}]})";
    const auto result = infer(first.value().message, fixture.operation());
    MIRA_CHECK(result && result.value().speaker == first.value() &&
               result.value().consolidation.count_of(CandidateDisposition::Applied) == 2);
    const auto &attribute_rows = result.value().claims.rows;
    MIRA_CHECK(attribute_rows.size() == 3 && attribute_rows[0].row == 0 &&
               attribute_rows[0].disposition == SocialClaimDisposition::Dropped &&
               attribute_rows[1].row == 1 &&
               attribute_rows[1].disposition == SocialClaimDisposition::Applied &&
               attribute_rows[2].row == 2 &&
               attribute_rows[2].disposition == SocialClaimDisposition::Applied);
    MIRA_CHECK(result.value().consolidation.entries.front().kind == MemoryKind::SocialAttribute);
    MIRA_CHECK(provider.captured && provider.captured->tools.empty() &&
               !provider.captured->data_policy.allow_uploads &&
               provider.captured->data_policy.store == false);
    const auto observed = recall();
    MIRA_CHECK(observed && observed.value() && observed.value()->assertions.records.size() == 1);
    const auto &record = observed.value()->assertions.records.front();
    MIRA_CHECK(record.statement == "导航开发" &&
               record.verification == MemoryVerification::Unverified &&
               record.social_attribute->source == SocialAttributeSource::Inferred &&
               record.provenance == std::vector{first.value().message.event});
    MIRA_CHECK(fixture.directory->close());
    fixture.directory.reset();
    SqliteSocialDirectoryOptions options;
    options.path = fixture.storage.root / "social-inference.db";
    auto reopened =
        SqliteSocialDirectory::open(fixture.storage.executor, options, fixture.storage.memory);
    MIRA_CHECK(reopened);
    fixture.directory = std::move(reopened).value();
    const auto replay = infer(first.value().message, fixture.operation());
    MIRA_CHECK(replay && replay.value().consolidation.entries.front().idempotent_replay);
    provider.answer = R"({"attributes":[{"field":"role","quote":"接口文档"}]})";
    const auto conflict = infer(first.value().message, fixture.operation());
    MIRA_CHECK(conflict && conflict.value().consolidation.count_of(
                               CandidateDisposition::RejectedConflict) == 1);
    const auto later = fixture.record(fixture.bilibili, "我现在负责质量验证。");
    MIRA_CHECK(later && later.value().speaker == first.value().speaker);
    provider.answer = R"({"attributes":[{"field":"role","quote":"质量验证"}]})";
    MIRA_CHECK(infer(later.value().message, fixture.operation()));
    const auto with_later = recall();
    MIRA_CHECK(with_later && with_later.value()->assertions.records.size() == 2);

    bool cancelled = false;
    auto interrupted = fixture.operation();
    interrupted.cancellation_requested = [&] { return cancelled; };
    provider.on_infer = [&] { cancelled = true; };
    const auto stopped = infer(later.value().message, interrupted);
    MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled);
    bool erased = false;
    provider.on_infer = [&] {
        erased = fixture.directory->erase_object(first.value().speaker).has_value();
    };
    const auto stale = infer(later.value().message, fixture.operation());
    MIRA_CHECK(erased && !stale && !recall());
    MIRA_CHECK(fixture.directory->read(unrelated.value().speaker).value());
    bool closed = false;
    provider.answer = R"({"attributes":[{"field":"role","quote":"设备运维"}]})";
    provider.on_infer = [&] { closed = fixture.directory->close().has_value(); };
    const auto after_close = infer(unrelated.value().message, fixture.operation());
    MIRA_CHECK(closed && !after_close);
    return 0;
}

int described_fields_require_the_configured_source_evidence() {
    SocialFixture fixture(true);
    MentionProvider provider;
    const std::vector<SocialAttributeField> fields{
        {"name",
         "The speaker's own name, excluding names of pets or other people.",
         {"我叫", "我是"}},
        {"pet", "A pet the speaker says they own."},
    };
    const auto infer = [&](const SocialMessageLocator &message) {
        return fixture.storage.executor
            .submit_auto([&] {
                return infer_and_record_social_message_attributes(
                    *fixture.directory, *fixture.storage.events, provider, fixture.environment,
                    message, fields, fixture.operation());
            })
            .get();
    };
    const auto pet = fixture.record(fixture.qq, "我养了一只橘猫，它叫胖虎。");
    MIRA_CHECK(pet);
    JsonValue::Array items(9, JsonValue::Object{{"field", "pet"}, {"quote", "胖虎"}});
    items[0] = JsonValue::Object{{"field", "name"}, {"quote", "胖虎"}};
    provider.answer = to_json_string(JsonValue::Object{{"attributes", std::move(items)}});
    const auto result = infer(pet.value().message);
    MIRA_CHECK(result && result.value().claims.truncated_rows == 1 &&
               result.value().claims.rows.size() == 8);
    MIRA_CHECK(result.value().claims.rows[0].reason == "missing-field-prefix" &&
               result.value().claims.rows[1].disposition == SocialClaimDisposition::Applied &&
               result.value().claims.rows[2].reason == "duplicate");
    MIRA_CHECK(provider.captured);
    const auto &prompt = std::get<TextPart>(provider.captured->input.at(1).content.front()).text;
    MIRA_CHECK(prompt.find(fields[0].description) != std::string::npos &&
               prompt.find(fields[0].value_prefixes[0]) != std::string::npos);
    const auto recall_name = [&] {
        return fixture.directory->recall_account_attribute(fixture.environment, fixture.qq, "name");
    };
    MIRA_CHECK(recall_name().value()->assertions.records.empty());
    const auto current = fixture.record(fixture.qq, "我叫 小许。它叫胖虎。");
    MIRA_CHECK(current);
    provider.answer = R"({"attributes":[{"field":"name","quote":"小许"}]})";
    const auto self = infer(current.value().message);
    MIRA_CHECK(self && self.value().claims.rows[0].disposition == SocialClaimDisposition::Applied);
    const auto recalled = recall_name();
    MIRA_CHECK(recalled && recalled.value()->assertions.records.size() == 1 &&
               recalled.value()->assertions.records[0].statement == "小许");
    provider.answer = "bad JSON";
    const auto malformed = infer(current.value().message);
    MIRA_CHECK(!malformed && malformed.error().code == ErrorCode::InvalidModelOutput);
    return 0;
}

int records_only_resolved_directed_relationships() {
    SocialFixture fixture(true);
    const auto first =
        fixture.record(fixture.qq, "我指导陈工，李工指导我，我也指导赵工和吴工，也指导老陈。");
    const auto chen = fixture.record(fixture.bilibili, "已收到。");
    MIRA_CHECK(first && chen);
    const SocialObjectKey li{fixture.environment, Id128::generate()};
    const SocialObjectKey zhao_a{fixture.environment, Id128::generate()};
    const SocialObjectKey zhao_b{fixture.environment, Id128::generate()};
    const SocialObjectKey external{{fixture.environment.tenant, Id128::generate()},
                                   Id128::generate()};
    MIRA_CHECK(fixture.directory->activate(chen.value().speaker, {2, {"陈工", "老陈"}}));
    MIRA_CHECK(fixture.directory->activate(li, {1, {"李工"}}));
    MIRA_CHECK(fixture.directory->activate(zhao_a, {1, {"赵工"}}));
    MIRA_CHECK(fixture.directory->activate(zhao_b, {1, {"赵工"}}));
    MIRA_CHECK(fixture.directory->activate(external, {1, {"吴工"}}));
    const auto read = [&](const SocialRelationshipKey &key) -> Result<MemoryQueryResult> {
        const auto scope = social_relationship_scope(key);
        if (!scope)
            return scope.error();
        MemoryQuery query;
        query.scopes = {scope.value()};
        query.social_relationship_predicate = "mentors";
        return fixture.storage.memory->query(query);
    };
    const SocialRelationshipKey forward{first.value().speaker, chen.value().speaker};
    const SocialRelationshipKey reverse{li, first.value().speaker};
    MentionProvider provider;
    const std::vector<std::string> predicates{"mentors"};
    const auto infer = [&](const SocialMessageLocator &message) {
        return fixture.storage.executor
            .submit_auto([&] {
                return infer_and_record_social_message_relationships(
                    *fixture.directory, *fixture.storage.events, provider, fixture.environment,
                    message, predicates, fixture.operation());
            })
            .get();
    };
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"陈工","quote":"我指导赵工","speaker_is_subject":true}]})";
    const auto fabricated = infer(first.value().message);
    MIRA_CHECK(fabricated && fabricated.value().claims.rows.size() == 1 &&
               fabricated.value().claims.rows[0].reason == "missing-source");
    provider.answer = "bad JSON";
    const auto malformed = infer(first.value().message);
    MIRA_CHECK(!malformed && malformed.error().code == ErrorCode::InvalidModelOutput);
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"陈工","quote":"不存在的引文","speaker_is_subject":true},{"predicate":"mentors","alias":"陈工","quote":"我指导陈工","speaker_is_subject":true},{"predicate":"mentors","alias":"李工","quote":"李工指导我","speaker_is_subject":false},{"predicate":"mentors","alias":"赵工","quote":"我也指导赵工和吴工","speaker_is_subject":true},{"predicate":"mentors","alias":"吴工","quote":"我也指导赵工和吴工","speaker_is_subject":true},{"predicate":"mentors","alias":"老陈","quote":"也指导老陈","speaker_is_subject":true},null,{},false]})";
    const auto report = infer(first.value().message);
    MIRA_CHECK(report && report.value().participants.speaker == first.value());
    MIRA_CHECK((report.value().unresolved == std::vector<std::size_t>{3, 4}));
    const auto &relationship_rows = report.value().claims.rows;
    MIRA_CHECK(report.value().claims.truncated_rows == 1 && relationship_rows.size() == 8 &&
               relationship_rows[0].reason == "missing-source" && relationship_rows[1].row == 1 &&
               relationship_rows[1].disposition == SocialClaimDisposition::Applied &&
               relationship_rows[3].reason == "ambiguous-target" &&
               relationship_rows[4].reason == "missing-target" &&
               relationship_rows[5].reason == "duplicate-target" &&
               relationship_rows[6].reason == "invalid-row" &&
               relationship_rows[7].reason == "invalid-row");
    MIRA_CHECK(std::get<SocialAliasResult>(report.value().participants.mentions[2]).resolution ==
               SocialAliasResolution::Ambiguous);
    MIRA_CHECK(std::get<SocialAliasResult>(report.value().participants.mentions[3]).resolution ==
               SocialAliasResolution::Missing);
    MIRA_CHECK(report.value().consolidation.count_of(CandidateDisposition::Applied) == 2 &&
               report.value().consolidation.entries.front().kind == MemoryKind::SocialRelationship);
    const auto out = read(forward);
    const auto in = read(reverse);
    MIRA_CHECK(out && in && out.value().records.size() == 1 && in.value().records.size() == 1);
    MIRA_CHECK(out.value().records.front().statement == "我指导陈工" &&
               out.value().records.front().provenance == std::vector{first.value().message.event} &&
               in.value().records.front().statement == "李工指导我" &&
               in.value().records.front().verification == MemoryVerification::Unverified);
    MIRA_CHECK(read({chen.value().speaker, first.value().speaker}).value().records.empty());
    const auto replay = infer(first.value().message);
    MIRA_CHECK(replay && replay.value().consolidation.entries.front().idempotent_replay &&
               replay.value().consolidation.entries.back().idempotent_replay);

    const auto later = fixture.record(fixture.qq, "我又指导了陈工。");
    MIRA_CHECK(later);
    SourceReadHook hook(*fixture.storage.events);
    bool updated = false;
    hook.on_read = [&] {
        updated = fixture.directory->activate(chen.value().speaker, {3, {"老陈"}}).has_value();
    };
    const std::vector<SocialMessageRelationshipCandidate> candidate{
        {"mentors", "陈工", "我又指导了陈工", true}};
    const auto stale = fixture.storage.executor
                           .submit_auto([&] {
                               return fixture.directory->record_inferred_message_relationships(
                                   fixture.environment, later.value().message, hook, candidate,
                                   fixture.operation());
                           })
                           .get();
    MIRA_CHECK(updated && !stale && stale.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(read(forward).value().records.size() == 1);
    MIRA_CHECK(fixture.directory->erase_object(li));
    MIRA_CHECK(read(reverse).value().records.empty() && read(forward).value().records.size() == 1);
    return 0;
}

int grounds_pronoun_relationships_in_prior_speakers() {
    SocialFixture fixture(true);
    const auto prior = fixture.record(fixture.qq, "这次重构由我负责。");
    const auto second_anchor = fixture.record(fixture.qq, "我把接口图带来了。");
    const auto bystander =
        fixture.record(SocialAccountKey{"qq", Id128::generate()}, "我来做笔记。");
    const auto current = fixture.record(fixture.bilibili, "他指导我重构接口。");
    MIRA_CHECK(prior && second_anchor && bystander && current);
    MIRA_CHECK(fixture.directory->activate(bystander.value().speaker, {2, {"他"}}));
    const SocialRelationshipKey expected{prior.value().speaker, current.value().speaker};
    const auto read = [&] {
        MemoryQuery query;
        query.scopes = {social_relationship_scope(expected).value()};
        query.social_relationship_predicate = "mentors";
        return fixture.storage.memory->query(query);
    };
    MentionProvider provider;
    const std::vector<std::string> predicates{"mentors"};
    const auto infer = [&](const SocialMessageLocator &message) {
        return fixture.storage.executor
            .submit_auto([&] {
                return infer_and_record_social_message_relationships(
                    *fixture.directory, *fixture.storage.events, provider, fixture.environment,
                    message, predicates, fixture.operation());
            })
            .get();
    };
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"他","quote":"他指导我重构接口","speaker_is_subject":false,"prior_row":3}]})";
    const auto invalid = infer(current.value().message);
    MIRA_CHECK(invalid && invalid.value().claims.rows.size() == 1 &&
               invalid.value().claims.rows[0].reason == "invalid-prior-row");
    MIRA_CHECK(read().value().records.empty());
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"他","quote":"他指导我重构接口","speaker_is_subject":false,"prior_row":0}]})";
    const auto recorded = infer(current.value().message);
    MIRA_CHECK(recorded && recorded.value().unresolved.empty() &&
               recorded.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(recorded.value().participants.mentions.size() == 1 &&
               std::get<SocialMessageEvidence>(recorded.value().participants.mentions.front()) ==
                   prior.value());
    MIRA_CHECK(provider.captured);
    const auto payload =
        parse_json(std::get<TextPart>(provider.captured->input.at(1).content.front()).text);
    MIRA_CHECK(payload);
    const auto *transcript = payload.value().find("transcript");
    MIRA_CHECK(transcript && transcript->is_array() && transcript->as_array()->size() == 4);
    const auto saved = read();
    const std::vector<EventId> evidence{current.value().message.event, prior.value().message.event};
    MIRA_CHECK(saved && saved.value().records.size() == 1 &&
               saved.value().records.front().provenance == evidence &&
               saved.value().records.front().verification == MemoryVerification::Unverified);
    const auto replay = infer(current.value().message);
    MIRA_CHECK(replay && replay.value().consolidation.entries.front().idempotent_replay);
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"他","quote":"他指导我重构接口","speaker_is_subject":false,"prior_row":1}]})";
    const auto conflict = infer(current.value().message);
    MIRA_CHECK(conflict && conflict.value().consolidation.count_of(
                               CandidateDisposition::RejectedConflict) == 1);

    auto foreign = prior.value().message;
    foreign.session = SessionId::generate();
    const std::vector<SocialMessageRelationshipCandidate> direct{
        {"mentors", "他", "他指导我重构接口", false, foreign}};
    const auto cross_session =
        fixture.storage.executor
            .submit_auto([&] {
                return fixture.directory->record_inferred_message_relationships(
                    fixture.environment, current.value().message, *fixture.storage.events, direct,
                    fixture.operation());
            })
            .get();
    MIRA_CHECK(!cross_session && cross_session.error().code == ErrorCode::InvalidArgument);

    const auto later = fixture.record(fixture.bilibili, "他又指导我整理文档。");
    MIRA_CHECK(later);
    bool erased = false;
    provider.on_infer = [&] {
        erased = fixture.directory->erase_object(bystander.value().speaker).has_value();
    };
    provider.answer =
        R"({"relationships":[{"predicate":"mentors","alias":"他","quote":"他又指导我整理文档","speaker_is_subject":false,"prior_row":0}]})";
    const auto stale = infer(later.value().message);
    MIRA_CHECK(erased && !stale && stale.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(read().value().records.size() == 1);
    MIRA_CHECK(fixture.directory->erase_object(prior.value().speaker));
    MIRA_CHECK(read().value().records.empty());
    return 0;
}
} // namespace

int main() {
    if (const auto result = resolves_candidate_names_and_preceding_speakers(); result != 0)
        return result;
    if (const auto result = literal_mentions_resolve_without_a_working_model(); result != 0)
        return result;
    if (const auto result = rejects_fabricated_mentions_and_out_of_scope_sources(); result != 0)
        return result;
    if (const auto result = invalidates_erased_or_cancelled_evidence(); result != 0)
        return result;
    if (const auto result = records_grounded_attributes_for_the_indexed_speaker(); result != 0)
        return result;
    if (const auto result = described_fields_require_the_configured_source_evidence(); result != 0)
        return result;
    if (const auto result = records_only_resolved_directed_relationships(); result != 0)
        return result;
    return grounds_pronoun_relationships_in_prior_speakers();
}
