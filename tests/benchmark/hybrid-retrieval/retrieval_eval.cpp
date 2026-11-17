// Live quality benchmark (MNT-202611-HR, DEC-112): drives the shipped
// SqliteMemoryStore through five retrieval configurations over a curated
// gold dataset, with embeddings from the shipped OpenAiCompatibleEmbedding
// provider and rerank from the shipped OpenAiCompatibleReranker. Dumps
// per-query rankings as JSON for the metrics script.
//
// Usage: retrieval_eval <dataset.json> <output.json>
// Requires SF_EMBEDDING_KEY in the environment.
#include <mira/adapters/net/openssl_tls.hpp>
#include <mira/adapters/net/socket_transport.hpp>
#include <mira/embedding_provider.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/reranker_provider.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <kairo/executor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace mira;

class EnvSecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        const char *key = std::getenv(reference.name.c_str());
        if (key == nullptr) {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.safe_message = "credential environment variable is not set: " + reference.name;
            return error;
        }
        return std::string{key};
    }
};

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

[[nodiscard]] MemoryScope scope_for_kind(MemoryKind kind, const SessionId &session,
                                         std::uint64_t ordinal) {
    MemoryScope scope;
    // Per-kind scope allowlists (design Context/Memory §6.3): Application
    // facts and Procedures live in an Application scope, Recovery lessons in
    // an Agent scope, everything else in the lab Environment scope.
    if (kind == MemoryKind::ApplicationFact || kind == MemoryKind::Procedure) {
        scope.kind = MemoryScopeKind::Application;
        scope.subject_id = "eval-app";
    } else if (kind == MemoryKind::RecoveryLesson) {
        scope.kind = MemoryScopeKind::Agent;
        scope.subject_id = "eval-agent";
    } else {
        scope.kind = MemoryScopeKind::Environment;
        scope.subject_id = "eval-lab";
    }
    scope.tenant_id = "eval-tenant-" + std::to_string(ordinal % 1);
    static_cast<void>(session);
    return scope;
}

