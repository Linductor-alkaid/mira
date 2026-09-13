#pragma once

#include <mira/context_retrieval.hpp>

#include <memory>
#include <span>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Intelligence Layer 2: optional rerank (DEC-032 §2/§6, M18)
//
// Layer 1 recalls broadly; Layer 2 optionally reorders a retrieval Top-K
// (30~100) down to the 5~20 items a model request actually wants (design
// §5.3). A reranker is a pure consumer of the retrieval result: it reorders
// and truncates, never grows membership, never touches storage, and never
// bypasses Layer 0 admission. Reranker absence is a legal configuration —
// callers fall back to the retrieval order Top-K (design §8), so every
// failure here degrades quality, never the request.
//
// Core ships no model reranker (DEC-032 §5): real cross-encoders arrive via
// the supply-chain review channel as host-injected IContextReranker
// implementations. The reference implementation below is a deterministic
// token-overlap scorer, which is what the Stage C comparison experiment uses
// as the frozen pipeline-behaviour C column.
// ---------------------------------------------------------------------------

// Output bound of one rerank call (RULE-08). The default covers the design's
// 5~20 convergence band; the comparison experiment pins 10 to stay comparable
// with the retrieval-v1 Recall@10 cutoff.
struct ContextRerankConfig final {
    std::size_t max_output = 20;

    [[nodiscard]] Result<void> validate() const;
};

// Fusion of the reranker signal with the existing ranking semantics: the
// final score keeps a share of the Layer 1 fused score instead of replacing
// it (design §5.3 "融合，不整替"). Both signals are min-max normalized inside
// one candidate set; an all-equal set normalizes to 0.5. Documented defaults,
// not a frozen contract (mirrors `RetrievalWeights`).
struct ContextRerankWeights final {
    double rerank = 0.60;
    double retrieval = 0.40;

    [[nodiscard]] Result<void> validate() const;
};

// One reranked candidate: the untouched Layer 1 candidate plus the reranker
// evidence. Provenance travels inside `candidate` verbatim; the reranker adds
// ordering signals only.
struct RankedContextItem final {
    ContextCandidate candidate;
    // Raw reference-reranker score before fusion (F1 + exact bonus for the
    // reference implementation; model score for real rerankers).
    double rerank_score = 0.0;
    double fused_score = 0.0;
    // 0-based position of the candidate in the input span, so callers can
    // measure how the reranker moved the retrieval order.
    std::size_t retrieval_rank = 0;
};

class IContextReranker {
  public:
    virtual ~IContextReranker() = default;
    // Reorders `candidates` (already ACL-filtered by Layer 1) and truncates
    // to the configured output bound. An empty input closes with an empty
    // result. Errors mean "rerank unavailable" — callers fall back to the
    // retrieval order; no partial output is returned.
    [[nodiscard]] virtual Result<std::vector<RankedContextItem>>
    rerank(const ContextQuery &query, std::span<const ContextCandidate> candidates) = 0;
};

struct TokenOverlapRerankerOptions final {
    // Added to the rerank score per verbatim exact-term hit.
    double exact_bonus = 1.0;

    [[nodiscard]] Result<void> validate() const;
};

// Deterministic reference reranker (no model): query/candidate texts are
// tokenized exactly like the Layer 1 lexical leg (lowercase [a-z0-9_] runs),
// the rerank score is the query-token F1 coverage of the candidate plus one
// `exact_bonus` per verbatim (case-insensitive) `exact_terms` hit. With the
// token-hash embedding supplier this is a hash-noise-free cross score — the
// deterministic counterpart of a cross-encoder for pipeline experiments.
// Internally stateless; safe to share across Executor-supervised calls.
class TokenOverlapContextReranker final : public IContextReranker {
  public:
    explicit TokenOverlapContextReranker(
        ContextRerankConfig config = ContextRerankConfig{},
        ContextRerankWeights weights = ContextRerankWeights{},
        TokenOverlapRerankerOptions options = TokenOverlapRerankerOptions{});
    ~TokenOverlapContextReranker() override;

    TokenOverlapContextReranker(const TokenOverlapContextReranker &) = delete;
    TokenOverlapContextReranker &operator=(const TokenOverlapContextReranker &) = delete;

    [[nodiscard]] Result<std::vector<RankedContextItem>>
    rerank(const ContextQuery &query, std::span<const ContextCandidate> candidates) override;

  private:
    ContextRerankConfig config_;
    ContextRerankWeights weights_;
    TokenOverlapRerankerOptions options_;
};

} // namespace mira
