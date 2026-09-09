#pragma once

// T4.2 离线 card bake：SDF 面壳占据网格 → 6 向紧致卡片布局 + Surfel 数据。
// 产物随 blob 持久化（"CDBK" 尾段），运行时 CardGenerator 优先加载，
// AABB 推导降级为 fallback（路线图第四批 T4.2）。
//
// 占据来源 = generate_sdf（common/SDF.cpp）产出的面壳体素网格
//（mesh::sdf_data::voxels）：体素中心距表面 < 半体素对角 → 255。面壳对
// 开放/单面几何（Sponza 类架构墙）同样有效——solid 判据（winding number）
// 会在这些资产上全空。bake 只做投影/布局，不改写 m.sdf，运行时 GI 数值
// 不受影响。

#include "ToolsCommon.h"

namespace primal::tools {

// 与 Engine RHICardBakeCard / RHICardBakeSurfel（RHIMeshAsset.h）逐字节同布局。
// blob 段按 32 B/条 memcpy 序列化，两侧 static_assert 锁尺寸。
namespace card_bake {

struct card
{
    f32 center[3];
    u32 axis_direction;   // 低字节 = axis (0=X,1=Y,2=Z)，次字节 = dir (0/1)
    f32 extent[3];        // 半尺寸
    u32 resolution;       // 紧致投影 × 64 texels/unit，钳 [16,256]（提示值；运行时按页重取整）
};
static_assert(sizeof(card) == 32, "card_bake card must mirror Engine RHICardBakeCard (32 B)");

struct surfel
{
    f32 pos[3];
    f32 area;
    f32 normal[3];
    u32 pad;
};
static_assert(sizeof(surfel) == 32, "card_bake surfel must mirror Engine RHICardBakeSurfel (32 B)");

struct data
{
    utl::vector<card> cards;
    utl::vector<surfel> surfels;

    bool HasCards() const { return !cards.empty(); }
    void clear() { cards.clear(); surfels.clear(); }
};

// generate_sdf 产出的占据网格视图（零拷贝）。voxels 为 z-major
// ((z*res)+y)*res+x，非零 = 占据；体素 (x,y,z) 中心 =
// grid_min + voxel_size*({x,y,z}+0.5)。
struct OccupancyGrid
{
    const u8* voxels{nullptr};
    u32 resolution{0};      // 每轴格数（立方网格）
    math::v3 grid_min{};    // 体素 (0,0,0) 外角
    math::v3 voxel_size{};  // 每轴步长
};

struct Params
{
    // 卡片分辨率公式（与运行时一致）：紧致投影大边 × texels_per_unit，钳 [16,256]。
    f32 texels_per_unit{64.f};
    u32 min_resolution{16};
    u32 max_resolution{256};
    // 与运行时 fallback 相同的剔除规则：投影面积 < 最大投影 5% 或 < 0.01 单位²
    // 的薄视角剔除，保证 baked/fallback 语义兼容。
    f32 min_view_fraction{0.05f};
    f32 min_view_area{0.01f};
    // 每个非空体素朝 6 向暴露面生成 surfel（关掉可省 blob 体积）。
    bool generate_surfels{true};
};

// 从占据网格烘焙卡片。空网格 / 全空占据 → 输出空 cards，运行时对该 mesh
// 走 fallback。
void bake(const OccupancyGrid& grid, const Params& params, data& out);

}  // namespace card_bake
}  // namespace primal::tools
