#include <mira/embedding_math.hpp>

#include <algorithm>
#include <cmath>

namespace mira {
std::optional<double> embedding_cosine_similarity(std::span<const float> left,
                                                  std::span<const float> right) noexcept {
    if (left.empty() || left.size() != right.size()) {
        return std::nullopt;
    }
    double dot = 0.0;
    double left_norm = 0.0;
    double right_norm = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double a = left[i];
        const double b = right[i];
        if (!std::isfinite(a) || !std::isfinite(b)) {
            return std::nullopt;
        }
        dot += a * b;
        left_norm += a * a;
        right_norm += b * b;
    }
    if (left_norm <= 0.0 || right_norm <= 0.0) {
        return std::nullopt;
    }
    return std::clamp(dot / (std::sqrt(left_norm) * std::sqrt(right_norm)), -1.0, 1.0);
}
} // namespace mira
