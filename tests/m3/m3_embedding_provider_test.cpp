#include "support/m3_support.hpp"

#include "support/test.hpp"

#include <mira/embedding_provider.hpp>
#include <mira/json.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// One embedding profile aimed at the mock transport.
[[nodiscard]] std::shared_ptr<const EmbeddingProfile>
make_embedding_profile(const std::string &origin = "https://api.test",
                       const std::string &model = "test-embedder", std::uint32_t expected_dims = 0,
                       std::size_t max_batch = 16) {
    auto profile = std::make_shared<EmbeddingProfile>();
    profile->display_name = "test-embedder";
    profile->endpoint_origin = origin;
    profile->api_prefix = "/v1";
    profile->model_selector = model;
    profile->expected_dims = expected_dims;
    profile->credential = SecretRef{"test-credential"};
    profile->deadlines.connect = std::chrono::milliseconds{2'000};
    profile->deadlines.total = std::chrono::milliseconds{10'000};
    profile->max_batch_texts = max_batch;
    return profile;
}

[[nodiscard]] std::shared_ptr<MockHttpTransport> wired_transport() {
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-embedding-test");
    return std::make_shared<MockHttpTransport>(secrets);
}

[[nodiscard]] std::string embeddings_body(std::vector<std::vector<float>> rows,
                                          bool with_index = true) {
    JsonValue::Array data;
    for (std::size_t position = 0; position < rows.size(); ++position) {
        JsonValue::Object entry;
        if (with_index) {
            entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(position)));
        }
        JsonValue::Array values;
        for (const float component : rows[position]) {
            values.emplace_back(JsonValue(static_cast<double>(component)));
        }
        entry.emplace_back("embedding", JsonValue(std::move(values)));
        data.emplace_back(JsonValue(std::move(entry)));
    }
    JsonValue::Object root;
    root.emplace_back("data", JsonValue(std::move(data)));
    return to_json_string(JsonValue(std::move(root)));
}

// The wire request reaches the scripted URL with the profile's model, the
// input array and the credential resolved only at the transport boundary.
int request_shape_and_credential_resolution() {
    auto transport = wired_transport();
    transport->enqueue_json(200, embeddings_body({{1.0F, 0.0F}}));
    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);

    const auto embedded = provider.embed("calibration note");
    MIRA_CHECK(embedded.has_value());
    MIRA_CHECK(embedded.value().size() == 2);

    const auto recorded = transport->recorded();
    MIRA_CHECK(recorded.size() == 1);
    MIRA_CHECK(recorded.front().url == "https://api.test/v1/embeddings");
    MIRA_CHECK(recorded.front().authorization == "sk-embedding-test");
    MIRA_CHECK(!recorded.front().keep_alive);
    bool content_type = false;
    bool accept_json = false;
    for (const auto &header : recorded.front().headers) {
        content_type =
            content_type || (header.first == "Content-Type" && header.second == "application/json");
        accept_json =
            accept_json || (header.first == "Accept" && header.second == "application/json");
    }
    MIRA_CHECK(content_type && accept_json);

    const auto body = parse_json(recorded.front().body);
    MIRA_CHECK(body.has_value());
    const auto *model = body.value().find("model");
    MIRA_CHECK(model != nullptr && model->is_string() &&
               model->as_string()->compare("test-embedder") == 0);
    const auto *input = body.value().find("input");
    MIRA_CHECK(input != nullptr && input->is_array() && input->as_array()->size() == 1);
    MIRA_CHECK(input->as_array()->front().is_string() &&
               input->as_array()->front().as_string()->compare("calibration note") == 0);
    return 0;
}

