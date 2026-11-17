// Offline host consumer for the memory vector leg with a real producer
// (DEC-111): OpenAiCompatibleEmbeddingProvider over a scripted transport,
// repair driven through the ContextMemorySupervisor, and MemoryAgentService
// recall answering a paraphrase through the semantic leg — including the
// degrade round when the producer fails. No network, no device input.
#include <mira/context_memory_supervisor.hpp>
#include <mira/embedding_provider.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_agent.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/model_contracts.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace mira;
using namespace std::chrono_literals;

// Scripted IHttpTransport: parses the /v1/embeddings request body, hands the
// input texts to the host-visible log, and answers with hand-authored
// geometry. Known statements project onto fixed axes so the paraphrase proof
// is deterministic; unknown texts get a diagonal fallback.
class ScriptedEmbeddingsTransport final : public IHttpTransport {
  public:
    explicit ScriptedEmbeddingsTransport(std::shared_ptr<ISecretResolver> secrets)
        : secrets_(std::move(secrets)) {}

    Result<HttpResponseInfo> execute(const HttpRequest &request, const TransportLimits & /*limits*/,
                                     const OperationContext & /*context*/,
                                     const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override {
        if (fail_next_.exchange(false)) {
            trace.write_started = true;
            return make_model_error(ModelDomainCode::TransportFailed, "scripted embedding outage",
                                    true);
        }
        const auto body = parse_json(request.body);
        if (!body || !body.value().is_object()) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "scripted transport received a non-JSON body");
        }
        const auto *input = body.value().find("input");
        if (input == nullptr || !input->is_array() || input->as_array()->empty()) {
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "scripted transport received no input array");
        }
        const auto &texts = *input->as_array();
        JsonValue::Array data;
        for (const auto &text : texts) {
            if (!text.is_string()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "scripted transport received a non-text input");
            }
            const std::string statement = *text.as_string();
            {
                const std::lock_guard lock(mutex_);
                seen_.push_back(statement);
            }
            static const std::map<std::string, std::vector<double>> kGeometry{
                {"user prefers concise replies", {1.0, 0.0}},
                {"whiteboard markers are dry", {0.0, 1.0}},
                {"keep answers short", {0.98, 0.02}},
            };
            const auto found = kGeometry.find(statement);
            const std::vector<double> vector =
                found != kGeometry.end() ? found->second : std::vector<double>{0.7, 0.7};
            JsonValue::Array components;
            for (const double component : vector) {
                components.emplace_back(JsonValue(component));
            }
            JsonValue::Object entry;
            entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(data.size())));
            entry.emplace_back("embedding", JsonValue(std::move(components)));
            data.emplace_back(JsonValue(std::move(entry)));
        }
        JsonValue::Object root;
        root.emplace_back("data", JsonValue(std::move(data)));
        const std::string payload = to_json_string(JsonValue(std::move(root)));
        on_chunk(payload);
        trace.write_started = true;
        trace.write_completed = true;
        trace.headers_received = true;
        HttpResponseInfo info;
        info.status = 200;
        info.headers.emplace_back("content-type", "application/json");
        info.body_bytes = payload.size();
        return info;
    }

    void fail_next() { fail_next_.store(true); }
    [[nodiscard]] std::vector<std::string> seen() const {
        const std::lock_guard lock(mutex_);
        return seen_;
    }

  private:
    std::shared_ptr<ISecretResolver> secrets_;
    mutable std::mutex mutex_;
    std::vector<std::string> seen_;
    std::atomic<bool> fail_next_{false};
};

class KeySecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        if (reference.name != "embedding-credential") {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.safe_message = "secret is not configured";
            return error;
        }
        // In a real deployment the resolver reads the key from the host's
        // secret store; it is resolved only at the transport boundary and
        // never logged (this offline example resolves a placeholder).
        return std::string{"offline-placeholder-key"};
    }
};

bool seed_record(IMemory &memory, const MemoryScope &scope, std::string statement) {
    MemoryCandidate candidate;
    candidate.proposed.id = MemoryId::generate();
    candidate.proposed.scope = scope;
    candidate.proposed.kind = MemoryKind::EnvironmentFact;
    candidate.proposed.statement = std::move(statement);
    candidate.proposed.provenance = {EventId::generate()};
    candidate.evidence = candidate.proposed.provenance;
    candidate.proposed.verification = MemoryVerification::Verified;
    candidate.proposed.confidence = 0.9F;
    const auto result =
        MemoryConsolidator{}.consolidate_candidates(memory, {candidate}, scope, Timestamp::now());
    return result && result.value().count_of(CandidateDisposition::Applied) == 1;
}

} // namespace

