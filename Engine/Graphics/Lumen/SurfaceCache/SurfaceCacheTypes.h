#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::lumen {

// Uses math::v4 instead of math::v3 to guarantee 16-byte alignment (matches DDGIVolumeData pattern)
struct SurfaceCacheCard {
    math::v4  center;         // xyz = card center in mesh local space, w unused
    math::v4  extent;         // xyz = half-extents from AABB, w unused
    uint32_t  axis_direction; // low byte = axis (0=X,1=Y,2=Z), high byte = direction (0/1)
    uint16_t  resolution;     // Texel resolution in atlas
    uint16_t  _pad0;
    uint32_t  atlas_offset_x;
    uint32_t  atlas_offset_y;
    uint32_t  mesh_instance_id;
    uint32_t  _pad1[3];
};
static_assert(sizeof(SurfaceCacheCard) == 64, "SurfaceCacheCard must be 64 bytes");

struct SurfaceCacheCardLookup {
    math::v4  aabb_min;      // xyz = min, w unused
    math::v4  aabb_max;      // xyz = max, w unused
    uint32_t  card_start;
    uint32_t  card_count;
    uint32_t  _pad[2];
};
static_assert(sizeof(SurfaceCacheCardLookup) == 48, "SurfaceCacheCardLookup must be 48 bytes");
static_assert(offsetof(SurfaceCacheCardLookup, _pad) == 40, "GPU owner-directory sideband offset");

struct SurfaceCacheParams {
    uint32_t  atlas_size;
    uint32_t  page_size;
    uint32_t  capture_budget_pages;
    uint32_t  max_cards;
    float     update_distance;
    float     importance_weight;
    uint32_t  max_lights_per_tile;
    uint32_t  indirect_rays_per_probe;
    float     indirect_temporal_weight;
    float     indirect_near_distance;
    uint32_t  lookup_count;     // number of CardLookup entries (mesh instances)
    uint32_t  _pad;
};

// T3.3 (D10): CPU mirrors of the indirect trace/resolve params UBOs.
// Field order matches the shader structs EXACTLY (std140; scalar-packed
// prefixes, vec4 arrays at 16B stride). Definition sites:
//   SurfaceCacheIndirectTrace.comp:64   IndirectTraceParams
//   SurfaceCacheIndirectResolve.comp:62 IndirectResolveParams
struct IndirectTraceParamsGPU {
    SurfaceCacheParams sc_params;   // 48B
    uint32_t tile_size;
    uint32_t rays_per_probe;
    uint32_t frame_index;
    float    near_distance;
    float    max_ray_distance;
    // 2026-09-10 C3.5 slice 2: was float _pad0/_pad1 — now the trace-refresh
    // stride amortization (shader early-outs non-refreshed tiles before the
    // march). Offsets land in what was std140 padding before the vec4 arrays;
    // sizeof unchanged (288).
    uint32_t refresh_stride;        // 0/1 = every tile every frame
    uint32_t refresh_phase;         // frame_index % refresh_stride
    math::v4 sdf_origins[3];
    math::v4 sdf_voxel_sizes[3];
    math::v4 sdf_extents[3];
    math::u32v4 sdf_resolutions[3]; // std140 u32 array stride = 16
    math::u32v4 sdf_cascade_count;  // x = count
};
static_assert(sizeof(IndirectTraceParamsGPU) == 288, "std140 layout drift");