// The happy path returns the vector; both the indexed and the positional
// response shapes are accepted.
int successful_response_parses() {
    auto transport = wired_transport();
    transport->enqueue_json(200, embeddings_body({{0.5F, -1.5F, 2.0F}}));
    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);
    const auto embedded = provider.embed("hello");
    MIRA_CHECK(embedded.has_value());
    MIRA_CHECK(embedded.value().size() == 3);
    MIRA_CHECK(std::fabs(embedded.value()[0] - 0.5F) < 1e-6F);
    MIRA_CHECK(std::fabs(embedded.value()[1] + 1.5F) < 1e-6F);
    MIRA_CHECK(std::fabs(embedded.value()[2] - 2.0F) < 1e-6F);

    // Positional shape (no "index" fields).
    transport->enqueue_json(200, embeddings_body({{7.0F, 8.0F}}, /*with_index=*/false));
    const auto positional = provider.embed("again");
    MIRA_CHECK(positional.has_value() && positional.value().size() == 2);
    return 0;
}

// The declared dimension is an assertion: a mismatching response is a
// protocol violation, not a degraded leg.
int expected_dims_mismatch_refused() {
    auto transport = wired_transport();
    OpenAiCompatibleEmbeddingProvider provider(
        make_embedding_profile("https://api.test", "test-embedder", 3), transport);
    transport->enqueue_json(200, embeddings_body({{1.0F, 0.0F}}));
    const auto mismatch = provider.embed("short");
    MIRA_CHECK(!mismatch.has_value());
    MIRA_CHECK(mismatch.error().domain == "mira.model");
    MIRA_CHECK(mismatch.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));

    // Oversized vectors are refused before they can reach the projection.
    transport->enqueue_json(200, embeddings_body({std::vector<float>(4'097, 0.25F)}));
    const auto oversized = provider.embed("too long");
    MIRA_CHECK(!oversized.has_value());
    MIRA_CHECK(oversized.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
    return 0;
}

// Non-2xx answers map through the shared model-plane error surface.
int http_error_status_maps() {
    auto transport = wired_transport();
    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);

    transport->enqueue_json(401, "{\"error\":\"bad key\"}");
    const auto unauthorized = provider.embed("x");
    MIRA_CHECK(!unauthorized.has_value());
    MIRA_CHECK(unauthorized.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::AuthenticationFailed));

    transport->enqueue_json(429, "{\"error\":\"slow down\"}");
    const auto limited = provider.embed("x");
    MIRA_CHECK(!limited.has_value());
    MIRA_CHECK(limited.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::RateLimited));
    MIRA_CHECK(limited.error().retryable);

    transport->enqueue_json(500, "{\"error\":\"upstream\"}");
    const auto overloaded = provider.embed("x");
    MIRA_CHECK(!overloaded.has_value());
    MIRA_CHECK(overloaded.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded));

    // Malformed JSON and shape violations are protocol failures.
    transport->enqueue_json(200, "{\"data\": \"not-an-array\"}");
    const auto malformed = provider.embed("x");
    MIRA_CHECK(!malformed.has_value());
    MIRA_CHECK(malformed.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));
    return 0;
}

// Batch results keep the input order, honoring the provider's index field.
int batch_ordering_follows_index() {
    auto transport = wired_transport();
    JsonValue::Array data;
    const std::vector<std::size_t> wire_order = {2, 0, 1};
    const std::vector<float> markers = {10.0F, 20.0F, 30.0F};
    for (std::size_t position = 0; position < wire_order.size(); ++position) {
        JsonValue::Object entry;
        entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(wire_order[position])));
        entry.emplace_back("embedding", JsonValue(JsonValue::Array{JsonValue(
                                            static_cast<double>(markers[wire_order[position]]))}));
        data.emplace_back(JsonValue(std::move(entry)));
    }
    JsonValue::Object root;
    root.emplace_back("data", JsonValue(std::move(data)));
    transport->enqueue_json(200, to_json_string(JsonValue(std::move(root))));

    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);
    const auto vectors = provider.embed_batch(std::vector<std::string>{"a", "b", "c"});
    MIRA_CHECK(vectors.has_value() && vectors.value().size() == 3);
    MIRA_CHECK(std::fabs(vectors.value()[0].front() - 10.0F) < 1e-6F);
    MIRA_CHECK(std::fabs(vectors.value()[1].front() - 20.0F) < 1e-6F);
    MIRA_CHECK(std::fabs(vectors.value()[2].front() - 30.0F) < 1e-6F);
    MIRA_CHECK(transport->recorded().size() == 1); // one wire request
    return 0;
}

