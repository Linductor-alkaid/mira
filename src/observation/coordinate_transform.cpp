#include <mira/coordinates.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace mira {

namespace {

[[nodiscard]] Error transform_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.coordinates";
    error.safe_message = std::move(message);
    return error;
}

using SpaceHash = Id128Hash;

[[nodiscard]] std::vector<const CoordinateTransform *>
ordered_chain_edges(std::span<const CoordinateTransform> available, const TransformChain &chain,
                    Error &out_error) {
    out_error = transform_error(ErrorCode::InvalidArgument, "chain contains no transforms");
    std::vector<const CoordinateTransform *> edges;
    edges.reserve(chain.transform_indices.size());
    for (const std::size_t index : chain.transform_indices) {
        if (index >= available.size()) {
            out_error = transform_error(ErrorCode::InvalidArgument,
                                        "chain references a transform outside the table");
            return {};
        }
        edges.push_back(&available[index]);
    }
    return edges;
}

} // namespace

Result<CoordinateSpaceKind> parse_coordinate_space_kind(std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t>(CoordinateSpaceKind::Unknown)) {
        Error error;
        error.code = ErrorCode::UnsupportedVersion;
        error.domain = "mira.coordinates";
        error.safe_message = "unknown coordinate space kind";
        return error;
    }
    return static_cast<CoordinateSpaceKind>(value);
}

Result<Rotation> parse_rotation(std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t>(Rotation::Unknown)) {
        Error error;
        error.code = ErrorCode::UnsupportedVersion;
        error.domain = "mira.coordinates";
        error.safe_message = "unknown rotation";
        return error;
    }
    return static_cast<Rotation>(value);
}

TransformQuality worse_transform_quality(TransformQuality lhs, TransformQuality rhs) noexcept {
    const auto rank = [](TransformQuality quality) noexcept {
        switch (quality) {
        case TransformQuality::Exact:
            return 0;
        case TransformQuality::Calibrated:
            return 1;
        case TransformQuality::Approximate:
        case TransformQuality::Unknown:
            break;
        }
        return 2;
    };
    return rank(lhs) >= rank(rhs) ? lhs : rhs;
}

Result<void> validate_coordinate_transform(const CoordinateTransform &transform) {
    if (transform.from.is_nil() || transform.to.is_nil()) {
        return transform_error(ErrorCode::InvalidArgument, "transform endpoints must be non-nil");
    }
    if (transform.from == transform.to) {
        return transform_error(ErrorCode::InvalidArgument, "transform endpoints must differ");
    }
    if (!transform.matrix.is_finite() || transform.matrix.is_singular()) {
        return transform_error(ErrorCode::InvalidArgument,
                               "transform matrix must be finite and non-singular");
    }
    if (!transform.valid_source_region.is_well_formed() ||
        !transform.valid_target_region.is_well_formed()) {
        return transform_error(ErrorCode::InvalidArgument,
                               "transform valid regions must be well formed");
    }
    if (!std::isfinite(transform.max_abs_error) || transform.max_abs_error < 0.0) {
        return transform_error(ErrorCode::InvalidArgument,
                               "transform error bound must be finite and non-negative");
    }
    return Result<void>{};
}

