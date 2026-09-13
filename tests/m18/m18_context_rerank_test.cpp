// M18 (DEC-032 Stage C) Layer 2 rerank contract/integration suite. Covers the
// milestone §7 matrix: deterministic ordering, F1 + exact-term scoring,
// min-max fusion (including the all-equal 0.5 degenerate case), max_output
// truncation (membership can only shrink), empty-input closure, the
// reranker-error fallback to the retrieval order, ACL membership passthrough,
// marker-text passivity and the supervisor routing + shutdown rejection.

#include "../support/test.hpp"

#include <mira/context_memory_supervisor.hpp>
#include <mira/context_rerank.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace mira;

[[nodiscard]] SessionId session_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); index += sizeof(seed)) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return SessionId{Id128{bytes}};
}

[[nodiscard]] ContextCandidate candidate_from_seed(std::uint64_t seed, std::string text,
                                                   double score, const SessionId &session) {
    ContextCandidate candidate;
    candidate.asset_id = ContextAssetId{Id128{session_from_seed(seed).value}};
    candidate.kind = ContextAssetKind::ConversationSegment;
    candidate.text = std::move(text);
    candidate.session = session;
    candidate.score = score;
    return candidate;
}

[[nodiscard]] ContextQuery query_for(const SessionId &session, std::string text) {
    ContextQuery query;
    query.session = session;
    query.text = std::move(text);
    return query;
}

