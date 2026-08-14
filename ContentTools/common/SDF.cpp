#include "SDF.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

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

void generate_sdf(mesh& m, u32 resolution) {
    // Clamp caller-supplied resolution: 0 = "use default". Upper bound 256
    // keeps memory sane (res³ cells × 11 bytes ≈ 190MB at the cap).
    if (resolution == 0) resolution = 32;
    resolution = std::clamp(resolution, 4u, 256u);

    const u32 res = resolution;
    m.sdf.resolution[0] = res;
    m.sdf.resolution[1] = res;
    m.sdf.resolution[2] = res;

    math::v3 min_b{ FLT_MAX, FLT_MAX, FLT_MAX };
    math::v3 max_b{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (const auto& v : m.vertices) {
        // Skip NaN/Inf positions (degenerate post-remesh vertices) so they
        // don't poison the bbox. std::min/max with NaN propagates NaN to
        // the result, which later triggers pack_float asserts downstream.
        if (!std::isfinite(v.position.x) ||
            !std::isfinite(v.position.y) ||
            !std::isfinite(v.position.z)) continue;
        min_b = Min(min_b, v.position);
        max_b = Max(max_b, v.position);
    }

    // If every vertex was non-finite, fall back to a unit box.
    if (min_b.x == FLT_MAX) { min_b = {0,0,0}; max_b = {1,1,1}; }

    math::v3 size = max_b - min_b;
    f32 max_dim = std::max({ size.x, size.y, size.z });
    if (!std::isfinite(max_dim) || max_dim < 0.0001f) max_dim = 1.0f;
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

    // ---- Build uniform spatial grid for triangle acceleration ----
    // The SDF voxel grid is res³ over [min_b, max_b]. We build a parallel
    // triangle grid at the same resolution: each triangle is inserted into
    // every cell its AABB overlaps. A voxel then only tests triangles in its
    // own cell + a ring of neighbors, reducing inner work from O(tris) to
    // O(local tris) — typically 100-1000× faster for large meshes.
    struct Grid {
        math::v3 origin;
        math::v3 inv_step;  // 1/step per axis
        std::vector<std::vector<u32>> cells;  // res³, each holds triangle start-indices (i.e. index into m.indices, multiple of 3)
    } grid;
    grid.origin = min_b;
    // Guard against zero-size axes (degenerate/flat mesh): step could be 0
    // on one axis, making inv_step = inf and poisoning all cell computations.
    grid.inv_step = {
        step.x > 1e-20f ? 1.f / step.x : (f32)res,
        step.y > 1e-20f ? 1.f / step.y : (f32)res,
        step.z > 1e-20f ? 1.f / step.z : (f32)res
    };
    grid.cells.resize((size_t)res * res * res);

    const u32 num_tris = num_indices / 3;
    for (u32 t = 0; t < num_tris; ++t) {
        const u32 i0 = m.indices[t * 3 + 0];
        const u32 i1 = m.indices[t * 3 + 1];
        const u32 i2 = m.indices[t * 3 + 2];
        const math::v3& v0 = m.vertices[i0].position;
        const math::v3& v1 = m.vertices[i1].position;
        const math::v3& v2 = m.vertices[i2].position;
        // Triangle AABB → grid-cell range (per-axis).
        // Use int (not u32) for floor result: negative coordinates (triangle
        // outside padded bbox, which shouldn't happen but can with degenerate
        // verts) would underflow u32 and cause OOB writes.
        auto cell_range = [&](f32 lo, f32 hi, f32 origin, f32 inv_s)
            -> std::pair<u32, u32> {
            f32 fc0 = (lo - origin) * inv_s;
            f32 fc1 = (hi - origin) * inv_s;
            long c0 = (long)std::floor(std::min(fc0, fc1));
            long c1 = (long)std::floor(std::max(fc0, fc1));
            if (c0 < 0) c0 = 0;
            if (c1 < 0) c1 = 0;
            if (c0 >= (long)res) c0 = res - 1;
            if (c1 >= (long)res) c1 = res - 1;
            return { (u32)c0, (u32)c1 };
        };
        auto [cx0, cx1] = cell_range(std::min({v0.x, v1.x, v2.x}),
                                      std::max({v0.x, v1.x, v2.x}),
                                      grid.origin.x, grid.inv_step.x);
        auto [cy0, cy1] = cell_range(std::min({v0.y, v1.y, v2.y}),
                                      std::max({v0.y, v1.y, v2.y}),
                                      grid.origin.y, grid.inv_step.y);
        auto [cz0, cz1] = cell_range(std::min({v0.z, v1.z, v2.z}),
                                      std::max({v0.z, v1.z, v2.z}),
                                      grid.origin.z, grid.inv_step.z);
        for (u32 cz = cz0; cz <= cz1; ++cz)
        for (u32 cy = cy0; cy <= cy1; ++cy)
        for (u32 cx = cx0; cx <= cx1; ++cx) {
            grid.cells[((size_t)cz * res + cy) * res + cx].push_back(t * 3);
        }
    }

    // Timestamp stamp to avoid double-testing triangles shared across cells.
    std::vector<u32> tri_stamp(num_tris, 0);
    u32 current_stamp = 0;

    for (u32 z = 0; z < res; ++z) {
        for (u32 y = 0; y < res; ++y) {
            for (u32 x = 0; x < res; ++x) {
                math::v3 p = min_b + step * (math::v3{ (f32)x, (f32)y, (f32)z } + 0.5f);
                f32 min_dist_sq = FLT_MAX;
                math::v3 closest_p = p;

                ++current_stamp;
                // Search 3³ neighborhood of cells around (x,y,z).
                const u32 x0 = x > 0 ? x - 1 : 0;
                const u32 y0 = y > 0 ? y - 1 : 0;
                const u32 z0 = z > 0 ? z - 1 : 0;
                const u32 x1 = x < res - 1 ? x + 1 : res - 1;
                const u32 y1 = y < res - 1 ? y + 1 : res - 1;
                const u32 z1 = z < res - 1 ? z + 1 : res - 1;

                for (u32 gz = z0; gz <= z1; ++gz)
                for (u32 gy = y0; gy <= y1; ++gy)
                for (u32 gx = x0; gx <= x1; ++gx) {
                    const auto& cell_tris = grid.cells[((size_t)gz * res + gy) * res + gx];
                    for (u32 i : cell_tris) {
                        const u32 tri_idx = i / 3;
                        if (tri_stamp[tri_idx] == current_stamp) continue;
                        tri_stamp[tri_idx] = current_stamp;

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
                }

                f32 dist = std::sqrt(min_dist_sq);
                if (!std::isfinite(dist) || dist > max_dim) dist = max_dim;
                u32 idx = z * res * res + y * res + x;

                // Inside/outside test via +X ray casting across ALL triangles.
                u32 intersectionCount = 0;
                for (u32 i = 0; i < num_indices; i += 3) {
                    math::v3 v0 = m.vertices[m.indices[i]].position;
                    math::v3 v1 = m.vertices[m.indices[i + 1]].position;
                    math::v3 v2 = m.vertices[m.indices[i + 2]].position;
                    math::v3 edge1 = v1 - v0;
                    math::v3 edge2 = v2 - v0;
                    math::v3 rayDir{1.0f, 0.0f, 0.0f};
                    math::v3 h = Cross(rayDir, edge2);
                    f32 a = Dot(edge1, h);
                    if (a > -1e-10f && a < 1e-10f) continue;
                    f32 f = 1.0f / a;
                    math::v3 s = p - v0;
                    f32 u = f * Dot(s, h);
                    if (u < 0.0f || u > 1.0f) continue;
                    math::v3 q = Cross(s, edge1);
                    f32 vRay = f * Dot(rayDir, q);
                    if (vRay < 0.0f || u + vRay > 1.0f) continue;
                    f32 t_ray = f * Dot(edge2, q);
                    if (t_ray > 1e-6f) intersectionCount++;
                }
                bool inside = (intersectionCount % 2 == 1);
                f32 signedDist = inside ? -dist : dist;
                m.sdf.data[idx] = math::pack_float<16>(signedDist + max_dim, 0.0f, max_dim * 2.0f);

                f32 voxel_diag = Length(step);
                m.sdf.voxels[idx] = (dist < voxel_diag * 0.5f) ? 255 : 0;

                math::v3 to_closest = closest_p - p;
                auto clamp_field = [max_dim](f32 v) -> f32 {
                    if (!std::isfinite(v)) return 0.f;
                    if (v < -max_dim) v = -max_dim;
                    if (v >  max_dim) v =  max_dim;
                    return v;
                };
                u32 vec_idx = idx * 4;
                m.sdf.vector_field[vec_idx + 0] = math::pack_float<16>(clamp_field(to_closest.x), -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 1] = math::pack_float<16>(clamp_field(to_closest.y), -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 2] = math::pack_float<16>(clamp_field(to_closest.z), -max_dim, max_dim);
                m.sdf.vector_field[vec_idx + 3] = 0;
            }
        }
    }
}

}  // namespace primal::tools
