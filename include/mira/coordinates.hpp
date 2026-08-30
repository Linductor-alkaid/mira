#pragma once

#include <mira/core_contracts.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace mira {

struct PointF final {
    double x = 0.0;
    double y = 0.0;
    [[nodiscard]] constexpr bool is_finite() const noexcept {
        return std::isfinite(x) && std::isfinite(y);
    }
    friend constexpr bool operator==(const PointF &, const PointF &) noexcept = default;
};

struct RectF final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] static constexpr RectF from_origin_size(double width, double height) noexcept {
        return RectF{0.0, 0.0, width, height};
    }
    [[nodiscard]] constexpr double width() const noexcept { return right - left; }
    [[nodiscard]] constexpr double height() const noexcept { return bottom - top; }
    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return right <= left || bottom <= top;
    }
    [[nodiscard]] constexpr bool is_finite() const noexcept {
        return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
               std::isfinite(bottom);
    }
    [[nodiscard]] constexpr bool is_well_formed() const noexcept {
        return is_finite() && right >= left && bottom >= top;
    }
    // Contains uses a half-open bound on the far edges so adjacent regions do
    // not both claim a shared border pixel.
    [[nodiscard]] constexpr bool contains(const PointF &point) const noexcept {
        return point.x >= left && point.y >= top && point.x < right && point.y < bottom;
    }
    [[nodiscard]] constexpr bool contains_inclusive(const PointF &point) const noexcept {
        return point.x >= left && point.y >= top && point.x <= right && point.y <= bottom;
    }
    [[nodiscard]] constexpr RectF union_with(const RectF &other) const noexcept {
        return RectF{left < other.left ? left : other.left, top < other.top ? top : other.top,
                     right > other.right ? right : other.right,
                     bottom > other.bottom ? bottom : other.bottom};
    }
    friend constexpr bool operator==(const RectF &, const RectF &) noexcept = default;
};

// Row-major 3x3 homogeneous matrix for 2D coordinate transforms:
//   | m00 m01 m02 |
//   | m10 m11 m12 |
//   | m20 m21 m22 |
// Mira's first-stage platform inputs only accept affine chains; the full
// homogeneous form exists so calibrated perspective chains can be expressed
// and validated later without changing the contract.
class Matrix3x3 final {
  public:
    constexpr Matrix3x3() noexcept : elements_{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0} {}

    [[nodiscard]] static constexpr Matrix3x3 affine(double a, double b, double tx, double c,
                                                    double d, double ty) noexcept {
        Matrix3x3 result;
        result.elements_ = {a, b, tx, c, d, ty, 0.0, 0.0, 1.0};
        return result;
    }
    [[nodiscard]] static constexpr Matrix3x3 scaling(double sx, double sy) noexcept {
        return affine(sx, 0.0, 0.0, 0.0, sy, 0.0);
    }
    [[nodiscard]] static constexpr Matrix3x3 translation(double tx, double ty) noexcept {
        return affine(1.0, 0.0, tx, 0.0, 1.0, ty);
    }
    [[nodiscard]] static Matrix3x3 rotation_degrees(double degrees) noexcept {
        const double radians = degrees * std::numbers::pi / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        return affine(cosine, -sine, 0.0, sine, cosine, 0.0);
    }

    [[nodiscard]] constexpr double at(std::size_t row, std::size_t column) const noexcept {
        return elements_[row * 3U + column];
    }
    [[nodiscard]] constexpr bool is_affine() const noexcept {
        return elements_[6U] == 0.0 && elements_[7U] == 0.0 && elements_[8U] == 1.0;
    }
    [[nodiscard]] constexpr bool is_finite() const noexcept {
        for (const double value : elements_) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] constexpr double determinant() const noexcept {
        return at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) -
               at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
               at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
    }
    [[nodiscard]] constexpr bool is_singular(double epsilon = 1e-12) const noexcept {
        if (!is_finite()) {
            return true;
        }
        const double value = determinant();
        return value <= epsilon && value >= -epsilon;
    }

    [[nodiscard]] constexpr PointF apply(const PointF &point) const noexcept {
        const double w = at(2, 0) * point.x + at(2, 1) * point.y + at(2, 2);
        if (w == 0.0 || !std::isfinite(w)) {
            return PointF{std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::quiet_NaN()};
        }
        return PointF{(at(0, 0) * point.x + at(0, 1) * point.y + at(0, 2)) / w,
                      (at(1, 0) * point.x + at(1, 1) * point.y + at(1, 2)) / w};
    }

    // Composition: (lhs * rhs) applies rhs first, then lhs.
    [[nodiscard]] constexpr Matrix3x3 operator*(const Matrix3x3 &other) const noexcept {
        Matrix3x3 result;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                double sum = 0.0;
                for (std::size_t index = 0; index < 3; ++index) {
                    sum += at(row, index) * other.at(index, column);
                }
                result.elements_[row * 3U + column] = sum;
            }
        }
        return result;
    }
    friend constexpr bool operator==(const Matrix3x3 &, const Matrix3x3 &) noexcept = default;

  private:
    std::array<double, 9> elements_{};
};

