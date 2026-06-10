#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace primal::geometry::algorithms::arc {

struct ArcParams {
    f32  radius{0.0f};
    f32  start_angle{0.0f};
    f32  end_angle{0.0f};
    bool three_point{true}; // true = 3-point definition in control_points
};

// Compute arc center and radius from 3 points
inline bool compute_arc_from_3_points(const math::v3& p0, const math::v3& p1, const math::v3& p2,
                                      math::v3& out_center, f32& out_radius,
                                      f32& out_start_angle, f32& out_end_angle) {
    // Midpoints
    math::v3 mid01 = (p0 + p1) * 0.5f;
    math::v3 mid12 = (p1 + p2) * 0.5f;
    // Direction vectors (work in XY plane)
    math::v3 d01 = p1 - p0;
    math::v3 d12 = p2 - p1;
    // Perpendicular bisectors in XY plane
    f32 det = -d01.x * d12.y + d01.y * d12.x; // cross product Z component
    if (std::abs(det) < 1e-6f) return false; // collinear
    f32 t = ((mid12.x - mid01.x) * (-d12.y) - (mid12.y - mid01.y) * (-d12.x)) / det;
    out_center.x = mid01.x + t * (-d01.y);
    out_center.y = mid01.y + t * d01.x;
    out_center.z = p0.z; // use first point's Z (work plane)
    out_radius = geometry::length(p0 - out_center);
    out_start_angle = std::atan2(p0.y - out_center.y, p0.x - out_center.x);
    out_end_angle   = std::atan2(p2.y - out_center.y, p2.x - out_center.x);
    return true;
}

inline void tessellate(const std::vector<math::v3>& points,
                       const void* params,
                       f32 tolerance,
                       std::vector<math::v3>& out_segments) {
    out_segments.clear();
    auto* ap = static_cast<const ArcParams*>(params);

    math::v3 center{};
    f32 radius = 0.0f, start_angle = 0.0f, end_angle = 0.0f;

    if (ap->three_point && points.size() >= 3) {
        if (!compute_arc_from_3_points(points[0], points[1], points[2],
                                       center, radius, start_angle, end_angle)) {
            out_segments = points; // Degenerate: treat as straight segments
            return;
        }
    } else {
        // Parametric: center is midpoint of endpoints, use provided radius/angles
        center = (points[0] + points[1]) * 0.5f;
        radius = ap->radius;
        start_angle = ap->start_angle;
        end_angle = ap->end_angle;
    }

    if (radius < 1e-6f) { out_segments = points; return; }

    // Compute arc sweep
    f32 sweep = end_angle - start_angle;
    // Ensure we go the short way for 3-point arcs
    if (ap->three_point) {
        // Check if the middle point is on the arc going the right direction
        f32 mid_angle = std::atan2(points[1].y - center.y, points[1].x - center.x);
        f32 diff = mid_angle - start_angle;
        while (diff > math::pi) diff -= 2.0f * math::pi;
        while (diff < -math::pi) diff += 2.0f * math::pi;
        if (diff < 0) { sweep = -(2.0f * math::pi - std::abs(sweep)); }
    }
    if (std::abs(sweep) < 1e-6f) sweep = 2.0f * math::pi; // Full circle

    // Subdivision: max chord error = tolerance
    f32 max_angle_step = 2.0f * std::acos(std::max(-1.0f, 1.0f - tolerance / radius));
    if (max_angle_step < 0.01f) max_angle_step = 0.01f;
    u32 num_steps = std::max(2u, static_cast<u32>(std::ceil(std::abs(sweep) / max_angle_step)));

    out_segments.reserve(num_steps + 1);
    for (u32 i = 0; i <= num_steps; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(num_steps);
        f32 angle = start_angle + sweep * t;
        out_segments.push_back(math::v3{
            center.x + radius * std::cos(angle),
            center.y + radius * std::sin(angle),
            center.z
        });
    }
}

inline f32 distance_to(const std::vector<math::v3>& points,
                       const void* params,
                       const math::v3& world_pos) {
    // Tessellate and compute segment distances
    std::vector<math::v3> segments;
    tessellate(points, params, 0.01f, segments);
    f32 min_dist = FLT_MAX;
    for (u32 i = 0; i + 1 < segments.size(); ++i) {
        math::v3 ab = segments[i + 1] - segments[i];
        math::v3 ap = world_pos - segments[i];
        f32 denom = geometry::dot(ab, ab);
        f32 t = (denom > 1e-8f) ? geometry::dot(ap, ab) / denom : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        math::v3 closest = segments[i] + ab * t;
        min_dist = std::min(min_dist, geometry::length(world_pos - closest));
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
    for (u32 i = 0; i + 1 < segments.size(); ++i) {
        total_len += geometry::length(segments[i + 1] - segments[i]);
    }
    if (total_len < 1e-6f) return segments[0];
    f32 target = t * total_len;
    f32 accumulated = 0.0f;
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

} // namespace primal::geometry::algorithms::arc
