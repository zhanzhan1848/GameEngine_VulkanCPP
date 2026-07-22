#pragma once

#include "Components/ComponentsCommon.h"

namespace primal::graphics { enum class ShaderTechnique : u8; }

namespace primal::material {

DEFINE_TYPED_ID(material_component_id);

class component final
{
public:
    constexpr explicit component(material_component_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    [[nodiscard]] constexpr material_component_id get_id() const { return _id; }
    [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    // Getters
    graphics::ShaderTechnique technique() const;
    f32 roughness() const;
    f32 metallic() const;
    f32 alpha_cutoff() const;
    void base_color(f32 out[4]) const;
    id::id_type albedo_texture() const;
    id::id_type normal_texture() const;
    id::id_type orm_texture() const;

    // Setters
    void set_technique(graphics::ShaderTechnique t);
    void set_roughness(f32 value);
    void set_metallic(f32 value);
    void set_base_color(const f32 value[4]);
    void set_alpha_cutoff(f32 value);
    void set_albedo_texture(id::id_type texture_id);
    void set_normal_texture(id::id_type texture_id);
    void set_orm_texture(id::id_type texture_id);

private:
    material_component_id _id;
};

} // namespace primal::material
