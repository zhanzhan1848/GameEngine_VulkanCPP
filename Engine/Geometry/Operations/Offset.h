#pragma once

#include "Geometry/Geometry.h"
#include <vector>
#include <cmath>

namespace primal::geometry {

// Create a parallel curve offset by `distance` in the XY plane (along perpendicular to tangent).
// Positive distance = offset to the left of the curve direction.
// Works with Spline and Line geometries. Returns a new Spline handle.
inline GeometryHandle offset(GeometryHandle source, f32 distance) {
    if (!source.is_valid() || distance == 0.0f) return source;

    const u32 numSamples = 32;
    std::vector<math::v3> offsetPts;
    offsetPts.reserve(numSamples);

    for (u32 i = 0; i < numSamples; ++i) {
        f32 t = f32(i) / f32(numSamples - 1);
        math::v3 pos = compute_point_at(source, t);
        math::v3 tan = compute_tangent_at(source, t);

        // Perpendicular in XZ plane (left of forward direction)
        f32 len = std::sqrt(tan.x * tan.x + tan.z * tan.z);
        math::v3 offPt;
        if (len > 1e-6f) {
            f32 nx = -tan.z / len;
            f32 nz = tan.x / len;
            offPt.x = pos.x + nx * distance;
            offPt.y = pos.y;
            offPt.z = pos.z + nz * distance;
        } else {
            offPt = pos;
        }
        offsetPts.push_back(offPt);
    }

    return create_spline(offsetPts, false);
}

} // namespace primal::geometry
