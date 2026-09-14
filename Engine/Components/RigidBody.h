#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/RigidBodyComponent.h"

namespace primal::rigidbody {

// 刚体组件：依赖同实体的 Collider。提供质量/材质/运动模式。
// 无 RigidBody 的 Collider = 静态体。

struct init_info
{
    u32      mode{ 1 };                    // 0=static 1=dynamic 2=kinematic
    f32      mass{ 1.0f };
    f32      restitution{ 0.1f };
    f32      friction{ 0.5f };
    math::v3 linear_velocity{ 0.f, 0.f, 0.f };
};

rigidbody::component create(init_info info, game_entity::entity entity);
void remove(component c);

u32 mode(component c);
f32 mass(component c);
f32 restitution(component c);
f32 friction(component c);
math::v3 linear_velocity(component c);

void set_mode(component c, u32 mode);
void set_mass(component c, f32 mass);
void set_restitution(component c, f32 restitution);
void set_friction(component c, f32 friction);
void set_linear_velocity(component c, math::v3 velocity);

} // namespace primal::rigidbody
