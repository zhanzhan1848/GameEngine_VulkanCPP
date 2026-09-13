// RigidBodySpawner.cpp — 见 RigidBodySpawner.h。

#include "RigidBodySpawner.h"
#include "../core/PfxSystem.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

rigid_body_spawner::rigid_body_spawner(emitter_id source, const rigid_spawn_policy& policy)
    : _source(source), _policy(policy) {}

void rigid_body_spawner::spawn_body_for(const particle_handle& h, attribute_store& store) {
    // 预算否决（ADR6）
    if (_tracked.size() >= _policy.max_live_bodies) {
        if (_policy.overflow_policy == rigid_spawn_policy::overflow::reject_newest) {
            ++_rejected;
            return;   // 粒子继续存活（纯视觉），只是不建 body
        }
        // retire_oldest：挤掉最老 → 强制提前杀死对应粒子（death 事件闭环）
        u64 oldest_order = ~0ull;
        size_t oldest = 0;
        for (size_t i = 0; i < _tracked.size(); ++i) {
            if (_tracked[i].birth_order < oldest_order) {
                oldest_order = _tracked[i].birth_order;
                oldest = i;
            }
        }
        void* resolved = store.resolve(_tracked[oldest].handle, attr::velocity);
        if (resolved) {
            // 寿命缩到当前 age 触发到期（下一 tick 由发射器回收并产生 death 事件）
            math::v4* vel = (math::v4*)resolved;
            math::v4* pos = store.v4_channel(attr::position) + _tracked[oldest].handle.row;
            vel->w = pos->w;   // age == lifetime ⇒ 下一 tick 到期回收
        }
        dispose_body(_tracked[oldest], nullptr);
        _tracked.erase(_tracked.begin() + (long)oldest);
        ++_retired;
    }

    const math::v4* pos = store.v4_channel(attr::position) + h.row;
    const math::v4* vel = store.v4_channel(attr::velocity) + h.row;

    simulation::body_desc d;
    d.s = _policy.shape;
    d.m = simulation::body_desc::mode::dynamic;
    d.half_extents = _policy.half_extents;
    d.position = math::v3{ pos->x, pos->y, pos->z };
    d.mass = _policy.mass;
    d.restitution = _policy.restitution;
    d.friction = _policy.friction;
    d.debug_draw_proxy = false;   // 刚体化粒子的 proxy 线框由 spawner 预算 HUD 决定，默认关

    const u64 body_id = simulation::body_create(d);
    if (body_id == 0) {
        ++_rejected;
        return;
    }
    if (vel->x != 0.f || vel->y != 0.f || vel->z != 0.f) {
        simulation::body_set_linear_velocity(body_id, math::v3{ vel->x, vel->y, vel->z });
    }
    _tracked.push_back(tracked{ body_id, h, _birth_counter++ });
}

void rigid_body_spawner::dispose_body(tracked& t, attribute_store* store) {
    switch (_policy.death_action) {
        case rigid_spawn_policy::on_death::freeze:
            // 冻结：body 保留在物理世界（停止追踪——粒子已死，不再回写）
            break;
        case rigid_spawn_policy::on_death::orphan:
            // 转常驻：与 destroy 相同的清理路径，但 body 保留（语义：脱离粒子生命周期）
            break;
        case rigid_spawn_policy::on_death::destroy:
        default:
            simulation::body_remove(t.body_id);
            break;
    }
    (void)store;
}

void rigid_body_spawner::tick(f32 dt, attribute_store& store) {
    (void)dt;

    // ---- 事件消费（v1 单消费者：drain 全部，处理 source 匹配项）----
    pfx_event events[128];
    u32 n = drain_events(events, 128);
    while (n > 0) {
        for (u32 i = 0; i < n; ++i) {
            const pfx_event& e = events[i];
            if (e.source != _source) continue;

            if (e.type == event_type::birth) {
                spawn_body_for(e.handle, store);
            } else if (e.type == event_type::death) {
                for (size_t k = 0; k < _tracked.size(); ++k) {
                    if (_tracked[k].handle == e.handle) {
                        dispose_body(_tracked[k], &store);
                        _tracked.erase(_tracked.begin() + (long)k);
                        break;
                    }
                }
            }
        }
        n = drain_events(events, 128);
    }

    // ---- 物理权威回写（ADR3 单写）：body transform → 粒子 position ----
    for (const tracked& t : _tracked) {
        simulation::body_transform bt;
        if (!simulation::body_get_transform(t.body_id, bt)) continue;
        void* resolved = store.resolve(t.handle, attr::position);
        if (!resolved) continue;   // 粒子行异常（正常不应发生——death 事件先于行复用）
        math::v4* pos = (math::v4*)resolved;
        pos->x = bt.position.x;
        pos->y = bt.position.y;
        pos->z = bt.position.z;
    }
}

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
