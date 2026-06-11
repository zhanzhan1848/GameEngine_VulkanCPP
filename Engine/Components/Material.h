#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/MaterialComponent.h"
#include "Graphics/Material/ShaderTechnique.h"

namespace primal::material {

struct init_info
{
    graphics::ShaderTechnique technique{graphics::ShaderTechnique::Opaque};
    id::id_type albedo_texture{id::invalid_id};
    id::id_type normal_texture{id::invalid_id};
    id::id_type orm_texture{id::invalid_id};
    f32 base_color[4]{1.f, 1.f, 1.f, 1.f};
    f32 roughness{0.5f};
    f32 metallic{0.0f};
    f32 alpha_cutoff{0.5f};
};

material::component create(init_info info, game_entity::entity entity);
void remove(material::component c);

graphics::ShaderTechnique get_technique(component c);
f32 get_roughness(component c);
f32 get_metallic(component c);
f32 get_alpha_cutoff(component c);
void get_base_color(component c, f32 out[4]);

void set_roughness(component c, f32 value);
void set_metallic(component c, f32 value);
void set_base_color(component c, const f32 value[4]);
void set_alpha_cutoff(component c, f32 value);
void set_technique(component c, graphics::ShaderTechnique technique);

} // namespace primal::material
