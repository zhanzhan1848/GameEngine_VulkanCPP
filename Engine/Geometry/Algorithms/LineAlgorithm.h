#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace primal::geometry::algorithms::line {

// Line has no extra params -- pass nullptr for params

inline void tessellate(const std::vector<math::v3>& points,
                       const void* /*params*/,
                       f32 /*tolerance*/,
                       std::vector<math::v3>& out_segments) {
    out_segments = points; // Lines are already discrete -- direct pass-through
}

inline f32 distance_to(const std::vector<math::v3>& points,
                       const void* /*params*/,
                       const math::v3& world_pos) {
    f32 min_dist = FLT_MAX;
    for (u32 i = 0; i + 1 < points.size(); ++i) {
        const math::v3& a = points[i];
        const math::v3& b = points[i + 1];
        math::v3 ab = b - a;
        math::v3 ap = world_pos - a;
        f32 denom = dot(ab, ab);
        f32 t = (denom > 1e-8f) ? geometry::dot(ap, ab) / denom : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        math::v3 closest = a + ab * t;
        f32 dist = geometry::length(world_pos - closest);
        min_dist = std::min(min_dist, dist);
    }
    return min_dist;
}

inline f32 total_length(const std::vector<math::v3>& points,
                        const void* /*params*/) {
    f32 len = 0.0f;
    for (u32 i = 0; i + 1 < points.size(); ++i) {
        len += geometry::length(points[i + 1] - points[i]);
    }
    return len;
}

inline math::v3 point_at(const std::vector<math::v3>& points,
                         const void* /*params*/,
                         f32 t) {
    if (points.size() < 2) return {};
    f32 total_len = total_length(points, nullptr);
    if (total_len < 1e-6f) return points[0];
    f32 target = t * total_len;
    f32 accumulated = 0.0f;
    for (u32 i = 0; i + 1 < points.size(); ++i) {
        f32 seg_len = geometry::length(points[i + 1] - points[i]);
        if (accumulated + seg_len >= target) {
            f32 seg_t = (seg_len > 1e-6f) ? (target - accumulated) / seg_len : 0.0f;
            return points[i] + (points[i + 1] - points[i]) * seg_t;
        }
        accumulated += seg_len;
    }
    return points.back();
}

inline math::v3 tangent_at(const std::vector<math::v3>& points,
                           const void* /*params*/,
                           f32 t) {
    // Approximate tangent by sampling nearby points
    f32 dt = 0.001f;
    math::v3 p0 = point_at(points, nullptr, std::max(0.0f, t - dt));
    math::v3 p1 = point_at(points, nullptr, std::min(1.0f, t + dt));
    math::v3 tang = p1 - p0;
    f32 len = geometry::length(tang);
    return (len > 1e-6f) ? tang / len : math::v3{1.0f, 0.0f, 0.0f};
}

inline aabb bounding_box(const std::vector<math::v3>& points,
                         const void* /*params*/) {
    if (points.empty()) return {};
    math::v3 mn = points[0], mx = points[0];
    for (const auto& p : points) {
        mn = math::v3{std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = math::v3{std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    return aabb{mn, mx};
}

// Helper to create the registry entry
inline CurveAlgorithmEntry make_entry() {
    CurveAlgorithmEntry e;
    e.tessellate   = tessellate;
    e.distance_to  = distance_to;
    e.total_length = total_length;
    e.point_at     = point_at;
    e.tangent_at   = tangent_at;
    e.bounding_box = bounding_box;
    return e;
}

} // namespace primal::geometry::algorithms::line
