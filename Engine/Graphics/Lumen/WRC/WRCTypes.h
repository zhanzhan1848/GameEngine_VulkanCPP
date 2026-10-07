#pragma once

#include "CommonHeaders.h"

namespace primal::graphics::lumen {

// ============================================================================
// WRCTypes — World Space Radiance Cache（[WRC]）核心常量与参数。
//
// 标签：[WRC] ｜ 关联：[DDGI] [SC] [SPGI]
// 设计文档：Docs/2026-09-18-world-space-radiance-cache-design-and-plan.md
//
// WRC 是独立代码域（§1 命名公约）：与 DDGI 逐项同构、互不包含。本文件是
// WRC 的常量/配置单一事实源；与 DDGI 共享的设施白名单见 §1.2-1（GIWarmup.h、
// GIDebugParams.h、GICacheView.h、GIPlatformProfile.h、LumenTypes.h）——
// 本文件刻意不 include 任何 DDGI 专有头。
//
// 身份约束（§0.2 能量单向律）：WRC 是纯 cache——SC trace 采样合法（填充）、
// SPGI 远场消费合法、SC LightEval 读 WRC 禁止、High 档像素直采禁止。
// ============================================================================

// ----------------------------------------------------------------------------
// 单一开关：PRIMAL_WRC_ENABLE。
// [2026-09-20 默认翻转 0→1]（差距分析 P4）：近场 2m 密度 / 卡片注入可见性
// 门控 / 远场时间方差 −31% 等已验证收益默认生效；此前默认 0 使默认运行
// 完全没吃到（用户截图即旧 DDGI 链形态）。A4 三场景基线周环比验收仍为
// 会话外项；回退：PRIMAL_WRC_ENABLE=0（管线不创建 WRC pass、SPGI 远场
// 走 DDGI 回退冻结边——行为与 A3 前一致，§5.3-5 回退判据）。
// Metal 由创建门平台守卫兜底（WRC.metal 八件套未移植，parity 后置）。
// ----------------------------------------------------------------------------
inline bool WRCEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("PRIMAL_WRC_ENABLE");
        return !(e && e[0] == '0');   // 默认 1；=0 显式回退
    }();
    return v;
}

// A1 诊断：PRIMAL_WRC_DUMP=1 — GIDebug wrc 通道输出探针 radiance 方向格
// 可视化（§4/A1 验收：GIDebug 探针视图无穿模热点）。
inline bool WRCDumpEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("PRIMAL_WRC_DUMP");
        return e && e[0] == '1';
    }();
    return v;
}

// ----------------------------------------------------------------------------
// 常量（§2.1 探针数据布局）
// ----------------------------------------------------------------------------

// K = 均匀球面方向数（Fibonacci 球面）。UE WSRC 用 16×16/32×32 方向格；
// K=64 是其 1/4 预算起步值。方向性保留到消费端（SPGI cos 加权积分）——
// 与 SH2 预积分的本质区别（irradiance 不在生成端烘死法线假设）。
inline constexpr u32 kWRCDirsPerProbe = 64;

// 每方向 radiance 以 RGB16F 精度存储：packHalf2x16 × 2 = 2 u32/方向
//（rgb → (r,g) | (b,空)）。带 pad 槽保持 u32 对齐。
inline constexpr u32 kWRCDirU32Stride = 2;   // u32 per direction

// 探针数据 stride（u32）: K×2（radiance）+ mean + var + state + pad。
//   [base + d*2 + 0] = packHalf2x16(r, g)
//   [base + d*2 + 1] = packHalf2x16(b, 0)
//   [base + K*2 + 0] = hit_dist_mean（可见性 Chebyshev 用）
//   [base + K*2 + 1] = hit_dist_var
//   [base + K*2 + 2] = state：bit0 valid / bit1 in-geometry 排除 /
//                      bits[16..31] 消费计数（A5 需求驱动预算优先级）
//   [base + K*2 + 3] = pad（保持 16B 对齐 stride）
inline constexpr u32 kWRCProbeStateValid      = 1u << 0;
inline constexpr u32 kWRCProbeStateInGeometry = 1u << 1;
inline constexpr u32 kWRCProbeConsumeShift    = 16;
inline constexpr u32 kWRCProbeConsumeMask     = 0xFFFFu << kWRCProbeConsumeShift;

