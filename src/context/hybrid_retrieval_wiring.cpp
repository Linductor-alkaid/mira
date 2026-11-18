// One-call assembly of the DEC-111/112 hybrid retrieval stack (DEC-113).
// Pure composition of existing contracts: profile fields in, provider
// construction, one irreversible agent injection — everything validated
// before that injection so a rejected call leaves the host untouched.
#include <mira/hybrid_retrieval_wiring.hpp>

#include <cmath>
#include <utility>

namespace mira {
namespace {

Error wiring_error(ErrorCode code, std::string message) {
    Error e;
    e.code = code;
    e.domain = "mira.hybrid_retrieval_wiring";
    e.safe_message = std::move(message);
    return e;
}

} // namespace

Result<void> HybridRetrievalConfig::validate() const {
    // Each leg is either fully configured or fully absent.
    const bool embedding_origin = !embedding_endpoint_origin.empty();
    const bool embedding_model = !embedding_model_selector.empty();
    if (embedding_origin != embedding_model) {
        return wiring_error(ErrorCode::InvalidArgument,
                            "embedding leg needs its endpoint origin and model selector set "
                            "together");
    }
    const bool rerank_origin = !rerank_endpoint_origin.empty();
    const bool rerank_model = !rerank_model_selector.empty();
    if (rerank_origin != rerank_model) {
        return wiring_error(ErrorCode::InvalidArgument,
                            "rerank leg needs its endpoint origin and model selector set "
                            "together");
    }
    if (!embedding_enabled() && !rerank_enabled()) {
        return wiring_error(ErrorCode::InvalidArgument, "no retrieval leg is configured");
    }
    if (credential_secret_name.empty()) {
        return wiring_error(ErrorCode::InvalidArgument,
                            "a configured leg needs a credential secret name");
    }
    if (deadline <= std::chrono::milliseconds{0}) {
        return wiring_error(ErrorCode::InvalidArgument, "deadline must be positive");
    }
    if (rerank_enabled() && rerank_max_output == 0) {
        return wiring_error(ErrorCode::InvalidArgument, "rerank_max_output must be positive");
    }
    if (!(near_dup_threshold >= 0.0 && near_dup_threshold <= 1.0) ||
        std::isnan(near_dup_threshold)) {
        return wiring_error(ErrorCode::InvalidArgument, "near_dup_threshold must be within [0,1]");
    }
    return {};
}

SqliteMemoryStoreOptions hybrid_retrieval_store_options(const HybridRetrievalConfig &config,
                                                        SqliteMemoryStoreOptions base) {
    base.fusion = config.fusion;
    base.near_dup_threshold = config.near_dup_threshold;
    return base;
}

Result<HybridRetrievalStack> wire_hybrid_retrieval(const HybridRetrievalConfig &config,
                                                   std::shared_ptr<IHttpTransport> transport,
                                                   IMemoryEmbeddingIndex *index,
                                                   MemoryAgentService *agent) {
    if (const Result<void> valid = config.validate(); !valid.has_value()) {
        return valid.error();
    }
    if (transport == nullptr) {
        return wiring_error(ErrorCode::InvalidArgument,
                            "a configured retrieval stack needs an HTTP transport");
    }

    TransportDeadlines deadlines;
    deadlines.total = config.deadline;

    // Build and validate every configured leg before touching the agent:
    // set_recall_embedder has no unset transition, so a half-wired failure
    // here would be permanent.
    std::shared_ptr<OpenAiCompatibleEmbeddingProvider> embedder;
    if (config.embedding_enabled()) {
        auto profile = std::make_shared<EmbeddingProfile>();
        profile->display_name = "hybrid-embedding";
        profile->endpoint_origin = config.embedding_endpoint_origin;
        profile->api_prefix = config.embedding_api_prefix;
        profile->model_selector = config.embedding_model_selector;
        profile->expected_dims = config.embedding_expected_dims;
        profile->credential = SecretRef{config.credential_secret_name};
        profile->deadlines = deadlines;
        if (const Result<void> valid = profile->validate(); !valid.has_value()) {
            return wiring_error(ErrorCode::InvalidArgument,
                                "embedding profile rejected: " + valid.error().safe_message);
        }
        embedder =
            std::make_shared<OpenAiCompatibleEmbeddingProvider>(std::move(profile), transport);
    }

    std::shared_ptr<OpenAiCompatibleReranker> reranker;
    if (config.rerank_enabled()) {
        auto profile = std::make_shared<RerankerProfile>();
        profile->display_name = "hybrid-rerank";
        profile->endpoint_origin = config.rerank_endpoint_origin;
        profile->api_prefix = config.rerank_api_prefix;
        profile->model_selector = config.rerank_model_selector;
        profile->credential = SecretRef{config.credential_secret_name};
        profile->deadlines = deadlines;
        ContextRerankConfig rerank_config;
        rerank_config.max_output = config.rerank_max_output;
        if (const Result<void> valid = rerank_config.validate(); !valid.has_value()) {
            return wiring_error(ErrorCode::InvalidArgument,
                                "rerank config rejected: " + valid.error().safe_message);
        }
        if (const Result<void> valid = profile->validate(); !valid.has_value()) {
            return wiring_error(ErrorCode::InvalidArgument,
                                "reranker profile rejected: " + valid.error().safe_message);
        }
        reranker = std::make_shared<OpenAiCompatibleReranker>(std::move(profile), transport,
                                                              rerank_config);
    }

    HybridRetrievalStack stack;
    if (embedder != nullptr) {
        stack.embedder_id = embedder->embedder_id();
        // The irreversible plane comes last: validation is complete.
        if (agent != nullptr) {
            if (const Result<void> wired = agent->set_recall_embedder(embedder);
                !wired.has_value()) {
                return wired.error();
            }
        }
        if (index != nullptr) {
            stack.indexer = std::make_unique<MemoryEmbeddingIndexer>(*index, *embedder);
        }
        stack.embedding_provider = std::move(embedder);
    }
    stack.reranker = std::move(reranker);
    return stack;
}

std::future<Result<MemoryEmbeddingRepairReport>>
HybridRetrievalStack::schedule_embedding_repair(ContextMemorySupervisor &supervisor,
                                                IMemory &memory, MemoryScope scope,
                                                MemoryEmbeddingRepairOptions options) {
    if (indexer == nullptr) {
        std::promise<Result<MemoryEmbeddingRepairReport>> promise;
        promise.set_value(wiring_error(ErrorCode::InvalidArgument,
                                       "the semantic leg is not wired for repair routing"));
        return promise.get_future();
    }
    return supervisor.schedule_embedding_repair(*indexer, memory, std::move(scope), options);
}

std::future<Result<std::vector<RankedContextItem>>>
HybridRetrievalStack::schedule_context_rerank(ContextMemorySupervisor &supervisor,
                                              ContextQuery query,
                                              std::vector<ContextCandidate> candidates) {
    if (reranker == nullptr) {
        std::promise<Result<std::vector<RankedContextItem>>> promise;
        promise.set_value(wiring_error(ErrorCode::InvalidArgument,
                                       "the rerank leg is not wired for rerank routing"));
        return promise.get_future();
    }
    return supervisor.schedule_context_rerank(*reranker, std::move(query), std::move(candidates));
}

} // namespace mira
