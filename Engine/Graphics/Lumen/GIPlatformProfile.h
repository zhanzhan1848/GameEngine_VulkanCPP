#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"

namespace primal::graphics::rhi { class RHIDeviceBase; }

namespace primal::graphics::lumen {

// ============================================================================
// GIPlatformProfile — 决策①的载体（计划 §3）
//
// 允许分歧的质量/性能参数集中在这一处；算法语义（radiance 模型、SH 数学、
// EMA、网格索引、序列化格式）必须三端一致，由 GLSL canonical 源定义。
//
// 能力位为静态（设备生命周期内不变）；质量默认档可被 GIAdaptiveGovernor
// （T4.1）在允许范围内向下调制，但不得突破能力位上限。
// ============================================================================
struct GIPlatformProfile {
    // ------------------------------------------------------------------
    // 静态能力位（C5 后端能力上报）
    // ------------------------------------------------------------------
    bool texture_array_dynamic_indexing = false; ///< descriptor indexing 可用
    u32  texture3d_read_budget = 4;    ///< 每线程 texture3D 采样上限（Apple ~4）
    u32  buffer_read_budget = 32;      ///< 每线程 device/constant buffer 读上限（Apple ~32）
    u32  storage_texture_budget = 8;   ///< 每 kernel 可写 storage image 数
    bool split_dispatch_preferred = false; ///< 读数超限时拆 pass（Metal split 模式的后端组合策略）

    // ------------------------------------------------------------------
    // 质量默认档（governor 调制基线）
    // ------------------------------------------------------------------
    u32  sdf_trace_steps = 16;         ///< SDF relaxed march 步数
    u32  rays_per_probe = 64;          ///< 注：split trace 内核需跨步改造才支持 >64
    u32  max_probes_per_frame = 16384; ///< 每帧 probe 更新预算
    u32  page_budget = 256;            ///< SC LightEval 每帧页预算
    bool spgi_enabled = false;         ///< Screen Probe GI 默认档（M5/T5.6 后 High+ 开）
    bool ssgi_enabled = true;

    // ------------------------------------------------------------------
    // 预设档
    // ------------------------------------------------------------------
    // Apple Silicon（Metal）：split dispatch 是常态
    // 2026-09-04 探针预算修正：4096→16384（全量更新）。
    // 4096/16384 = 25%/帧 → 75% 探针持有旧值 → 更新/未更新探针混插
    // → gather 插值空间不连续（棋盘格）+ 逐帧轮转子集（闪烁）。
    // Vulkan 全量更新无此问题。性能：16384 探针 × 128 射线 = 2M ray march，
    // Apple Silicon M4 实测可接受。
    static GIPlatformProfile AppleSilicon() {
        GIPlatformProfile p{};
        p.texture_array_dynamic_indexing = false;
        p.texture3d_read_budget = 4;
        p.buffer_read_budget = 32;
        p.storage_texture_budget = 8;
        p.split_dispatch_preferred = true;
        p.rays_per_probe = 64;
        p.max_probes_per_frame = 16384;  // 全量更新（与 Vulkan 一致）
        p.page_budget = 128;
        p.spgi_enabled = false;
        p.ssgi_enabled = true;
        return p;
    }

    // 桌面 Vulkan/D3D：预算宽松
    static GIPlatformProfile Desktop() {
        GIPlatformProfile p{};
        p.texture_array_dynamic_indexing = true;
        p.texture3d_read_budget = 16;
        p.buffer_read_budget = 64;
        p.storage_texture_budget = 8;
        p.split_dispatch_preferred = false;
        p.rays_per_probe = 64;
        p.max_probes_per_frame = 16384;
        p.page_budget = 256;
        p.spgi_enabled = false;
        p.ssgi_enabled = true;
        return p;
    }

    // Dawn/WASM：保守档（性能红线优先）
    static GIPlatformProfile Web() {
        GIPlatformProfile p{};
        p.texture_array_dynamic_indexing = false;
        p.texture3d_read_budget = 4;
        p.buffer_read_budget = 32;
        p.storage_texture_budget = 4;
        p.split_dispatch_preferred = true;
        p.rays_per_probe = 32;
        p.max_probes_per_frame = 1024;
        p.page_budget = 64;
        p.spgi_enabled = false;
        p.ssgi_enabled = false;
        return p;
    }

    // 按设备平台选择预设（未来可加硬件查询细化）
    static GIPlatformProfile Detect(const rhi::RHIDeviceBase* device);
};

} // namespace primal::graphics::lumen