// [P2 2026-09-21 probe-repr-plan] AoS→SoA 布局 v2（单 storage buffer 内双区域）：
//   radiance 区域：[rbase + d*2 (+1)]，rbase = probeIdx × 128（512B 对齐——
//                  每 probe 的 64 方向连续，wave 内探针并行不再被 4 u32 meta
//                  尾巴拖垮 cache 行；meta/position 更新频率与 radiance 不同，
//                  分离后 relocation/分类读不再拉取 520B）。
//   meta 区域：    [metaBase + 0..3]，metaBase = probeTotal × 128，
//                  条目 = mean / var / state / pad（与 AoS 尾 4 u32 同序）。
// 消费端全部经 kWRCProbeRadianceStrideU32 / kWRCMetaBase(total) 定位；
// [block=8][dir=64][slot=8] tile 化在出现跨探针 wave 消费端之前不引入
//（现全部消费端为单探针粒度，probe-major 已是最优连续性）。
inline constexpr u32 kWRCProbeRadianceStrideU32 = kWRCDirsPerProbe * kWRCDirU32Stride;  // 128
inline constexpr u32 kWRCProbeMetaStrideU32     = 4;                                    // mean/var/state/pad
// 兼容别名（AoS 总 stride，仍用于缓冲分配尺寸与 dump ring 尺寸）。
inline constexpr u32 kWRCProbeStrideU32         = kWRCProbeRadianceStrideU32 + kWRCProbeMetaStrideU32;
// meta 区域基址（u32 偏移）——shader 侧镜像：metaBase = total * (K*2)。
inline constexpr u32 kWRCMetaBaseU32(u32 probeTotal) {
    return probeTotal * kWRCProbeRadianceStrideU32;
}
// 兼容旧状态位名。
inline constexpr u32 kWRCProbeStateValidOld     = kWRCProbeStateValid;

// 内存预算复核（§2.1）：(64×2+4)×4B = 528B/探针；
// near 32×16×32 = 16384 探针 ≈ 8.5 MB，far 16×8×16 = 2048 ≈ 1.1 MB，
// 双体积 ≈ 9.6 MB < §6-3 的 25 MB cap。（半精度打包替代 float4 直存的
// 64 MB；RGB16F 精度由 packHalf2x16 保持——radiance 值域 0..64 内半精度
// 量化 ≤1/2048，远低于 GI 频率的可感阈。）

// 天空契约档位（§2.2：与 DDGITraceFinalize 同源的 sky tier，常量独立定义
// ——不复用 DDGI 侧数值来源，两条线的天空档位可独立演化）。
// PRIMAL_WRC_SKY_TIER env 可调（缺省 2.0；调低收紧逃逸射线的均匀天空填充）。
// [2026-09-20 观感轮 默认 0.15→2.0 + 天空去双重压缩]：旧 0.15 档在
// EvalSky×tier²（双重压缩）下天空贡献 ≈0，探针场只剩受光面反弹——中庭
// 二层/一层天光梯度消失（Lumen 参考的主要结构）。中性色 × tier=2.0 后
// f2/f1 = 1.34（参考 1.47，旧值 1.06），室内/一层 0.15（参考 0.20）；
// GI_DCGain amp 0.08（有界小幅振荡，E 稳定 0.93±2%，无发散——超历史
// 0.05 断言线，已知偏差入档）。tier 1.5 位于过渡区（梯度 1.04 且振荡
// 更大 0.11）——禁止回落该区间。回退：PRIMAL_WRC_SKY_TIER=0.15。
inline float WRCSkyTier() {
    static const float v = [] {
        const char* e = std::getenv("PRIMAL_WRC_SKY_TIER");
        if (!e) return 2.0f;
        const float f = static_cast<float>(std::atof(e));
        return (f >= 0.0f && f <= 2.0f) ? f : 2.0f;
    }();
    return v;
}

