#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::lumen {

// ============================================================================
// GICacheView — GI 三缓存统一 Cache View 接口（轴化重构轮 X2/A3）
//
// 轻量纯数据视图：消费者（DDGI finalize←SC、GIGather←DDGI、SPGI←SC/DDGI）
// 依赖这些视图 + 缓存模块上的构建方法（SurfaceCachePass::GetCacheView /
// LumenDDGIPass::GetVolumeView），而非 pass 内部 buffer 细节。
//
// 本头文件为 header-only，无 cpp。视图只搬运句柄/尺寸，不持有资源、
// 不改变任何描述符 binding 或上传布局——消费端按原有字段逐字节写入。
// ============================================================================

/// Surface Cache atlas + 卡片缓冲的只读快照。
struct SurfaceCacheView {
    /// Material payload atlas. Alpha packs validity/transmission/thickness;
    /// consumers use it to distinguish a transmissive back face from an
    /// opaque card whose opposite side must stay rejected.
    rhi::ResourceHandle albedo_atlas{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle lighting_atlas{ rhi::handles::INVALID_RESOURCE };  ///< 三缓冲 lighting atlas（GetCacheView 的 atlas_idx 槽位）
    /// [E0-B 2026-09-21] 直射剥离副本（同槽位轮转；LightEval 另存
    /// result − direct·ρ/π）。可缺省（INVALID = 上游未分配，消费者回退混合 atlas）。
    rhi::ResourceHandle lighting_nodirect_atlas{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle card_data{ rhi::handles::INVALID_RESOURCE };       ///< CardGenerator 卡片数据 SSBO
    rhi::ResourceHandle card_lookup{ rhi::handles::INVALID_RESOURCE };     ///< CardGenerator mesh→card 查找 SSBO
    rhi::ResourceHandle grid_offsets{ rhi::handles::INVALID_RESOURCE };    ///< T1.3 uniform card grid offsets（可缺省）
    rhi::ResourceHandle grid_cards{ rhi::handles::INVALID_RESOURCE };      ///< T1.3 uniform card grid cards（可缺省）
    rhi::ResourceHandle depth_atlas{ rhi::handles::INVALID_RESOURCE };     ///< T3.4 (D3): axis-depth per texel — plane-consistency test

    /// 可选：管线侧把 atlas 以某个 RG 名字 import 后回填（视图本身不建图）。
    rendergraph::RGResourceHandle lighting_rg{};

    u32 atlas_size = 0;       ///< == SurfaceCachePass::GetAtlasSize()（来自 LumenConfig）
    u32 card_count = 0;       ///< 当前帧实际分配的卡片数（CardGenerator::GetCardCount，活跃值）
    u32 max_card_count = 0;   ///< 卡片缓冲容量（CardGenerator::GetMaxCardCount == LumenConfig::surface_cache_max_cards）
                              ///< 旧 SPGI 注入点上传的是容量值，视图同时携带以保持上传逐字节等价
    u32 lookup_count = 0;     ///< mesh lookup 条目数（CardGenerator::GetLookupCount）

    bool IsValid() const { return lighting_atlas != rhi::handles::INVALID_RESOURCE; }
};

/// DDGI 单个 volume 的只读快照（irradiance 三缓冲取 frame_idx % 3 槽位）。
struct DDGIVolumeView {
    // Internal consumer contract: negative depth mean denotes unmeasured.
    bool measured_visibility = false;
    rhi::ResourceHandle irradiance{ rhi::handles::INVALID_RESOURCE };  ///< irradiance_buffers_[frame_idx % 3]
    rhi::ResourceHandle depth{ rhi::handles::INVALID_RESOURCE }; ///< matching SH slot; mean[64], variance[64] per probe

    math::v3 origin{};     ///< volume_data_.ProbeOrigin.xyz
    math::v3 counts{};    ///< params_.probe_count_{x,y,z}（float 化，与旧上传一致）
    float spacing = 0.0f;  ///< params_.probe_spacing

    // 2026-09-17 P0（probe-selection-density-audit §5）：重定位后有效起点
    // （float4 per probe，绝对 probeIdx 索引，w=1.0 有效）。GIGather 近场
    // 可见性从该锚点测 distToProbe；可缺省（INVALID → shader w=0 回退格点）。
    rhi::ResourceHandle relocated_origins{ rhi::handles::INVALID_RESOURCE };

    // GI 能量根治方案 Phase 3.1: 每探针 openSky 统计（float/probe，逃逸占比
    // EMA）。gather 端天空分类加权用；可缺省（INVALID → 分类关闭）。
    rhi::ResourceHandle opensky{ rhi::handles::INVALID_RESOURCE };

    bool IsValid() const { return irradiance != rhi::handles::INVALID_RESOURCE; }
};

// ============================================================================
// GITemporalStrategy — GI 时域累积策略（X2/A3 先立枚举 + 模块侧存储映射；
// X3 将扩成完整"五轴"GI 配置轴的一部分）
//
// 当前各模块映射：
//   - LumenDDGIPass:  EMA → 现有 irradiance/depth EMA（默认，行为不变）；
//                     EMAVarianceClamp / Reprojection → 暂映射为 EMA（TODO X3）
//   - ScreenProbeGIPass: F3 (M7) 已实装 — EMA → 现有重投影+双线性+相对钳制
//                     路径（默认）；EMAVarianceClamp → +邻域 AABB 方差钳制
//                     （TemporalLib::varianceClampAABB，L1 随 L0 等比收缩）；
//                     Reprojection → 纯双线性历史复用（跳过钳制/梯度限幅）
//   - SurfaceCachePass:  枚举存储占位（SC 自身无 probe 时域链，X3 归并）
// ============================================================================
enum class GITemporalStrategy {
    EMA,                ///< 指数滑动平均（现状默认）
    EMAVarianceClamp,   ///< EMA + 方差钳制
    Reprojection        ///< 重投影历史复用（X3 落地）
};

/// 日志用名称（SetTemporalStrategy 的 cpp 侧打印）。
inline const char* GITemporalStrategyName(GITemporalStrategy s) {
    switch (s) {
        case GITemporalStrategy::EMA:               return "EMA";
        case GITemporalStrategy::EMAVarianceClamp:  return "EMAVarianceClamp";
        case GITemporalStrategy::Reprojection:      return "Reprojection";
    }
    return "<unknown>";
}

} // namespace primal::graphics::lumen