// Batches larger than the profile cap split into consecutive requests.
int batch_splits_at_cap() {
    auto transport = wired_transport();
    transport->enqueue_json(200, embeddings_body({{1.0F}, {2.0F}}));
    transport->enqueue_json(200, embeddings_body({{3.0F}}));
    OpenAiCompatibleEmbeddingProvider provider(
        make_embedding_profile("https://api.test", "test-embedder", 0, 2), transport);
    const auto vectors = provider.embed_batch(std::vector<std::string>{"a", "b", "c"});
    MIRA_CHECK(vectors.has_value() && vectors.value().size() == 3);
    MIRA_CHECK(std::fabs(vectors.value()[2].front() - 3.0F) < 1e-6F);
    MIRA_CHECK(transport->recorded().size() == 2);
    return 0;
}

// The producer identity is deterministic in the descriptor: the same
// configuration yields the same id, a different model or host does not.
int identity_is_deterministic_in_the_descriptor() {
    const auto first = embedding_identity_from_descriptor(
        "mira.embedding.openai-compatible/v1|api.test|bge-m3|1024");
    const auto again = embedding_identity_from_descriptor(
        "mira.embedding.openai-compatible/v1|api.test|bge-m3|1024");
    const auto other_model = embedding_identity_from_descriptor(
        "mira.embedding.openai-compatible/v1|api.test|bge-other|1024");
    const auto other_host = embedding_identity_from_descriptor(
        "mira.embedding.openai-compatible/v1|api.mirror|bge-m3|1024");
    MIRA_CHECK(first == again);
    MIRA_CHECK(first != other_model);
    MIRA_CHECK(first != other_host);
    MIRA_CHECK(!first.is_nil());

    auto transport = wired_transport();
    OpenAiCompatibleEmbeddingProvider provider(
        make_embedding_profile("https://api.test", "bge-m3", 1024), transport);
    MIRA_CHECK(provider.embedder_id() ==
               embedding_identity_from_descriptor(provider.profile().canonical_descriptor()));
    return 0;
}

// Transport-level failures propagate to the caller unchanged.
int transport_failure_propagates() {
    auto transport = wired_transport();
    MockStep step;
    step.failure = make_model_error(ModelDomainCode::TransportFailed, "socket refused", true);
    transport->enqueue(std::move(step));
    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);
    const auto embedded = provider.embed("x");
    MIRA_CHECK(!embedded.has_value());
    MIRA_CHECK(embedded.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::TransportFailed));
    MIRA_CHECK(embedded.error().retryable);
    return 0;
}

// Profile validation refuses incomplete configurations; empty texts are
// refused before any wire call.
int profile_and_input_validation() {
    auto transport = wired_transport();
    OpenAiCompatibleEmbeddingProvider provider(make_embedding_profile(), transport);

    const auto embedded = provider.embed("");
    MIRA_CHECK(!embedded.has_value());
    MIRA_CHECK(transport->recorded().empty()); // no wire call was spent

    auto bad_origin = std::make_shared<EmbeddingProfile>();
    bad_origin->endpoint_origin = "api.test";
    bad_origin->model_selector = "m";
    bad_origin->credential = SecretRef{"c"};
    MIRA_CHECK(!bad_origin->validate().has_value());

    auto no_model = std::make_shared<EmbeddingProfile>();
    no_model->endpoint_origin = "https://api.test";
    no_model->credential = SecretRef{"c"};
    MIRA_CHECK(!no_model->validate().has_value());

    auto no_credential = std::make_shared<EmbeddingProfile>();
    no_credential->endpoint_origin = "https://api.test";
    no_credential->model_selector = "m";
    MIRA_CHECK(!no_credential->validate().has_value());
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += request_shape_and_credential_resolution();
    failures += successful_response_parses();
    failures += expected_dims_mismatch_refused();
    failures += http_error_status_maps();
    failures += batch_ordering_follows_index();
    failures += batch_splits_at_cap();
    failures += identity_is_deterministic_in_the_descriptor();
    failures += transport_failure_propagates();
    failures += profile_and_input_validation();
    return failures == 0 ? 0 : 1;
}
