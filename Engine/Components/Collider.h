#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/ColliderComponent.h"

namespace primal::collider {

// 碰撞体组件：形状 + 层。语义（设计文档 §3）：
//   - 有 Collider 无 RigidBody 的实体 = 静态体（关卡碰撞）；
//   - Collider + RigidBody = 动态体（由 RigidBody 提供质量/材质/模式）。
// 物理体物化时机由 EngineDLL/上层编排（M2：Add 即物化，play 状态机在 M3）。

struct init_info
{
    u32      shape{ 0 };                              // 0=box 1=sphere
    math::v3 half_extents{ 0.5f, 0.5f, 0.5f };        // box 半尺寸 / sphere x=半径
    math::v3 offset{ 0.f, 0.f, 0.f };                 // 相对实体原点的偏移
    bool     is_trigger{ false };
    u32      layer{ 1 };                              // simulation::layers::kDynamic
};

collider::component create(init_info info, game_entity::entity entity);
void remove(component c);

u32 shape(component c);
math::v3 half_extents(component c);
math::v3 offset(component c);
bool is_trigger(component c);
u32 layer(component c);

void set_shape(component c, u32 shape);
void set_half_extents(component c, math::v3 half);
void set_offset(component c, math::v3 offset);
void set_is_trigger(component c, bool trigger);
void set_layer(component c, u32 layer);

} // namespace primal::collider
