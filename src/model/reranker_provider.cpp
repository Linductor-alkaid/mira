#include <mira/reranker_provider.hpp>

#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mira {

namespace {

constexpr std::size_t kMaxCandidates = 128;

[[nodiscard]] Error rerank_error(ContextDomainCode code, std::string message) {
    // Domain-consistent errors: callers fall back to the retrieval order per
    // the IContextReranker contract; the mapping mirrors the reference
    // reranker's failure surface (context_rerank.cpp).
    return make_context_error(code, std::move(message));
}

[[nodiscard]] TransportLimits limits_from_profile(const RerankerProfile &profile) {
    TransportLimits limits;
    limits.deadlines = profile.deadlines;
    limits.max_redirects = profile.max_redirects;
    const auto host = profile.endpoint_host();
    if (!host.empty()) {
        limits.allowed_hosts.push_back(host);
    }
    limits.allow_private_endpoints = profile.allow_private_endpoint;
    return limits;
}

[[nodiscard]] double normalize(double value, double minimum, double maximum) {
    // Same normalization contract as the reference reranker: an all-equal
    // candidate set normalizes to 0.5 so a signal-free set keeps its order.
    if (maximum == minimum) {
        return 0.5;
    }
    return (value - minimum) / (maximum - minimum);
}

// Fail-closed reader for the /v1/rerank response:
// { "results": [ { "index": int, "relevance_score": number } ... ] }.
// Candidates the provider did not score (top_n smaller than the batch)
// compete on the retrieval share with a zero rerank signal.
[[nodiscard]] Result<std::vector<double>> parse_rerank_response(const std::string &body,
                                                                std::size_t candidate_count) {
    const auto parsed = parse_json(body);
    if (!parsed) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "rerank response is not valid JSON");
    }
    const JsonValue &root = parsed.value();
    const auto *results = root.find("results");
    if (results == nullptr || !results->is_array() || results->as_array()->empty()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "rerank response carries no results");
    }
    std::vector<double> scores(candidate_count, 0.0);
    std::vector<char> seen(candidate_count, 0);
    for (const auto &entry : *results->as_array()) {
        if (!entry.is_object()) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "rerank result entry is not an object");
        }
        const auto *index = entry.find("index");
        const auto *score = entry.find("relevance_score");
        if (index == nullptr || !index->is_integer() || score == nullptr || !score->is_number()) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "rerank result entry is missing index or score");
        }
        const auto position = index->as_integer();
        const double relevance = score->as_number().value_or(0.0);
        if (!position || position.value() < 0 ||
            static_cast<std::size_t>(position.value()) >= candidate_count ||
            !std::isfinite(relevance)) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "rerank result index or score is out of range");
        }
        if (std::exchange(seen[static_cast<std::size_t>(position.value())], static_cast<char>(1)) !=
            0) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "rerank result index is duplicated");
        }
        scores[static_cast<std::size_t>(position.value())] = relevance;
    }
    return scores;
}

} // namespace

Result<void> RerankerProfile::validate() const {
    if (endpoint_origin.find("://") == std::string::npos || endpoint_host().empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "reranker profile endpoint origin must carry a valid host");
    }
    if (!api_prefix.empty() && api_prefix.front() != '/') {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "reranker profile api prefix must start with '/'");
    }
    if (model_selector.empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "reranker profile requires a model selector");
    }
    if (credential.name.empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "reranker profile requires a credential reference");
    }
    if (max_candidates == 0 || max_candidates > kMaxCandidates) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "reranker profile candidate cap must be within (0, 128]");
    }
    return Result<void>{};
}

std::string RerankerProfile::endpoint_host() const {
    const auto scheme = endpoint_origin.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const auto begin = scheme + 3;
    if (begin < endpoint_origin.size() && endpoint_origin[begin] == '[') {
        const auto bracket = endpoint_origin.find(']', begin + 1);
        return bracket == std::string::npos
                   ? std::string{}
                   : endpoint_origin.substr(begin + 1, bracket - begin - 1);
    }
    const auto end = endpoint_origin.find_first_of(":/", begin);
    return endpoint_origin.substr(begin,
                                  end == std::string::npos ? std::string::npos : end - begin);
}

