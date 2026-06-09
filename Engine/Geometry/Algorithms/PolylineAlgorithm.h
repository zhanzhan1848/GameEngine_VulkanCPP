#pragma once

#include "Geometry/GeometryTypes.h"
#include "Geometry/GeometryAlgorithmRegistry.h"
#include "Geometry/Algorithms/LineAlgorithm.h"
#include "Geometry/Algorithms/ArcAlgorithm.h"
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace primal::geometry::algorithms::polyline {

struct PolylineParams {
    const SegmentType* segment_types{nullptr}; // array of length points.size()-1
    u32                segment_count{0};
};

inline void tessellate(const std::vector<math::v3>& points,
                       const void* params,
                       f32 tolerance,
                       std::vector<math::v3>& out_segments) {
    out_segments.clear();
    auto* pp = static_cast<const PolylineParams*>(params);
    if (points.size() < 2 || !pp || !pp->segment_types) {
        line::tessellate(points, nullptr, tolerance, out_segments);
        return;
    }

    for (u32 i = 0; i < pp->segment_count && i + 1 < points.size(); ++i) {
        std::vector<math::v3> seg_points = {points[i], points[i + 1]};
        std::vector<math::v3> seg_out;

        if (pp->segment_types[i] == SegmentType::Arc && i + 2 < points.size()) {
            // Arc segment: need 3 points (use next point as through-point)
            // For polyline arcs, we use the endpoint and a mid-control point
            // Simple approach: treat as line for now, full arc support in Phase 4
            seg_points = {points[i], points[i + 1]};
            line::tessellate(seg_points, nullptr, tolerance, seg_out);
        } else {
            line::tessellate(seg_points, nullptr, tolerance, seg_out);
        }

        if (i > 0 && !seg_out.empty()) seg_out.erase(seg_out.begin()); // Avoid duplicates
        out_segments.insert(out_segments.end(), seg_out.begin(), seg_out.end());
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
    for (u32 i = 0; i + 1 < segments.size(); ++i)
        len += geometry::length(segments[i + 1] - segments[i]);
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

} // namespace primal::geometry::algorithms::polyline
