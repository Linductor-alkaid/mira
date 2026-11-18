// Offline host consumer for the DEC-113 default hybrid retrieval wiring:
// one call to wire_hybrid_retrieval installs the embedding producer (recall
// prior + projection indexer), the remote cross-encoder reranker and the
// store-side E-configuration (rank fusion + near-dup suppression). The full
// chain runs on one scripted transport serving both /v1/embeddings and
// /v1/rerank: supervisor-driven repair, semantic recall over a zero-overlap
// paraphrase, a Layer 2 rerank through the stack's convenience routing, and
// the degrade round. No network.
#include <mira/event_store.hpp>
#include <mira/hybrid_retrieval_wiring.hpp>
#include <mira/json.hpp>
#include <mira/model_transport.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {
using namespace mira;

// One scripted transport for both provider legs: embeddings calls pop
// preloaded vector responses; rerank calls answer with reversed scripted
// relevance (the last document is the most relevant). `fail_next` injects
// one transport-level failure for the degrade round.
class ScriptedHybridTransport final : public IHttpTransport {
  public:
    explicit ScriptedHybridTransport(std::shared_ptr<ISecretResolver> secrets)
        : secrets_(std::move(secrets)) {}

    Result<HttpResponseInfo> execute(const HttpRequest &request, const TransportLimits & /*limits*/,
                                     const OperationContext & /*context*/,
                                     const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override {
        if (fail_next_.exchange(false)) {
            trace.write_started = true;
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.model";
            error.safe_message = "scripted provider outage";
            error.retryable = true;
            return error;
        }
        std::string body;
        if (request.url.size() >= std::string("/v1/embeddings").size() &&
            request.url.substr(request.url.size() - std::string("/v1/embeddings").size()) ==
                "/v1/embeddings") {
            std::lock_guard lock(mutex_);
            if (embedding_bodies_.empty()) {
                Error error;
                error.code = ErrorCode::Internal;
                error.domain = "mira.test";
                error.safe_message = "embedding script is exhausted";
                return error;
            }
            body = embedding_bodies_.front();
            embedding_bodies_.pop_front();
        } else if (request.url.size() >= std::string("/v1/rerank").size() &&
                   request.url.substr(request.url.size() - std::string("/v1/rerank").size()) ==
                       "/v1/rerank") {
            const auto parsed = parse_json(request.body);
            const auto *documents = parsed.has_value() && parsed.value().is_object()
                                        ? parsed.value().find("documents")
                                        : nullptr;
            const std::size_t count =
                documents != nullptr && documents->is_array() ? documents->as_array()->size() : 0;
            // Scripted relevance reverses the wire order, so the reranker
            // must visibly invert the retrieval order.
            JsonValue::Array results;
            for (std::size_t index = 0; index < count; ++index) {
                const std::size_t scored = count - 1 - index;
                JsonValue::Object entry;
                entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(scored)));
                entry.emplace_back("document", JsonValue());
                entry.emplace_back("relevance_score",
                                   JsonValue(static_cast<double>(scored + 1) / 10.0));
                results.emplace_back(JsonValue(std::move(entry)));
            }
            JsonValue::Object root;
            root.emplace_back("id", JsonValue(std::string("offline-hybrid")));
            root.emplace_back("results", JsonValue(std::move(results)));
            body = to_json_string(JsonValue(std::move(root)));
        } else {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "mira.test";
            error.safe_message = "scripted transport received an unexpected path";
            return error;
        }
        on_chunk(body);
        trace.write_started = true;
        trace.write_completed = true;
        trace.headers_received = true;
        HttpResponseInfo info;
        info.status = 200;
        info.headers.emplace_back("content-type", "application/json");
        info.body_bytes = body.size();
        return info;
    }

    void push_embedding(std::vector<float> vector) {
        JsonValue::Array data;
        JsonValue::Object entry;
        JsonValue::Array values;
        for (const float component : vector) {
            values.emplace_back(JsonValue(static_cast<double>(component)));
        }
        entry.emplace_back("embedding", JsonValue(std::move(values)));
        data.emplace_back(JsonValue(std::move(entry)));
        JsonValue::Object root;
        root.emplace_back("data", JsonValue(std::move(data)));
        std::lock_guard lock(mutex_);
        embedding_bodies_.push_back(to_json_string(JsonValue(std::move(root))));
    }

    void fail_next() { fail_next_.store(true); }

  private:
    std::shared_ptr<ISecretResolver> secrets_;
    std::mutex mutex_;
    std::deque<std::string> embedding_bodies_;
    std::atomic<bool> fail_next_{false};
};

class KeySecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        if (reference.name != "hybrid-credential") {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.domain = "mira.test";
            error.safe_message = "secret is not configured";
            return error;
        }
        // In a real deployment the resolver reads the host's secret store;
        // the plaintext never enters profiles, logs or events.
        return std::string{"offline-placeholder-key"};
    }
};

[[nodiscard]] bool seed_record(IMemory &memory, const MemoryScope &scope, std::string statement) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = MemoryKind::EnvironmentFact;
    record.statement = std::move(statement);
    record.recorded_at = std::chrono::system_clock::now();
    record.validity.valid_from = record.recorded_at;
    record.provenance = {EventId::generate()};
    record.verification = MemoryVerification::Verified;
    record.confidence = 0.9F;
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return memory.apply(mutation).has_value();
}

[[nodiscard]] std::vector<ContextCandidate> make_candidates() {
    const std::vector<std::string> texts = {"verbose complete answer", "calibration aside",
                                            "short crisp answer"};
    std::vector<ContextCandidate> candidates;
    for (std::size_t ordinal = 0; ordinal < texts.size(); ++ordinal) {
        ContextCandidate candidate;
        Id128::Bytes bytes{};
        bytes[0] = static_cast<std::uint8_t>(ordinal + 1);
        candidate.asset_id = ContextAssetId{Id128{bytes}};
        candidate.kind = ContextAssetKind::ConversationSegment;
        candidate.text = texts[ordinal];
        candidate.score = 1.0 - static_cast<double>(ordinal) * 0.1;
        candidates.push_back(candidate);
    }
    return candidates;
}

} // namespace