Result<TransformChain> resolve_transform_chain(std::span<const CoordinateTransform> available,
                                               CoordinateSpaceId from, CoordinateSpaceId to,
                                               EnvironmentEpoch epoch) {
    if (from.is_nil() || to.is_nil()) {
        return transform_error(ErrorCode::InvalidArgument, "chain endpoints must be non-nil");
    }
    if (from == to) {
        return transform_error(ErrorCode::InvalidArgument, "chain endpoints must differ");
    }

    // Breadth-first search over same-epoch edges keeps the resolved path
    // shortest in edge count; determinism comes from first-found order.
    std::unordered_map<Id128, std::vector<std::size_t>, SpaceHash> adjacency;
    std::size_t same_epoch = 0;
    for (std::size_t index = 0; index < available.size(); ++index) {
        const auto &candidate = available[index];
        if (candidate.environment_epoch != epoch) {
            continue;
        }
        adjacency[candidate.from.value].push_back(index);
        ++same_epoch;
    }
    if (same_epoch == 0) {
        return transform_error(ErrorCode::StaleObservation,
                               "no transforms exist for the requested epoch");
    }

    struct SearchNode {
        Id128 space;
        std::size_t edge_index = std::numeric_limits<std::size_t>::max();
        Id128 previous;
    };
    std::deque<Id128> frontier;
    std::unordered_set<Id128, SpaceHash> visited;
    std::unordered_map<Id128, SearchNode, SpaceHash> came_from;
    frontier.push_back(from.value);
    visited.insert(from.value);
    bool found = false;
    while (!frontier.empty() && !found) {
        const Id128 current = frontier.front();
        frontier.pop_front();
        const auto entry = adjacency.find(current);
        if (entry == adjacency.end()) {
            continue;
        }
        for (const std::size_t edge_index : entry->second) {
            const CoordinateTransform &edge = available[edge_index];
            if (edge.to.value == current) {
                continue;
            }
            if (!visited.insert(edge.to.value).second) {
                continue;
            }
            came_from.emplace(edge.to.value, SearchNode{edge.to.value, edge_index, current});
            if (edge.to == to) {
                found = true;
                break;
            }
            frontier.push_back(edge.to.value);
        }
    }
    if (!found) {
        return transform_error(ErrorCode::InvalidObservation, "no transform chain between spaces");
    }

    TransformChain chain;
    chain.from = from;
    chain.to = to;
    chain.environment_epoch = epoch;
    chain.quality = TransformQuality::Exact;
    Id128 cursor = to.value;
    while (cursor != from.value) {
        const SearchNode &step = came_from.at(cursor);
        const CoordinateTransform &edge = available[step.edge_index];
        if (const auto validated = validate_coordinate_transform(edge); !validated) {
            return validated.error();
        }
        chain.path.emplace_back();
        chain.path.back().value = cursor;
        chain.transform_indices.push_back(step.edge_index);
        chain.composed = edge.matrix * chain.composed;
        chain.max_abs_error += edge.max_abs_error;
        chain.quality = worse_transform_quality(chain.quality, edge.quality);
        cursor = step.previous;
    }
    CoordinateSpaceId start;
    start.value = from.value;
    chain.path.push_back(start);
    std::reverse(chain.path.begin(), chain.path.end());
    std::reverse(chain.transform_indices.begin(), chain.transform_indices.end());
    return chain;
}

Result<TransformedPoint> transform_point_through(std::span<const CoordinateTransform> available,
                                                 const TransformChain &chain,
                                                 const PointF &source) {
    if (!source.is_finite()) {
        return transform_error(ErrorCode::InvalidArgument, "source point must be finite");
    }
    if (chain.composed.is_singular() || !chain.composed.is_finite()) {
        return transform_error(ErrorCode::InvalidObservation, "chain matrix is not usable");
    }
    Error structural_error;
    const auto edges = ordered_chain_edges(available, chain, structural_error);
    if (edges.empty()) {
        return structural_error;
    }
    PointF current = source;
    for (const CoordinateTransform *edge : edges) {
        if (!edge->valid_source_region.contains_inclusive(current)) {
            return transform_error(ErrorCode::InvalidObservation,
                                   "point is outside a transform's valid source region");
        }
        current = edge->matrix.apply(current);
        if (!current.is_finite()) {
            return transform_error(ErrorCode::InvalidObservation,
                                   "transform produced a non-finite point");
        }
        if (!edge->valid_target_region.contains_inclusive(current)) {
            return transform_error(ErrorCode::InvalidObservation,
                                   "point is outside a transform's valid target region");
        }
    }
    return TransformedPoint{current, chain.max_abs_error};
}

Result<TransformedBox> transform_box_through(std::span<const CoordinateTransform> available,
                                             const TransformChain &chain, const RectF &source) {
    if (!source.is_well_formed()) {
        return transform_error(ErrorCode::InvalidArgument, "source box must be well formed");
    }
    if (chain.composed.is_singular() || !chain.composed.is_finite()) {
        return transform_error(ErrorCode::InvalidObservation, "chain matrix is not usable");
    }
    Error structural_error;
    const auto edges = ordered_chain_edges(available, chain, structural_error);
    if (edges.empty()) {
        return structural_error;
    }

    // All four corners are transformed per segment: rotating or shearing
    // transforms cannot be represented by moving only two corners.
    std::vector<PointF> corners = {
        PointF{source.left, source.top}, PointF{source.right, source.top},
        PointF{source.right, source.bottom}, PointF{source.left, source.bottom}};
    bool axis_aligned = true;
    for (const CoordinateTransform *edge : edges) {
        for (PointF &corner : corners) {
            if (!edge->valid_source_region.contains_inclusive(corner)) {
                return transform_error(ErrorCode::InvalidObservation,
                                       "box corner is outside a transform's valid source region");
            }
            corner = edge->matrix.apply(corner);
            if (!corner.is_finite()) {
                return transform_error(ErrorCode::InvalidObservation,
                                       "transform produced a non-finite corner");
            }
            if (!edge->valid_target_region.contains_inclusive(corner)) {
                return transform_error(ErrorCode::InvalidObservation,
                                       "box corner is outside a transform's valid target region");
            }
        }
        axis_aligned = axis_aligned && edge->matrix.at(0, 1) == 0.0 &&
                       edge->matrix.at(1, 0) == 0.0 && edge->matrix.is_affine();
    }
    RectF bounds = RectF{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const PointF &corner : corners) {
        bounds = bounds.union_with(RectF{corner.x, corner.y, corner.x, corner.y});
    }
    return TransformedBox{bounds, chain.max_abs_error, axis_aligned};
}

