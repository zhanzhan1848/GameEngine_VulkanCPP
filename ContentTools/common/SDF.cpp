#include "SDF.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "../Engine/Graphics/RHI/Core/RHIMath.h"

namespace primal::tools {
namespace {

using namespace primal::math;
using namespace primal::graphics::rhi::math;

// Per-component min/max — Geometry.cpp has identical helpers in its own
// anonymous namespace; duplicating here avoids coupling SDF.cpp to that file.
inline v3 Min(const v3& a, const v3& b) {
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
}

inline v3 Max(const v3& a, const v3& b) {
    return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
}

// Closest point on triangle abc to point p. Ericson, Real-Time Collision
// Detection. Identical to the copy in Geometry.cpp's anonymous namespace;
// kept file-local to avoid leaking into the public primal::tools API.
math::v3 closest_point_triangle(const math::v3& p, const math::v3& a,
                                const math::v3& b, const math::v3& c) {
    math::v3 ab = b - a;
    math::v3 ac = c - a;
    math::v3 ap = p - a;
    f32 d1 = dot(ab, ap);
    f32 d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    math::v3 bp = p - b;
    f32 d3 = dot(ab, bp);
    f32 d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        f32 v = d1 / (d1 - d3);
        return a + ab * v;
    }

    math::v3 cp = p - c;
    f32 d5 = dot(ab, cp);
    f32 d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        f32 w = d2 / (d2 - d6);
        return a + ac * w;
    }

    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }

    f32 denom = 1.0f / (va + vb + vc);
    f32 v = vb * denom;
    f32 w = vc * denom;
    return a + ab * v + ac * w;
}

}  // namespace
}  // namespace primal::tools

namespace primal::tools {

void generate_sdf(mesh& m) {
    constexpr u32 res = 32;
    m.sdf.resolution[0] = res;
    m.sdf.resolution[1] = res;
    m.sdf.resolution[2] = res;

    math::v3 min_b{ FLT_MAX, FLT_MAX, FLT_MAX };
    math::v3 max_b{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (const auto& v : m.vertices) {
        min_b = Min(min_b, v.position);
        max_b = Max(max_b, v.position);
    }

    math::v3 size = max_b - min_b;
    f32 max_dim = std::max({ size.x, size.y, size.z });
    if (max_dim < 0.0001f) max_dim = 1.0f;
    f32 padding = max_dim * 0.2f;
    min_b -= padding;
    max_b += padding;
    size = max_b - min_b;

    m.sdf.bounds_min[0] = min_b.x;
    m.sdf.bounds_min[1] = min_b.y;
    m.sdf.bounds_min[2] = min_b.z;
    m.sdf.bounds_max[0] = max_b.x;
    m.sdf.bounds_max[1] = max_b.y;
    m.sdf.bounds_max[2] = max_b.z;

    m.sdf.data.resize(res * res * res);
    m.sdf.voxels.resize(res * res * res);
    m.sdf.vector_field.resize(res * res * res * 4);

    math::v3 step = size / (f32)res;

    const u32 num_indices = (u32)m.indices.size();

    for (u32 z = 0; z < res; ++z) {
        for (u32 y = 0; y < res; ++y) {
            for (u32 x = 0; x < res; ++x) {
                math::v3 p = min_b + step * (math::v3{ (f32)x, (f32)y, (f32)z } + 0.5f);
                f32 min_dist_sq = FLT_MAX;
                math::v3 closest_p = p;

                for (u32 i = 0; i < num_indices; i += 3) {
                    math::v3 v0 = m.vertices[m.indices[i]].position;
                    math::v3 v1 = m.vertices[m.indices[i + 1]].position;
                    math::v3 v2 = m.vertices[m.indices[i + 2]].position;

                    math::v3 cp = closest_point_triangle(p, v0, v1, v2);
                    f32 d2 = LengthSquared(p - cp);
                    if (d2 < min_dist_sq) {
                        min_dist_sq = d2;
                        closest_p = cp;
                    }
                }

                f32 dist = std::sqrt(min_dist_sq);
                u32 idx = z * res * res + y * res + x;
                m.sdf.data[idx] = math::pack_float<16>(dist, 0.0f, max_dim);

                f32 voxel_diag = Length(step);
                m.sdf.voxels[idx] = (dist < voxel_diag * 0.5f) ? 255 : 0;

                math::v3 to_closest = closest_p - p;
                u32 vec_idx = idx * 4;
                m.sdf.vector_field[vec_idx + 0] = math::pack_float<16>(to_closest.x, -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 1] = math::pack_float<16>(to_closest.y, -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 2] = math::pack_float<16>(to_closest.z, -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 3] = 0;
            }
        }
    }
}

}  // namespace primal::tools