int main() {
    kairo::Executor executor;
    if (!executor.initialize(kairo::ExecutorConfig{})) {
        std::cerr << "executor initialize failed\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-hybrid-wiring-" + RuntimeId::generate().to_string());
    std::filesystem::create_directories(root);
    {
        auto events = std::make_shared<FileEventStore>(root / "events");

        // Store-side half of the wiring, before open(): the E-configuration
        // of the DEC-113 benchmark becomes the shipped default.
        HybridRetrievalConfig config;
        config.embedding_endpoint_origin = "https://embeddings.invalid";
        config.embedding_model_selector = "demo-embedder";
        config.rerank_endpoint_origin = "https://rerank.invalid";
        config.rerank_model_selector = "demo-reranker";
        config.credential_secret_name = "hybrid-credential";
        config.deadline = std::chrono::milliseconds{5'000};
        SqliteMemoryStoreOptions memory_options = hybrid_retrieval_store_options(config);
        memory_options.path = root / "memory.db";
        if (!memory_options.validate().has_value()) {
            std::cerr << "store options rejected\n";
            (void)executor.shutdown(true);
            return 3;
        }
        auto opened = SqliteMemoryStore::open(executor, memory_options);
        if (!opened) {
            std::cerr << "memory store open failed\n";
            (void)executor.shutdown(true);
            return 3;
        }
        auto memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};

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

        // Host-owned HTTP stack; Core never touches sockets. One scripted
        // transport serves both provider legs offline.
        auto transport =
            std::make_shared<ScriptedHybridTransport>(std::make_shared<KeySecretResolver>());

        // Host-side half of the wiring, after the store exists: both legs in
        // one call, validated before the irreversible recall injection.
        ContextMemorySupervisor supervisor(executor, SupervisorConfig{}, events.get(),
                                           RuntimeId::generate(), session);
        MemoryAgentService service(executor, memory, events, RuntimeId::generate());
        auto stack = wire_hybrid_retrieval(config, transport, memory.get(), &service);
        if (!stack) {
            std::cerr << "hybrid wiring failed: " << stack.error().safe_message << "\n";
            (void)supervisor.begin_shutdown();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 5;
        }
        std::cout << "wired: identity=" << stack.value().embedder_id.to_string()
                  << " legs=embedding+rerank+indexer fusion=ReciprocalRank near_dup=0.85\n";

        // Repair plane: the stack routes the bounded pass (Deferrable); the
        // indexer embeds per record, so the two seeds take two responses.
        transport->push_embedding({1.0F, 0.0F});
        transport->push_embedding({0.0F, 1.0F});
        auto repair = stack.value().schedule_embedding_repair(supervisor, *memory, scope).get();
        if (!repair || repair.value().repaired != 2 || repair.value().index_lag != 0) {
            std::cerr << "embedding repair failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 6;
        }
        std::cout << "repair: scanned=" << repair.value().scanned
                  << " repaired=" << repair.value().repaired << " lag=" << repair.value().index_lag
                  << "\n";

        // Recall plane: a zero-lexical-overlap paraphrase is answered by the
        // vector leg alone; the producer failure afterwards only degrades.
        if (!service.initialize()) {
            std::cerr << "memory agent initialize failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
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
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        MemoryAgentRequest request;
        request.task = binding.work.task_id;
        request.query = "keep answers short";
        transport->push_embedding({1.0F, 0.1F});
        auto handle = service.submit(request, context);
        bool recalled = false;
        if (handle &&
            handle.value().result.wait_for(std::chrono::seconds{5}) == std::future_status::ready) {
            const auto outcome = handle.value().result.get();
            recalled = outcome.has_value() && !outcome.value().degraded &&
                       !outcome.value().candidates.empty();
        }
        if (!recalled) {
            std::cerr << "semantic recall failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 8;
        }
        std::cout << "recall: paraphrase answered, degraded=false\n";

        // Layer 2 plane: the stack routes one rerank (Interactive); the
        // scripted relevance reverses the retrieval order.
        ContextQuery rerank_query;
        rerank_query.session = session;
        rerank_query.text = request.query;
        auto ranked = stack.value()
                          .schedule_context_rerank(supervisor, rerank_query, make_candidates())
                          .get();
        if (!ranked || ranked.value().empty() ||
            ranked.value().front().candidate.text != "short crisp answer") {
            std::cerr << "layer 2 rerank failed\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 9;
        }
        std::cout << "rerank: top=" << ranked.value().front().candidate.text
                  << " retrieval_rank=" << ranked.value().front().retrieval_rank << "\n";

        // Degrade round: a provider outage settles the recall with the
        // degraded flag instead of failing the caller.
        transport->fail_next();
        auto failed_handle = service.submit(request, context);
        bool degraded = false;
        if (failed_handle && failed_handle.value().result.wait_for(std::chrono::seconds{5}) ==
                                 std::future_status::ready) {
            const auto outcome = failed_handle.value().result.get();
            degraded = outcome.has_value() && outcome.value().degraded;
        }
        if (!degraded) {
            std::cerr << "provider outage did not degrade the recall\n";
            (void)supervisor.begin_shutdown();
            (void)service.close();
            (void)memory->close();
            (void)executor.shutdown(true);
            return 10;
        }
        std::cout << "degrade: settled=true degraded=true\n";

        (void)supervisor.begin_shutdown();
        (void)service.close();
        (void)memory->close();
    }
    std::filesystem::remove_all(root);
    (void)executor.shutdown(true);
    return 0;
}
