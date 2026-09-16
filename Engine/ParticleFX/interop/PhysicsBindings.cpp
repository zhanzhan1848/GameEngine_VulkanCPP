// PhysicsBindings.cpp — 见 PhysicsBindings.h。

#include "PhysicsBindings.h"
#include "../core/PfxSystem.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>

namespace primal::pfx::interop {

physics_bindings& physics_bindings::get() {
    static physics_bindings inst;
    return inst;
}

void physics_bindings::set_enabled(emitter_id id, const rigid_spawn_policy& policy) {
    if (id == invalid_emitter) return;
    auto it = _bindings.find(id);
    if (it != _bindings.end()) {
        // reload:先处置旧跟踪体(destroy 策略 → body_remove),否则泄漏成
        // "幽灵碰撞体"残留物理世界推挤新粒子(层矩阵对 kParticle↔kDynamic 仍生效)
        it->second.dispose_all_bodies();
        it->second = rigid_body_spawner(id, policy);
        return;
    }
    _bindings.emplace(id, rigid_body_spawner(id, policy));
}

void physics_bindings::remove(emitter_id id) {
    auto it = _bindings.find(id);
    if (it != _bindings.end()) {
        it->second.dispose_all_bodies();   // destroy 策略的 body 不残留进物理世界
        _bindings.erase(it);
    }
}

void physics_bindings::clear() {
    for (auto& [_, sp] : _bindings) sp.dispose_all_bodies();
    _bindings.clear();
}

u32 physics_bindings::total_live_bodies() const {
    u32 total = 0;
    for (const auto& [_, sp] : _bindings) total += sp.live_bodies();
    return total;
}

void physics_bindings::tick() {
    if (_bindings.empty()) return;

    // ---- 事件 drain 一次 → 按 source 路由（多消费者路由，取代 spawner 各自 drain）----
    pfx_event ev[256];
    u32 n = drain_events(ev, 256);
    while (n > 0) {
        for (u32 i = 0; i < n; ++i) {
            auto it = _bindings.find(ev[i].source);
            if (it == _bindings.end()) continue;
            if (ev[i].type == event_type::birth &&
                total_live_bodies() >= _global_body_budget) {
                continue;   // 全局预算否决：birth 事件丢弃（粒子存活，纯视觉）
            }
            pfx_emitter* em = emitter_get(ev[i].source);
            if (!em) continue;
            it->second.consume(&ev[i], 1, em->store());
        }
        n = drain_events(ev, 256);
    }

    // ---- 逐绑定物理权威回写（ADR3 单写）----
    for (auto& [id, sp] : _bindings) {
        pfx_emitter* em = emitter_get(id);
        if (!em) continue;
        sp.writeback(em->store());
    }
}

void physics_bindings_tick() { physics_bindings::get().tick(); }

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
