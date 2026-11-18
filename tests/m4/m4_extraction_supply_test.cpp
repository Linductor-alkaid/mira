#include "../support/m36_support.hpp"
#include "../support/m3_support.hpp"
#include "../support/test.hpp"

#include <mira/event_store.hpp>
#include <mira/extraction_model_provider.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {

using namespace mira;
using namespace mira::testing;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-extraction-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

// Deterministic provider stub (m19 pattern): records the request, can fail,
// refuse or report a non-completed status.
class StubExtractionProvider final : public IModelProvider {
  public:
    explicit StubExtractionProvider(std::string response_json)
        : profile_(std::make_shared<ModelProfile>(
              make_profile(ProtocolDialect::OpenAIResponsesV1, "https://extraction.test"))),
          response_json_(std::move(response_json)) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &,
                                              const ProviderInferOptions &) override {
        ++calls_;
        last_request_ = request;
        if (fail_) {
            Error unavailable;
            unavailable.code = ErrorCode::Unavailable;
            unavailable.domain = "test";
            unavailable.safe_message = "provider down";
            return unavailable;
        }
        ModelResponse response;
        response.contract_version = SchemaVersion{1, 0};
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_->id;
        response.requested_model = profile_->model_selector;
        response.status = status_;
        MessageOutput message;
        if (refuse_) {
            OutputRefusalPart refusal;
            refusal.safe_summary = "cannot help with that";
            message.content.emplace_back(std::move(refusal));
        } else {
            OutputTextPart text;
            text.text = response_json_;
            message.content.emplace_back(std::move(text));
        }
        response.output.emplace_back(std::move(message));
        return response;
    }

    [[nodiscard]] const ModelRequest &last_request() const noexcept { return last_request_; }

    bool fail_ = false;
    bool refuse_ = false;
    ModelCompletionStatus status_ = ModelCompletionStatus::Completed;
    std::atomic<std::size_t> calls_{0};

  private:
    std::shared_ptr<ModelProfile> profile_;
    std::string response_json_;
    ModelRequest last_request_;
};

[[nodiscard]] EventEnvelope extraction_event(const SessionId &session, std::uint64_t sequence,
                                             std::string type, std::string data) {
    EventEnvelope event;
    event.event_id = EventId::generate();
    event.runtime_id = RuntimeId::generate();
    event.session_id = session;
    event.session_sequence = sequence;
    event.timestamp = Timestamp::now();
    event.payload = EventPayload{std::move(type), std::move(data), EventClass::State};
    return event;
}

[[nodiscard]] std::string candidate_json(const std::string &kind, const std::string &content,
                                         const std::vector<int> &sources, double confidence) {
    std::string sources_json;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (index > 0) {
            sources_json += ",";
        }
        sources_json += std::to_string(sources[index]);
    }
    return "{\"kind\":\"" + kind + "\",\"content\":\"" + content + "\",\"sources\":[" +
           sources_json + "],\"confidence\":" + std::to_string(confidence) + "}";
}

[[nodiscard]] std::string candidates_output(const std::string &items) {
    return "{\"candidates\":[" + items + "]}";
}

struct StoreFixture final {
    StoreFixture() {
        executor.initialize(kairo::ExecutorConfig{});
        SqliteMemoryStoreOptions options;
        options.path = temp_dir() / ("supply-" + RuntimeId::generate().to_string() + ".db");
        auto opened = SqliteMemoryStore::open(executor, options);
        memory = std::move(opened).value();
    }
    ~StoreFixture() {
        if (memory != nullptr) {
            (void)memory->close();
        }
        (void)executor.shutdown(true);
    }
    kairo::Executor executor;
    std::shared_ptr<SqliteMemoryStore> memory;
};