struct IndirectResolveParamsGPU {
    SurfaceCacheParams sc_params;   // 48B
    uint32_t tile_size;
    uint32_t rays_per_probe;
    float    temporal_weight;
    uint32_t frame_index;
    uint32_t lookup_count;
    // B (plan §三.B): mirrors the trace's refresh amortization so resolve can
    // identify tiles NOT traced this frame and carry/clear them instead of
    // consuming stale ray_hits. Offsets 68/72 — std140 rounds the UBO to 80.
    uint32_t refresh_stride;        // 0/1 = every tile every frame
    uint32_t refresh_phase;         // frame_index % refresh_stride
    // C (plan §三.C): the trace's SDF hit band (max(voxel×1.25, 0.30)) — the
    // query contract's along-axis tolerance floor. Offset 76 (std140 → 80).
    float    hit_band;
    // A (plan §三.A): tile-grid/reconstruct path enable (PRIMAL_SC_TILE_RECON)
    // and the card count for the resolve's aux owning-card scan. Offsets 80/84.
    uint32_t recon_enabled;
    uint32_t card_count;
    // [WRC 计划 Stream D2]（PRIMAL_SC_PAGE_TABLE=1）: 过表寻址开关——
    // resolve 查询契约经 card_page_table 解析卡片 atlas 原点。
    uint32_t page_table_enabled;
    // [F0 2026-09-20 能量不对称修复 A1] SC tile 天空档接线——天空单一事实源
    // 收口：tile 反弹源改由 GIDebug().sky_color_intensity × 基色梯度
    // (2.75,2.88,3.00) 驱动（与 DDGI/SPGI/WRC 探针路径同一旋钮）。
    // [K-1 2026-09-21] rgb 改传档位 (tier,tier,tier)，w=1 方向化使能
    // （shader 侧 EvaluateSkyBase(rayDir)）；override 时 rgb=常量基色，w=0。
    // 回滚：PRIMAL_SC_TILE_SKY_OVERRIDE="1.1,1.152,1.2"
    //（= 旧硬编码 vec3(2.75,2.88,3.00)*0.4）。Offset 96（92→pad 16 对齐）。
    math::v4 tile_sky_color;        // xyz = sky radiance/tier, w = 方向化使能
    // [稳定性 2026-09-21] tile 增长率钳制：SC↔tile↔prev 回路局部增益>1 的
    // 区域（封闭龛）指数泵浦 → 全局坍黑（实测 SC_INDIRECT=0 时 6/6 稳定，
    // 默认 ~20% 坍黑率）。每帧 tile 新增幅度上限 cap×prev + ε——回路增益
    // 上界化， legitimate 增长仍可在数帧内跟上。PRIMAL_SC_TILE_GROWTH_CAP
    // （默认 0.5；0 = 关）。Offset 112（std140 块 128）。
    float    tile_growth_cap;
    // [SC per-mesh 计划 P4]（PRIMAL_SC_RAY_LOOKUP=1）：射线最近有效表面
    // lookup 开关（resolve shader IndirectResolveParams.ray_lookup 同位）。
    uint32_t ray_lookup;
    float    ray_max_distance;
    float    _pad;
};
static_assert(sizeof(IndirectResolveParamsGPU) == 128, "std140 layout drift");

// Per-card dispatch info for flattened texel dispatch.
// CPU builds prefix-sum array, GPU binary-searches to map thread_id → card → atlas UV.
struct CardDispatchInfo {
    uint32_t texel_offset;     // prefix-sum: starting texel index for this card
    uint32_t texel_count;      // resolution * resolution
    uint32_t resolution;       // card texel resolution (square)
    uint32_t atlas_offset_x;   // atlas pixel origin X
    uint32_t atlas_offset_y;   // atlas pixel origin Y
    uint32_t card_index;       // original index in the full SurfaceCacheCard buffer
    uint32_t _pad[2];
};
static_assert(sizeof(CardDispatchInfo) == 32, "CardDispatchInfo must be 32 bytes");

