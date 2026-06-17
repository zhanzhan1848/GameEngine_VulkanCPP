#pragma once

#include "Components/ComponentsCommon.h"
#include "EngineAPI/Light.h"

namespace primal::light {

DEFINE_TYPED_ID(light_component_id);

class component final
{
public:
    constexpr explicit component(light_component_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    [[nodiscard]] constexpr light_component_id get_id() const { return _id; }
    [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    // Getters
    bool is_enabled() const;
    f32 intensity() const;
    math::v3 color() const;
    math::v3 attenuation() const;
    f32 range() const;
    f32 umbra() const;
    f32 penumbra() const;
    graphics::light::type light_type() const;

    // Setters
    void set_enabled(bool value);
    void set_intensity(f32 value);
    void set_color(math::v3 value);
    void set_attenuation(math::v3 value);
    void set_range(f32 value);
    void set_cone_angles(f32 umbra, f32 penumbra);
    void set_light_type(graphics::light::type type);

private:
    light_component_id _id;
};

} // namespace primal::light
