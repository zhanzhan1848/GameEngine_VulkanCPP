#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/Camera.h"
#include "EngineAPI/CameraComponent.h"

namespace primal::camera {

struct init_info
{
    graphics::camera::type   type{ graphics::camera::perspective };
    math::v3                 up{ 0.0f, 1.0f, 0.0f };
    union
    {
        f32                   field_of_view{ 0.25f };
        f32                   view_width;
    };
    union
    {
        f32                   aspect_ratio{ 16.0f / 9.0f };
        f32                   view_height;
    };
    f32                       near_z{ 0.1f };
    f32                       far_z{ 64.0f };
};

camera::component create(init_info info, game_entity::entity entity);
void remove(component c);

math::v3 up_vector(component c);
f32 field_of_view(component c);
f32 aspect_ratio(component c);
f32 view_width(component c);
f32 view_height(component c);
f32 near_z(component c);
f32 far_z(component c);
graphics::camera::type projection_type(component c);

math::m4x4 view(component c);
math::m4x4 projection(component c);
math::m4x4 inverse_projection(component c);
math::m4x4 view_projection(component c);
math::m4x4 inverse_view_projection(component c);

void set_up_vector(component c, math::v3 up);
void set_field_of_view(component c, f32 fov);
void set_aspect_ratio(component c, f32 aspect);
void set_view_width(component c, f32 width);
void set_view_height(component c, f32 height);
void set_range(component c, f32 near_z, f32 far_z);

} // namespace primal::camera
