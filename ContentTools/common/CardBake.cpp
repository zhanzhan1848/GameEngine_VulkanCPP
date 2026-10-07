#include "CardBake.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace primal::tools {
namespace card_bake {
namespace {

// math::v3 按轴下标取分量（simd::float3 与可移植结构体通用）。
inline f32 comp(const math::v3& v, u32 i)
{
    return (i == 0) ? v.x : (i == 1) ? v.y : v.z;
}

// axis (0=X,1=Y,2=Z) → (u,v) 投影平面轴下标（v1/运行时同一映射）。
inline void uv_of(u32 axis, u32& u, u32& v)
{
    u = (axis == 0) ? 1u : 0u;
    v = (axis == 2) ? 1u : 2u;
}

// 步长防零（退化网格单轴尺寸可为 0）。
inline void grid_steps(const OccupancyGrid& grid, f32 step[3])
{
    for (u32 i = 0; i < 3; ++i)
        step[i] = comp(grid.voxel_size, i) > 1e-9f ? comp(grid.voxel_size, i) : 1e-6f;
}

// 暴露面采样：v2 的覆盖评估样本集，同时也是 surfel 输出（同序生成）。
struct Sample
{
    f32 pos[3];
    f32 area;
    u32 cell[3];
    u8 dir;   // 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z
};

// 遍历全部非空体素的 6 向暴露面。generate_surfels 时按 v1 语义同步产出
// surfel（pos = 体素面中心，area = 面积，normal = 单位轴对齐外法线）。
void collect_samples(const OccupancyGrid& grid, const Params& params,
                     std::vector<Sample>& samples, data& out)
{
    const u32 res = grid.resolution;
    f32 step[3];
    grid_steps(grid, step);

    auto voxel_at = [&](s32 x, s32 y, s32 z) -> u8 {
        if (x < 0 || y < 0 || z < 0) return 0;
        if (x >= (s32)res || y >= (s32)res || z >= (s32)res) return 0;
        return grid.voxels[((size_t)z * res + y) * res + x];
    };

    static const f32 face_dirs[6][3] = {
        { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f },
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f },
    };

    const f32 area_xy = step[0] * step[1];
    const f32 area_xz = step[0] * step[2];
    const f32 area_yz = step[1] * step[2];

    for (s32 z = 0; z < (s32)res; ++z)
    for (s32 y = 0; y < (s32)res; ++y)
    for (s32 x = 0; x < (s32)res; ++x) {
        if (!voxel_at(x, y, z)) continue;
        const math::v3 center = grid.grid_min
            + grid.voxel_size * (math::v3{ (f32)x, (f32)y, (f32)z } + 0.5f);
        const f32 half[3] = { step[0] * 0.5f, step[1] * 0.5f, step[2] * 0.5f };

        for (u8 d = 0; d < 6; ++d) {
            const f32* n = face_dirs[d];
            if (voxel_at(x + (s32)n[0], y + (s32)n[1], z + (s32)n[2])) continue;
            Sample s{};
            s.pos[0] = center.x + n[0] * half[0];
            s.pos[1] = center.y + n[1] * half[1];
            s.pos[2] = center.z + n[2] * half[2];
            s.area = (n[0] != 0.f) ? area_yz : (n[1] != 0.f) ? area_xz : area_xy;
            s.cell[0] = (u32)x; s.cell[1] = (u32)y; s.cell[2] = (u32)z;
            s.dir = d;
            samples.push_back(s);

            if (params.generate_surfels) {
                surfel o{};
                o.pos[0] = s.pos[0]; o.pos[1] = s.pos[1]; o.pos[2] = s.pos[2];
                o.area = s.area;
                o.normal[0] = n[0]; o.normal[1] = n[1]; o.normal[2] = n[2];
                o.pad = 0;
                out.surfels.push_back(o);
            }
        }
    }
}

// ============================================================================
// v1：全局紧致包围盒 × 6 居中视图（原 T4.2 算法）。
// ============================================================================

}  // namespace

void bake_v1(const OccupancyGrid& grid, const Params& params, data& out)
{
    out.clear();
    const u32 res = grid.resolution;
    if (grid.voxels == nullptr || res < 4) return;

    f32 step[3];
    grid_steps(grid, step);

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
        u32 u, v;
        uv_of(axis, u, v);
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
            u32 u, v;
            uv_of(axis, u, v);
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
        std::vector<Sample> samples;
        collect_samples(grid, params, samples, out);
    }
}

