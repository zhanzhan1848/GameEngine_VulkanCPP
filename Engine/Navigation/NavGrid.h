#pragma once

// NavGrid.h — 高度场导航网格与寻路（ai-navigation S1 核心，纯数据无全局状态）。
//
// 路线决策（2026-10-15）：third_party 无 Recast/Detour，本引擎按"列式高度场 +
// 8 邻域跨度图 + A* + 视线拉直"自研基础导航；足够覆盖 S1 合同（可达/不可达、
// 坡度/台阶边界、角色尺寸腐蚀、取消/重放/序列化），战术 AI/人群避让不在基础层。
// 单位：米/秒，右手系 Y 向上。

#include "Navigation/NavigationTypes.h"

#include <string>
#include <vector>

namespace primal::navigation {

// 一列内一段实心区间 [ymin, ymax]；列内按 ymin 升序且互不重叠（间隔 >= cell_height）。
struct grid_span {
    f32 ymin{ 0.f };
    f32 ymax{ 0.f };
    bool walkable{ false };
    u16  region{ 0 };       // 0 = 不可走/未编号；1..region_count 为连通域
};

struct nav_grid {
    u32 width{ 0 }, depth{ 0 };            // XZ 列数
    f32 cell_size{ 0.25f }, cell_height{ 0.05f };
    math::v3 bmin{}, bmax{};               // 烘焙包围盒（含 padding）
    agent_profile profile{};

    // column c 的 span 位于展平区间 [span_offsets[c], span_offsets[c+1])。
    std::vector<u32> span_offsets;         // size = width*depth + 1
    std::vector<grid_span> spans;          // 展平存储
    u32 region_count{ 0 };
    u32 walkable_span_count{ 0 };
    u64 source_hash{ 0 };                  // 输入几何 FNV-1a（派生资产身份）
    static constexpr u32 nav_version = 1;  // 序列化格式版本

    bool empty() const { return spans.empty(); }
    u32 column_index(u32 cx, u32 cz) const { return cz * width + cx; }
    bool in_bounds(u32 cx, u32 cz) const { return cx < width && cz < depth; }
    math::v3 cell_center(u32 cx, u32 cz, f32 y) const
    {
        return math::v3{ bmin.x + (f32(cx) + 0.5f) * cell_size, y,
                         bmin.z + (f32(cz) + 0.5f) * cell_size };
    }
    // 世界坐标 → 列（越界返回 false）
    bool world_to_column(const math::v3& p, u32& cx, u32& cz) const;
    // 列内找"顶面在 y±tol 内"的 span；找不到返回 -1
    s32 span_at(u32 column, f32 y, f32 tol) const;
};

// ---- 烘焙（NavGrid.cpp）----
nav_error bake(const bake_input& in, const agent_profile& profile, nav_grid& out);

// ---- 查询 ----
struct path_result {
    nav_error error{ nav_error::ok };
    std::vector<math::v3> waypoints;   // 含起点与目标在可走面上的投影；空 = 失败
    u32 region_from{ 0 }, region_to{ 0 };
};

path_result find_path(const nav_grid& g, const math::v3& from, const math::v3& to,
                      f32 nearest_radius);

// 可走面投影：成功时 out 为表面点（水平取最近列中心，y 取 span 顶）。
nav_error nearest_walkable(const nav_grid& g, const math::v3& p, f32 max_radius, math::v3& out);

// ---- 序列化（确定性文本；仅持久化可走 span 与网格参数）----
nav_error serialize(const nav_grid& g, std::string& out_text);
nav_error deserialize(const char* text, size_t len, nav_grid& out_grid);

// 输入身份哈希（FNV-1a 64：几何 + 采样参数 + 外形）
u64 hash_bake_input(const bake_input& in, const agent_profile& profile);

} // namespace primal::navigation
