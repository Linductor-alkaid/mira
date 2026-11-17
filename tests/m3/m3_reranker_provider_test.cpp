#include "support/m3_support.hpp"

#include "support/test.hpp"

#include <mira/context_retrieval.hpp>
#include <mira/json.hpp>
#include <mira/reranker_provider.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] std::shared_ptr<const RerankerProfile>
make_profile(const std::string &origin = "https://api.test",
             const std::string &model = "test-reranker", std::size_t max_candidates = 32) {
    auto profile = std::make_shared<RerankerProfile>();
    profile->display_name = "test-reranker";
    profile->endpoint_origin = origin;
    profile->api_prefix = "/v1";
    profile->model_selector = model;
    profile->credential = SecretRef{"test-credential"};
    profile->deadlines.connect = std::chrono::milliseconds{2'000};
    profile->deadlines.total = std::chrono::milliseconds{10'000};
    profile->max_candidates = max_candidates;
    return profile;
}

[[nodiscard]] std::shared_ptr<MockHttpTransport> wired_transport() {
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-reranker-test");
    return std::make_shared<MockHttpTransport>(secrets);
}

// Deterministic ordinal id (m17/m18 technique): fixed bytes derived from a
// counter, no random_device in the test path.
[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

[[nodiscard]] ContextCandidate make_candidate(std::uint64_t ordinal, std::string text,
                                              double score) {
    ContextCandidate candidate;
    candidate.asset_id = ContextAssetId{ordinal_id(ordinal)};
    candidate.kind = ContextAssetKind::ConversationSegment;
    candidate.text = std::move(text);
    candidate.score = score;
    candidate.exact_hit = false;
    candidate.lexical_score = 0.0;
    candidate.vector_hit = false;
    candidate.vector_similarity = 0.0;
    return candidate;
}

[[nodiscard]] ContextQuery make_query(std::string text) {
    ContextQuery query;
    query.session = SessionId{ordinal_id(999)};
    query.text = std::move(text);
    return query;
}

// Response body in the exact shape the real /v1/rerank endpoint returns
// (captured live: entries carry index + relevance_score; document is null).
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
    root.emplace_back("id", JsonValue(std::string("probe")));
    root.emplace_back("results", JsonValue(std::move(results)));
    return to_json_string(JsonValue(std::move(root)));
}

// The wire request reaches the scripted URL with model/query/documents/top_n
// and the credential resolved only at the transport boundary.
int request_shape_and_credential_resolution() {
    auto transport = wired_transport();
    transport->enqueue_json(200, rerank_body({{0, 0.9}}));
    OpenAiCompatibleReranker reranker(make_profile(), transport);

    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "用户偏好简洁回复", 0.5));
    auto ranked = reranker.rerank(make_query("回答别太啰嗦"), candidates);
    MIRA_CHECK(ranked.has_value() && ranked.value().size() == 1);

    const auto recorded = transport->recorded();
    MIRA_CHECK(recorded.size() == 1);
    MIRA_CHECK(recorded.front().url == "https://api.test/v1/rerank");
    MIRA_CHECK(recorded.front().authorization == "sk-reranker-test");
    MIRA_CHECK(!recorded.front().keep_alive);
    const auto body = parse_json(recorded.front().body);
    MIRA_CHECK(body.has_value());
    const auto *model = body.value().find("model");
    MIRA_CHECK(model != nullptr && model->is_string() &&
               model->as_string()->compare("test-reranker") == 0);
    const auto *query = body.value().find("query");
    MIRA_CHECK(query != nullptr && query->is_string() &&
               query->as_string()->compare("回答别太啰嗦") == 0);
    const auto *documents = body.value().find("documents");
    MIRA_CHECK(documents != nullptr && documents->is_array() && documents->as_array()->size() == 1);
    const auto *top_n = body.value().find("top_n");
    MIRA_CHECK(top_n != nullptr && top_n->is_integer() && top_n->as_integer().value() == 1);
    return 0;
}

// The rerank signal dominates the fusion at the documented weights, the
// retrieval rank travels back, and provenance stays verbatim.
int rerank_signal_reorders_with_fusion() {
    auto transport = wired_transport();
    // Retrieval order: a (score .9) > b (.5) > c (.1); the reranker inverts.
    transport->enqueue_json(200, rerank_body({{2, 0.95}, {1, 0.55}, {0, 0.10}}));
    OpenAiCompatibleReranker reranker(make_profile(), transport);

    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "alpha", 0.9));
    candidates.push_back(make_candidate(2, "beta", 0.5));
    candidates.push_back(make_candidate(3, "gamma", 0.1));
    auto ranked = reranker.rerank(make_query("anything"), candidates);
    MIRA_CHECK(ranked.has_value() && ranked.value().size() == 3);
    MIRA_CHECK(ranked.value()[0].candidate.text == "gamma");
    MIRA_CHECK(ranked.value()[0].retrieval_rank == 2);
    MIRA_CHECK(std::fabs(ranked.value()[0].rerank_score - 0.95) < 1e-9);
    MIRA_CHECK(ranked.value()[2].candidate.text == "alpha");
    MIRA_CHECK(ranked.value()[2].retrieval_rank == 0);
    // The fused score keeps a retrieval share: inversion is not total when
    // the retrieval scores are far apart.
    MIRA_CHECK(ranked.value()[0].fused_score > ranked.value()[1].fused_score);
    return 0;
}