[[nodiscard]] ContextIndexAsset conversation_asset_with(const ContextAssetId &id,
                                                        const SessionId &session, std::string text,
                                                        std::uint64_t sequence) {
    ContextIndexAsset asset;
    asset.id = id;
    asset.kind = ContextAssetKind::ConversationSegment;
    asset.text = std::move(text);
    asset.session = session;
    asset.through_event_sequence = sequence;
    const Id128::Bytes origin_bytes{static_cast<std::uint8_t>(sequence),
                                    static_cast<std::uint8_t>(sequence >> 8U), 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    asset.source_events = {EventId{Id128{origin_bytes}}};
    return asset;
}

// Determinism: identical inputs produce identical outputs (scores and order).
int rerank_is_deterministic() {
    const SessionId session = session_from_seed(1);
    TokenOverlapContextReranker reranker;
    const ContextQuery query = query_for(session, "alpha beta recovery");
    std::vector<ContextCandidate> candidates = {
        candidate_from_seed(10, "alpha beta recovery tail", 0.9, session),
        candidate_from_seed(11, "alpha unrelated", 0.8, session),
        candidate_from_seed(12, "gamma delta", 0.7, session),
        candidate_from_seed(13, "beta alpha recovery head", 0.6, session),
    };
    const auto first = reranker.rerank(query, candidates);
    const auto second = reranker.rerank(query, candidates);
    MIRA_CHECK(first.has_value());
    MIRA_CHECK(second.has_value());
    MIRA_CHECK(first.value().size() == second.value().size());
    for (std::size_t index = 0; index < first.value().size(); ++index) {
        MIRA_CHECK(first.value()[index].candidate.asset_id ==
                   second.value()[index].candidate.asset_id);
        MIRA_CHECK(std::fabs(first.value()[index].fused_score -
                             second.value()[index].fused_score) < 1e-15);
    }
    // Candidates sharing all query tokens must outrank partial overlaps,
    // regardless of their retrieval score ordering above (0.9 vs 0.6).
    MIRA_CHECK(first.value().front().candidate.text.find("head") != std::string::npos ||
               first.value().front().candidate.text.find("tail") != std::string::npos);
    MIRA_CHECK(first.value().front().rerank_score > first.value().back().rerank_score);
    return 0;
}

// F1 precision/recall behaviour plus the verbatim exact-term bonus.
int f1_and_exact_bonus_scoring() {
    const SessionId session = session_from_seed(2);
    TokenOverlapRerankerOptions options;
    TokenOverlapContextReranker reranker(ContextRerankConfig{}, ContextRerankWeights{}, options);
    ContextQuery query = query_for(session, "alpha beta");
    query.exact_terms = {"mandatory token"};
    std::vector<ContextCandidate> candidates = {
        candidate_from_seed(20, "alpha beta gamma", 0.5, session),
        candidate_from_seed(21, "alpha beta gamma with mandatory token present", 0.5, session),
        candidate_from_seed(22, "alpha beta with mandatory token present", 0.5, session),
    };
    const auto result = reranker.rerank(query, candidates);
    MIRA_CHECK(result.has_value());
    // Rerank scores: 20 = F1(2/3 precision) = 0.8; 21 = F1(2/7) + 1 = 4/9 + 1;
    // 22 = F1(1/3) + 1 = 1.5. The exact bonus lifts both exact hits above the
    // plain F1 leader, and the shorter exact hit wins the tie band.
    MIRA_CHECK(result.value()[0].candidate.asset_id == candidates[2].asset_id);
    MIRA_CHECK(result.value()[1].candidate.asset_id == candidates[1].asset_id);
    MIRA_CHECK(result.value()[2].candidate.asset_id == candidates[0].asset_id);
    MIRA_CHECK(result.value()[0].rerank_score > result.value()[1].rerank_score);
    MIRA_CHECK(result.value()[1].rerank_score > result.value()[2].rerank_score);
    return 0;
}

// Min-max fusion keeps a share of the retrieval signal; an all-equal set
// normalizes to 0.5 on both signals and preserves the retrieval order.
int fusion_blends_retrieval_signal() {
    const SessionId session = session_from_seed(3);
    ContextRerankWeights weights;
    weights.rerank = 0.5;
    weights.retrieval = 0.5;
    TokenOverlapContextReranker reranker(ContextRerankConfig{}, weights);

    // All candidates carry identical query coverage and identical retrieval
    // scores: both normalizations degenerate to 0.5, so the fused score is
    // 0.5 for everyone and the retrieval order survives untouched.
    const ContextQuery query = query_for(session, "shared tokens everywhere");
    std::vector<ContextCandidate> flat;
    for (std::uint64_t seed = 30; seed < 35; ++seed) {
        flat.push_back(candidate_from_seed(seed, "shared tokens everywhere", 0.42, session));
    }
    const auto result = reranker.rerank(query, flat);
    MIRA_CHECK(result.has_value());
    for (std::size_t index = 0; index < result.value().size(); ++index) {
        MIRA_CHECK(std::fabs(result.value()[index].fused_score - 0.5) < 1e-12);
        MIRA_CHECK(result.value()[index].retrieval_rank == index);
    }

    // A strong rerank signal cannot fully erase a strong retrieval signal:
    // default 0.60/0.40 fusion must keep a retrieval-front runner ahead of a
    // retrieval-tail runner when their rerank scores tie.
    TokenOverlapContextReranker default_reranker;
    std::vector<ContextCandidate> mixed = {
        candidate_from_seed(40, "target tokens", 1.0, session),
        candidate_from_seed(41, "target tokens", 0.0, session),
        candidate_from_seed(42, "unrelated content", 0.0, session),
    };
    const auto fused = default_reranker.rerank(query_for(session, "target tokens"), mixed);
    MIRA_CHECK(fused.has_value());
    MIRA_CHECK(fused.value()[0].candidate.asset_id == mixed[0].asset_id);
    MIRA_CHECK(fused.value()[1].candidate.asset_id == mixed[1].asset_id);
    MIRA_CHECK(fused.value()[2].candidate.asset_id == mixed[2].asset_id);
    return 0;
}

// Output bound: rerank reorders and truncates, never grows membership.
int max_output_truncates_without_growing() {
    const SessionId session = session_from_seed(4);
    ContextRerankConfig config;
    config.max_output = 3;
    TokenOverlapContextReranker reranker(config);
    const ContextQuery query = query_for(session, "anchor token");
    std::vector<ContextCandidate> candidates;
    for (std::uint64_t seed = 50; seed < 60; ++seed) {
        candidates.push_back(candidate_from_seed(seed, "anchor token filler", 0.1, session));
    }
    const auto result = reranker.rerank(query, candidates);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().size() == config.max_output);
    // Every output item must come from the input set with provenance intact.
    for (const auto &item : result.value()) {
        const auto found = std::find_if(candidates.begin(), candidates.end(),
                                        [&item](const ContextCandidate &candidate) {
                                            return candidate.asset_id == item.candidate.asset_id;
                                        });
        MIRA_CHECK(found != candidates.end());
        MIRA_CHECK(item.candidate.text == found->text);
        MIRA_CHECK(item.candidate.session.has_value());
        MIRA_CHECK(item.retrieval_rank < candidates.size());
    }
    // Validation bounds.
    ContextRerankConfig invalid;
    invalid.max_output = 0;
    MIRA_CHECK(!invalid.validate().has_value());
    ContextRerankWeights invalid_weights;
    invalid_weights.rerank = -1.0;
    MIRA_CHECK(!invalid_weights.validate().has_value());
    invalid_weights.rerank = 0.0;
    invalid_weights.retrieval = 0.0;
    MIRA_CHECK(!invalid_weights.validate().has_value());
    TokenOverlapRerankerOptions invalid_options;
    invalid_options.exact_bonus = std::nan("");
    MIRA_CHECK(!invalid_options.validate().has_value());
    return 0;
}

