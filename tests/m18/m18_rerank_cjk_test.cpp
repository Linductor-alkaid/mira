// TokenOverlapContextReranker on text written without spaces (DEC-147). The
// overlap runs over character pairs, so a candidate sharing a word with the
// query outranks one sharing only scattered characters.

#include "../support/test.hpp"

#include <mira/context_rerank.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace mira;

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    bytes[0] = static_cast<std::uint8_t>(ordinal);
    return Id128{bytes};
}

[[nodiscard]] SessionId session_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    std::memcpy(bytes.data(), &seed, sizeof(seed));
    return SessionId{Id128{bytes}};
}

[[nodiscard]] ContextCandidate candidate(std::uint64_t ordinal, std::string text, double score) {
    ContextCandidate value;
    value.asset_id = ContextAssetId{ordinal_id(ordinal)};
    value.kind = ContextAssetKind::ConversationSegment;
    value.text = std::move(text);
    value.score = score;
    return value;
}

int chinese_candidates_rank_by_shared_pairs() {
    ContextQuery query;
    query.session = session_from_seed(1);
    query.text = "打印机卡纸";
    std::vector<ContextCandidate> candidates = {
        candidate(10, "打印纸用完了", 0.9),         // shares one pair
        candidate(11, "打印机卡纸时先开前盖", 0.5), // shares every pair
        candidate(12, "纸盒在打印机旁边", 0.7),     // shares two pairs
        candidate(13, "印表机", 0.1),               // shares characters, no pair
    };
    TokenOverlapContextReranker reranker;
    const auto result = reranker.rerank(query, candidates);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().size() == 4);
    MIRA_CHECK(result.value()[0].candidate.asset_id == candidates[1].asset_id);
    MIRA_CHECK(result.value()[1].candidate.asset_id == candidates[2].asset_id);
    MIRA_CHECK(result.value()[2].candidate.asset_id == candidates[0].asset_id);
    MIRA_CHECK(result.value()[3].candidate.asset_id == candidates[3].asset_id);
    MIRA_CHECK(result.value()[0].rerank_score > result.value()[1].rerank_score);
    MIRA_CHECK(result.value()[1].rerank_score > result.value()[2].rerank_score);
    // Shared characters without a shared pair score nothing.
    MIRA_CHECK(result.value()[3].rerank_score == 0.0);
    // The retrieval scores ride along untouched.
    for (const auto &ranked : result.value()) {
        MIRA_CHECK(ranked.candidate.text == candidates[ranked.retrieval_rank].text);
    }
    return 0;
}

} // namespace

int main() { return chinese_candidates_rank_by_shared_pairs(); }
