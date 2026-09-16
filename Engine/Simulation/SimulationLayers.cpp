#include "SimulationLayers.h"

namespace primal::simulation::layers {

bool can_collide(u32 a, u32 b)
{
    if (a == b) {
        // static-static / trigger-trigger 永不互碰；dynamic、debris 同层互碰
        return a == kDynamic || a == kDebris;
    }
    // 有序化后查策略：只列 (小, 大) 一侧
    const u32 lo = (a < b) ? a : b;
    const u32 hi = (a < b) ? b : a;

    // static vs dynamic/trigger：碰；static vs debris：不碰（碎屑只与场景碰）
    // dynamic vs trigger：碰（触发器事件）；dynamic vs debris：碰
    // debris vs trigger：碰；无其余
    if (lo == kStatic)  return hi == kDynamic || hi == kTrigger || hi == kParticle;
    if (lo == kDynamic) return hi == kDebris || hi == kTrigger || hi == kParticle;
    // kParticle:只与 static/dynamic 碰(上方已列),与 debris/trigger 及同层不碰
    if (lo == kDebris)  return hi == kTrigger;
    return false;
}

u32 broad_phase_layer_for(u32 object_layer)
{
    switch (object_layer) {
        case kStatic:
        case kTrigger:
            return kBroadPhaseStatic;
        case kDynamic:
        case kDebris:
        default:
            return kBroadPhaseMoving;
    }
}

jolt_tables::jolt_tables()
    : bp_interface{ kCount, kBroadPhaseCount }
    , object_filter{ kCount }
{
    // 先填映射与碰撞矩阵——bp_filter 构造时会对它们做快照
    for (u32 a = 0; a < kCount; ++a) {
        bp_interface.MapObjectToBroadPhaseLayer(
            a, JPH::BroadPhaseLayer{ static_cast<JPH::BroadPhaseLayer::Type>(broad_phase_layer_for(a)) });
        for (u32 b = a; b < kCount; ++b) {
            if (can_collide(a, b)) {
                object_filter.EnableCollision(a, b);
            }
        }
    }
    bp_filter.emplace(bp_interface, kBroadPhaseCount, object_filter, kCount);
}

} // namespace primal::simulation::layers
