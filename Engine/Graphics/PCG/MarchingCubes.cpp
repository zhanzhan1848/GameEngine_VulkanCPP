#include "Graphics/PCG/MarchingCubes.h"

#include <cmath>
#include <unordered_map>
#include <cstdint>

namespace primal::graphics::pcg {

namespace {

// 8 corner offsets within a unit cell. Bit position in the mask = corner index.
// Convention:
//   corner 0: (0,0,0)  corner 1: (1,0,0)
//   corner 2: (1,0,1)  corner 3: (0,0,1)
//   corner 4: (0,1,0)  corner 5: (1,1,0)
//   corner 6: (1,1,1)  corner 7: (0,1,1)
constexpr int8_t kCornerOffset[8][3] = {
    {0,0,0}, {1,0,0}, {1,0,1}, {0,0,1},
    {0,1,0}, {1,1,0}, {1,1,1}, {0,1,1},
};

// 12 edges of a cell, each as (corner_a, corner_b).
constexpr int8_t kCellEdges[12][2] = {
    {0,1}, {1,2}, {2,3}, {3,0},  // bottom face (y=0)
    {4,5}, {5,6}, {6,7}, {7,4},  // top face    (y=1)
    {0,4}, {1,5}, {2,6}, {3,7},  // verticals
};

// For each grid edge (along axis a ∈ {0,1,2}), the 4 surrounding cells share it.
// Given the grid vertex at corner of cell (i,j,k) and edge direction axis,
// the 4 cells are at offsets in the 2 perpendicular axes.
//
// For edge along +X starting at grid vertex (i,j,k):
//   4 surrounding cells (varying y,z; x = i, since cell (i,*) has +X edge from (i,*,*) to (i+1,*,*)):
//     cell(i, j-1, k-1), cell(i, j, k-1), cell(i, j-1, k), cell(i, j, k)
//
// We order them counter-clockwise around the edge (looking down the edge from -a to +a)
// so that emitted triangles have consistent winding.
//
// Index into perpendicular axis pair:
//   axis 0 (X): perpendicular = (1,2) = (Y,Z)
//   axis 1 (Y): perpendicular = (2,0) = (Z,X)
//   axis 2 (Z): perpendicular = (0,1) = (X,Y)
constexpr int8_t kPerpAxes[3][2] = {
    {1, 2},  // axis X → perp Y, Z
    {2, 0},  // axis Y → perp Z, X
    {0, 1},  // axis Z → perp X, Y
};

inline math::v3 GridIndexToWorld(u32 i, u32 j, u32 k,
                                  const math::v3& origin,
                                  const math::v3& voxel) {
    return math::v3{
        origin.x + voxel.x * static_cast<f32>(i),
        origin.y + voxel.y * static_cast<f32>(j),
        origin.z + voxel.z * static_cast<f32>(k),
    };
}

} // namespace

MarchingCubesResult GenerateSurfaceNetsCPU(
    const PCGField& field,
    const math::v3& bounds_min,
    const math::v3& bounds_max,
    u32 resolution,
    f32 iso_value) {

    MarchingCubesResult result;

    if (resolution < 2 || resolution > 256) return result;

    const math::v3 extent{
        bounds_max.x - bounds_min.x,
        bounds_max.y - bounds_min.y,
        bounds_max.z - bounds_min.z,
    };
    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f) return result;

    const math::v3 voxel{
        extent.x / static_cast<f32>(resolution),
        extent.y / static_cast<f32>(resolution),
        extent.z / static_cast<f32>(resolution),
    };

    const u32 n = resolution + 1;  // grid vertices per axis
    const u32 n2 = n * n;
    const u32 n3 = n * n * n;

    // --- Sample grid vertices: scalar[i + n*j + n2*k] ---
    std::vector<f32> scalar(n3);
    for (u32 k = 0; k < n; ++k) {
        for (u32 j = 0; j < n; ++j) {
            for (u32 i = 0; i < n; ++i) {
                math::v3 p = GridIndexToWorld(i, j, k, bounds_min, voxel);
                scalar[i + n * j + n2 * k] = field.SampleFloat(p);
            }
        }
    }

    // --- Per-cell: compute mask, emit dual vertex if straddling ---
    // Cell (i,j,k) has corners at grid vertices (i+c.x, j+c.y, k+c.z) for c in kCornerOffset.
    // dual_id[i + res*j + res^2*k] = index into result.positions, or u32(-1) if not straddling.
    std::vector<u32> dual_id(static_cast<size_t>(resolution) * resolution * resolution, u32(-1));

