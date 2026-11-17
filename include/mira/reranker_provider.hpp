#pragma once

#include <mira/context_rerank.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_transport.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// OpenAI/Cohere-style /v1/rerank cross-encoder adapter for Layer 2 (DEC-112).
// The protocol adapter lives in Core over the injected IHttpTransport boundary
// — the same division of labor as OpenAiCompatibleProvider and
// OpenAiCompatibleEmbeddingProvider: no socket or TLS code in Core, the HTTP
// stack is an adapter the host assembles and owns.
//
// The adapter implements the IContextReranker contract verbatim: it reorders
// and truncates, never grows membership; an empty input closes with an empty
// result; any transport/protocol failure returns one Error for the whole
// batch and the caller falls back to the retrieval order. The reranker ranks
// topical relevance only — it never adjudicates memory conflicts and carries
// no admission authority (design Context Intelligence §5.6).
// ---------------------------------------------------------------------------

// Host-side configuration of one remote reranker. The model travels per
// request from this profile; the credential is resolved only at the transport
// boundary and never logged.
struct RerankerProfile final {
    std::string display_name;
    // "https://api.example.com" — scheme plus host (plus optional port).
    std::string endpoint_origin;
    // Default "/v1"; trailing slashes are trimmed before "/rerank".
    std::string api_prefix = "/v1";
    // Wire model name (for example "BAAI/bge-reranker-v2-m3").
    std::string model_selector;
    SecretRef credential;
    TransportDeadlines deadlines;
    std::uint32_t max_redirects = 2;
    bool allow_private_endpoint = false;
    // Upper bound of one rerank call (candidate batch admission, DEC-112).
    std::size_t max_candidates = 32;

    [[nodiscard]] Result<void> validate() const;
    // Host component of the origin (no scheme, no port).
    [[nodiscard]] std::string endpoint_host() const;
    // origin + trimmed prefix + "/rerank".
    [[nodiscard]] std::string endpoint_url() const;
};

class OpenAiCompatibleReranker final : public IContextReranker {
  public:
    // The profile and transport must outlive the adapter (the transport is
    // invoked synchronously on the caller's Executor worker).
    OpenAiCompatibleReranker(std::shared_ptr<const RerankerProfile> profile,
                             std::shared_ptr<IHttpTransport> transport,
                             ContextRerankConfig config = ContextRerankConfig{},
                             ContextRerankWeights weights = ContextRerankWeights{});
    ~OpenAiCompatibleReranker() override;

    OpenAiCompatibleReranker(const OpenAiCompatibleReranker &) = delete;
    OpenAiCompatibleReranker &operator=(const OpenAiCompatibleReranker &) = delete;

    [[nodiscard]] Result<std::vector<RankedContextItem>>
    rerank(const ContextQuery &query, std::span<const ContextCandidate> candidates) override;

    [[nodiscard]] const RerankerProfile &profile() const noexcept { return *profile_; }
    [[nodiscard]] const TransportTrace &last_trace() const noexcept { return last_trace_; }

  private:
    std::shared_ptr<const RerankerProfile> profile_;
    std::shared_ptr<IHttpTransport> transport_;
    ContextRerankConfig config_;
    ContextRerankWeights weights_;
    TransportTrace last_trace_;
};

} // namespace mira