// Config pairing, credential requirement and the wired profile shape.
int config_validation_and_profile_assembly() {
    ExtractionSupplyConfig config;
    MIRA_CHECK(!config.validate().has_value());
    config.endpoint_origin = "https://supply.test";
    MIRA_CHECK(!config.validate().has_value()); // origin without its model
    config.model_selector = "demo-extractor";
    MIRA_CHECK(!config.validate().has_value()); // no credential name
    config.credential_secret_name = "supply-credential";
    MIRA_CHECK(config.validate().has_value());

    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    auto refused = wire_extraction_supply(config, nullptr);
    MIRA_CHECK(!refused.has_value());
    auto wired = wire_extraction_supply(config, transport);
    MIRA_CHECK(wired.has_value());
    const auto &stack = wired.value();
    MIRA_CHECK(stack.profile->dialect == ProtocolDialect::OpenAIResponsesV1);
    MIRA_CHECK(stack.profile->model_selector == "demo-extractor");
    MIRA_CHECK(stack.profile->credential.name == "supply-credential");
    MIRA_CHECK(stack.profile->capabilities.text.supported);
    MIRA_CHECK(stack.profile->capabilities.strict_json_schema.supported);
    MIRA_CHECK(stack.profile->default_data_policy.store.has_value() &&
               !*stack.profile->default_data_policy.store);
    MIRA_CHECK(stack.provider != nullptr);
    MIRA_CHECK(stack.consolidation_model != nullptr);
    MIRA_CHECK(&stack.model() == stack.consolidation_model.get());
    MIRA_CHECK(stack.consolidation_model->options().degrade_on_error);
    return 0;
}

