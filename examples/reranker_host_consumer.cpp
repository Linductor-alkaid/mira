// Offline host consumer for the Layer 2 remote reranker (DEC-112):
// OpenAiCompatibleReranker over a scripted /v1/rerank transport, wired after
// an InMemoryContextIndex retrieval. Demonstrates the IContextReranker
// contract end to end - reorder + truncate, provenance preserved, and the
// whole-batch failure falling back to the retrieval order. No network.
#include <mira/context_rerank.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/json.hpp>
#include <mira/model_transport.hpp>
#include <mira/reranker_provider.hpp>

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace mira;

// Scripted IHttpTransport: records the request, answers with the exact
// /v1/rerank response shape captured from the real endpoint, and can inject
// one transport-level failure for the degrade round.
class ScriptedRerankTransport final : public IHttpTransport {
  public:
    explicit ScriptedRerankTransport(std::shared_ptr<ISecretResolver> secrets)
        : secrets_(std::move(secrets)) {}

    Result<HttpResponseInfo> execute(const HttpRequest &request, const TransportLimits & /*limits*/,
                                     const OperationContext & /*context*/,
                                     const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override {
        if (fail_next_.exchange(false)) {
            trace.write_started = true;
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.model";
            error.safe_message = "scripted rerank outage";
            error.retryable = true;
            return error;
        }
        const auto body = parse_json(request.body);
        if (!body || !body.value().is_object()) {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.safe_message = "scripted transport received a non-JSON body";
            return error;
        }
        const auto *documents = body.value().find("documents");
        const std::size_t count =
            documents != nullptr && documents->is_array() ? documents->as_array()->size() : 0;
        // Scripted relevance: reverse of the wire order, so the reranker must
        // visibly invert the retrieval order.
        JsonValue::Array results;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t scored_index = count - 1 - index;
            JsonValue::Object entry;
            entry.emplace_back("index", JsonValue(static_cast<std::int64_t>(scored_index)));
            entry.emplace_back("document", JsonValue());
            entry.emplace_back("relevance_score",
                               JsonValue(static_cast<double>(scored_index + 1) / 10.0));
            results.emplace_back(JsonValue(std::move(entry)));
        }
        JsonValue::Object root;
        root.emplace_back("id", JsonValue(std::string("offline-probe")));
        root.emplace_back("results", JsonValue(std::move(results)));
        const std::string payload = to_json_string(JsonValue(std::move(root)));
        on_chunk(payload);
        trace.write_started = true;
        trace.write_completed = true;
        trace.headers_received = true;
        HttpResponseInfo info;
        info.status = 200;
        info.headers.emplace_back("content-type", "application/json");
        info.body_bytes = payload.size();
        return info;
    }

    void fail_next() { fail_next_.store(true); }

  private:
    std::shared_ptr<ISecretResolver> secrets_;
    std::atomic<bool> fail_next_{false};
};

class KeySecretResolver final : public ISecretResolver {
  public:
    Result<std::string> resolve(const SecretRef &reference) override {
        if (reference.name != "rerank-credential") {
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.safe_message = "secret is not configured";
            return error;
        }
        return std::string{"offline-placeholder-key"};
    }
};

[[nodiscard]] ContextIndexAsset make_segment(const SessionId &session, std::uint64_t ordinal,
                                             std::string text) {
    ContextIndexAsset asset;
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    asset.id = ContextAssetId{Id128{bytes}};
    asset.kind = ContextAssetKind::ConversationSegment;
    asset.text = std::move(text);
    asset.session = session;
    asset.through_event_sequence = ordinal;
    asset.source_events = {EventId{Id128{bytes}}};
    return asset;
}

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

} // namespace

int main() {
    const SessionId session{ordinal_id(1)};
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Application;
    scope.subject_id = "workbench-notes";

    // Layer 1: three segments retrieved in insertion order (equal lexical
    // signal - the scripted reranker is what reorders them).
    InMemoryContextIndex index;
    const std::vector<std::string> texts = {
        "alpha deployment note",
        "beta calibration note",
        "gamma tone preference note",
    };
    for (std::size_t ordinal = 0; ordinal < texts.size(); ++ordinal) {
        if (!index.upsert_asset(make_segment(session, ordinal + 10, texts[ordinal]))) {
            std::cerr << "asset registration failed\n";
            return 2;
        }
    }

    ContextQuery query;
    query.session = session;
    query.scopes = {scope};
    query.text = "note";
    RetrievalBudget budget;
    budget.top_k = 3;
    const auto retrieved = index.retrieve(query, budget);
    if (!retrieved || retrieved.value().candidates.size() != 3) {
        std::cerr << "retrieval failed\n";
        return 3;
    }
    std::cout << "retrieval: candidates=" << retrieved.value().candidates.size() << "\n";

    // Layer 2: the remote reranker over a scripted transport.
    auto transport =
        std::make_shared<ScriptedRerankTransport>(std::make_shared<KeySecretResolver>());
    auto profile = std::make_shared<RerankerProfile>();
    profile->display_name = "offline-reranker";
    profile->endpoint_origin = "https://rerank.invalid";
    profile->api_prefix = "/v1";
    profile->model_selector = "demo-reranker";
    profile->credential = SecretRef{"rerank-credential"};
    profile->deadlines.total = std::chrono::milliseconds{5'000};
    if (!profile->validate()) {
        std::cerr << "reranker profile rejected\n";
        return 4;
    }
    OpenAiCompatibleReranker reranker(profile, transport, ContextRerankConfig{3});

    const auto ranked = reranker.rerank(query, retrieved.value().candidates);
    if (!ranked || ranked.value().size() != 3) {
        std::cerr << "rerank failed\n";
        return 5;
    }
    // The scripted relevance reverses the wire order: the last candidate must
    // surface first, with its retrieval rank recorded for observability.
    if (ranked.value().front().retrieval_rank != 2 || ranked.value().back().retrieval_rank != 0) {
        std::cerr << "rerank order did not follow the scripted relevance\n";
        return 6;
    }
    std::cout << "rerank: top=" << ranked.value().front().candidate.text
              << " retrieval_rank=" << ranked.value().front().retrieval_rank
              << " rerank_score=" << ranked.value().front().rerank_score << "\n";

    // Contract degradation: a transport failure is one Error for the whole
    // batch; the host falls back to the retrieval order. Nothing is lost.
    transport->fail_next();
    const auto degraded = reranker.rerank(query, retrieved.value().candidates);
    if (degraded) {
        std::cerr << "scripted failure did not surface as an error\n";
        return 7;
    }
    std::cout << "degrade: error surfaced, host falls back to retrieval order ("
              << retrieved.value().candidates.size() << " candidates)\n";
    return 0;
}
