#include "support/test.hpp"
#include <mira/embedding_math.hpp>

#include <array>
#include <cmath>
#include <limits>

int main() {
    using mira::embedding_cosine_similarity;
    const std::array<float, 2> x{1, 0}, y{0, 1}, negative{-1, 0}, zero{0, 0};
    MIRA_CHECK(embedding_cosine_similarity(x, x) == 1.0);
    MIRA_CHECK(embedding_cosine_similarity(x, negative) == -1.0);
    MIRA_CHECK(embedding_cosine_similarity(x, y) == 0.0);
    MIRA_CHECK(!embedding_cosine_similarity(x, zero));
    MIRA_CHECK(!embedding_cosine_similarity({}, {}));
    MIRA_CHECK(!embedding_cosine_similarity(x, std::span<const float>(x).first(1)));
    for (const float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const std::array<float, 2> invalid{bad, 1};
        MIRA_CHECK(!embedding_cosine_similarity(x, invalid));
        MIRA_CHECK(!embedding_cosine_similarity(invalid, x));
    }
    for (const float scale :
         {std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()}) {
        const std::array<float, 2> extreme{scale, scale};
        const auto similarity = embedding_cosine_similarity(extreme, extreme);
        MIRA_CHECK(similarity && std::isfinite(*similarity));
        MIRA_CHECK(std::abs(*similarity - 1.0) < 1e-12);
    }
    return 0;
}
