#pragma once

// EmissiveGI.h — 发光粒子 → Lumen T4.4 emissive 闭环注入器
// （设计文档：Docs/2026-09-13-pfx-emissive-gi-injection-design.md，v1 聚合单槽）。
//
// 复用 T4.4 全链：SetInstanceEmissive 表 → capture → emissive_atlas →
// LightEval（8.0 钳制）→ SC↔probe 反弹。PFX 仅新增聚合写入器，零 shader 改动。
// 强度门禁复用：注入前逐通道钳制 ≤ 8.0（与 LightEval 双重钳制，保下游不变量）。

#include "../PfxTypes.h"

#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

// 单个光斑样本（light 渲染器 stage 数据的聚合输入形态）
struct light_sample {
    math::v3 color;       // rgb
    f32 intensity;        // HDR 强度
};

// T4.4 强度钳制（与 LightEval 一致；双重钳制保下游不变量）
constexpr f32 kMaxEmissiveRadiance{ 8.0f };

// 窄接口：宿主以 SurfaceCachePass::SetInstanceEmissive 适配注入
struct emissive_writer {
    virtual void write(u32 instance_id, math::v3 rgb) = 0;
    virtual ~emissive_writer() = default;
};

struct emissive_gi_config {
    u32 reserved_instance_id{ 1023 };   // 保留槽（kMaxEmissiveInstances-1）
    f32 radiance_scale{ 1.0f };
    u32 min_alive{ 1 };                 // 活光斑低于此值不注入
};

class emissive_gi_injector {
public:
    explicit emissive_gi_injector(emissive_gi_config cfg = {}) : _cfg(cfg) {}

    // 聚合光斑 radiance（逐通道钳制 ≤ kMaxEmissiveRadiance × scale 前置缩放）。
    // 活光斑数 < min_alive 时不注入并返回 false。
    bool aggregate(const std::vector<light_sample>& lights, math::v3& out_radiance) const;

    // 每帧入口：聚合 + 经 writer 写保留槽。writer 为空或未注入时静默。
    bool tick(const std::vector<light_sample>& lights, emissive_writer& writer) const;

    const emissive_gi_config& config() const { return _cfg; }

private:
    emissive_gi_config _cfg;
};

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