// ============================================================================
// v2：coverage 驱动的贴面（surface-hugging）局部卡——Lumen Surface Cache
// 对齐（2026-09-19 计划 Phase 1）。
//
//   样本 S = 6 向暴露面采样（面壳体素），面积加权和 = 总表面积
//   候选   = 同向暴露体素的 6-连通块（深度感知：内外表面分离）
//   贴面   = 卡平面放在块迎向侧深度后方 backoff 处（运行时 near=0.05
//            不动，卡面后撤保证所属表面全部落在近平面前方）
//   可见性 = 块内样本沿 -dir 至卡平面体素 march 无占据
//   贪心   = 反复选"新增覆盖面积"最大的候选，直到 coverage 达标 /
//            增益 < 阈值 / 卡数达 per-mesh 上限（默认 12，对齐 UE）
// ============================================================================

void bake_v2(const OccupancyGrid& grid, const Params& params, data& out)
{
    out.clear();
    const u32 res = grid.resolution;
    if (grid.voxels == nullptr || res < 4) return;

    f32 step[3];
    grid_steps(grid, step);

    // per-mesh 卡数上限：env 可调（对齐 UE Max Lumen Mesh Cards）。
    u32 max_cards = params.max_cards;
    if (const char* e = std::getenv("PRIMAL_SC_BAKE_MAX_CARDS_PER_MESH"))
        max_cards = (u32)std::max(1, atoi(e));
    const char* axial_env = std::getenv("PRIMAL_SC_BAKE_FULL_AXIAL_SLAB");
    const bool full_axial_slab = axial_env && std::strcmp(axial_env, "1") == 0;

    // ---- 1. 样本集（+ surfel 输出） --------------------------------------
    std::vector<Sample> samples;
    collect_samples(grid, params, samples, out);
    if (samples.empty()) {
        out.surfels.clear();
        return;
    }
    const size_t n = samples.size();
    f32 total_area = 0.f;
    for (const Sample& s : samples) total_area += s.area;
    if (total_area <= 0.f) {
        out.surfels.clear();
        return;
    }

    // ---- 2. 同向暴露面的分桶连通块（计划：约 4×4×4 体素块分桶 + 块级
    // 6-连通 BFS，深度感知）。裸体素级连通会被 SDF 壳的锯齿边界打成上万
    // 碎片（非立方 bbox 的各向异性步长下壳层恰处临界标记，边界呈分形），
    // 块级分桶把锯齿吸收进块内；卡片包围仍取样本紧致 cell 包围，不随块
    // 膨胀。深度方向同样按块分层——内外表面不因 (u,v) 投影重叠而合并。
    constexpr u32 kInvalid = 0xFFFFFFFFu;
    constexpr u32 kBlockVoxels = 4;
    const u32 block_res = (res + kBlockVoxels - 1) / kBlockVoxels;
    auto block_key = [&](u32 x, u32 y, u32 z) -> u64 {
        return ((u64)(z / kBlockVoxels) * block_res + (y / kBlockVoxels)) * block_res
             + (x / kBlockVoxels);
    };
    auto occupied = [&](s32 x, s32 y, s32 z) -> bool {
        if (x < 0 || y < 0 || z < 0 || x >= (s32)res || y >= (s32)res || z >= (s32)res)
            return false;
        return grid.voxels[((size_t)z * res + y) * res + x] != 0;
    };

    // 样本按 (dir, block) 排序 → 每块一个连续样本段。
    std::vector<u32> order(n);
    for (u32 i = 0; i < n; ++i) order[i] = i;
    auto dir_of_key = [&](u32 i) { return samples[i].dir; };
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const u32 da = dir_of_key(a), db = dir_of_key(b);
        if (da != db) return da < db;
        return block_key(samples[a].cell[0], samples[a].cell[1], samples[a].cell[2])
             < block_key(samples[b].cell[0], samples[b].cell[1], samples[b].cell[2]);
    });
    struct BlockRange
    {
        u32 dir;
        u64 key;
        u32 begin, end;
        f32 dmin, dmax;   // 块内样本沿暴露轴的深度区间
    };
    std::vector<BlockRange> ranges;
    std::unordered_map<u64, u32> range_index[6];
    {
        u32 p = 0;
        while (p < n) {
            const u32 d = samples[order[p]].dir;
            const u32 axis_pd = d >> 1;
            const u64 bk = block_key(samples[order[p]].cell[0],
                                     samples[order[p]].cell[1],
                                     samples[order[p]].cell[2]);
            u32 q = p;
            f32 dmin = 3.402823466e+38f, dmax = -3.402823466e+38f;
            while (q < n && samples[order[q]].dir == d) {
                const u64 k2 = block_key(samples[order[q]].cell[0],
                                         samples[order[q]].cell[1],
                                         samples[order[q]].cell[2]);
                if (k2 != bk) break;
                dmin = std::min(dmin, samples[order[q]].pos[axis_pd]);
                dmax = std::max(dmax, samples[order[q]].pos[axis_pd]);
                ++q;
            }
            range_index[d].emplace(bk, (u32)ranges.size());
            ranges.push_back({ d, bk, p, q, dmin, dmax });
            p = q;
        }
    }

    // 块级 BFS 连通分量。
    std::vector<u32> comp_of(n, kInvalid);
    std::vector<std::vector<u32>> comps;
    std::vector<u8> range_done(ranges.size(), 0);
    static const s32 kDirVec[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (u32 ri = 0; ri < ranges.size(); ++ri) {
        if (range_done[ri]) continue;
        const u32 d = ranges[ri].dir;
        std::vector<u32> comp(order.begin() + ranges[ri].begin,
                              order.begin() + ranges[ri].end);
        for (u32 k = ranges[ri].begin; k < ranges[ri].end; ++k)
            comp_of[order[k]] = (u32)comps.size();
        std::vector<u32> queue;
        range_done[ri] = 1;
        queue.push_back(ri);
        while (!queue.empty()) {
            const u32 cur = queue.back();
            queue.pop_back();
            const u32 bx = (u32)(ranges[cur].key % block_res);
            const u32 by = (u32)((ranges[cur].key / block_res) % block_res);
            const u32 bz = (u32)(ranges[cur].key / ((u64)block_res * block_res));
            for (const auto& dv : kDirVec) {
                const s32 nx = (s32)bx + dv[0];
                const s32 ny = (s32)by + dv[1];
                const s32 nz = (s32)bz + dv[2];
                if (nx < 0 || ny < 0 || nz < 0) continue;
                if (nx >= (s32)block_res || ny >= (s32)block_res || nz >= (s32)block_res) continue;
                const u64 nk = ((u64)nz * block_res + (u32)ny) * block_res + (u32)nx;
                auto it = range_index[d].find(nk);
                if (it == range_index[d].end()) continue;      // 邻块无同向暴露面
                if (range_done[it->second]) continue;          // 已入块
                // 深度门槛：相邻块深度区间不重叠（容差 B−1 体素）时不合并
                // ——错层表面（如凸台立面与基座立面仅隔数体素）各成连通块，
                // 各拿各的贴面卡；否则错层块并卡后，浅面被深面壳体挡在
                // 可见性 march 之外，永远不可捕获。
                const BlockRange& nb = ranges[it->second];
                const f32 tol = (f32)(kBlockVoxels - 1) * step[d >> 1];
                if (ranges[cur].dmin > nb.dmax + tol || nb.dmin > ranges[cur].dmax + tol)
                    continue;
                range_done[it->second] = 1;
                queue.push_back(it->second);
                comp.insert(comp.end(),
                            order.begin() + nb.begin,
                            order.begin() + nb.end);
                for (u32 k = nb.begin; k < nb.end; ++k)
                    comp_of[order[k]] = (u32)comps.size();
            }
        }
        comps.push_back(std::move(comp));
    }

    // ---- 3. 候选卡（每连通块一张）+ 可见性 -------------------------------
    struct Candidate
    {
        card c{};
        std::vector<u32> visible;   // 块内可见样本下标
    };
    std::vector<Candidate> candidates;
    candidates.reserve(comps.size());

    for (auto& members : comps) {
        const u32 d = samples[members[0]].dir;
        const u32 axis = d >> 1;
        const u32 neg = d & 1u;
        u32 uu, vv;
        uv_of(axis, uu, vv);

        // 块的投影 cell 包围 + 深度范围。
        s32 umin = INT32_MAX, umax = -1, vmin = INT32_MAX, vmax = -1;
        f32 dmin = 3.402823466e+38f, dmax = -3.402823466e+38f;
        for (u32 si : members) {
            const Sample& s = samples[si];
            umin = std::min(umin, (s32)s.cell[uu]); umax = std::max(umax, (s32)s.cell[uu]);
            vmin = std::min(vmin, (s32)s.cell[vv]); vmax = std::max(vmax, (s32)s.cell[vv]);
            dmin = std::min(dmin, s.pos[axis]);     // pos 为网格绝对坐标
            dmax = std::max(dmax, s.pos[axis]);
        }

        // 视线方向合同（captureOne：dir bit=1 → fwd=+axis，bit=0 → fwd=−axis）：
        // 捕获法线朝 +axis 的表面（neg=0）时视线沿 −axis 从上往下看，dir bit=0，
        // 卡平面放在块最高点上方；法线朝 −axis（neg=1）时视线沿 +axis 仰视，
        // dir bit=1，平面放在块最低点下方。即卡平面永远贴在表面**背后**
        // （−fwd 侧）——运行时捕获近平面 near=0.05（深度编码耦合，WRC 遗留
        // #1 证伪回滚结论：一律不动），backoff = 2 体素 + 0.05 保证全部所属
        // 表面落在近平面前方且不被自身壳体遮挡。
        const f32 backoff = 2.f * step[axis] + 0.05f;

        card c{};
        c.axis_direction = axis | (neg << 8);
        c.center[axis] = neg ? (dmin - backoff) : (dmax + backoff);
        // 仅离线候选 opt-in：plane 位于 dmin-backoff 或 dmax+backoff，
        // 因此从 plane 到块的远端是完整 (dmax-dmin)+backoff。
        // 原半段范围会让有效 capture 点落在 card AABB 外，运行时网格漏列该卡。
        // 未启用时保留旧 CDB2 字节行为，等待画面门禁后再考虑默认迁移。
        c.extent[axis] = (dmax - dmin) * (full_axial_slab ? 1.0f : 0.5f)
                       + backoff + step[axis] * 0.5f;
        // 投影包围 = cell 范围 + 1 体素外垫（cell 中心即样本位置，+0.5 归位）。
        c.center[uu] = comp(grid.grid_min, uu)
                     + ((f32)umin + (f32)(umax - umin + 1) * 0.5f + 0.5f) * step[uu];
        c.extent[uu] = ((f32)(umax - umin + 1) * 0.5f + 1.f) * step[uu];
        c.center[vv] = comp(grid.grid_min, vv)
                     + ((f32)vmin + (f32)(vmax - vmin + 1) * 0.5f + 0.5f) * step[vv];
        c.extent[vv] = ((f32)(vmax - vmin + 1) * 0.5f + 1.f) * step[vv];

        // 绝对面积下限（v2 不做"最大投影 5%"剔除——局部小卡正是改进目标）。
        if (c.extent[uu] * c.extent[vv] * 4.f < params.min_view_area) continue;

        // 分辨率提示：与 v1/运行时同一公式。
        const f32 proj_size = 2.0f * std::max(c.extent[uu], c.extent[vv]);
        f32 res_hint = proj_size * params.texels_per_unit;
        res_hint = std::clamp(res_hint, (f32)params.min_resolution, (f32)params.max_resolution);
        c.resolution = (u32)res_hint;

        // 可见性：块内样本投影必落卡内（cell 包围 + 外垫），遮挡判定 =
        // 沿 −fwd 自样本体素 march 至卡平面，途中任一占据体素 → 有比所属
        // 表面更近的遮挡物，该样本从此卡不可见。
        // 卡平面在体素坐标中的分界：plane_cell = (center−grid_min)/step−0.5。
        const f32 plane_cell = (c.center[axis] - comp(grid.grid_min, axis)) / step[axis] - 0.5f;
        Candidate cand{};
        cand.c = c;
        cand.visible.reserve(members.size());
        for (u32 si : members) {
            const Sample& s = samples[si];
            bool occluded = false;
            if (neg == 0) {
                // 法线 +axis / 视线 −axis：平面在上方，遮挡体素下标
                // [cell+1, plane_cell]。
                const s32 hi = std::min((s32)res - 1, (s32)std::floor(plane_cell));
                for (s32 i = (s32)s.cell[axis] + 1; i <= hi; ++i) {
                    s32 c3[3] = { (s32)s.cell[0], (s32)s.cell[1], (s32)s.cell[2] };
                    c3[axis] = i;
                    if (occupied(c3[0], c3[1], c3[2])) { occluded = true; break; }
                }
            } else {
                // 法线 −axis / 视线 +axis：平面在下方，遮挡体素下标
                // [plane_cell, cell−1]。
                const s32 lo = std::max(0, (s32)std::ceil(plane_cell));
                for (s32 i = (s32)s.cell[axis] - 1; i >= lo; --i) {
                    s32 c3[3] = { (s32)s.cell[0], (s32)s.cell[1], (s32)s.cell[2] };
                    c3[axis] = i;
                    if (occupied(c3[0], c3[1], c3[2])) { occluded = true; break; }
                }
            }
            if (!occluded) cand.visible.push_back(si);
        }
        if (!cand.visible.empty())
            candidates.push_back(std::move(cand));
    }
    if (candidates.empty()) return;

    // ---- 4. 贪心选卡（新增覆盖面积最大优先） -----------------------------
    // 覆盖分母 = **可捕获面积**：仅计属于某候选卡可见集的样本。SDF 壳的
    // 锯齿/裂缝内壁（各向异性步长下壳层临界标记的伪表面，面积可达几何面
    // 数倍）永远无法通过可见性 march，计入分母会让 coverage 永远达不到
    // 阈值、贪心浪费在噪声上。可捕获面 ≈ 真实朝外表面。
    f32 total_area_capturable = 0.f;
    {
        std::vector<u8> capturable(n, 0);
        for (const Candidate& cand : candidates)
            for (u32 si : cand.visible) capturable[si] = 1;
        for (u32 i = 0; i < n; ++i)
            if (capturable[i]) total_area_capturable += samples[i].area;
    }
    if (total_area_capturable <= 0.f) return;

    std::vector<u8> covered(n, 0);
    f32 covered_area = 0.f;
    std::vector<u32> chosen;
    const bool dbg = getenv("PRIMAL_SC_BAKE_DEBUG") != nullptr;
    if (dbg)
        printf("[bake_v2] samples=%zu area_geo=%.3f area_capturable=%.3f comps=%zu candidates=%zu\n",
               n, (double)total_area, (double)total_area_capturable,
               comps.size(), candidates.size());
    for (u32 round = 0; round < max_cards; ++round) {
        if (covered_area >= params.coverage_threshold * total_area_capturable) break;
        u32 best = kInvalid;
        f32 best_gain = params.min_gain_fraction * total_area_capturable;   // 早停下限
        for (u32 ci = 0; ci < candidates.size(); ++ci) {
            f32 gain = 0.f;
            for (u32 si : candidates[ci].visible)
                if (!covered[si]) gain += samples[si].area;
            if (gain > best_gain) { best_gain = gain; best = ci; }
        }
        if (best == kInvalid) break;
        chosen.push_back(best);
        if (dbg)
            printf("[bake_v2] round %u -> cand %u gain %.3f (cov %.3f) axis=%u neg=%u\n",
                   round, best, (double)best_gain,
                   (double)(covered_area / total_area_capturable),
                   candidates[best].c.axis_direction & 0xFF,
                   (candidates[best].c.axis_direction >> 8) & 0xFF);
        for (u32 si : candidates[best].visible) {
            if (!covered[si]) { covered[si] = 1; covered_area += samples[si].area; }
        }
    }

    out.cards.reserve(chosen.size());
    for (u32 ci : chosen) out.cards.push_back(candidates[ci].c);
    out.coverage = covered_area / total_area_capturable;
}

// ---- env 分流入口 ---------------------------------------------------------

void bake(const OccupancyGrid& grid, const Params& params, data& out)
{
    // PRIMAL_SC_BAKE_V2=0 回落 v1（回滚路径 = 环境变量 + 资产重烘）。
    const char* e = std::getenv("PRIMAL_SC_BAKE_V2");
    if (e && e[0] == '0')
        bake_v1(grid, params, out);
    else
        bake_v2(grid, params, out);
}

}  // namespace card_bake
}  // namespace primal::tools