// top_n truncation means unscored candidates compete on the retrieval share
// with a zero rerank signal; membership never grows.
int unscored_candidates_compete_on_retrieval_share() {
    auto transport = wired_transport();
    transport->enqueue_json(200, rerank_body({{0, 0.8}}));
    OpenAiCompatibleReranker reranker(make_profile(), transport, ContextRerankConfig{5});
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "scored", 0.2));
    candidates.push_back(make_candidate(2, "unscored", 0.9));
    auto ranked = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(ranked.has_value() && ranked.value().size() == 2);
    MIRA_CHECK(std::fabs(ranked.value()[0].rerank_score - 0.8) < 1e-9 ||
               std::fabs(ranked.value()[1].rerank_score - 0.8) < 1e-9);
    // The unscored candidate keeps a strong retrieval share; the fused order
    // must be deterministic and both members survive.
    bool have_scored = false;
    bool have_unscored = false;
    for (const auto &item : ranked.value()) {
        have_scored = have_scored || item.candidate.text == "scored";
        have_unscored = have_unscored || item.candidate.text == "unscored";
    }
    MIRA_CHECK(have_scored && have_unscored);
    return 0;
}

// All-equal rerank scores normalize to 0.5 and the retrieval order survives
// (stable sort on ties), mirroring the reference reranker.
int signal_free_set_keeps_retrieval_order() {
    auto transport = wired_transport();
    transport->enqueue_json(200, rerank_body({{0, 0.5}, {1, 0.5}, {2, 0.5}}));
    OpenAiCompatibleReranker reranker(make_profile(), transport);
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "first", 0.3));
    candidates.push_back(make_candidate(2, "second", 0.3));
    candidates.push_back(make_candidate(3, "third", 0.3));
    auto ranked = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(ranked.has_value());
    MIRA_CHECK(ranked.value()[0].candidate.text == "first");
    MIRA_CHECK(ranked.value()[1].candidate.text == "second");
    MIRA_CHECK(ranked.value()[2].candidate.text == "third");
    return 0;
}

// Shape violations and duplicate indices are protocol violations; the whole
// batch fails (no partial output).
int shape_violations_fail_closed() {
    auto transport = wired_transport();
    OpenAiCompatibleReranker reranker(make_profile(), transport);
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "a", 0.5));
    candidates.push_back(make_candidate(2, "b", 0.4));

    transport->enqueue_json(200, rerank_body({{0, 0.5}, {0, 0.6}}));
    auto duplicated = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!duplicated.has_value());
    MIRA_CHECK(duplicated.error().domain == "mira.model");
    MIRA_CHECK(duplicated.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));

    transport->enqueue_json(200, rerank_body({{5, 0.5}}));
    auto out_of_range = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!out_of_range.has_value());

    transport->enqueue_json(200, "{\"results\":[{\"index\":0}]}");
    auto missing_score = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!missing_score.has_value());

    transport->enqueue_json(200, "{\"results\":[]}");
    auto empty_results = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!empty_results.has_value());
    return 0;
}

// Non-2xx answers map through the shared model-plane error surface.
int http_error_status_maps() {
    auto transport = wired_transport();
    OpenAiCompatibleReranker reranker(make_profile(), transport);
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "a", 0.5));

    transport->enqueue_json(401, "{\"error\":\"bad key\"}");
    auto unauthorized = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!unauthorized.has_value());
    MIRA_CHECK(unauthorized.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::AuthenticationFailed));

    transport->enqueue_json(500, "{\"error\":\"upstream\"}");
    auto overloaded = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!overloaded.has_value());
    MIRA_CHECK(overloaded.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded));
    MIRA_CHECK(overloaded.error().retryable);
    return 0;
}

// The batch admission bound is fail-closed and spends no wire call; an empty
// batch closes with an empty result.
int batch_bounds_and_empty_input() {
    auto transport = wired_transport();
    OpenAiCompatibleReranker reranker(make_profile("https://api.test", "test-reranker", 2),
                                      transport);
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "a", 0.5));
    candidates.push_back(make_candidate(2, "b", 0.4));
    candidates.push_back(make_candidate(3, "c", 0.3));

    auto over = reranker.rerank(make_query("q"), candidates);
    MIRA_CHECK(!over.has_value());
    MIRA_CHECK(over.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::InvalidLimits));
    MIRA_CHECK(transport->recorded().empty());

    auto empty = reranker.rerank(make_query("q"), {});
    MIRA_CHECK(empty.has_value() && empty.value().empty());
    MIRA_CHECK(transport->recorded().empty());
    return 0;
}

// Profile/config/query validation is fail-closed without wire calls.
int profile_and_input_validation() {
    auto transport = wired_transport();
    OpenAiCompatibleReranker reranker(make_profile(), transport);

    auto bad = std::make_shared<RerankerProfile>();
    bad->endpoint_origin = "api.test";
    bad->model_selector = "m";
    bad->credential = SecretRef{"c"};
    MIRA_CHECK(!bad->validate().has_value());

    auto no_model = std::make_shared<RerankerProfile>();
    no_model->endpoint_origin = "https://api.test";
    no_model->credential = SecretRef{"c"};
    MIRA_CHECK(!no_model->validate().has_value());

    OpenAiCompatibleReranker invalid_profile(no_model, transport);
    std::vector<ContextCandidate> candidates;
    candidates.push_back(make_candidate(1, "a", 0.5));
    auto rejected = invalid_profile.rerank(make_query("q"), candidates);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(transport->recorded().empty());

    auto empty_query = reranker.rerank(make_query(""), candidates);
    MIRA_CHECK(!empty_query.has_value());
    MIRA_CHECK(transport->recorded().empty());
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += request_shape_and_credential_resolution();
    failures += rerank_signal_reorders_with_fusion();
    failures += unscored_candidates_compete_on_retrieval_share();
    failures += signal_free_set_keeps_retrieval_order();
    failures += shape_violations_fail_closed();
    failures += http_error_status_maps();
    failures += batch_bounds_and_empty_input();
    failures += profile_and_input_validation();
    return failures == 0 ? 0 : 1;
}
