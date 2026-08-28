#pragma once

#include "CommonHeaders.h"
#include "Graphics/Dawn/DawnDebugParams.h"

namespace primal::graphics::lumen {

// ============================================================================
// GIDebugParams — 三端统一的 GI 调试旋钮（C3，T4.5）
//
// 取代 DawnDebugParams 的 GI 部分：同一份参数对象被 Vulkan/Metal/Dawn 三端
// 的 pass 在 AddPass 时读取——"三端同一旋钮生效"。
// 旧 WASM sidebar 的 DawnDebugParams 滑条通过 ApplyLegacy() 桥接进来，
// 行为不变。
// X3/A6 注意分面：本结构是【运行时热调面】（进程级单例，AddPass 时读取）；
// init 时【配置面】是 LumenConfig.axes（GIFiveAxisConfig，LumenTypes.h，
// SetLumenConfig 生效）——两者同名域（如 pass 开关 vs 轴1 空间缓存组合）
// 分属不同生命周期，勿混用。
// ============================================================================
struct GIDebugParams {
    // ------------------------------------------------------------------
    // 逐 pass 开关（false = 跳过 dispatch；已有输出保持上帧内容）
    // ------------------------------------------------------------------
    bool ddgi_trace = true;          ///< DDGI 近场 trace/update 链
    bool ddgi_far_volume = true;     ///< T4.4 远场粗 volume
    bool sc_capture = true;          ///< Surface Cache capture
    bool sc_lighting = true;         ///< SC LightEval（页预算调度）
    bool sc_shadow = true;           ///< T3.3 SDF 阴影 split-pass
    bool gi_gather = true;           ///< GIGather（近场+远场 gather）
    bool fusion = true;              ///< FusionIndirect/Composite

    // ------------------------------------------------------------------
    // 权重热调（CPU 侧即时生效；shader 内常量旋钮随 T6.1 CB 化）
    // ------------------------------------------------------------------
    float sky_color_intensity = 0.4f;    ///< DDGI 天空色强度（原 DawnDebugParams[4]）
    float albedo_intensity = 0.5f;       ///< DDGI 多反弹 albedo 强度（原 [5]）
    float probe_hysteresis = 0.2f;       ///< probe EMA 滞回（原 [6]）
    float fusion_glossy_strength = 0.75f; ///< T4.3 光泽项强度（0.75 为实测达标值）

    // ------------------------------------------------------------------
    // freeze frame —— 冻结 probe 更新（读旧槽数据，用于 A/B 对比）
    // ------------------------------------------------------------------
    bool freeze_probe_updates = false;

    // ------------------------------------------------------------------
    // M6/T6.1: probe 调试可视化模式（面板开关）。T6.3 sub-A 已接线 Vulkan
    // 数据面：GIGatherModule 在 DDGIGIGather dispatch 时读取本字段经 CB
    // （binding 6 debugVis）传入 gather kernel——!=Off 时输出替换为
    // IrradianceAtlas（8-probe 三线性 SH L0，网外用远场 L0）/ DepthAtlas
    // （最近 probe octahedral 平均深度）/ Confidence（三线性加权置信度）
    // 可视化色，spec 输出清零；Off（默认）零开销、输出逐字节不变。
    // Metal/Dawn 端 gather kernel 未接（引擎侧 Metal gather 不存在，
    // DDGIProbeAtlas 测试壳可视化另行走 atlas 预滤波路径）。
    // ------------------------------------------------------------------
    enum class ProbeVisMode : u32 {
        Off = 0,
        IrradianceAtlas = 1,
        DepthAtlas = 2,
        Confidence = 3,
        // 逐像素视线 DDA + 探针位置球体（半径 ≈0.2×spacing）。颜色 = L0 能量
        // 热力阶 + 埋没红染（octahedral 平均深度 < 0.25×spacing）。不做场景
        // 遮挡（穿墙可见——埋墙诊断正是目的）。Vulkan GIGather 独有。
        ProbePositions = 4,
    };
    ProbeVisMode probe_vis = ProbeVisMode::Off;

    // ------------------------------------------------------------------
    // 旧接口桥接（WASM sidebar → 统一旋钮）
    // ------------------------------------------------------------------
    void ApplyLegacy(const rhi::DawnDebugParams& p) {
        sky_color_intensity = p.skyColorIntensity;
        albedo_intensity = p.albedoIntensity;
        probe_hysteresis = p.probeHysteresis;
    }
};

// 进程级实例（Meyers singleton，WASM 安全）
inline GIDebugParams& GIDebug() {
    static GIDebugParams instance;
    return instance;
}

} // namespace primal::graphics::lumen