// Params for merged lighting pass (replaces SurfaceCacheLightingParams).
// Eliminates tile-based dispatch; uses flattened per-card texel dispatch instead.
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;   // 48 bytes
    uint32_t           total_texels; // sum of all card resolutions squared
    uint32_t           card_count;   // number of active cards in dispatch
    uint32_t           light_count;
    // 能量所有权 Phase 4a: openSky V2 开关（PRIMAL_GI_OPENSKY_V2=0 回滚）。
    uint32_t           opensky_v2;
    // Probe-fed indirect (Lumen hierarchy): near-volume grid so LightEval can
    // gather incoming irradiance from the DDGI probes (appended at the end —
    // backends whose shaders predate this field keep reading the first 64B).
    float              ddgi_origin_spacing[4];  // xyz origin, w spacing
    uint32_t           ddgi_counts_valid[4];    // xyz counts, w valid flag
    // T3.3: SC Radiosity gate (x = indirect_radiosity_on) — appended at the
    // end exactly like the probe-feed block above.
    uint32_t           radiosity_gate[4];       // x = on, yzw reserved
    // WRC 计划 Stream B1/B2（能量单向化，PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1）：
    // probe_feed_bootstrap = 引导因子（窗口内 1→0 线性，窗口后 0；旧语义恒
    // 1，shader 端乘法恒等）。radiosity_solo = 1 → volume 外 texel 的间接项
    // 只来自 tile radiosity，legacy prev_light 自反馈双轨停用。
    // append-at-end 惯例：旧后端 shader 不读也安全。
    float              probe_feed_bootstrap;
    uint32_t           radiosity_solo;
    // [WRC 计划 Stream D2]（PRIMAL_SC_PAGE_TABLE=1）: LightEval 采样契约
    // 过页表寻址——1 时卡片 atlas 原点经 card_page_table 解析。
    uint32_t           page_table_enabled;
};

struct SurfaceCacheFrameData {
    // [双稳态修复] GI 暖启动时钟（frame - 内容就绪帧）——暖启动门禁
    // 以此计数，消除异步内容加载时序导致的分支选择。
    uint32_t gi_frame = 0;
    math::v3   camera_position;
    float      _pad;
    uint32_t   frame_index;
    uint32_t   light_count;
    float      _pad2[2];

    // T3.2: light injected by the caller (pipeline passes settings_.lighting);
    // defaults preserve the old hardcoded directional light.
    math::v3   light_direction{0.707f, -1.0f, 0.408f};
    float      _pad3;
    math::v3   light_color{5.0f, 5.0f, 5.0f};
    float      _pad4;

    // A4 (T0.4): real per-slot light list for the LightEval upload, packed in
    // the GPU LightInfo layout (48 B/slot = 3 v4):
    //   [0] position  = (xyz, w = radius/range)
    //   [1] color     = (rgb premultiplied by intensity, w unused)
    //   [2] direction = (xyz, w = type: 1.0 directional, 0.0 point/spot)
    // sc_light_count == 0 → pass falls back to the legacy single
    // directional slot built from light_direction/light_color.
    static constexpr uint32_t kSCMaxLights = 64;
    uint32_t   sc_light_count = 0;
    math::v4   sc_lights[kSCMaxLights * 3] = {};

