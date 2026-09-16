#pragma once

// PhysicsBindings.h — 逐粒子刚体绑定的运行时管理器（路线图 P2.1，ADR3/ADR6）。
//
// 职责：把 RigidBodySpawner（N3 已验证的建/删/回写实现）接入运行时管线——
// 事件由本管理器每帧 drain 一次（取代 spawner 各自 drain 的单消费者假设），
// 按 source 路由进对应 spawner 的 consume，再逐绑定 writeback 物理 transform。
//
// 生命周期：EngineDLL 在发射器 create/reload 时 set_enabled（程序 instance.physics
// 声明刚体时），destroy/reload 失配时 remove；PhysicsShutdown 时 clear。
// 全局预算：total_live_bodies 达 global_body_budget 后不再路由 birth 事件
//（事件按总线纪律自然丢弃）；per-emitter 上限由各自 policy 管辖。

#include "RigidBodySpawner.h"

#include <unordered_map>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

class physics_bindings {
public:
    static physics_bindings& get();

    // 启用/更新某发射器的刚体绑定（policy 由程序 instance.physics 构造）。
    void set_enabled(emitter_id id, const rigid_spawn_policy& policy);
    // 移除绑定（连带按 on_death 策略处置 body——dispose 走 spawner 析构外的显式清理）。
    void remove(emitter_id id);
    void clear();

    // 每帧（物理 step + pfx tick 之后）：drain 一次 → 按 source 路由 → writeback。
    void tick();

    void set_global_body_budget(u32 cap) { _global_body_budget = cap; }
    u32  binding_count() const { return (u32)_bindings.size(); }
    u32  total_live_bodies() const;

private:
    std::unordered_map<emitter_id, rigid_body_spawner> _bindings;
    u32 _global_body_budget{ 1024 };
};

// EngineDLL PipelineTick 尾部的便捷入口（安全：无绑定时零开销）。
void physics_bindings_tick();

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
