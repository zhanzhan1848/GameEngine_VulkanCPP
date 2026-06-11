#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace primal::geometry::algorithms::spline {

struct SplineParams {
    bool closed{false};
};

// Catmull-Rom interpolation between p1 and p2 with tangent context p0, p3
inline math::v3 catmull_rom(const math::v3& p0, const math::v3& p1,
                             const math::v3& p2, const math::v3& p3,
                             f32 t) {
    f32 t2 = t * t;
    f32 t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                    (-p0 + p2) * t +
                    (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                    (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

inline void tessellate(const std::vector<math::v3>& points,
                       const void* params,
                       f32 tolerance,
                       std::vector<math::v3>& out_segments) {
    out_segments.clear();
    auto* sp = static_cast<const SplineParams*>(params);
    const u32 n = static_cast<u32>(points.size());
    if (n < 2) return;

    const u32 segments = n - 1;
    const u32 steps_per_segment = std::max(8u, static_cast<u32>(32.0f / tolerance));

    out_segments.reserve(segments * steps_per_segment + 1);

    for (u32 i = 0; i < segments; ++i) {
        math::v3 p0 = (i > 0) ? points[i - 1] : (sp->closed ? points[n - 1] : points[i] - (points[i + 1] - points[i]));
        math::v3 p1 = points[i];
        math::v3 p2 = points[i + 1];
        math::v3 p3 = (i + 2 < n) ? points[i + 2] : (sp->closed ? points[0] : points[i + 1] + (points[i + 1] - points[i]));

        for (u32 s = 0; s <= steps_per_segment; ++s) {
            if (i > 0 && s == 0) continue; // Avoid duplicate at segment boundaries
            f32 t = static_cast<f32>(s) / static_cast<f32>(steps_per_segment);
            out_segments.push_back(catmull_rom(p0, p1, p2, p3, t));
        }
    }
}

inline f32 distance_to(const std::vector<math::v3>& points,
                       const void* params,
                       const math::v3& world_pos) {
    std::vector<math::v3> segments;
    tessellate(points, params, 0.01f, segments);
    f32 min_dist = FLT_MAX;
    for (u32 i = 0; i + 1 < segments.size(); ++i) {
        math::v3 ab = segments[i + 1] - segments[i];
        math::v3 ap = world_pos - segments[i];
        f32 denom = geometry::dot(ab, ab);
        f32 t = (denom > 1e-8f) ? geometry::dot(ap, ab) / denom : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        min_dist = std::min(min_dist, geometry::length(world_pos - (segments[i] + ab * t)));
    }
    return min_dist;
}

inline f32 total_length(const std::vector<math::v3>& points,
                        const void* params) {
    std::vector<math::v3> segments;
    tessellate(points, params, 0.01f, segments);
    f32 len = 0.0f;
    for (u32 i = 0; i + 1 < segments.size(); ++i) {
        len += geometry::length(segments[i + 1] - segments[i]);
    }
    return len;
}

inline math::v3 point_at(const std::vector<math::v3>& points,
                         const void* params,
                         f32 t) {
    std::vector<math::v3> segments;
    tessellate(points, params, 0.01f, segments);
    if (segments.empty()) return {};
    f32 total_len = 0.0f;
    for (u32 i = 0; i + 1 < segments.size(); ++i)
        total_len += geometry::length(segments[i + 1] - segments[i]);
    if (total_len < 1e-6f) return segments[0];
    f32 target = t * total_len, accumulated = 0.0f;
    for (u32 i = 0; i + 1 < segments.size(); ++i) {
        f32 seg_len = geometry::length(segments[i + 1] - segments[i]);
        if (accumulated + seg_len >= target) {
            f32 seg_t = (seg_len > 1e-6f) ? (target - accumulated) / seg_len : 0.0f;
            return segments[i] + (segments[i + 1] - segments[i]) * seg_t;
        }
        accumulated += seg_len;
    }
    return segments.back();
}

inline math::v3 tangent_at(const std::vector<math::v3>& points,
                           const void* params,
                           f32 t) {
    f32 dt = 0.001f;
    math::v3 p0 = point_at(points, params, std::max(0.0f, t - dt));
    math::v3 p1 = point_at(points, params, std::min(1.0f, t + dt));
    math::v3 tang = p1 - p0;
    f32 len = geometry::length(tang);
    return (len > 1e-6f) ? tang / len : math::v3{1.0f, 0.0f, 0.0f};
}

inline aabb bounding_box(const std::vector<math::v3>& points,
                         const void* params) {
    std::vector<math::v3> segments;
    tessellate(points, params, 0.01f, segments);
    if (segments.empty()) return {};
    math::v3 mn = segments[0], mx = segments[0];
    for (const auto& p : segments) {
        mn = math::v3{std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = math::v3{std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    return aabb{mn, mx};
}

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

} // namespace primal::geometry::algorithms::spline
