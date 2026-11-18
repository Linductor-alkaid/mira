#pragma once

#include <mira/context_memory_supervisor.hpp>
#include <mira/embedding_provider.hpp>
#include <mira/memory_agent.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/reranker_provider.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// One-call assembly of the DEC-111/112 hybrid retrieval stack (DEC-113).
// The capability pieces already exist and are host-assembled piece by piece
// (store options, recall embedder, projection indexer, Layer 2 reranker);
// this header is the default wiring surface: one config struct, one call per
// plane, with all validation strictly before the irreversible injection.
// It adds no protocol, scoring or concurrency path of its own — every call
// forwards to an existing contract.
// ---------------------------------------------------------------------------

// Host data for the two optional remote legs plus the store-side defaults.
// Credentials travel as a SecretRef name only; the plaintext is resolved at
// the transport boundary and never enters this struct. Each leg is enabled
// only when its endpoint origin and model selector are set together; with
// both legs unset the config is rejected (there is nothing to wire).
//
// The defaults encode the DEC-113 benchmark outcome (E configuration of
// memory-hybrid-retrieval-v1): rank fusion over the store legs and
// near-duplicate suppression calibrated for bge-m3 (~0.86 measured for
// paraphrase near-dup pairs). A different embedder must recalibrate
// `near_dup_threshold`; 0.0 disables the pass.
struct HybridRetrievalConfig final {
    // ---- Semantic leg (optional; DEC-111 producer) ----
    // "https://api.example.com" — scheme plus host (plus optional port).
    std::string embedding_endpoint_origin;
    std::string embedding_api_prefix = "/v1";
    // Wire model name (for example "BAAI/bge-m3").
    std::string embedding_model_selector;
    // Advisory dimension pin (0 accepts any dimension within the contract
    // bound); forwarded to the embedding profile.
    std::uint32_t embedding_expected_dims = 0;

    // ---- Rerank leg (optional; DEC-112 cross-encoder adapter) ----
    std::string rerank_endpoint_origin;
    std::string rerank_api_prefix = "/v1";
    // Wire model name (for example "BAAI/bge-reranker-v2-m3").
    std::string rerank_model_selector;
    // Layer 2 output cap forwarded to the reranker config (benchmark head
    // size: rerank over the retrieval top-K, keep the top-N).
    std::size_t rerank_max_output = 8;

    // ---- Shared leg configuration ----
    // SecretRef name resolved at the transport boundary; never a secret.
    std::string credential_secret_name;
    // Total HTTP deadline for both providers; the other transport deadlines
    // keep their documented defaults.
    std::chrono::milliseconds deadline{30'000};

    // ---- Store-side defaults (applied by hybrid_retrieval_store_options) --
    RetrievalFusion fusion = RetrievalFusion::ReciprocalRank;
    // Near-duplicate suppression over the ranked head; 0.0 disables.
    double near_dup_threshold = 0.85;

    [[nodiscard]] bool embedding_enabled() const noexcept {
        return !embedding_endpoint_origin.empty() && !embedding_model_selector.empty();
    }
    [[nodiscard]] bool rerank_enabled() const noexcept {
        return !rerank_endpoint_origin.empty() && !rerank_model_selector.empty();
    }
    [[nodiscard]] Result<void> validate() const;
};

// The owned halves of the wired stack. The providers and the indexer are
// owned here; the store, the agent and the supervisor are borrowed per call
// and must outlive every use of the stack (the indexer holds store and
// provider references). Legs that were not configured (or had no target)
// stay null, and `embedder_id` is the DEC-111 producer identity of the
// wired embedding profile (nil when the semantic leg is absent).
struct HybridRetrievalStack final {
    std::shared_ptr<OpenAiCompatibleEmbeddingProvider> embedding_provider;
    std::shared_ptr<OpenAiCompatibleReranker> reranker;
    std::unique_ptr<MemoryEmbeddingIndexer> indexer;
    EmbedderId embedder_id{};

    // Convenience routing: one bounded vector-projection repair pass through
    // the supervisor's Deferrable path. Resolves with InvalidArgument when
    // the semantic leg was not wired (or no index was supplied).
    [[nodiscard]] std::future<Result<MemoryEmbeddingRepairReport>> schedule_embedding_repair(
        ContextMemorySupervisor &supervisor, IMemory &memory, MemoryScope scope,
        MemoryEmbeddingRepairOptions options = MemoryEmbeddingRepairOptions{});

    // Convenience routing: one Layer 2 rerank through the supervisor's
    // Interactive path (failure degrades to the retrieval order at the call
    // site). Resolves with InvalidArgument when the rerank leg was not wired.
    [[nodiscard]] std::future<Result<std::vector<RankedContextItem>>>
    schedule_context_rerank(ContextMemorySupervisor &supervisor, ContextQuery query,
                            std::vector<ContextCandidate> candidates);
};

// Store-side half of the wiring, applied BEFORE SqliteMemoryStore::open():
// writes the configured fusion mode and near-dup threshold over `base`.
// Pure projection — the values are re-validated by the store options at
// open(), so a bad threshold fails at open rather than silently ranking.
[[nodiscard]] SqliteMemoryStoreOptions
hybrid_retrieval_store_options(const HybridRetrievalConfig &config,
                               SqliteMemoryStoreOptions base = {});

// Host-side half of the wiring, applied AFTER the store (and optionally the
// memory agent) exist. Builds and validates both profiles first; only when
// every configured leg has validated does it touch anything irreversible
// (`MemoryAgentService::set_recall_embedder` cannot be unset), so a failed
// call leaves the agent exactly as it was. The transport is host-assembled
// (Core never owns sockets); `index` receives the projection indexer and
// `agent` receives the recall embedder — either pointer may be null to skip
// that plane. The store backing `index` must outlive the returned stack.
[[nodiscard]] Result<HybridRetrievalStack>
wire_hybrid_retrieval(const HybridRetrievalConfig &config,
                      std::shared_ptr<IHttpTransport> transport, IMemoryEmbeddingIndex *index,
                      MemoryAgentService *agent);

} // namespace mira
