#pragma once

#include <mira/memory_embedding.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_transport.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// OpenAI-compatible /v1/embeddings producer for the memory vector leg
// (DEC-111; design Context/Memory §14.3). The protocol adapter lives in Core
// over the injected IHttpTransport boundary — the same division of labor as
// OpenAiCompatibleProvider: no socket or TLS code in Core, the HTTP stack is
// an adapter the host assembles and owns.
// ---------------------------------------------------------------------------

// Host-side configuration of one embedding producer. The profile is the
// single source of the producer identity: the canonical descriptor derived
// from it seeds the EmbedderId, so switching model or endpoint host switches
// identity and retires the previous vectors' comparability automatically.
struct EmbeddingProfile final {
    std::string display_name;
    // "https://api.example.com" — scheme plus host (plus optional port).
    std::string endpoint_origin;
    // Default "/v1"; trailing slashes are trimmed before "/embeddings".
    std::string api_prefix = "/v1";
    // Wire model name (for example "BAAI/bge-m3").
    std::string model_selector;
    // When non-zero, every response must carry exactly this many dimensions;
    // a mismatch is refused instead of poisoning the projection. 0 accepts
    // any dimension within the memory contract bound (4096).
    std::uint32_t expected_dims = 0;
    // Resolved only at the transport boundary; never logged, never stored.
    SecretRef credential;
    TransportDeadlines deadlines;
    std::uint32_t max_redirects = 2;
    bool allow_private_endpoint = false;
    // Upper bound of one /v1/embeddings request in embed_batch.
    std::size_t max_batch_texts = 16;

    [[nodiscard]] Result<void> validate() const;
    // Host component of the origin (no scheme, no port); empty when the
    // origin cannot be parsed.
    [[nodiscard]] std::string endpoint_host() const;
    // Deterministic identity seed: protocol family + host + model + dims.
    [[nodiscard]] std::string canonical_descriptor() const;
    // origin + trimmed prefix + "/embeddings".
    [[nodiscard]] std::string endpoint_url() const;
};

// EmbedderId derived from a canonical descriptor: SHA-256 of the descriptor,
// first 16 bytes (same derivation family as the context asset ids). The same
// descriptor always yields the same identity; a different descriptor never
// collides with an existing identity in practice.
[[nodiscard]] EmbedderId embedding_identity_from_descriptor(std::string_view descriptor);

class OpenAiCompatibleEmbeddingProvider final : public IEmbeddingProvider {
  public:
    // The profile and transport must outlive the provider (references are
    // used per call; the transport is invoked synchronously on the caller's
    // Executor worker).
    OpenAiCompatibleEmbeddingProvider(std::shared_ptr<const EmbeddingProfile> profile,
                                      std::shared_ptr<IHttpTransport> transport);
    ~OpenAiCompatibleEmbeddingProvider() override;

    OpenAiCompatibleEmbeddingProvider(const OpenAiCompatibleEmbeddingProvider &) = delete;
    OpenAiCompatibleEmbeddingProvider &
    operator=(const OpenAiCompatibleEmbeddingProvider &) = delete;

    // Deterministic identity of the configured profile (DEC-111).
    [[nodiscard]] EmbedderId embedder_id() const override;

    // One POST per text. A transport, protocol or dimension failure is an
    // Error; the caller degrades (the vector leg is optional by contract).
    [[nodiscard]] Result<std::vector<float>> embed(std::string_view text) override;

    // One POST for up to max_batch_texts texts; larger batches are split
    // into consecutive requests. Results keep the input order.
    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_batch(std::span<const std::string> texts) override;

    [[nodiscard]] const EmbeddingProfile &profile() const noexcept { return *profile_; }
    [[nodiscard]] const TransportTrace &last_trace() const noexcept { return last_trace_; }

  private:
    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_request(const std::vector<std::string> &texts);

    std::shared_ptr<const EmbeddingProfile> profile_;
    std::shared_ptr<IHttpTransport> transport_;
    EmbedderId identity_;
    TransportTrace last_trace_;
};

} // namespace mira
