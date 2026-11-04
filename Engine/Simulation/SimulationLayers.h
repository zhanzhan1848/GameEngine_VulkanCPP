#pragma once

// SimulationLayers.h — 对象层策略 → Jolt 过滤表的桥。
// 策略（谁能碰谁）的单一事实源是 SimulationAPI.h 的 layers::can_collide 纯函数；
// 本文件只在 Initialize 时把该策略物化成 Jolt 的三张表。

#include "SimulationAPI.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
// 纪律：std/第三方头必须在所有 namespace 开括号之外包含。曾把 <optional> 放在
// namespace layers 内——macOS 因前缀头链已全局包含过而被 include guard 掩蔽，
// MSVC 首次包含落点在 namespace 内，std 声明全部污染进 layers::std（R7，94 条级联错）。
#include <optional>

namespace primal::simulation::layers {

// 三张表 + ObjectVsBroadPhaseLayerFilterTable 持有前两张的引用。
// 注意构造顺序：bp_filter 的位表在构造时从 bp_interface/object_filter **快照**，
// 所以必须先填层映射与碰撞矩阵，再 emplace bp_filter。

struct jolt_tables {
    JPH::BroadPhaseLayerInterfaceTable      bp_interface;
    JPH::ObjectLayerPairFilterTable         object_filter;
    std::optional<JPH::ObjectVsBroadPhaseLayerFilterTable> bp_filter;

    jolt_tables();
};

} // namespace primal::simulation::layers
