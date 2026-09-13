#pragma once

// RigidBodySpawner.h — 粒子刚体化消费者（架构文档 §3.5/N3，ADR3/ADR6）。
//
// 契约：发射器出生的每颗粒子诞生即对应一个 Jolt body（形状/质量来自策略），
// 物理持有变换权威（每帧回写粒子 position 通道，渲染经 SpriteBridge 读取）；
// 粒子死亡按 on_death 策略处置 body。粒子核心零感知——本模块只是事件消费者。
//
// 预算否决（ADR6）：活跃 body 数达 max_live_bodies 时按 overflow 策略
// （reject=拒新 / retire=挤最老）降级；retire 会强制提前杀死对应粒子（发 death 事件）。

#include "../PfxTypes.h"
#include "../core/AttributeStore.h"
#include "Simulation/SimulationAPI.h"

#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

struct rigid_spawn_policy {
    simulation::body_desc::shape shape{ simulation::body_desc::shape::box };
    math::v3 half_extents{ 0.05f, 0.05f, 0.05f };   // box 半尺寸 / sphere.x = 半径
    f32 mass{ 1.f };
    f32 restitution{ 0.2f };
    f32 friction{ 0.5f };

    enum class on_death : u8 { destroy = 0, freeze = 1, orphan = 2 };
    on_death death_action{ on_death::destroy };

    u32 max_live_bodies{ 256 };
    enum class overflow : u8 { reject_newest = 0, retire_oldest = 1 };
    overflow overflow_policy{ overflow::reject_newest };

    f32 cull_distance{ 0.f };   // >0：距源超此距离不建 body（纯视觉降级）
};

// v1 单消费者假设：spawner 自行 drain 系统事件流，处理属于 source 的部分，
// 其余事件转发给 forward 输出（多消费者路由在事件总线升级时替换）。
class rigid_body_spawner {
public:
    rigid_body_spawner(emitter_id source, const rigid_spawn_policy& policy);

    // 每帧（物理 step 之后）：消费事件 → 建体/处置 → 物理 transform 回写粒子通道。
    void tick(f32 dt, attribute_store& store);

    u32 live_bodies() const { return (u32)_tracked.size(); }
    u32 rejected_total() const { return _rejected; }
    u32 retired_total() const { return _retired; }

    const rigid_spawn_policy& policy() const { return _policy; }

private:
    struct tracked {
        u64 body_id{ 0 };
        particle_handle handle{};
        u64 birth_order{ 0 };
    };

    void spawn_body_for(const particle_handle& h, attribute_store& store);
    void dispose_body(tracked& t, attribute_store* store);

    emitter_id                _source{ invalid_emitter };
    rigid_spawn_policy        _policy;
    std::vector<tracked>      _tracked;
    u64                       _birth_counter{ 0 };
    u32                       _rejected{ 0 };
    u32                       _retired{ 0 };
};

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
