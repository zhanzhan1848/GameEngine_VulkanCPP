#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::lumen {

// ============================================================================
// WRCCacheView — [WRC] 纯数据快照（对齐 GICacheView.h 的视图模式）。
//
// 消费者（A3 起：ScreenProbeGIPass 远场段）经本视图取 WRC 数据，不摸
// LumenWRCPass 内部。header-only；视图只搬运句柄/尺寸，不持有资源。
//
// 排布契约（WRCTypes.h kWRCProbeStrideU32）：单 storage buffer 内每探针
// K 个方向的 RGB16F 打包 radiance + Chebyshev mean/var + state（含消费
// 计数）。消费端 cos 加权积分在此 radiance 方向格上进行（方向性保留到
// 消费端——与 DDGI SH 预积分的本质区别）。
// ============================================================================

// 与 WRCTypes.h 常量同步的视图侧副本（消费端无需 include pass 头）。
inline constexpr u32 kWRCViewDirsPerProbe = 64;
inline constexpr u32 kWRCViewProbeStrideU32 = kWRCViewDirsPerProbe * 2u + 4u;
// state 位域（同 WRCTypes.h）
inline constexpr u32 kWRCViewStateValid      = 1u << 0;
inline constexpr u32 kWRCViewStateInGeometry = 1u << 1;
inline constexpr u32 kWRCViewConsumeShift    = 16;

struct WRCCacheView {
    rhi::ResourceHandle probe_data{ rhi::handles::INVALID_RESOURCE };  ///< 探针 radiance storage（WRCSlotsForFrame 槽位）

    math::v3 origin{};      ///< 探针格原点（probe[0] 世界坐标）
    math::v3 counts{};      ///< xyz 探针数
    float spacing = 0.0f;   ///< 探针间距

    u32 dirs_per_probe = kWRCViewDirsPerProbe;   ///< K
    u32 probe_count_total = 0;                   ///< counts.x*y*z

    bool IsValid() const { return probe_data != rhi::handles::INVALID_RESOURCE && spacing > 0.0f; }
};

} // namespace primal::graphics::lumen