    auto sample_at = [&](u32 gi, u32 gj, u32 gk) -> f32 {
        return scalar[gi + n * gj + n2 * gk];
    };

    for (u32 k = 0; k < resolution; ++k) {
        for (u32 j = 0; j < resolution; ++j) {
            for (u32 i = 0; i < resolution; ++i) {
                // Corner samples (8) and positions
                f32 cv[8];
                math::v3 cp[8];
                u8 mask = 0;
                for (u8 c = 0; c < 8; ++c) {
                    u32 gi = i + kCornerOffset[c][0];
                    u32 gj = j + kCornerOffset[c][1];
                    u32 gk = k + kCornerOffset[c][2];
                    cv[c] = sample_at(gi, gj, gk);
                    cp[c] = GridIndexToWorld(gi, gj, gk, bounds_min, voxel);
                    if (cv[c] > iso_value) mask |= (1u << c);
                }

                if (mask == 0 || mask == 0xFF) continue;  // not straddling

                // Average edge crossings → dual vertex position
                f32 sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
                u32 count = 0;
                for (u32 e = 0; e < 12; ++e) {
                    u8 a = kCellEdges[e][0];
                    u8 b = kCellEdges[e][1];
                    bool a_in = (mask >> a) & 1u;
                    bool b_in = (mask >> b) & 1u;
                    if (a_in == b_in) continue;
                    // Edge crosses surface; interpolate
                    f32 denom = cv[b] - cv[a];
                    f32 t = (denom != 0.0f) ? (iso_value - cv[a]) / denom : 0.5f;
                    t = std::clamp(t, 0.0f, 1.0f);
                    sum_x += cp[a].x + t * (cp[b].x - cp[a].x);
                    sum_y += cp[a].y + t * (cp[b].y - cp[a].y);
                    sum_z += cp[a].z + t * (cp[b].z - cp[a].z);
                    ++count;
                }
                if (count == 0) continue;

                f32 inv_count = 1.0f / static_cast<f32>(count);
                math::v3 dual_pos{sum_x * inv_count, sum_y * inv_count, sum_z * inv_count};

                // Normal via central differences (step = 1.0 × voxel)
                f32 gx = field.SampleFloat({dual_pos.x + voxel.x, dual_pos.y, dual_pos.z})
                       - field.SampleFloat({dual_pos.x - voxel.x, dual_pos.y, dual_pos.z});
                f32 gy = field.SampleFloat({dual_pos.x, dual_pos.y + voxel.y, dual_pos.z})
                       - field.SampleFloat({dual_pos.x, dual_pos.y - voxel.y, dual_pos.z});
                f32 gz = field.SampleFloat({dual_pos.x, dual_pos.y, dual_pos.z + voxel.z})
                       - field.SampleFloat({dual_pos.x, dual_pos.y, dual_pos.z - voxel.z});
                f32 gl = std::sqrt(gx*gx + gy*gy + gz*gz) + 1e-12f;
                // Outward normal = +∇field. Empirically determined: the engine's
                // forward shader expects normals pointing toward increasing field
                // value (matching SDF convention where field grows away from the
                // surface). The earlier -∇field assumption produced a fully-black
                // surface because every vertex normal ended up pointing away from
                // the scene's dominant light direction.
                math::v3 normal{gx / gl, gy / gl, gz / gl};

                // Y-planar UV
                f32 u = (dual_pos.x - bounds_min.x) / extent.x;
                f32 v = (dual_pos.z - bounds_min.z) / extent.z;

                u32 id = static_cast<u32>(result.positions.size() / 3);
                result.positions.push_back(dual_pos.x);
                result.positions.push_back(dual_pos.y);
                result.positions.push_back(dual_pos.z);
                result.normals.push_back(normal.x);
                result.normals.push_back(normal.y);
                result.normals.push_back(normal.z);
                result.uvs.push_back(u);
                result.uvs.push_back(v);

                dual_id[i + resolution * j + resolution * resolution * k] = id;
            }
        }
    }

    // --- Face generation: per grid edge ---
    // For each interior grid edge along axis a, if surface crosses it
    // (endpoint signs differ), gather up to 4 surrounding cells' dual vertices
    // and emit a quad (or triangle in degenerate cases).
    auto cell_straddles = [&](u32 i, u32 j, u32 k) -> bool {
        if (i >= resolution || j >= resolution || k >= resolution) return false;
        return dual_id[i + resolution * j + resolution * resolution * k] != u32(-1);
    };
    auto cell_dual = [&](u32 i, u32 j, u32 k) -> u32 {
        return dual_id[i + resolution * j + resolution * resolution * k];
    };

