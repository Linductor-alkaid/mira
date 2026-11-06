#pragma once

#include <optional>
#include <span>

namespace mira {

// No similarity exists for empty, mismatched, non-finite or zero-norm
// vectors. A valid negative cosine remains a value, not an error sentinel.
// Retrieval callers apply their own nonnegative candidate threshold.
[[nodiscard]] std::optional<double>
embedding_cosine_similarity(std::span<const float> left, std::span<const float> right) noexcept;

} // namespace mira