[[nodiscard]] MemoryVerification floor_for_kind(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Preference:
        return MemoryVerification::Unverified;
    case MemoryKind::Episode:
    case MemoryKind::EnvironmentFact:
    case MemoryKind::ApplicationFact:
        return MemoryVerification::Observed;
    case MemoryKind::RecoveryLesson:
        return MemoryVerification::Verified;
    default:
        return MemoryVerification::Observed;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: retrieval_eval <dataset.json> <output.json>\n";
        return 1;
    }
    std::ifstream dataset_file(argv[1]);
    if (!dataset_file) {
        std::cerr << "dataset not found\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << dataset_file.rdbuf();
    const auto dataset = parse_json(buffer.str());
    if (!dataset) {
        std::cerr << "dataset is not valid JSON\n";
        return 1;
    }

    kairo::Executor executor;
    if (!executor.initialize(kairo::ExecutorConfig{})) {
        return 2;
    }
    auto secrets = std::make_shared<EnvSecretResolver>();
    auto tls_factory = std::make_shared<adapters::net::OpenSslTlsChannelFactory>();
    if (!tls_factory->initialize()) {
        return 3;
    }
    auto transport =
        std::make_shared<adapters::net::SocketHttpTransport>(executor, secrets, tls_factory);
    if (!transport->start()) {
        return 4;
    }

    // ---- embedding plane (shipped adapter, live) ----
    auto embedding_profile = std::make_shared<EmbeddingProfile>();
    embedding_profile->display_name = "benchmark";
    embedding_profile->endpoint_origin = "https://api.siliconflow.cn";
    embedding_profile->api_prefix = "/v1";
    embedding_profile->model_selector = "BAAI/bge-m3";
    embedding_profile->credential = SecretRef{"SF_EMBEDDING_KEY"};
    embedding_profile->deadlines.total = std::chrono::milliseconds{60'000};
    embedding_profile->max_batch_texts = 32;
    OpenAiCompatibleEmbeddingProvider embedder(embedding_profile, transport);

    const auto &records_json = *dataset.value().find("records")->as_array();
    const auto &queries_json = *dataset.value().find("queries")->as_array();

    std::vector<std::string> statements;
    std::vector<std::string> record_ids;
    std::vector<MemoryKind> record_kinds;
    std::map<std::string, MemoryId> record_memory_ids;
    for (const auto &entry : records_json) {
        const auto *id = entry.find("id");
        const auto *kind = entry.find("kind");
        const auto *statement = entry.find("statement");
        record_ids.push_back(id->as_string()->c_str());
        statements.push_back(*statement->as_string());
        const auto parsed_kind = memory_kind_from(kind->as_string()->c_str());
        if (!parsed_kind) {
            std::cerr << 'unknown memory kind: ' << kind->as_string()->c_str() << '\n';
            return 5;
        }
        record_kinds.push_back(*parsed_kind);
    }
    std::vector<std::string> query_texts;
    std::vector<std::string> query_ids;
    for (const auto &entry : queries_json) {
        query_ids.push_back(*entry.find("id")->as_string());
        query_texts.push_back(*entry.find("text")->as_string());
    }

    std::cout << "embedding " << statements.size() << " statements + " << query_texts.size()
              << " queries...\n";
    auto statement_vectors = embedder.embed_batch(statements);
    if (!statement_vectors) {
        std::cerr << "statement embedding failed: " << statement_vectors.error().safe_message
                  << "\n";
        return 7;
    }
    auto query_vectors = embedder.embed_batch(query_texts);
    if (!query_vectors) {
        std::cerr << "query embedding failed\n";
        return 7;
    }

    // ---- stores: one per configuration family ----
    const auto make_options = [&](RetrievalFusion fusion, double dedup) {
        SqliteMemoryStoreOptions options;
        options.path = std::filesystem::temp_directory_path() /
                       ("retrieval-eval-" + std::to_string(static_cast<int>(fusion)) + "-" +
                        std::to_string(static_cast<int>(dedup * 100)) + ".db");
        options.fusion = fusion;
        options.near_dup_threshold = dedup;
        return options;
    };
    struct StoreConfig final {
        std::string name;
        RetrievalFusion fusion;
        double dedup;
        bool with_embedding;
    };
    const std::vector<StoreConfig> store_configs = {
        {"A_lexical_only", RetrievalFusion::WeightedSum, 0.0, false},
        {"B_weighted", RetrievalFusion::WeightedSum, 0.0, true},
        {"C_rrf", RetrievalFusion::ReciprocalRank, 0.0, true},
        {"D_rrf_dedup", RetrievalFusion::ReciprocalRank, 0.85, true},
    };
    const EmbedderId producer = embedder.embedder_id();

    struct Ranked final {
        std::string query_id;
        std::vector<std::string> ids;
    };
    std::map<std::string, std::vector<Ranked>> rankings;

    std::vector<std::unique_ptr<SqliteMemoryStore>> stores;
    std::size_t ordinal = 100;
    for (const auto &config : store_configs) {
        auto options = make_options(config.fusion, config.dedup);
        auto opened = SqliteMemoryStore::open(executor, options);
        if (!opened) {
            std::cerr << "store open failed\n";
            return 8;
        }
        stores.push_back(std::move(opened).value());
        auto &store = stores.back();

        // Seed with identical content (fresh store per config).
        for (std::size_t index = 0; index < record_ids.size(); ++index) {
            const MemoryKind kind = record_kinds[index];
            const MemoryScope scope = scope_for_kind(kind, SessionId{}, ordinal);
            MemoryRecord record;
            record.id = MemoryId{ordinal_id(ordinal)};
            record.scope = scope;
            record.kind = kind;
            record.statement = statements[index];
            record.recorded_at = std::chrono::system_clock::now() +
                                 std::chrono::seconds(static_cast<int64_t>(index));
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId{ordinal_id(ordinal + 5000)}};
            record.verification = floor_for_kind(kind);
            record.confidence = 0.8F;
            MemoryMutation mutation;
            mutation.id = MutationId{ordinal_id(ordinal + 9000)};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = scope;
            mutation.proposed = record;
            mutation.evidence = record.provenance;
            if (!store->apply(mutation).has_value()) {
                std::cerr << "seed failed for " << record_ids[index] << " ("
                          << store->apply(mutation).error().safe_message << ")\n";
                return 9;
            }
            if (config.with_embedding) {
                (void)store->index_embedding(record.id, producer, statement_vectors.value()[index]);
            }
            record_memory_ids[record_ids[index]] = record.id;
            ++ordinal;
        }

        const MemoryScope query_scopes[] = {
            scope_for_kind(MemoryKind::EnvironmentFact, SessionId{}, 0),
            scope_for_kind(MemoryKind::ApplicationFact, SessionId{}, 0),
            scope_for_kind(MemoryKind::RecoveryLesson, SessionId{}, 0)};
        for (std::size_t q = 0; q < query_ids.size(); ++q) {
            MemoryQuery query;
            for (const auto &scope : query_scopes) {
                query.scopes.push_back(scope);
            }
            query.text = query_texts[q];
            if (config.with_embedding) {
                query.query_embedding = MemoryEmbedding{query_vectors.value()[q], producer};
            }
            auto result = store->query(query);
            if (!result) {
                std::cerr << "query failed: " << result.error().safe_message << "\n";
                return 10;
            }
            Ranked ranked;
            ranked.query_id = query_ids[q];
            for (const auto &record : result.value().records) {
                for (const auto &dataset_id : record_ids) {
                    if (record_memory_ids[dataset_id] == record.id) {
                        ranked.ids.push_back(dataset_id);
                        break;
                    }
                }
            }
            rankings[config.name].push_back(ranked);
        }
    }

    // ---- config E: rerank over D's top-8 (shipped reranker, live) ----
    auto rerank_profile = std::make_shared<RerankerProfile>();
    rerank_profile->display_name = "benchmark";
    rerank_profile->endpoint_origin = "https://api.siliconflow.cn";
    rerank_profile->api_prefix = "/v1";
    rerank_profile->model_selector = "BAAI/bge-reranker-v2-m3";
    rerank_profile->credential = SecretRef{"SF_EMBEDDING_KEY"};
    rerank_profile->deadlines.total = std::chrono::milliseconds{60'000};
    OpenAiCompatibleReranker reranker(rerank_profile, transport, ContextRerankConfig{5});

    std::map<std::string, std::string> id_to_statement;
    for (std::size_t index = 0; index < record_ids.size(); ++index) {
        id_to_statement[record_ids[index]] = statements[index];
    }
    for (const auto &ranked : rankings["D_rrf_dedup"]) {
        std::vector<ContextCandidate> candidates;
        std::vector<std::string> candidate_ids;
        std::size_t take = std::min<std::size_t>(ranked.ids.size(), 8);
        for (std::size_t index = 0; index < take; ++index) {
            ContextCandidate candidate;
            candidate.asset_id = ContextAssetId{ordinal_id(7000 + index)};
            candidate.kind = ContextAssetKind::ConversationSegment;
            candidate.text = id_to_statement[ranked.ids[index]];
            candidate.score = 1.0 - static_cast<double>(index) * 0.01;
            candidates.push_back(candidate);
            candidate_ids.push_back(ranked.ids[index]);
        }
        ContextQuery query;
        query.session = SessionId{ordinal_id(99)};
        const std::string &text = query_texts[std::distance(
            query_ids.begin(), std::find(query_ids.begin(), query_ids.end(), ranked.query_id))];
        query.text = text;
        auto result = reranker.rerank(query, candidates);
        Ranked ranked_out;
        ranked_out.query_id = ranked.query_id;
        if (result.has_value()) {
            for (const auto &item : result.value()) {
                const std::size_t retrieval_rank = item.retrieval_rank;
                ranked_out.ids.push_back(candidate_ids.at(retrieval_rank));
            }
        }
        // On failure the host falls back to the retrieval order (contract).
        if (ranked_out.ids.empty()) {
            ranked_out.ids.assign(candidate_ids.begin(), candidate_ids.end());
        }
        rankings["E_rrf_dedup_rerank"].push_back(ranked_out);
    }

    // ---- dump rankings JSON ----
    JsonValue::Object root;
    for (const auto &[config_name, ranked_list] : rankings) {
        JsonValue::Object per_query;
        for (const auto &ranked : ranked_list) {
            JsonValue::Array ids;
            for (const auto &id : ranked.ids) {
                ids.emplace_back(JsonValue(id));
            }
            per_query.emplace_back(ranked.query_id, JsonValue(std::move(ids)));
        }
        root.emplace_back(config_name, JsonValue(std::move(per_query)));
    }
    std::ofstream output(argv[2]);
    output << to_json_string(JsonValue(std::move(root))) << "\n";

    for (const auto &store : stores) {
        (void)store->close();
    }
    (void)transport->shutdown();
    (void)executor.shutdown(true);
    std::cout << "rankings written to " << argv[2] << "\n";
    return 0;
}
