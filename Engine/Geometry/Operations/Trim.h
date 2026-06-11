#pragma once

#include "Geometry/Geometry.h"
#include <vector>
#include <cmath>

namespace primal::geometry {

// Keep only the portion of the curve in [t_start, t_end] where t ∈ [0,1].
// Samples the source curve and creates a new Spline from the clipped segment.
inline GeometryHandle trim(GeometryHandle source, f32 t_start, f32 t_end) {
    if (!source.is_valid()) return GeometryHandle{};
    t_start = std::max(0.0f, std::min(1.0f, t_start));
    t_end = std::max(t_start, std::min(1.0f, t_end));

    const u32 numSamples = 24;
    std::vector<math::v3> pts;
    pts.reserve(numSamples);

    for (u32 i = 0; i < numSamples; ++i) {
        f32 t = t_start + (t_end - t_start) * f32(i) / f32(numSamples - 1);
        pts.push_back(compute_point_at(source, t));
    }

    return create_spline(pts, false);
}

} // namespace primal::geometry
