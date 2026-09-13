#pragma once

// BudgetManager.h — 全局预算 / 重要性 LOD（架构文档 §2 渲染器表 Scalability 行、§7-N6）。
// 纯 CPU 决策器：按相机距离与屏幕占比给出发射器 LOD 因子与 spawn 缩放；
// 预算上限提供全局否决（ADR6 的系统级部分——刚体预算在 RigidBodySpawner 内）。
// 确定性：同输入同输出，无隐式全局状态。

#include "../PfxTypes.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::host {

struct budget_config {
    u32 max_emitters{ 32 };             // 并发发射器上限（超出者按优先级拒新）
    u32 max_live_particles{ 200000 };   // 全局活粒子上限
    f32 lod_near{ 20.f };               // 距离 < near ⇒ 因子 1
    f32 lod_far{ 120.f };               // 距离 > far ⇒ 因子 = min_factor
    f32 min_factor{ 0.1f };             // 最远 LOD 的 spawn 缩放下限
};

class budget_manager {
public:
    explicit budget_manager(const budget_config& config = {}) : _config(config) {}

    void set_config(const budget_config& config) { _config = config; }
    const budget_config& config() const { return _config; }

    // 距离 → spawn 缩放因子（线性衰减 near→far，clamp [min_factor, 1]）。
    f32 lod_factor(f32 distance_to_camera) const;

    // 全局活粒子预算否决：申请 n 个新粒子，返回允许数。
    u32 approve_spawn(u32 current_live, u32 requested) const {
        const u32 room = _config.max_live_particles > current_live
                             ? _config.max_live_particles - current_live
                             : 0u;
        return requested < room ? requested : room;
    }

    // 发射器预算否决：当前并发数是否允许再开一个。
    bool approve_emitter(u32 current_emitters) const { return current_emitters < _config.max_emitters; }

private:
    budget_config _config;
};

} // namespace primal::pfx::host

#endif // !DISABLE_PARTICLE_FX
