#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/Light.h"
#include "EngineAPI/LightComponent.h"

namespace primal::light {

struct init_info
{
    u64                 light_set_key{ 0 };
    graphics::light::type type{ graphics::light::directional };
    f32                 intensity{ 1.0f };
    math::v3            color{ 1.0f, 1.0f, 1.0f };
    math::v3            attenuation{ 1.0f, 0.0f, 0.0f };
    f32                 range{ 10.0f };
    f32                 umbra{ 0.0f };
    f32                 penumbra{ 0.0f };
    bool                is_enabled{ true };
};

light::component create(init_info info, game_entity::entity entity);
void remove(component c);

bool is_enabled(component c);
f32 intensity(component c);
math::v3 color(component c);
math::v3 attenuation(component c);
f32 range(component c);
f32 umbra(component c);
f32 penumbra(component c);
graphics::light::type light_type(component c);

void set_enabled(component c, bool value);
void set_intensity(component c, f32 value);
void set_color(component c, math::v3 value);
void set_attenuation(component c, math::v3 value);
void set_range(component c, f32 value);
void set_cone_angles(component c, f32 umbra, f32 penumbra);
void set_light_type(component c, graphics::light::type type);

} // namespace primal::light
