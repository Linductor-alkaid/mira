#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/event_store.hpp>
#include <mira/hybrid_retrieval_wiring.hpp>
#include <mira/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
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
        auto path = base / ("mira-m4-hybrid-wiring-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

// Deterministic ordinal id (m17/m18 technique).
[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

[[nodiscard]] std::string embeddings_body(const std::vector<std::vector<float>> &rows) {
    JsonValue::Array data;
    for (std::size_t position = 0; position < rows.size(); ++position) {
        JsonValue::Object entry;
        entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(position)));
        JsonValue::Array values;
        for (const float component : rows[position]) {
            values.emplace_back(JsonValue(static_cast<double>(component)));
        }
        entry.emplace_back("embedding", JsonValue(std::move(values)));
        data.emplace_back(JsonValue(std::move(entry)));
    }
    JsonValue::Object root;
    root.emplace_back("data", JsonValue(std::move(data)));
    return to_json_string(JsonValue(std::move(root)));
}

// Exact /v1/rerank response shape captured live (index + relevance_score).
[[nodiscard]] std::string rerank_body(const std::vector<std::pair<int, double>> &scored) {
    JsonValue::Array results;
    for (const auto &[index, score] : scored) {
        JsonValue::Object entry;
        entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(index)));
        entry.emplace_back("document", JsonValue());
        entry.emplace_back("relevance_score", JsonValue(score));
        results.emplace_back(JsonValue(std::move(entry)));
    }
    JsonValue::Object root;
    root.emplace_back("id", JsonValue(std::string("wiring-test")));
    root.emplace_back("results", JsonValue(std::move(results)));
    return to_json_string(JsonValue(std::move(root)));
}

[[nodiscard]] HybridRetrievalConfig full_config() {
    HybridRetrievalConfig config;
    config.embedding_endpoint_origin = "https://embeddings.invalid";
    config.embedding_model_selector = "demo-embedder";
    config.rerank_endpoint_origin = "https://rerank.invalid";
    config.rerank_model_selector = "demo-reranker";
    config.credential_secret_name = "wiring-credential";
    config.deadline = std::chrono::milliseconds{5'000};
    return config;
}

[[nodiscard]] std::shared_ptr<MockHttpTransport> wired_transport() {
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("wiring-credential", "resolved-marker-key");
    return std::make_shared<MockHttpTransport>(secrets);
}

struct Harness final {
    explicit Harness(SqliteMemoryStoreOptions options)
        : events(std::make_shared<FileEventStore>(temp_dir() /
                                                  ("events-" + RuntimeId::generate().to_string()))),
          supervisor(executor, SupervisorConfig{}) {
        if (options.path.empty()) {
            options.path = temp_dir() / ("memory-" + RuntimeId::generate().to_string() + ".db");
        }
        (void)executor.initialize(kairo::ExecutorConfig{});
        auto opened = SqliteMemoryStore::open(executor, options);
        memory = std::move(opened).value();
    }
    ~Harness() {
        (void)supervisor.begin_shutdown();
        if (memory != nullptr) {
            (void)memory->close();
        }
        (void)executor.shutdown(true);
    }

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    kairo::Executor executor;
    std::shared_ptr<FileEventStore> events;
    std::shared_ptr<SqliteMemoryStore> memory;
    ContextMemorySupervisor supervisor;
};

[[nodiscard]] MemoryMutation add_for(const MemoryScope &scope, std::string statement,
                                     float confidence) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = MemoryKind::EnvironmentFact;
    record.statement = std::move(statement);
    record.recorded_at = std::chrono::system_clock::now();
    record.validity.valid_from = record.recorded_at;
    record.provenance = {EventId::generate()};
    record.verification = MemoryVerification::Verified;
    record.confidence = confidence;
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return mutation;
}