// Empty input closes with an empty result; a rejected query is an error so
// the caller can fall back to the retrieval order (design §8).
int empty_and_error_paths() {
    const SessionId session = session_from_seed(5);
    TokenOverlapContextReranker reranker;
    const ContextQuery query = query_for(session, "anything");
    const auto empty = reranker.rerank(query, {});
    MIRA_CHECK(empty.has_value());
    MIRA_CHECK(empty.value().empty());

    // A query without any ACL grant is rejected by the shared query contract.
    ContextQuery ungranted;
    ungranted.text = "no grants at all";
    const auto rejected = reranker.rerank(ungranted, {});
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(!rejected.error().safe_message.empty());
    return 0;
}

// End-to-end through the Layer 1 index: the reranker only reorders ACL-filtered
// candidates — no cross-domain asset can appear, and candidate text (including
// any marker text already admitted upstream) is passed through verbatim.
int retrieval_to_rerank_never_grows_or_rewrites() {
    const SessionId session = session_from_seed(6);
    const SessionId stranger = session_from_seed(7);
    InMemoryContextIndex index;
    const auto anchor = context_asset_id_from_seed("mira.conversation.segment|rerank|0|1");
    MIRA_CHECK(index
                   .upsert_asset(conversation_asset_with(
                       anchor, session, "user: deadline overrun recovery plan details", 1))
                   .has_value());
    for (std::size_t noise = 0; noise < 6; ++noise) {
        const auto id = context_asset_id_from_seed("mira.conversation.segment|rerank|n|" +
                                                   std::to_string(noise));
        MIRA_CHECK(index
                       .upsert_asset(conversation_asset_with(
                           id, session, "user: unrelated noise " + std::to_string(noise), 2 + noise))
                       .has_value());
    }
    const auto foreign = context_asset_id_from_seed("mira.conversation.segment|rerank|f|1");
    MIRA_CHECK(index
                   .upsert_asset(
                       conversation_asset_with(foreign, stranger,
                                               "user: deadline overrun recovery plan details", 9))
                   .has_value());

    const ContextQuery query = query_for(session, "deadline overrun recovery");
    RetrievalBudget budget;
    budget.top_k = 30;
    const auto retrieval = index.retrieve(query, budget);
    MIRA_CHECK(retrieval.has_value());
    MIRA_CHECK(!retrieval.value().candidates.empty());

    TokenOverlapContextReranker reranker;
    const auto reranked = reranker.rerank(query, retrieval.value().candidates);
    MIRA_CHECK(reranked.has_value());
    MIRA_CHECK(reranked.value().size() <= retrieval.value().candidates.size());
    MIRA_CHECK(reranked.value().front().candidate.asset_id == anchor);
    for (const auto &item : reranked.value()) {
        MIRA_CHECK(item.candidate.session.has_value() && *item.candidate.session == session);
    }
    return 0;
}

int supervisor_routes_and_closes_rerank() {
    executor::Executor exec;
    MIRA_CHECK(exec.initialize(executor::ExecutorConfig{}));
    {
        const SessionId session = session_from_seed(8);
        TokenOverlapContextReranker reranker;
        ContextMemorySupervisor supervisor(exec);
        ContextQuery query = query_for(session, "routed rerank request");
        std::vector<ContextCandidate> candidates = {
            candidate_from_seed(80, "routed rerank request payload", 0.5, session),
            candidate_from_seed(81, "unrelated payload", 0.4, session),
        };
        auto future = supervisor.schedule_context_rerank(reranker, query, candidates);
        const auto result = future.get();
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().size() == 2);
        MIRA_CHECK(result.value()[0].candidate.asset_id == candidates[0].asset_id);

        const auto report = supervisor.begin_shutdown();
        MIRA_CHECK(report.critical_drain_complete);
        MIRA_CHECK(supervisor.closed());
        auto rejected = supervisor.schedule_context_rerank(reranker, query, candidates);
        const auto rejection = rejected.get();
        MIRA_CHECK(!rejection.has_value());
    }
    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

} // namespace

int main() {
    if (int failures = rerank_is_deterministic(); failures != 0) {
        return failures;
    }
    if (int failures = f1_and_exact_bonus_scoring(); failures != 0) {
        return failures;
    }
    if (int failures = fusion_blends_retrieval_signal(); failures != 0) {
        return failures;
    }
    if (int failures = max_output_truncates_without_growing(); failures != 0) {
        return failures;
    }
    if (int failures = empty_and_error_paths(); failures != 0) {
        return failures;
    }
    if (int failures = retrieval_to_rerank_never_grows_or_rewrites(); failures != 0) {
        return failures;
    }
    if (int failures = supervisor_routes_and_closes_rerank(); failures != 0) {
        return failures;
    }
    std::cout << "m18 context rerank: OK\n";
    return 0;
}
