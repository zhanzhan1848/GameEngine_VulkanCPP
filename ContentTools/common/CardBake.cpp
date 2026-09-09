#include "CardBake.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace primal::tools {
namespace card_bake {
namespace {

// math::v3 按轴下标取分量（simd::float3 与可移植结构体通用）。
inline f32 comp(const math::v3& v, u32 i)
{
    return (i == 0) ? v.x : (i == 1) ? v.y : v.z;
}

}  // namespace

void bake(const OccupancyGrid& grid, const Params& params, data& out)
{
    out.clear();
    const u32 res = grid.resolution;
    if (grid.voxels == nullptr || res < 4) return;
    const size_t voxel_total = (size_t)res * res * res;
    (void)voxel_total;
    // 步长防零（退化网格单轴尺寸可为 0）。
    const f32 step[3] = {
        comp(grid.voxel_size, 0) > 1e-9f ? comp(grid.voxel_size, 0) : 1e-6f,
        comp(grid.voxel_size, 1) > 1e-9f ? comp(grid.voxel_size, 1) : 1e-6f,
        comp(grid.voxel_size, 2) > 1e-9f ? comp(grid.voxel_size, 2) : 1e-6f,
    };

    // 紧致包围（体素单位）：全部非空体素的逐轴 min/max。
    s32 bmin[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    s32 bmax[3] = { -1, -1, -1 };
    for (u32 z = 0; z < res; ++z)
    for (u32 y = 0; y < res; ++y) {
        const u8* row = grid.voxels + ((size_t)z * res + y) * res;
        for (u32 x = 0; x < res; ++x) {
            if (!row[x]) continue;
            bmin[0] = std::min(bmin[0], (s32)x); bmax[0] = std::max(bmax[0], (s32)x);
            bmin[1] = std::min(bmin[1], (s32)y); bmax[1] = std::max(bmax[1], (s32)y);
            bmin[2] = std::min(bmin[2], (s32)z); bmax[2] = std::max(bmax[2], (s32)z);
        }
    }
    if (bmax[0] < 0) return;   // 全空占据 → 运行时 fallback

    // ---- 6 向卡片：每向紧致投影包围盒 → center/extent --------------------
    // 面壳以体素中心标记表面（距表面 < 半体素对角），实际表面可超出最外
    // 占据中心约 1 个体素——每侧外垫 1 体素（span = (cells + 2) * step），
    // center 不变。
    f32 areas[3] = { 0.f, 0.f, 0.f };
    f32 max_area = 0.f;
    for (u32 axis = 0; axis < 3; ++axis) {
        const u32 u = (axis == 0) ? 1u : 0u;
        const u32 v = (axis == 2) ? 1u : 2u;
        const f32 fu = (f32)(bmax[u] - bmin[u] + 1) * step[u];
        const f32 fv = (f32)(bmax[v] - bmin[v] + 1) * step[v];
        areas[axis] = fu * fv;
        max_area = std::max(max_area, areas[axis]);
    }

    for (u32 axis = 0; axis < 3; ++axis) {
        if (areas[axis] < params.min_view_area) continue;
        if (areas[axis] < max_area * params.min_view_fraction) continue;

        for (u32 dir = 0; dir < 2; ++dir) {
            card c{};
            for (u32 k = 0; k < 3; ++k) {
                const s32 cells = bmax[k] - bmin[k] + 1;
                c.center[k] = comp(grid.grid_min, k)
                            + ((f32)bmin[k] + (f32)cells * 0.5f) * step[k];
                c.extent[k] = (f32)(cells + 2) * step[k] * 0.5f;
            }
            c.axis_direction = axis | (dir << 8);

            // 分辨率提示：紧致投影大边全尺寸 × texels/unit，钳 [min,max]
            //（运行时仍按页取整，公式单一来源在 RebuildCardAllocation）。
            const u32 u = (axis == 0) ? 1u : 0u;
            const u32 v = (axis == 2) ? 1u : 2u;
            const f32 proj_size = 2.0f * std::max(c.extent[u], c.extent[v]);
            f32 res_hint = proj_size * params.texels_per_unit;
            res_hint = std::clamp(res_hint, (f32)params.min_resolution, (f32)params.max_resolution);
            c.resolution = (u32)res_hint;

            out.cards.push_back(c);
        }
    }
    if (out.cards.empty()) return;

    // ---- Surfel：每个非空体素朝 6 向暴露面生成 ---------------------------
    if (params.generate_surfels) {
        const f32 area_xy = step[0] * step[1];
        const f32 area_xz = step[0] * step[2];
        const f32 area_yz = step[1] * step[2];

        auto voxel_at = [&](s32 x, s32 y, s32 z) -> u8 {
            if (x < 0 || y < 0 || z < 0) return 0;
            if (x >= (s32)res || y >= (s32)res || z >= (s32)res) return 0;
            return grid.voxels[((size_t)z * res + y) * res + x];
        };

        const f32 face_dirs[6][3] = {
            { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f },
            { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f },
            { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f },
        };
        for (s32 z = 0; z < (s32)res; ++z)
        for (s32 y = 0; y < (s32)res; ++y)
        for (s32 x = 0; x < (s32)res; ++x) {
            if (!voxel_at(x, y, z)) continue;
            const math::v3 center = grid.grid_min
                + grid.voxel_size * (math::v3{ (f32)x, (f32)y, (f32)z } + 0.5f);
            const f32 half[3] = { step[0] * 0.5f, step[1] * 0.5f, step[2] * 0.5f };

            for (const auto& n : face_dirs) {
                if (voxel_at(x + (s32)n[0], y + (s32)n[1], z + (s32)n[2])) continue;
                surfel s{};
                s.pos[0] = center.x + n[0] * half[0];
                s.pos[1] = center.y + n[1] * half[1];
                s.pos[2] = center.z + n[2] * half[2];
                s.area = (n[0] != 0.f) ? area_yz : (n[1] != 0.f) ? area_xz : area_xy;
                s.normal[0] = n[0]; s.normal[1] = n[1]; s.normal[2] = n[2];
                s.pad = 0;
                out.surfels.push_back(s);
            }
        }
    }
}

}  // namespace card_bake
}  // namespace primal::tools