    // GBuffer-gather capture inputs (Vulkan Capture path): main-pass GBuffer
    // handles + inverse view-projection for world-position reconstruction.
    math::m4x4 inv_view_projection;
    uint32_t   render_width  = 0;
    uint32_t   render_height = 0;
    rhi::ResourceHandle gbuffer_depth  { rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_albedo { rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_normal { rhi::handles::INVALID_RESOURCE };

    // G2 capture 侧（2026-09-16）: PCSS light-space 深度 + 矩阵（与表面直射
    // 同源同帧，round3 导出的 shadow_depth_rg 契约）。卡片 texel 直射可见性
    // 优先消费它（粗 SDF 最近点采样对 0.5m 薄墙必然漏光——拱腹卡片被烘焙
    // 假直射的根因）；级联全越界回退原 SDF march。
    rendergraph::RGResourceHandle shadow_depth[4];
    math::m4x4 shadow_matrices[4];
    uint32_t   shadow_cascade_count = 0;

    // Probe-fed indirect (Lumen hierarchy): near-volume irradiance (9 vec4 per
    // probe, previous-frame slot — SC runs before the DDGI update) + grid
    // params. ddgi_valid = 0 leaves LightEval on the legacy self-feedback.
    rhi::ResourceHandle ddgi_irradiance{ rhi::handles::INVALID_RESOURCE };
    // C 系列：同一槽位的探针八面体深度（mean+variance/texel），LightEval 的
    // gather 用它做 Chebyshev 可见性——封闭区域卡片隔墙采到受光带探针是
    // 室内平填充的根因（Docs/2026-09-09 计划 §C2/C3）。
    rhi::ResourceHandle ddgi_depth{ rhi::handles::INVALID_RESOURCE };
    // GI 能量根治方案 Phase 3.2: 每探针 openSky 统计（卡片 texel gather 的
    // 天空分类）；INVALID = 分类关闭。
    rhi::ResourceHandle ddgi_opensky{ rhi::handles::INVALID_RESOURCE };
    math::v3   ddgi_origin{};
    float      ddgi_spacing = 0.0f;
    uint32_t   ddgi_counts[3] = {0, 0, 0};
    uint32_t   ddgi_valid = 0;
};

// ============================================================================
// Uniform card grid (T1.3): world-space cell → card index list.
//
// Layout: a single u32 buffer whose first 16 words are this header, followed
// by (cell_count + 1) prefix-sum offsets into a second buffer of card indices.
// Floats are bit-cast into u32 so shaders need no extra CB/UBO plumbing —
// every consumer binds just the two SSBOs and self-describes from the header.
// ============================================================================

struct SurfaceCacheCardGridHeader {
    uint32_t min_bits[3];      // float bits of grid_min.xyz
    uint32_t card_count;       // total cards indexed by the grid
    uint32_t max_bits[3];      // float bits of grid_max.xyz
    uint32_t cell_size_bits;   // float bits of cell_size (world units)
    uint32_t dims[3];          // cells per axis
    uint32_t header_words;     // = 16 (u32 words before the offsets array)
    uint32_t reserved[4];
};
static_assert(sizeof(SurfaceCacheCardGridHeader) == 64, "Header must stay 16 u32 words");

constexpr uint32_t kMaxCardsPerGridCell = 8;   // build-side cap; shaders mirror this

// ============================================================================
// [SC per-mesh 计划 P2] 静态页合成信息——离线 SCS1 页 → SC atlas 一次性合成
// 的 per-card 参数。与 SurfaceCacheStaticComposite.comp 逐字节镜像（144 B =
// 9×vec4）。合成后静态卡跳过 capture/CardFill（§3.2 外来写入生产约束）。
// ============================================================================
struct StaticCompositeInfo {
    uint32_t meta0[4];   // atlas_offset_x, atlas_offset_y, card_res, page_res
    uint32_t meta1[4];   // texel_offset, (count slot: 首条=static_count), world_axis_dir, local_axis_dir
    uint32_t meta2[4];   // pool_albedo_word, pool_normal_word, pool_depth_word, pool_triangle_word
    float    center_w[4];   // xyz 世界卡中心
    float    mcol0[4];      // 世界矩阵线性列 0
    float    mcol1[4];
    float    mcol2[4];
    float    local0[4];     // xyz emissive RGB, w = local extent.x
    float    local1[4];     // xy = local extent.yz, zw reserved
};
static_assert(sizeof(StaticCompositeInfo) == 144, "StaticCompositeInfo must mirror the composite shader (9 x vec4 = 144 B)");

struct SurfaceCacheOutput {
    rendergraph::RGResourceHandle lighting_atlas;
    // [E0-B 2026-09-21] nodirect 副本的 RG 身份（单身份跨 SC 写/SPGI 读，
    // 对齐 lighting_atlas 的 T1.1 C2 别名治理）。可缺省。
    rendergraph::RGResourceHandle lighting_nodirect_atlas;
};

} // namespace