// 探针 radiance EMA α（§2.2：独立于 DDGI 的 0.02/0.2 双系数，起步 0.1）。
inline constexpr float kWRCRadianceAlpha = 0.1f;

// ----------------------------------------------------------------------------
// WRCRuntimeParams — 初始化配置（镜像 DDGIRuntimeParams 的角色，字段独立）
// ----------------------------------------------------------------------------

struct WRCRuntimeParams {
    u32   probe_count_x = 32;
    u32   probe_count_y = 16;
    u32   probe_count_z = 32;
    float probe_spacing = 3.0f;
    float ray_max_distance = 50.0f;
    // §2.2 预算化：每帧 trace/update 的探针数上限（UE NumProbesToTraceBudget
    // 精神）。默认全量；A5 需求驱动预算按消费计数优先级调度。
    u32   update_budget_probes_per_frame = 16384;
    // 近场命中判定：超过该距离的命中视为远场（消费端语义边界，A3 使用）。
    float near_distance = 0.0f;   // 0 = volume 自身范围即近场（起步值）
};

// ----------------------------------------------------------------------------
// WRCVolumeData — GPU 常量缓冲（std140，各 WRC shader 逐字节镜像）。
// 独立声明（§2.5：entry 语义在 WRCVolumeData 独立声明，非 DDGIVolumeData）。
// ----------------------------------------------------------------------------
struct WRCVolumeData {
    math::v4 ProbeOrigin;            // offset   0: xyz 格点原点，w = spacing
    u32      ProbeCounts[4];         // offset  16: xyz counts，w = signedSDF 旗标
                                     //   （Phase 4 零穿越判定，PRIMAL_WRC_UNSIGNED_TRACE=0 默认随离线场）
    u32      DirsPerProbe;           // offset  32: K
    u32      ProbeCountTotal;        // offset  36
    u32      RaysPerProbe;           // offset  40: = K（1 方向 1 射线）
    u32      ProbeUpdateCount;       // offset  44: 本帧预算内更新的探针数
    float    RayMaxDistance;         // offset  48
    float    TemporalAlpha;          // offset  52: kWRCRadianceAlpha
    u32      FrameIndex;             // offset  56
    u32      WarmupFrames;           // offset  60: GIWarmupFrames()——暖启动期 EMA 冻结（§6-2）
    math::v4 SdfOrigins[3];          // offset  64
    math::v4 SdfVoxelSizes[3];       // offset 112
    math::v4 SdfExtents[3];          // offset 160
    u32      SdfResolutions[3];      // offset 208
    u32      SdfCascadeCount;        // offset 220
    math::v4 LightDirection;         // offset 224: xyz = dir，w 未用
    math::v4 LightColor;             // offset 240
    math::v4 SkyColor;               // offset 256: xyz = 天空色，w = 档位门（0 关）
    u32      SCLookupCount;          // offset 272
    u32      SCAtlasSize;            // offset 276
    u32      SCGridValid;            // offset 280: uniform card grid 有效性
    u32      DebugChannel;           // offset 284: GIDebug wrc 通道（0 关 / 1 radiance dump）
    math::v4 SCQueryParams;          // offset 288: [R0/R1 2026-09-20] x = SC 查询
                                     //   容差覆盖（PRIMAL_WRC_QUERY_TOL，0 = 编译期
                                     //   常量 max(voxel*1.25, 0.9)）；y = 查询侧 UV
                                     //   接受域米制 pad（PRIMAL_SC_QPAD_M，0.45 默认，
                                     //   boundsFail 掠射带收口）；z/w 保留。
};

static_assert(sizeof(WRCVolumeData) == 304, "WRCVolumeData std140 layout drift");

} // namespace primal::graphics::lumen
