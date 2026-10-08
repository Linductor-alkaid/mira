#pragma once

// Internal cross-file helpers of the dialect layer. Not public API; the
// public surface is <mira/model_dialect.hpp>.

#include <mira/model_dialect.hpp>

#include <cstddef>

namespace mira {

// Matches the Messages dialect thinking budget: provider reasoning maps to a
// bounded ThinkingPart or fails closed, never truncates silently.
inline constexpr std::size_t kMaxThinkingBytes = 4ULL * 1024ULL * 1024ULL;

// Shared by the Responses terminal decoder and the Chat Completions decoder.
[[nodiscard]] ModelUsage parse_usage_object(const JsonValue *usage, bool from_stream_eof);

// Encode-stage gates and artifact rendering shared by the OpenAI mappers.
[[nodiscard]] Result<void> gate_generation_options(const ModelProfile &profile,
                                                   const ModelGenerationOptions &generation);
[[nodiscard]] Result<void> require_image_media_type(const ArtifactRef &reference);
[[nodiscard]] Result<std::string> fetch_image_data_url(const ArtifactRef &reference,
                                                       IArtifactSource &artifacts);

} // namespace mira