    auto emit_face = [&](const u32 cell_ids[4], int valid_count) {
        // Triangulate the polygon formed by the valid dual vertices.
        // For 4 valid: emit 2 triangles (quad). For 3 valid: emit 1 triangle.
        // For ≤2 valid: skip.
        if (valid_count < 3) return;
        // Extract valid vertex ids into a compact array (CCW order preserved).
        u32 v[4];
        int n_valid = 0;
        for (int s = 0; s < 4; ++s) {
            if (cell_ids[s] != u32(-1)) v[n_valid++] = cell_ids[s];
        }
        if (n_valid < 3) return;
        // Fan triangulation
        for (int t = 1; t < n_valid - 1; ++t) {
            result.indices.push_back(v[0]);
            result.indices.push_back(v[t]);
            result.indices.push_back(v[t + 1]);
        }
    };

    // Iterate interior grid vertices and 3 axes
    for (u32 axis = 0; axis < 3; ++axis) {
        u32 a0 = axis;                 // edge axis
        u32 p1 = kPerpAxes[axis][0];   // first perpendicular axis
        u32 p2 = kPerpAxes[axis][1];   // second perpendicular axis

        // Iterate cells; the edge starts at grid vertex (i,j,k) and goes +a0.
        // The 4 surrounding cells are (i,j,k), (i-1 along p1, ...), etc.
        // Index by (i, j, k) → grid vertex. Cell(i,j,k) owns grid vertex (i,j,k) as its (0,0,0) corner.
        // 4 surrounding cells share the edge from (i,j,k) to (i,j,k)+e_{a0}:
        //   cell at (i - e_{a0}*0, ...) — actually let's parametrize directly.

        for (u32 k = 0; k < n; ++k) {
            for (u32 j = 0; j < n; ++j) {
                for (u32 i = 0; i < n; ++i) {
                    // Grid vertex (i,j,k). Edge endpoint +1 along a0.
                    u32 gp[3] = {i, j, k};
                    if (gp[a0] + 1 >= n) continue;  // edge would exit grid
                    u32 gp2[3] = {i, j, k};
                    gp2[a0] += 1;

                    f32 s1 = scalar[gp[0] + n * gp[1] + n2 * gp[2]];
                    f32 s2 = scalar[gp2[0] + n * gp2[1] + n2 * gp2[2]];
                    bool sign_diff = (s1 > iso_value) != (s2 > iso_value);
                    if (!sign_diff) continue;

                    // 4 surrounding cells: offset by -1 in perpendicular axes (so that the
                    // cell's (0,0,0) corner = grid vertex (i,j,k) is one of the 4 cells).
                    // Cells share the edge gp→gp2. Their (a0) coordinate = i (fixed).
                    // Their (p1, p2) coords are either 0 or -1 offset from (gp[p1], gp[p2]).
                    //
                    // Counter-clockwise winding around the edge (looking from gp toward gp2):
                    //   cell at (p1=0, p2=0)  → cell_a
                    //   cell at (p1=-1, p2=0) → cell_b
                    //   cell at (p1=-1, p2=-1)→ cell_c
                    //   cell at (p1=0, p2=-1) → cell_d
                    u32 cell_idx[3];
                    cell_idx[a0] = gp[a0];
                    cell_idx[p1] = gp[p1];
                    cell_idx[p2] = gp[p2];

                    auto make_cell = [&](int dp1, int dp2) -> u32 {
                        u32 ci[3] = {cell_idx[0], cell_idx[1], cell_idx[2]};
                        ci[p1] = (dp1 < 0 && ci[p1] == 0) ? u32(-1) : ci[p1] + dp1;
                        ci[p2] = (dp2 < 0 && ci[p2] == 0) ? u32(-1) : ci[p2] + dp2;
                        if (ci[0] == u32(-1) || ci[1] == u32(-1) || ci[2] == u32(-1)) return u32(-1);
                        if (ci[0] >= resolution || ci[1] >= resolution || ci[2] >= resolution) return u32(-1);
                        return cell_dual(ci[0], ci[1], ci[2]);
                    };

                    u32 ids[4] = {
                        make_cell( 0,  0),
                        make_cell(-1,  0),
                        make_cell(-1, -1),
                        make_cell( 0, -1),
                    };

                    int valid_count = 0;
                    for (int s = 0; s < 4; ++s) if (ids[s] != u32(-1)) valid_count++;
                    emit_face(ids, valid_count);
                }
            }
        }
    }

    return result;
}

} // namespace primal::graphics::pcg