int main() {
    kairo::Executor executor;
    if (!executor.initialize(kairo::ExecutorConfig{})) {
        std::cerr << "executor initialize failed\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-embedding-recall-" + RuntimeId::generate().to_string());
    std::filesystem::create_directories(root);
    {
        auto events = std::make_shared<FileEventStore>(root / "events");
        SqliteMemoryStoreOptions options;
        options.path = root / "memory.db";
        auto opened = SqliteMemoryStore::open(executor, options);
        if (!opened) {
            std::cerr << "memory store open failed\n";
            (void)executor.shutdown(true);
            return 3;
        }
        auto memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};

        // DEC-107: work bindings use the host-derived User/Session domains.
        PrincipalContext principal;
        principal.tenant_id = TenantId::generate();
        principal.user_id = UserId::generate();
        principal.identity_version = 1;
        const auto session = SessionId::generate();
        const MemoryScope scope = memory_session_scope(principal, session);
        if (!seed_record(*memory, scope, "user prefers concise replies") ||
            !seed_record(*memory, scope, "whiteboard markers are dry")) {
            std::cerr << "record seeding failed\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 4;
        }

        // Producer plane: profile + scripted transport + protocol adapter.
        auto profile = std::make_shared<EmbeddingProfile>();
        profile->display_name = "offline-embedder";
        profile->endpoint_origin = "https://embeddings.invalid";
        profile->api_prefix = "/v1";
        profile->model_selector = "demo-embedder";
        profile->expected_dims = 2;
        profile->credential = SecretRef{"embedding-credential"};
        profile->deadlines.total = 5s;
        if (!profile->validate()) {
            std::cerr << "embedding profile rejected\n";
            (void)memory->close();
            (void)executor.shutdown(true);
            return 5;
        }
        auto transport =
            std::make_shared<ScriptedEmbeddingsTransport>(std::make_shared<KeySecretResolver>());
        auto provider = std::make_shared<OpenAiCompatibleEmbeddingProvider>(profile, transport);
        std::cout << "producer identity: " << provider->embedder_id().to_string() << "\n";

        // Repair plane: the supervisor drives bounded passes (Deferrable).
        MemoryEmbeddingIndexer indexer(*memory, *provider);
        ContextMemorySupervisor supervisor(executor, SupervisorConfig{});
        auto repair = supervisor.schedule_embedding_repair(indexer, *memory, scope).get();
        if (!repair || repair.value().repaired != 2 || repair.value().index_lag != 0) {
            std::cerr << "embedding repair failed\n";
            (void)supervisor.begin_shutdown();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 6;
        }
        std::cout << "repair: scanned=" << repair.value().scanned
                  << " repaired=" << repair.value().repaired << " lag=" << repair.value().index_lag
                  << "\n";

        // Recall plane: the service embeds each query; a paraphrase that
        // shares no lexical token with the statement is answered by the
        // vector leg alone.
        MemoryAgentService service(executor, memory, events, RuntimeId::generate());
        if (!service.set_recall_embedder(provider)) {
            std::cerr << "recall embedder wiring failed\n";
            (void)supervisor.begin_shutdown();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 7;
        }
        if (!service.initialize()) {
            std::cerr << "memory agent initialize failed\n";
            (void)supervisor.begin_shutdown();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 7;
        }
        MemoryWorkBinding binding;
        binding.principal = principal;
        binding.work = {TaskId::generate(), session, 1, "tone task", ModelProfileId::generate()};
        binding.scopes = {scope};
        binding.environment_epoch = 1;
        if (!service.bind(binding)) {
            std::cerr << "work binding failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 7;
        }

        OperationContext context;
        context.task = binding.work.task_id;
        context.session = binding.work.session_id;
        context.task_epoch = binding.work.task_epoch;
        context.started_at = Timestamp::now();
        context.deadline = std::chrono::steady_clock::now() + 5s;

        MemoryAgentRequest request;
        request.task = binding.work.task_id;
        request.query = "keep answers short";
        auto handle = service.submit(request, context);
        const auto settled =
            handle && handle.value().result.wait_for(5s) == std::future_status::ready;
        bool recalled_ok = false;
        std::size_t candidates = 0;
        if (settled) {
            const auto outcome = handle.value().result.get();
            if (outcome && !outcome.value().degraded) {
                recalled_ok = true;
                candidates = outcome.value().candidates.size();
            }
        }
        if (!recalled_ok || candidates != 1) {
            std::cerr << "semantic recall failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 8;
        }
        std::cout << "recall: candidates=" << candidates << " degraded=false\n";

        // Degrade round: the producer fails, recall still settles and the
        // result is marked degraded (the vector leg is optional by design).
        transport->fail_next();
        auto failed_handle = service.submit(request, context);
        const auto failed_settled =
            failed_handle && failed_handle.value().result.wait_for(5s) == std::future_status::ready;
        bool degraded_flag = false;
        if (failed_settled) {
            const auto outcome = failed_handle.value().result.get();
            degraded_flag = outcome.has_value() && outcome.value().degraded;
        }
        if (!degraded_flag) {
            std::cerr << "producer failure did not degrade the recall\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 9;
        }
        std::cout << "degrade: settled=true degraded=true\n";
        std::cout << "wire texts observed: " << transport->seen().size() << "\n";

        (void)supervisor.begin_shutdown();
        (void)service.close();
        (void)memory->close();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    (void)executor.shutdown(true);
    return 0;
}
