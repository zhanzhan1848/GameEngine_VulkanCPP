#pragma once

// T4.2 离线 card bake：SDF 面壳占据网格 → 卡片布局 + Surfel 数据。
// 产物随 blob 持久化（"CDBK"/"CDB2" 尾段），运行时 CardGenerator 优先加载，
// AABB 推导降级为 fallback（路线图第四批 T4.2）。
//
// v1（原版）：全局紧致包围盒 × 6 个居中正交视图——卡片居中于整个对象，
// 不贴面（对凹体，内壁被外壳遮挡，无 cache 表示）。
// v2（2026-09-19 SC 卡片 Lumen 对齐计划 Phase 1）：coverage 驱动的贴面
// （surface-hugging）局部卡——按 6 向暴露面 surfel 的同向 6-连通块出候选
// 卡，卡平面贴到块深度并对薄板收缩，可见性 = 体素 march（迎向视角无遮挡），
// 贪心选卡直到 coverage 达标 / 增益耗尽 / 每 mesh 卡数上限（对齐 UE
// "Max Lumen Mesh Cards" = 12）。bake() 按 env PRIMAL_SC_BAKE_V2 分流
//（默认 v2；0 = v1，回滚即资产重烘）。
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

// CDBK 段版本。v1 段头 = magic("CDBK") | card_count | cards | surfel_count
// | surfels；v2 段头 = magic("CDB2") | version(u32) | card_count | cards |
// surfel_count | surfels。magic 区分（而非复用 count 字段做版本探测）是因为
// 旧段 card_count ≥ 1 与任何版本号值都存在碰撞；reader 双 magic 都收，
// 旧 blob 视作 v1（零迁移）。
constexpr u32 kCardBakeMagicV1{ 0x4B424443 };  // "CDBK"
constexpr u32 kCardBakeMagicV2{ 0x32424443 };  // "CDB2"
constexpr u32 kCardBakeVersionV2{ 2u };

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

    // v2：最终卡集合覆盖的 surfel 面积 / 总 surfel 面积（[0,1]）。v1 不填
    //（保持 0）。随 bake 报告产出，供 cook/重烘工具聚合 mean/min。
    f32 coverage{ 0.f };

    bool HasCards() const { return !cards.empty(); }
    void clear() { cards.clear(); surfels.clear(); coverage = 0.f; }
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
    // 的薄视角剔除，保证 baked/fallback 语义兼容。（5% 规则仅 v1 消费；v2 的
    // 局部小卡正是改进目标，运行时已对 baked 卡片豁免该规则。）
    f32 min_view_fraction{0.05f};
    f32 min_view_area{0.01f};
    // 每个非空体素朝 6 向暴露面生成 surfel（关掉可省 blob 体积）。
    bool generate_surfels{true};

    // ---- v2 专属（coverage 驱动贪心的停止条件）----
    // 覆盖率达标阈值：已覆盖 surfel 面积 / 总面积 ≥ 此值即停。
    f32 coverage_threshold{0.85f};
    // 增益早停：最佳候选新增覆盖 < 总面积的此比例即停（防长尾碎卡）。
    f32 min_gain_fraction{0.005f};
    // 每 mesh 卡数上限（对齐 UE "Max Lumen Mesh Cards" 默认 12）。
    u32 max_cards{12};
};

// 从占据网格烘焙卡片（env 分流：PRIMAL_SC_BAKE_V2=0 走 v1，默认 v2）。
// 空网格 / 全空占据 → 输出空 cards，运行时对该 mesh 走 fallback。
void bake(const OccupancyGrid& grid, const Params& params, data& out);
// v1：全局紧致包围盒 × 6 居中视图（原 T4.2 算法，保留作回滚路径）。
void bake_v1(const OccupancyGrid& grid, const Params& params, data& out);
// v2：coverage 驱动贴面局部卡（Lumen 对齐）。
void bake_v2(const OccupancyGrid& grid, const Params& params, data& out);

}  // namespace card_bake
}  // namespace primal::tools
