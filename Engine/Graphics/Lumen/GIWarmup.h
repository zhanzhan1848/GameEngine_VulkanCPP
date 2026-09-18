#pragma once

#include "CommonHeaders.h"

#include <algorithm>
#include <cstdlib>

namespace primal::graphics::lumen {

// ============================================================================
// GIWarmup — PRIMAL_SC_WARMUP_FRAMES 单点定义（GI 能量所有权修复 Phase 1a，
// 审计 §2.4 新发现②）。
//
// 同名旋钮此前语义漂移：SC 侧默认 90（SurfaceCachePass），DDGI 侧默认 0
// （LumenDDGIPass）——不设 env 时两模块的 warmup 行为本就不一致。
// 统一默认 90：两模块同窗冻结是 C3.5 slice 3 的设计意图——atlas 在纯直射
// 下确定性收敛后，SC probe feed 与 DDGI 探针更新同时启动，避免多稳态分支
// 在瞬态上被选择（09-10 十二）。DDGI 侧默认 0→90 是本阶段唯一显式行为
// 变更，理由记录于修复计划 §执行日志。
// ============================================================================
inline u32 GIWarmupFrames() {
    static const u32 v = [] {
        const char* e = std::getenv("PRIMAL_SC_WARMUP_FRAMES");
        if (e) return (u32)std::max(0, std::atoi(e));
        return 90u;
    }();
    return v;
}

// ============================================================================
// GIProbeFeedBootstrap — WRC 计划 Stream B1（能量单向化）单一开关：
// PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1 时，SC LightEval 的探针馈送
// （gatherProbeIrradiance，能量审计边 E1）从"暖启动后常开"改为"暖启动
// 引导项"——GIWarmup 窗口内生效、窗口内线性衰减到 0，窗口后恒 0；同时
// DDGI 探针更新解除窗口冻结（从第 0 帧起更新，引导项才有真实反弹可供给），
// SC 的常备间接来源交棒 tile radiosity（B2 单轨）。默认 0 = 旧语义逐位不变。
// 回滚：置 0。
// ============================================================================
inline bool GIProbeFeedBootstrapEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("PRIMAL_GI_PROBE_FEED_BOOTSTRAP");
        return e && e[0] == '1';
    }();
    return v;
}

// 引导因子：窗口内 1 → 0 线性，窗口后 0。bootstrap 关闭时恒 1
// （shader 端乘法恒等，旧语义逐位不变）。
inline float GIProbeBootstrapFactor(u32 frame) {
    if (!GIProbeFeedBootstrapEnabled()) return 1.0f;
    const u32 w = GIWarmupFrames();
    if (w == 0u) return 0.0f;
    const float t = (float)frame / (float)w;
    return std::max(0.0f, std::min(1.0f, 1.0f - t));
}

} // namespace primal::graphics::lumen
