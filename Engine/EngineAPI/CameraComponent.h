#pragma once

#include "Components/ComponentsCommon.h"
#include "EngineAPI/Camera.h"

namespace primal::camera {

DEFINE_TYPED_ID(camera_component_id);

class component final
{
public:
    constexpr explicit component(camera_component_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    [[nodiscard]] constexpr camera_component_id get_id() const { return _id; }
    [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    // Getters
    math::v3 up_vector() const;
    f32 field_of_view() const;
    f32 aspect_ratio() const;
    f32 view_width() const;
    f32 view_height() const;
    f32 near_z() const;
    f32 far_z() const;
    graphics::camera::type projection_type() const;

    math::m4x4 view() const;
    math::m4x4 projection() const;
    math::m4x4 inverse_projection() const;
    math::m4x4 view_projection() const;
    math::m4x4 inverse_view_projection() const;

    // Setters
    void set_up_vector(math::v3 up);
    void set_field_of_view(f32 fov);
    void set_aspect_ratio(f32 aspect);
    void set_view_width(f32 width);
    void set_view_height(f32 height);
    void set_range(f32 near_z, f32 far_z);

private:
    camera_component_id _id;
};

} // namespace primal::camera