enum class CoordinateSpaceKind : std::uint8_t {
    NativeDisplayPixels,
    LogicalDisplay,
    Window,
    ContentViewport,
    CanonicalViewport,
    FramePixels,
    UiNodeLocal,
    RoiLocal,
    Unknown,
};

[[nodiscard]] Result<CoordinateSpaceKind> parse_coordinate_space_kind(std::uint8_t value) noexcept;

// A named coordinate space. Extent is expressed in the space's own units
// (pixels for pixel spaces, logical units for logical spaces, 1.0 for the
// canonical viewport). Spaces are per display/window context; Mira has no
// single global [0,1]^2 space.
struct CoordinateSpace final {
    CoordinateSpaceId id;
    CoordinateSpaceKind kind = CoordinateSpaceKind::Unknown;
    std::optional<DisplayId> display;
    std::optional<ObservationId> observation;
    std::optional<FrameId> frame;
    double extent_width = 0.0;
    double extent_height = 0.0;
    friend constexpr bool operator==(const CoordinateSpace &,
                                     const CoordinateSpace &) noexcept = default;
};

enum class TransformQuality : std::uint8_t {
    Exact,       // Platform-guaranteed mapping with no measurable error.
    Calibrated,  // Fit/calibrated mapping with a declared error bound.
    Approximate, // Heuristic mapping; policy may reject it for input use.
    Unknown,
};

[[nodiscard]] TransformQuality worse_transform_quality(TransformQuality lhs,
                                                       TransformQuality rhs) noexcept;

// A single validated edge between two spaces within one environment epoch.
struct CoordinateTransform final {
    CoordinateSpaceId from;
    CoordinateSpaceId to;
    Matrix3x3 matrix;
    RectF valid_source_region;
    RectF valid_target_region;
    TransformQuality quality = TransformQuality::Exact;
    // Declared worst-case absolute error in target units, applied to each
    // transformed coordinate. Chain resolution sums segment bounds.
    double max_abs_error = 0.0;
    EnvironmentEpoch environment_epoch = 0;
};

enum class Rotation : std::uint8_t { Rotation0, Rotation90, Rotation180, Rotation270, Unknown };

[[nodiscard]] Result<Rotation> parse_rotation(std::uint8_t value) noexcept;
[[nodiscard]] constexpr double rotation_degrees(Rotation rotation) noexcept {
    switch (rotation) {
    case Rotation::Rotation90:
        return 90.0;
    case Rotation::Rotation180:
        return 180.0;
    case Rotation::Rotation270:
        return 270.0;
    case Rotation::Rotation0:
    case Rotation::Unknown:
        break;
    }
    return 0.0;
}

// Structural validation shared by every transform before it enters a chain:
// finite matrix, non-singular, well-formed regions and non-empty endpoints.
[[nodiscard]] Result<void> validate_coordinate_transform(const CoordinateTransform &transform);

// Result of resolving a path between two spaces. transform_indices refer to
// the caller-provided transform table, in application order.
struct TransformChain final {
    CoordinateSpaceId from;
    CoordinateSpaceId to;
    EnvironmentEpoch environment_epoch = 0;
    Matrix3x3 composed;
    std::vector<CoordinateSpaceId> path;
    std::vector<std::size_t> transform_indices;
    double max_abs_error = 0.0;
    TransformQuality quality = TransformQuality::Exact;
};

// Finds the shortest transform path from `from` to `to` within `epoch`.
// Transforms from other epochs are ignored; if no path exists the result is a
// stable NotFound/InvalidObservation error, never a guessed mapping.
[[nodiscard]] Result<TransformChain>
resolve_transform_chain(std::span<const CoordinateTransform> available, CoordinateSpaceId from,
                        CoordinateSpaceId to, EnvironmentEpoch epoch);

struct TransformedPoint final {
    PointF point;
    double error_bound = 0.0;
};

struct TransformedBox final {
    RectF bounds;
    double error_bound = 0.0;
    // False when the segment transforms rotate/shear the box, i.e. the
    // returned bounds are the axis-aligned hull of four transformed corners.
    bool axis_aligned = true;
};

[[nodiscard]] Result<TransformedPoint>
transform_point_through(std::span<const CoordinateTransform> available, const TransformChain &chain,
                        const PointF &source);

[[nodiscard]] Result<TransformedBox>
transform_box_through(std::span<const CoordinateTransform> available, const TransformChain &chain,
                      const RectF &source);

// Helper factories for the standard display chain. Extents are in the space's
// own units; pixels_per_logical is native pixels per logical unit (px/dp), and
// rotation is the display's current rotation relative to natural orientation.
[[nodiscard]] Result<CoordinateTransform> make_native_to_logical_transform(
    CoordinateSpaceId native_space, CoordinateSpaceId logical_space, const RectF &native_region,
    double native_width, double native_height, double pixels_per_logical, Rotation rotation);

[[nodiscard]] Result<CoordinateTransform>
make_logical_to_canonical_transform(CoordinateSpaceId logical_space,
                                    CoordinateSpaceId canonical_space, const RectF &logical_region,
                                    double logical_width, double logical_height);

[[nodiscard]] Result<CoordinateTransform>
make_scale_transform(CoordinateSpaceId from, CoordinateSpaceId to, const RectF &source_region,
                     double source_width, double source_height, double target_width,
                     double target_height);

} // namespace mira