// The transcript shape, the strict output contract and the bounded request.
int transcript_and_request_shape() {
    StubExtractionProvider provider("{}");
    ExtractionModelOptions options;
    options.max_transcript_events = 3;
    ProviderConsolidationModel model(provider, options);

    const auto session = SessionId::generate();
    std::vector<EventEnvelope> events = {
        extraction_event(session, 11, "TaskFactVerified", R"({"key":"backup","value":"/var"})"),
        extraction_event(session, 12, "ToolOutput", "line1\nline2"),
        extraction_event(session, 13, "Note", std::string(500, 'x')),
        extraction_event(session, 14, "Ignored", "beyond bound"),
    };
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "shape";

    // No events: no model call at all.
    MIRA_CHECK(model.propose({}, scope).empty());
    MIRA_CHECK(provider.calls_.load() == 0);

    auto proposals = model.propose(events, scope);
    MIRA_CHECK(proposals.empty()); // "{}" has no candidates array: degrade
    MIRA_CHECK(provider.calls_.load() == 1);

    const auto &request = provider.last_request();
    MIRA_CHECK(request.input.size() == 2);
    MIRA_CHECK(request.input[0].provenance.source == "mira.memory.extraction.system.v1");
    MIRA_CHECK(request.input[1].provenance.source == "mira.memory.extraction.transcript.v1");
    MIRA_CHECK(request.output_contract.mode == OutputMode::StrictJsonSchema);
    MIRA_CHECK(request.generation.max_output_tokens == 2'048);
    MIRA_CHECK(request.budget.max_requests == 1);
    MIRA_CHECK(request.data_policy.store.has_value() && !*request.data_policy.store);

    const auto &transcript = std::get<TextPart>(request.input[1].content.front()).text;
    MIRA_CHECK(transcript.find("[0|seq=11] TaskFactVerified: {\"key\":\"backup\"") !=
               std::string::npos);
    MIRA_CHECK(transcript.find("line1 line2") != std::string::npos);
    MIRA_CHECK(transcript.find("line1\nline2") == std::string::npos);
    MIRA_CHECK(transcript.find(std::string(200, 'x')) != std::string::npos);
    MIRA_CHECK(transcript.find(std::string(201, 'x')) == std::string::npos);
    MIRA_CHECK(transcript.find("[3|seq=14]") == std::string::npos);
    return 0;
}

// Happy path: candidates map with evidence bound to the cited events, and the
// end-to-end consolidate() run applies them under the pipeline's trust gates.
int happy_path_and_end_to_end() {
    const auto session = SessionId::generate();
    std::vector<EventEnvelope> events = {
        extraction_event(session, 1, "BackupDone", "archive written"),
        extraction_event(session, 2, "OpsStep", "compose down"),
        extraction_event(session, 3, "OpsStep", "compose up"),
        extraction_event(session, 4, "UserUtterance", "keep answers short"),
    };
    const auto response = candidates_output(
        candidate_json("environment-fact", "backup archive path is /var/backups/mira.db", {0},
                       0.8) +
        "," +
        candidate_json("procedure", "restart the stack with compose down then up", {1, 2}, 0.7) +
        "," + candidate_json("preference", "answers should stay short", {3}, 0.9));
    StubExtractionProvider provider(response);
    ProviderConsolidationModel model(provider);

    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "supply-e2e";
    auto proposals = model.propose(events, scope);
    MIRA_CHECK(proposals.size() == 3);
    std::set<MemoryKind> kinds;
    for (const auto &candidate : proposals) {
        MIRA_CHECK(candidate.model_assisted);
        MIRA_CHECK(candidate.proposed.verification == MemoryVerification::Unverified);
        MIRA_CHECK(candidate.reason == MutationReasonCode::Consolidation);
        kinds.insert(candidate.proposed.kind);
    }
    const std::set<MemoryKind> expected_kinds{MemoryKind::EnvironmentFact, MemoryKind::Procedure,
                                              MemoryKind::Preference};
    MIRA_CHECK(kinds == expected_kinds);
    const std::vector<EventId> evidence_zero{events[0].event_id};
    const std::vector<EventId> evidence_pair{events[1].event_id, events[2].event_id};
    const std::vector<EventId> evidence_three{events[3].event_id};
    MIRA_CHECK(proposals[0].evidence == evidence_zero);
    MIRA_CHECK(proposals[1].evidence == evidence_pair);
    MIRA_CHECK(proposals[2].evidence == evidence_three);

    // End to end: the deterministic extractor sees nothing in these events,
    // so every applied candidate came from the model through the pipeline.
    StoreFixture fixture;
    MemoryConsolidator consolidator{ConsolidationPolicy{}, &model};
    auto report = consolidator.consolidate(*fixture.memory, events, scope, Timestamp::now());
    MIRA_CHECK(report.has_value());
    // DEC-115: every model candidate writes automatically at its Unverified
    // source trust level (no kind-based approval default remains).
    MIRA_CHECK(report.value().count_of(CandidateDisposition::Applied) == 3);
    MIRA_CHECK(report.value().pending_approval.empty());

    MemoryQuery query;
    query.scopes = {scope};
    query.text = "answers should stay short";
    auto found = fixture.memory->query(query);
    MIRA_CHECK(found.has_value() && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().statement == "answers should stay short");
    MIRA_CHECK(found.value().records.front().verification == MemoryVerification::Unverified);
    return 0;
}

// Fail-closed parsing: unknown kinds, fabricated citations, empty content and
// source-less statements drop individually; structural failures degrade.
int fail_closed_drops() {
    const auto session = SessionId::generate();
    std::vector<EventEnvelope> events = {extraction_event(session, 1, "OpsStep", "step one"),
                                         extraction_event(session, 2, "OpsStep", "step two")};
    const std::string unknown_kind = candidate_json("mood", "feels good", {0}, 0.9);
    const std::string fabricated = candidate_json("procedure", "fabricated step", {99}, 0.8);
    const std::string empty_content = candidate_json("procedure", "", {0}, 0.8);
    const std::string no_sources = candidate_json("procedure", "no sources", {}, 0.8);
    const std::string no_confidence = candidate_json("procedure", "confidence omitted", {0}, 0.0);
    const std::string valid = candidate_json("procedure", "valid survival step", {0, 1}, 0.6);
    const auto response = candidates_output(unknown_kind + "," + fabricated + "," + empty_content +
                                            "," + no_sources + "," + no_confidence + "," + valid);
    StubExtractionProvider provider(response);
    ProviderConsolidationModel model(provider);
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "drops";
    auto proposals = model.propose(events, scope);
    MIRA_CHECK(proposals.size() == 2);
    MIRA_CHECK(proposals[0].proposed.statement == "confidence omitted");
    MIRA_CHECK(proposals[0].proposed.confidence == 0.0F);
    MIRA_CHECK(proposals[1].proposed.statement == "valid survival step");
    MIRA_CHECK(proposals[1].evidence.size() == 2);

    // Structural failures degrade to an empty set under the default options.
    StubExtractionProvider malformed("not json at all");
    ProviderConsolidationModel degrade_model(malformed);
    MIRA_CHECK(degrade_model.propose(events, scope).empty());
    StubExtractionProvider no_candidates("{}");
    ProviderConsolidationModel missing_model(no_candidates);
    MIRA_CHECK(missing_model.propose(events, scope).empty());
    return 0;
}

// The degrade/throw split: provider errors and refusals are empty sets under
// degrade_on_error (default) and throws under the strict mode.
int degrade_and_throw_modes() {
    const auto session = SessionId::generate();
    std::vector<EventEnvelope> events = {extraction_event(session, 1, "OpsStep", "step one")};
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "modes";

    StubExtractionProvider provider("{}");
    provider.fail_ = true;
    ProviderConsolidationModel degrading(provider);
    MIRA_CHECK(degrading.propose(events, scope).empty());

    provider.fail_ = false;
    provider.refuse_ = true;
    MIRA_CHECK(degrading.propose(events, scope).empty());

    provider.refuse_ = false;
    provider.status_ = ModelCompletionStatus::Failed;
    MIRA_CHECK(degrading.propose(events, scope).empty());

    ExtractionModelOptions strict_options;
    strict_options.degrade_on_error = false;
    ProviderConsolidationModel strict(provider, strict_options);
    bool threw = false;
    try {
        (void)strict.propose(events, scope);
    } catch (const std::runtime_error &error) {
        threw = std::string(error.what()).find("did not complete") != std::string::npos;
    }
    MIRA_CHECK(threw);

    provider.status_ = ModelCompletionStatus::Completed;
    provider.fail_ = true;
    threw = false;
    try {
        (void)strict.propose(events, scope);
    } catch (const std::runtime_error &error) {
        threw = std::string(error.what()).find("provider down") != std::string::npos;
    }
    MIRA_CHECK(threw);
    return 0;
}

// The service-path supply: the stack's processor factory drives a Record/
// Compress proposal over the shared scripted transport.
int agent_processor_factory_smoke() {
    ExtractionSupplyConfig config;
    config.endpoint_origin = "https://supply.test";
    config.model_selector = "demo-extractor";
    config.credential_secret_name = "supply-credential";
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("supply-credential", "sk-supply-marker");
    auto transport = std::make_shared<MockHttpTransport>(secrets);
    // The memory agent's proposal JSON inside one completed responses wire
    // body (the real OpenAiCompatibleProvider wire, scripted).
    const std::string proposal =
        R"({"summary":[{"text":"compressed checkpoint text","citations":[0]}]})";
    // to_json_string(JsonValue(text)) emits the complete quoted string token,
    // so the suffix only closes the content/message/output containers.
    const std::string body =
        R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":)" +
        to_json_string(JsonValue(proposal)) +
        R"(}]}],"usage":{"input_tokens":8,"output_tokens":3}})";
    transport->enqueue_json(200, body);

    auto wired = wire_extraction_supply(config, transport);
    MIRA_CHECK(wired.has_value());
    MemoryAgentFixture fixture;
    fixture.start(wired.value().agent_processor(fixture.executor));
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Compress;
    request.history = {history_text("first line"), history_text("second line")};
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle.has_value());
    MIRA_CHECK(handle.value().result.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
    const auto compress_outcome = handle.value().result.get();
    if (!compress_outcome.has_value()) {
        std::cerr << "compress failed: " << compress_outcome.error().safe_message
                  << " recorded=" << transport->recorded().size() << '\n';
        for (const auto &call : transport->recorded()) {
            std::cerr << "  call status-body: " << call.url << " " << call.body.substr(0, 120)
                      << '\n';
        }
        return 1;
    }
    const auto recorded = transport->recorded();
    MIRA_CHECK(recorded.size() == 1);
    MIRA_CHECK(recorded.front().authorization == "sk-supply-marker");
    MIRA_CHECK(recorded.front().url.rfind("/v1/responses") != std::string::npos);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += config_validation_and_profile_assembly();
    failures += transcript_and_request_shape();
    failures += happy_path_and_end_to_end();
    failures += fail_closed_drops();
    failures += degrade_and_throw_modes();
    failures += agent_processor_factory_smoke();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