// The store-side half must project the config onto the options before open,
// and the shipped store default is now ReciprocalRank (DEC-113).
int default_options_and_store_projection() {
    SqliteMemoryStoreOptions options;
    MIRA_CHECK(options.fusion == RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(options.near_dup_threshold == 0.0);

    const auto config = full_config();
    auto projected = hybrid_retrieval_store_options(config);
    MIRA_CHECK(projected.fusion == RetrievalFusion::ReciprocalRank);
    MIRA_CHECK(projected.near_dup_threshold == 0.85);
    projected.path = temp_dir() / "projection.db";
    MIRA_CHECK(projected.validate().has_value());

    HybridRetrievalConfig weighted;
    weighted.embedding_endpoint_origin = "https://embeddings.invalid";
    weighted.embedding_model_selector = "demo-embedder";
    weighted.credential_secret_name = "wiring-credential";
    weighted.fusion = RetrievalFusion::WeightedSum;
    weighted.near_dup_threshold = 0.0;
    auto opt_in = hybrid_retrieval_store_options(weighted, SqliteMemoryStoreOptions{});
    MIRA_CHECK(opt_in.fusion == RetrievalFusion::WeightedSum);
    MIRA_CHECK(opt_in.near_dup_threshold == 0.0);
    return 0;
}

// Full wiring: both legs, the projection indexer, the recall embedder and
// the convenience routing — then the DEC-111 chain end to end on a scripted
// transport (repair pass + semantic recall with the correct identity).
int full_wire_writes_recall_and_repairs() {
    const auto config = full_config();
    Harness harness(hybrid_retrieval_store_options(config));
    const auto transport = wired_transport();

    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.identity_version = 1;
    const auto session = SessionId::generate();
    const MemoryScope scope = memory_session_scope(principal, session);
    MIRA_CHECK(harness.memory->apply(add_for(scope, "user prefers concise replies", 0.9F)));
    MIRA_CHECK(harness.memory->apply(add_for(scope, "whiteboard markers are dry", 0.8F)));

    MemoryAgentService service(harness.executor, harness.memory, harness.events,
                               RuntimeId::generate());
    auto stack = wire_hybrid_retrieval(config, transport, harness.memory.get(), &service);
    MIRA_CHECK(stack.has_value());
    MIRA_CHECK(stack.value().embedding_provider != nullptr);
    MIRA_CHECK(stack.value().reranker != nullptr);
    MIRA_CHECK(stack.value().indexer != nullptr);
    const auto expected_identity = embedding_identity_from_descriptor(
        stack.value().embedding_provider->profile().canonical_descriptor());
    MIRA_CHECK(stack.value().embedder_id.to_string() == expected_identity.to_string());
    const auto &profile = stack.value().embedding_provider->profile();
    MIRA_CHECK(profile.model_selector == "demo-embedder");
    MIRA_CHECK(profile.credential.name == "wiring-credential");
    MIRA_CHECK(profile.deadlines.total == std::chrono::milliseconds{5'000});

    // Repair plane (Deferrable routing): the bounded pass embeds per record
    // (index_record calls embed one statement at a time), so the two seeded
    // records consume two scripted single-vector responses.
    transport->enqueue_json(200, embeddings_body({{1.0F, 0.0F}}));
    transport->enqueue_json(200, embeddings_body({{0.0F, 1.0F}}));
    auto repair =
        stack.value().schedule_embedding_repair(harness.supervisor, *harness.memory, scope).get();
    MIRA_CHECK(repair.has_value());
    MIRA_CHECK(repair.value().repaired == 2);
    MIRA_CHECK(repair.value().index_lag == 0);

    // Recall plane: the service embeds the query (identity carried through),
    // the vector leg answers a zero-lexical-overlap paraphrase and the
    // result is not degraded.
    MIRA_CHECK(service.initialize());
    MemoryWorkBinding binding;
    binding.principal = principal;
    binding.work = {TaskId::generate(), session, 1, "wiring task", ModelProfileId::generate()};
    binding.scopes = {scope};
    binding.environment_epoch = 1;
    MIRA_CHECK(service.bind(binding));
    OperationContext context;
    context.task = binding.work.task_id;
    context.session = binding.work.session_id;
    context.task_epoch = binding.work.task_epoch;
    context.started_at = Timestamp::now();
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    transport->enqueue_json(200, embeddings_body({{1.0F, 0.1F}}));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "keep answers short";
    auto handle = service.submit(request, context);
    MIRA_CHECK(handle.has_value());
    MIRA_CHECK(handle.value().result.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
    const auto outcome = handle.value().result.get();
    MIRA_CHECK(outcome.has_value() && !outcome.value().degraded);
    MIRA_CHECK(!outcome.value().candidates.empty());

    // Exactly three calls hit the transport: two repair embeds (per-record
    // calls) and the query embedding — all to the /v1/embeddings path with
    // the credential resolved only at the transport boundary.
    const auto recorded = transport->recorded();
    MIRA_CHECK(recorded.size() == 3);
    for (const auto &call : recorded) {
        const auto suffix = call.url.rfind("/v1/embeddings");
        MIRA_CHECK(suffix != std::string::npos);
        MIRA_CHECK(call.url.size() - suffix == std::string("/v1/embeddings").size());
        MIRA_CHECK(call.authorization == "resolved-marker-key");
    }
    (void)service.close();
    return 0;
}

// Single-leg configurations leave the other leg's members null and fail
// closed on its routing; the wired leg routes successfully.
int single_leg_configs_fail_closed() {
    // Embedding only: rerank routing is refused.
    {
        HybridRetrievalConfig config;
        config.embedding_endpoint_origin = "https://embeddings.invalid";
        config.embedding_model_selector = "demo-embedder";
        config.credential_secret_name = "wiring-credential";
        Harness harness(hybrid_retrieval_store_options(config));
        auto stack =
            wire_hybrid_retrieval(config, wired_transport(), harness.memory.get(), nullptr);
        MIRA_CHECK(stack.has_value());
        MIRA_CHECK(stack.value().embedding_provider != nullptr);
        MIRA_CHECK(stack.value().indexer != nullptr);
        MIRA_CHECK(stack.value().reranker == nullptr);
        auto refused =
            stack.value().schedule_context_rerank(harness.supervisor, ContextQuery{}, {}).get();
        MIRA_CHECK(!refused.has_value());
        MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    }
    // Rerank only: repair routing is refused, rerank routing follows the
    // scripted relevance.
    {
        HybridRetrievalConfig config;
        config.rerank_endpoint_origin = "https://rerank.invalid";
        config.rerank_model_selector = "demo-reranker";
        config.rerank_max_output = 2;
        config.credential_secret_name = "wiring-credential";
        Harness harness(hybrid_retrieval_store_options(config));
        auto transport = wired_transport();
        auto stack = wire_hybrid_retrieval(config, transport, harness.memory.get(), nullptr);
        MIRA_CHECK(stack.has_value());
        MIRA_CHECK(stack.value().embedding_provider == nullptr);
        MIRA_CHECK(stack.value().indexer == nullptr);
        MIRA_CHECK(stack.value().reranker != nullptr);
        MIRA_CHECK(stack.value().embedder_id.is_nil());
        auto refused =
            stack.value()
                .schedule_embedding_repair(harness.supervisor, *harness.memory, MemoryScope{})
                .get();
        MIRA_CHECK(!refused.has_value());
        MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);

        transport->enqueue_json(200, rerank_body({{1, 0.90}, {0, 0.10}}));
        ContextQuery query;
        query.session = SessionId{ordinal_id(999)};
        query.text = "tone";
        std::vector<ContextCandidate> candidates;
        for (const std::uint64_t ordinal : {std::uint64_t{0}, std::uint64_t{1}}) {
            ContextCandidate candidate;
            candidate.asset_id = ContextAssetId{ordinal_id(ordinal + 10)};
            candidate.kind = ContextAssetKind::ConversationSegment;
            candidate.text = ordinal == 0 ? "verbose answer" : "short answer";
            candidate.score = 1.0;
            candidates.push_back(candidate);
        }
        auto ranked =
            stack.value().schedule_context_rerank(harness.supervisor, query, candidates).get();
        MIRA_CHECK(ranked.has_value());
        MIRA_CHECK(ranked.value().size() == 2);
        MIRA_CHECK(ranked.value().front().retrieval_rank == 1);
        MIRA_CHECK(ranked.value().front().candidate.text == "short answer");
    }
    return 0;
}

// A rejected wire call must not touch the agent: no embed request may reach
// the transport on a later recall, and the agent accepts a valid re-wire.
int invalid_config_leaves_agent_untouched() {
    Harness harness(SqliteMemoryStoreOptions{});
    const auto transport = wired_transport();

    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.identity_version = 1;
    const auto session = SessionId::generate();
    const MemoryScope scope = memory_session_scope(principal, session);
    MIRA_CHECK(harness.memory->apply(add_for(scope, "laser interlock requires badge", 0.9F)));

    MemoryAgentService service(harness.executor, harness.memory, harness.events,
                               RuntimeId::generate());

    HybridRetrievalConfig broken = full_config();
    broken.rerank_model_selector.clear(); // rerank origin without its model
    auto refused = wire_hybrid_retrieval(broken, transport, harness.memory.get(), &service);
    MIRA_CHECK(!refused.has_value());

    MIRA_CHECK(service.initialize());
    MemoryWorkBinding binding;
    binding.principal = principal;
    binding.work = {TaskId::generate(), session, 1, "probe task", ModelProfileId::generate()};
    binding.scopes = {scope};
    binding.environment_epoch = 1;
    MIRA_CHECK(service.bind(binding));
    OperationContext context;
    context.task = binding.work.task_id;
    context.session = binding.work.session_id;
    context.task_epoch = binding.work.task_epoch;
    context.started_at = Timestamp::now();
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "totally unrelated wording";
    auto handle = service.submit(request, context);
    MIRA_CHECK(handle.has_value());
    MIRA_CHECK(handle.value().result.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
    const auto outcome = handle.value().result.get();
    MIRA_CHECK(outcome.has_value() && !outcome.value().degraded);
    // The regression this gate guards: if the failed wire had injected an
    // embedder before validating, this recall would have attempted an embed
    // call (and degraded when the empty script refused it).
    MIRA_CHECK(transport->recorded().empty());
    (void)service.close();

    // A fresh (not yet initialized) service accepts a valid wiring with the
    // same store and transport.
    HybridRetrievalConfig valid;
    valid.embedding_endpoint_origin = "https://embeddings.invalid";
    valid.embedding_model_selector = "demo-embedder";
    valid.credential_secret_name = "wiring-credential";
    MemoryAgentService fresh(harness.executor, harness.memory, harness.events,
                             RuntimeId::generate());
    auto rewired = wire_hybrid_retrieval(valid, transport, harness.memory.get(), &fresh);
    MIRA_CHECK(rewired.has_value());
    MIRA_CHECK(rewired.value().embedding_provider != nullptr);
    return 0;
}

// DEC-113 equivalence anchor: with no query vector, the new default fusion
// orders exactly like the historical WeightedSum mode.
int no_vector_queries_order_identically() {
    const std::vector<std::pair<std::string, float>> corpus = {
        {"calibration file lives in the shared drive", 0.9F},
        {"calibration device is serviced twice a year", 0.7F},
        {"file format must be npy", 0.5F},
        {"the coffee machine is broken again", 0.8F},
    };
    const auto seed_all = [&](SqliteMemoryStore &store) {
        MemoryScope scope;
        scope.kind = MemoryScopeKind::Environment;
        scope.subject_id = "equivalence";
        for (const auto &entry : corpus) {
            if (!store.apply(add_for(scope, entry.first, entry.second)).has_value()) {
                return MemoryScope{};
            }
        }
        return scope;
    };
    SqliteMemoryStoreOptions default_options;
    default_options.max_per_scope_kind = 8;
    SqliteMemoryStoreOptions weighted_options = default_options;
    weighted_options.fusion = RetrievalFusion::WeightedSum;

    Harness default_store(default_options);
    Harness weighted_store(weighted_options);
    const auto scope_a = seed_all(*default_store.memory);
    const auto scope_b = seed_all(*weighted_store.memory);
    MIRA_CHECK(!scope_a.subject_id.empty() && !scope_b.subject_id.empty());

    MemoryQuery query;
    query.scopes = {scope_a};
    query.text = "calibration file";
    auto rrf_result = default_store.memory->query(query);
    query.scopes = {scope_b};
    auto weighted_result = weighted_store.memory->query(query);
    MIRA_CHECK(rrf_result.has_value() && weighted_result.has_value());
    MIRA_CHECK(!rrf_result.value().records.empty());
    MIRA_CHECK(rrf_result.value().records.size() == weighted_result.value().records.size());
    for (std::size_t index = 0; index < rrf_result.value().records.size(); ++index) {
        MIRA_CHECK(rrf_result.value().records[index].statement ==
                   weighted_result.value().records[index].statement);
    }
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += default_options_and_store_projection();
    failures += full_wire_writes_recall_and_repairs();
    failures += single_leg_configs_fail_closed();
    failures += invalid_config_leaves_agent_untouched();
    failures += no_vector_queries_order_identically();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