Result<CoordinateTransform> make_scale_transform(CoordinateSpaceId from, CoordinateSpaceId to,
                                                 const RectF &source_region, double source_width,
                                                 double source_height, double target_width,
                                                 double target_height) {
    if (source_width <= 0.0 || source_height <= 0.0 || target_width <= 0.0 ||
        target_height <= 0.0) {
        return transform_error(ErrorCode::InvalidArgument, "extents must be positive");
    }
    CoordinateTransform transform;
    transform.from = from;
    transform.to = to;
    transform.matrix =
        Matrix3x3::scaling(target_width / source_width, target_height / source_height);
    transform.valid_source_region = source_region;
    const double scale_x = transform.matrix.at(0, 0);
    const double scale_y = transform.matrix.at(1, 1);
    transform.valid_target_region =
        RectF{source_region.left * scale_x, source_region.top * scale_y,
              source_region.right * scale_x, source_region.bottom * scale_y};
    if (const auto validated = validate_coordinate_transform(transform); !validated) {
        return validated.error();
    }
    return transform;
}

Result<CoordinateTransform> make_logical_to_canonical_transform(CoordinateSpaceId logical_space,
                                                                CoordinateSpaceId canonical_space,
                                                                const RectF &logical_region,
                                                                double logical_width,
                                                                double logical_height) {
    return make_scale_transform(logical_space, canonical_space, logical_region, logical_width,
                                logical_height, 1.0, 1.0);
}

Result<CoordinateTransform> make_native_to_logical_transform(
    CoordinateSpaceId native_space, CoordinateSpaceId logical_space, const RectF &native_region,
    double native_width, double native_height, double pixels_per_logical, Rotation rotation) {
    if (native_width <= 0.0 || native_height <= 0.0) {
        return transform_error(ErrorCode::InvalidArgument, "native extents must be positive");
    }
    if (!std::isfinite(pixels_per_logical) || pixels_per_logical <= 0.0) {
        return transform_error(ErrorCode::InvalidArgument, "pixels per logical must be positive");
    }
    if (rotation == Rotation::Unknown) {
        return transform_error(ErrorCode::InvalidArgument, "rotation must be known");
    }

    // Maps a point in the current native pixel space (sensor orientation) to
    // the rotation-normalized logical space the user currently sees. Extent
    // swapping encodes the rotation; the matrix is an affine rotation plus
    // the native-to-logical scale.
    const double inverse = 1.0 / pixels_per_logical;
    Matrix3x3 matrix;
    double logical_width = 0.0;
    double logical_height = 0.0;
    switch (rotation) {
    case Rotation::Rotation0:
        matrix = Matrix3x3::scaling(inverse, inverse);
        logical_width = native_width * inverse;
        logical_height = native_height * inverse;
        break;
    case Rotation::Rotation90:
        // native top-left -> logical top-right
        matrix = Matrix3x3::affine(0.0, -inverse, native_height * inverse, inverse, 0.0, 0.0);
        logical_width = native_height * inverse;
        logical_height = native_width * inverse;
        break;
    case Rotation::Rotation180:
        matrix = Matrix3x3::affine(-inverse, 0.0, native_width * inverse, 0.0, -inverse,
                                   native_height * inverse);
        logical_width = native_width * inverse;
        logical_height = native_height * inverse;
        break;
    case Rotation::Rotation270:
        matrix = Matrix3x3::affine(0.0, inverse, 0.0, -inverse, 0.0, native_width * inverse);
        logical_width = native_height * inverse;
        logical_height = native_width * inverse;
        break;
    case Rotation::Unknown:
        return transform_error(ErrorCode::InvalidArgument, "rotation must be known");
    }

    CoordinateTransform transform;
    transform.from = native_space;
    transform.to = logical_space;
    transform.matrix = matrix;
    transform.valid_source_region = native_region;
    transform.valid_target_region = RectF::from_origin_size(logical_width, logical_height);
    if (const auto validated = validate_coordinate_transform(transform); !validated) {
        return validated.error();
    }
    return transform;
}

} // namespace mira
