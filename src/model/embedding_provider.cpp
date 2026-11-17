#include <mira/embedding_provider.hpp>

#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mira {

namespace {

constexpr std::size_t kMaxEmbeddingDims = 4'096;
constexpr std::size_t kMaxBatchTexts = 256;

[[nodiscard]] TransportLimits limits_from_profile(const EmbeddingProfile &profile) {
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

// Fails-closed shape reader for the shared /v1/embeddings response:
// { "data": [ { "index"?: int, "embedding": [number...] } ... ] }.
// Anything else is a ProtocolViolation, never a best-effort guess.
[[nodiscard]] Result<std::vector<std::vector<float>>>
parse_embeddings_response(const std::string &body, std::size_t expected_count,
                          std::uint32_t expected_dims) {
    const auto parsed = parse_json(body);
    if (!parsed) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "embedding response is not valid JSON");
    }
    const JsonValue &root = parsed.value();
    const auto *data = root.find("data");
    if (data == nullptr || !data->is_array() || data->as_array()->size() != expected_count) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "embedding response data does not match the request size");
    }
    const auto &entries = *data->as_array();

    // Ordering: providers that return an "index" field must return exactly
    // 0..n-1; providers that omit it are answered positionally. A mixture is
    // a protocol violation.
    std::vector<std::size_t> order(entries.size(), 0);
    bool any_index = false;
    bool all_index = true;
    for (std::size_t position = 0; position < entries.size(); ++position) {
        const auto *index = entries[position].find("index");
        if (index != nullptr && index->is_integer()) {
            any_index = true;
            const auto value = index->as_integer();
            if (!value || value.value() < 0 ||
                static_cast<std::size_t>(value.value()) >= entries.size()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "embedding response index is out of range");
            }
            order[position] = static_cast<std::size_t>(value.value());
        } else {
            all_index = false;
        }
    }
    if (any_index && !all_index) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "embedding response index is partially present");
    }
    if (any_index) {
        std::vector<char> seen(entries.size(), 0);
        for (const auto slot : order) {
            if (std::exchange(seen[slot], static_cast<char>(1)) != 0) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "embedding response index is duplicated");
            }
        }
    } else {
        for (std::size_t position = 0; position < order.size(); ++position) {
            order[position] = position;
        }
    }

    std::vector<std::vector<float>> vectors(entries.size());
    for (std::size_t position = 0; position < entries.size(); ++position) {
        const auto *embedding = entries[position].find("embedding");
        if (embedding == nullptr || !embedding->is_array() || embedding->as_array()->empty()) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "embedding response entry carries no embedding array");
        }
        const auto &values = *embedding->as_array();
        if (values.size() > kMaxEmbeddingDims) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "embedding response exceeds the dimension bound");
        }
        if (expected_dims != 0 && values.size() != expected_dims) {
            return make_model_error(ModelDomainCode::ProtocolViolation,
                                    "embedding response dimension does not match the profile");
        }
        std::vector<float> decoded;
        decoded.reserve(values.size());
        for (const auto &component : values) {
            const auto number = component.as_number();
            if (!number || !std::isfinite(number.value())) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "embedding response carries a non-finite component");
            }
            decoded.push_back(static_cast<float>(number.value()));
        }
        vectors[order[position]] = std::move(decoded);
    }
    return vectors;
}

} // namespace

Result<void> EmbeddingProfile::validate() const {
    if (endpoint_origin.find("://") == std::string::npos || endpoint_host().empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile endpoint origin must carry a valid host");
    }
    if (!api_prefix.empty() && api_prefix.front() != '/') {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile api prefix must start with '/'");
    }
    if (model_selector.empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile requires a model selector");
    }
    if (credential.name.empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile requires a credential reference");
    }
    if (expected_dims > kMaxEmbeddingDims) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile expected dims exceed the bound");
    }
    if (max_batch_texts == 0 || max_batch_texts > kMaxBatchTexts) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding profile batch cap must be within (0, 256]");
    }
    return Result<void>{};
}

std::string EmbeddingProfile::endpoint_host() const {
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

std::string EmbeddingProfile::canonical_descriptor() const {
    // The identity seed covers every configuration dimension that changes
    // vector geometry. Host, model and dims are the essentials; the scheme
    // family pins the wire shape so a future dialect cannot share identity.
    return "mira.embedding.openai-compatible/v1|" + endpoint_host() + "|" + model_selector + "|" +
           std::to_string(expected_dims);
}

std::string EmbeddingProfile::endpoint_url() const {
    std::string prefix = api_prefix;
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    return endpoint_origin + prefix + "/embeddings";
}

EmbedderId embedding_identity_from_descriptor(std::string_view descriptor) {
    const auto digest = digest_string(descriptor);
    Id128::Bytes bytes{};
    static_assert(Id128::Bytes{}.size() <= digest.bytes.size());
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return EmbedderId{Id128{bytes}};
}

OpenAiCompatibleEmbeddingProvider::OpenAiCompatibleEmbeddingProvider(
    std::shared_ptr<const EmbeddingProfile> profile, std::shared_ptr<IHttpTransport> transport)
    : profile_(std::move(profile)), transport_(std::move(transport)) {
    identity_ = embedding_identity_from_descriptor(profile_->canonical_descriptor());
}

OpenAiCompatibleEmbeddingProvider::~OpenAiCompatibleEmbeddingProvider() = default;

EmbedderId OpenAiCompatibleEmbeddingProvider::embedder_id() const { return identity_; }

Result<std::vector<std::vector<float>>>
OpenAiCompatibleEmbeddingProvider::embed_request(const std::vector<std::string> &texts) {
    JsonValue::Object root;
    root.emplace_back("model", JsonValue(profile_->model_selector));
    JsonValue::Array input;
    input.reserve(texts.size());
    for (const auto &text : texts) {
        input.emplace_back(JsonValue(text));
    }
    root.emplace_back("input", JsonValue(std::move(input)));

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
    return parse_embeddings_response(wire.body, texts.size(), profile_->expected_dims);
}

Result<std::vector<float>> OpenAiCompatibleEmbeddingProvider::embed(std::string_view text) {
    if (text.empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "embedding text must not be empty");
    }
    auto vectors = embed_request({std::string(text)});
    if (!vectors) {
        return vectors.error();
    }
    auto values = std::move(vectors).value();
    return std::move(values.front());
}

Result<std::vector<std::vector<float>>>
OpenAiCompatibleEmbeddingProvider::embed_batch(std::span<const std::string> texts) {
    if (texts.empty()) {
        return std::vector<std::vector<float>>{};
    }
    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    const auto cap = std::max<std::size_t>(1, profile_->max_batch_texts);
    std::vector<std::string> batch;
    batch.reserve(std::min(texts.size(), cap));
    for (const auto &text : texts) {
        if (text.empty()) {
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "embedding text must not be empty");
        }
        batch.emplace_back(text);
        if (batch.size() == cap) {
            auto part = embed_request(batch);
            if (!part) {
                return part.error();
            }
            auto part_vectors = std::move(part).value();
            vectors.insert(vectors.end(), std::make_move_iterator(part_vectors.begin()),
                           std::make_move_iterator(part_vectors.end()));
            batch.clear();
        }
    }
    if (!batch.empty()) {
        auto part = embed_request(batch);
        if (!part) {
            return part.error();
        }
        auto part_vectors = std::move(part).value();
        vectors.insert(vectors.end(), std::make_move_iterator(part_vectors.begin()),
                       std::make_move_iterator(part_vectors.end()));
    }
    return vectors;
}

} // namespace mira
