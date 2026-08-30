#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_profile.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// Raw HTTP-level exchange result handed to dialect decoders.
struct WireHttpResponse final {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Supplies artifact payload bytes at encode time. Implementations wrap an
// ArtifactStore; the mapper only ever sees bounded bytes.
class IArtifactSource {
  public:
    virtual ~IArtifactSource() = default;
    virtual Result<std::vector<std::byte>> fetch(const ArtifactRef &reference) = 0;
};

// A null source for requests that carry no binary parts.
class NullArtifactSource final : public IArtifactSource {
  public:
    Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "artifact source is not configured");
    }
};

// Encodes canonical requests into one fixed wire dialect and decodes terminal
// wire responses back into canonical ModelResponses. Mappers never switch
// endpoints, never clamp non-default values and never trust provider-side
// schema validation.
class IDialectMapper {
  public:
    virtual ~IDialectMapper() = default;
    [[nodiscard]] virtual ProtocolDialect dialect() const = 0;
    // `stream` requests an SSE response; mappers that have not verified SSE
    // fixtures must reject it with CapabilityMismatch.
    [[nodiscard]] virtual Result<JsonValue>
    encode_request(const ModelRequest &request, const ModelProfile &profile, bool stream,
                   IArtifactSource &artifacts) const = 0;
    [[nodiscard]] virtual Result<ModelResponse>
    decode_response(const ModelRequest &request, const ModelProfile &profile,
                    const WireHttpResponse &wire) const = 0;
};

// ---------------------------------------------------------------------------
// openai.responses.v1
// ---------------------------------------------------------------------------

class ResponsesV1Mapper final : public IDialectMapper {
  public:
    [[nodiscard]] ProtocolDialect dialect() const override {
        return ProtocolDialect::OpenAIResponsesV1;
    }
    [[nodiscard]] Result<JsonValue> encode_request(const ModelRequest &request,
                                                   const ModelProfile &profile, bool stream,
                                                   IArtifactSource &artifacts) const override;
    [[nodiscard]] Result<ModelResponse> decode_response(const ModelRequest &request,
                                                        const ModelProfile &profile,
                                                        const WireHttpResponse &wire) const override;
};

// ---------------------------------------------------------------------------
// openai.chat-completions.v1
// ---------------------------------------------------------------------------

class ChatCompletionsV1Mapper final : public IDialectMapper {
  public:
    [[nodiscard]] ProtocolDialect dialect() const override {
        return ProtocolDialect::OpenAIChatCompletionsV1;
    }
    [[nodiscard]] Result<JsonValue> encode_request(const ModelRequest &request,
                                                   const ModelProfile &profile, bool stream,
                                                   IArtifactSource &artifacts) const override;
    [[nodiscard]] Result<ModelResponse> decode_response(const ModelRequest &request,
                                                        const ModelProfile &profile,
                                                        const WireHttpResponse &wire) const override;
};

// Maps a non-2xx HTTP result to a stable model error. Shared by both dialects;
// unknown statuses fail closed as ProtocolViolation rather than being guessed.
[[nodiscard]] Error map_http_error_status(const WireHttpResponse &wire);

// Decodes one parsed Responses API response object (synchronous body or the
// `response` member of a terminal SSE event). Shared by the sync mapper and
// the SSE reducer so both paths produce identical canonical items.
[[nodiscard]] Result<ModelResponse>
decode_responses_terminal_body(const ModelRequest &request, const ModelProfile &profile,
                               const JsonValue &body);

// Parses an RFC 7231 Retry-After header (seconds or HTTP-date) into a bounded
// delay; nullopt when absent, invalid or beyond the cap.
[[nodiscard]] std::optional<std::chrono::milliseconds>
parse_retry_after(const std::vector<std::pair<std::string, std::string>> &headers,
                  std::chrono::milliseconds cap);

// Extracts provider rate-limit headers into canonical metadata.
[[nodiscard]] RateLimitMetadata
parse_rate_limit_headers(const std::vector<std::pair<std::string, std::string>> &headers);

[[nodiscard]] std::string base64_encode(std::span<const std::byte> bytes);

} // namespace mira
