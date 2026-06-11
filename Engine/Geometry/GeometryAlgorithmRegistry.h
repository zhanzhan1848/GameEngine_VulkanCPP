#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>

namespace primal::geometry {

struct CurveAlgorithmEntry {
    // All functions take: control_points, opaque params pointer, and result/output params
    using TessellateFn = void(*)(const std::vector<math::v3>& points,
                                 const void* params,
                                 f32 tolerance,
                                 std::vector<math::v3>& out_segments);
    using DistanceFn   = f32(*)(const std::vector<math::v3>& points,
                                const void* params,
                                const math::v3& world_pos);
    using LengthFn     = f32(*)(const std::vector<math::v3>& points,
                               const void* params);
    using PointAtFn    = math::v3(*)(const std::vector<math::v3>& points,
                                     const void* params,
                                     f32 t);
    using TangentAtFn  = math::v3(*)(const std::vector<math::v3>& points,
                                     const void* params,
                                     f32 t);
    using BBoxFn       = aabb(*)(const std::vector<math::v3>& points,
                                 const void* params);

    TessellateFn tessellate{nullptr};
    DistanceFn   distance_to{nullptr};
    LengthFn     total_length{nullptr};
    PointAtFn    point_at{nullptr};
    TangentAtFn  tangent_at{nullptr};
    BBoxFn       bounding_box{nullptr};
};

void register_algorithm(GeometryType type, const CurveAlgorithmEntry& entry);
const CurveAlgorithmEntry* get_algorithm(GeometryType type);

} // namespace primal::geometry
