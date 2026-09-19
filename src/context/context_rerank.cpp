#include <mira/context_rerank.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error rerank_error(ContextDomainCode code, std::string message) {
    return make_context_error(code, std::move(message));
}

// Deterministic ASCII lowercasing + tokenizer mirroring the Layer 1 lexical
// leg (src/context/context_retrieval.cpp): rerank and recall must agree on
// what a token is, otherwise the fusion compares incomparable signals.
[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char &character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

[[nodiscard]] std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : to_lower_ascii(text)) {
        const bool word = (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
        if (word) {
            current += character;
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    return to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

[[nodiscard]] bool is_finite(double value) noexcept { return std::isfinite(value); }

// Min-max normalization inside one candidate set; an all-equal set maps to
// 0.5 so a flat signal neither rewards nor punishes any candidate.
[[nodiscard]] double normalize(double value, double minimum, double maximum) noexcept {
    if (!(maximum > minimum)) {
        return 0.5;
    }
    return (value - minimum) / (maximum - minimum);
}

[[nodiscard]] double query_candidate_f1(const std::multiset<std::string> &query_tokens,
                                        const std::multiset<std::string> &candidate_tokens) {
    if (query_tokens.empty() || candidate_tokens.empty()) {
        return 0.0;
    }
    std::size_t intersection = 0;
    for (const auto &token : candidate_tokens) {
        if (query_tokens.find(token) != query_tokens.end()) {
            ++intersection;
        }
    }
    if (intersection == 0) {
        return 0.0;
    }
    const double precision =
        static_cast<double>(intersection) / static_cast<double>(candidate_tokens.size());
    const double recall =
        static_cast<double>(intersection) / static_cast<double>(query_tokens.size());
    return 2.0 * precision * recall / (precision + recall);
}

} // namespace

Result<void> ContextRerankConfig::validate() const {
    if (max_output == 0 || max_output > 4'096) {
        return rerank_error(ContextDomainCode::InvalidLimits, "rerank output bound out of range");
    }
    return {};
}

Result<void> ContextRerankWeights::validate() const {
    if (!is_finite(rerank) || !is_finite(retrieval) || rerank < 0.0 || retrieval < 0.0 ||
        rerank + retrieval <= 0.0) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank weights must be finite, non-negative and not all zero");
    }
    return {};
}

Result<void> TokenOverlapRerankerOptions::validate() const {
    if (!is_finite(exact_bonus) || exact_bonus < 0.0) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "exact bonus must be finite and non-negative");
    }
    return {};
}

TokenOverlapContextReranker::TokenOverlapContextReranker(ContextRerankConfig config,
                                                         ContextRerankWeights weights,
                                                         TokenOverlapRerankerOptions options)
    : config_(config), weights_(weights), options_(options) {}

TokenOverlapContextReranker::~TokenOverlapContextReranker() = default;

Result<std::vector<RankedContextItem>>
TokenOverlapContextReranker::rerank(const ContextQuery &query,
                                    std::span<const ContextCandidate> candidates) {
    if (const Result<void> valid = config_.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank configuration rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = weights_.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank weights rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = options_.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank options rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = query.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidItem,
                            "query rejected: " + valid.error().safe_message);
    }
    if (candidates.size() > config_.max_output * 64) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "candidate batch exceeds the rerank admission bound");
    }

    if (candidates.empty()) {
        return std::vector<RankedContextItem>{};
    }

    const auto query_token_list = tokenize(query.text);
    const std::multiset<std::string> query_tokens(query_token_list.begin(), query_token_list.end());
    std::vector<double> rerank_scores(candidates.size(), 0.0);
    std::vector<double> retrieval_scores(candidates.size(), 0.0);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const ContextCandidate &candidate = candidates[index];
        const std::multiset<std::string> candidate_tokens = [&] {
            const auto tokens = tokenize(candidate.text);
            return std::multiset<std::string>(tokens.begin(), tokens.end());
        }();
        double score = query_candidate_f1(query_tokens, candidate_tokens);
        for (const auto &term : query.exact_terms) {
            if (contains_case_insensitive(candidate.text, term)) {
                score += options_.exact_bonus;
            }
        }
        rerank_scores[index] = score;
        retrieval_scores[index] = candidate.score;
    }

    const auto [rerank_min, rerank_max] =
        std::minmax_element(rerank_scores.begin(), rerank_scores.end());
    const auto [retrieval_min, retrieval_max] =
        std::minmax_element(retrieval_scores.begin(), retrieval_scores.end());

    std::vector<RankedContextItem> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        RankedContextItem item;
        item.candidate = candidates[index];
        item.rerank_score = rerank_scores[index];
        item.retrieval_rank = index;
        item.fused_score =
            weights_.rerank * normalize(rerank_scores[index], *rerank_min, *rerank_max) +
            weights_.retrieval * normalize(retrieval_scores[index], *retrieval_min, *retrieval_max);
        ranked.push_back(std::move(item));
    }

    // Stable on ties: equal fused scores keep the retrieval order (the fusion
    // must be able to leave a signal-free set exactly as Layer 1 ranked it).
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedContextItem &lhs, const RankedContextItem &rhs) {
                         return lhs.fused_score > rhs.fused_score;
                     });
    if (ranked.size() > config_.max_output) {
        ranked.resize(config_.max_output);
    }
    return ranked;
}

} // namespace mira