std::string RerankerProfile::endpoint_url() const {
    std::string prefix = api_prefix;
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return endpoint_origin + prefix + "/rerank";
}

OpenAiCompatibleReranker::OpenAiCompatibleReranker(std::shared_ptr<const RerankerProfile> profile,
                                                   std::shared_ptr<IHttpTransport> transport,
                                                   ContextRerankConfig config,
                                                   ContextRerankWeights weights)
    : profile_(std::move(profile)), transport_(std::move(transport)), config_(config),
      weights_(weights) {}

OpenAiCompatibleReranker::~OpenAiCompatibleReranker() = default;

Result<std::vector<RankedContextItem>>
OpenAiCompatibleReranker::rerank(const ContextQuery &query,
                                 std::span<const ContextCandidate> candidates) {
    if (const Result<void> valid = config_.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank configuration rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = weights_.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "rerank weights rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = profile_->validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "reranker profile rejected: " + valid.error().safe_message);
    }
    if (const Result<void> valid = query.validate(); !valid.has_value()) {
        return rerank_error(ContextDomainCode::InvalidItem,
                            "query rejected: " + valid.error().safe_message);
    }
    if (candidates.size() > profile_->max_candidates) {
        return rerank_error(ContextDomainCode::InvalidLimits,
                            "candidate batch exceeds the rerank admission bound");
    }
    if (candidates.empty()) {
        return std::vector<RankedContextItem>{};
    }
    if (query.text.empty()) {
        return rerank_error(ContextDomainCode::InvalidItem, "rerank query text must not be empty");
    }

    JsonValue::Object root;
    root.emplace_back("model", JsonValue(profile_->model_selector));
    root.emplace_back("query", JsonValue(query.text));
    JsonValue::Array documents;
    documents.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        documents.emplace_back(JsonValue(candidate.text));
    }
    root.emplace_back("documents", JsonValue(std::move(documents)));
    root.emplace_back("top_n", JsonValue(static_cast<std::int64_t>(
                                   std::min(candidates.size(), config_.max_output))));

    HttpRequest http;
    http.method = "POST";
    http.keep_alive = false;
    http.url = profile_->endpoint_url();
    http.headers.emplace_back("Content-Type", "application/json");
    http.headers.emplace_back("Accept", "application/json");
    http.authorization = profile_->credential;
    http.credential_scheme = HttpCredentialScheme::Bearer;
    http.body = to_json_string(JsonValue(std::move(root)));

    TransportTrace trace;
    std::string body;
    const auto exchange = transport_->execute(
        http, limits_from_profile(*profile_), OperationContext{},
        [&body](std::string_view chunk) { body.append(chunk); }, trace);
    if (!exchange) {
        last_trace_ = trace;
        return exchange.error();
    }
    last_trace_ = trace;
    WireHttpResponse wire;
    wire.status = exchange.value().status;
    wire.headers = exchange.value().headers;
    wire.body = std::move(body);
    if (wire.status < 200 || wire.status >= 300) {
        return map_http_error_status(wire);
    }
    const auto scores = parse_rerank_response(wire.body, candidates.size());
    if (!scores) {
        return scores.error();
    }

    const std::vector<double> &ranked_scores = scores.value();
    const auto [rerank_min, rerank_max] =
        std::minmax_element(ranked_scores.begin(), ranked_scores.end());
    std::vector<double> retrieval_scores(candidates.size(), 0.0);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        retrieval_scores[index] = candidates[index].score;
    }
    const auto [retrieval_min, retrieval_max] =
        std::minmax_element(retrieval_scores.begin(), retrieval_scores.end());

    std::vector<RankedContextItem> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        RankedContextItem item;
        item.candidate = candidates[index];
        item.rerank_score = ranked_scores[index];
        item.retrieval_rank = index;
        item.fused_score =
            weights_.rerank * normalize(ranked_scores[index], *rerank_min, *rerank_max) +
            weights_.retrieval * normalize(retrieval_scores[index], *retrieval_min, *retrieval_max);
        ranked.push_back(std::move(item));
    }
    // Stable on ties: equal fused scores keep the retrieval order, exactly
    // like the reference reranker.
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
