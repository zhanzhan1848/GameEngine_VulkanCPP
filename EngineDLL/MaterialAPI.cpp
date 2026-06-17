#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "../Engine/Components/Entity.h"
#include "../Engine/Components/Material.h"
#include "../Engine/EngineAPI/MaterialComponent.h"
#include "../Engine/Components/ComponentTraits.h"
#include "../Engine/Graphics/Material/ShaderTechnique.h"

using namespace primal;

namespace {

struct material_descriptor
{
    u32 technique;
    f32 base_color[4];
    f32 roughness;
    f32 metallic;
    f32 alpha_cutoff;
    id::id_type albedo_texture;
    id::id_type normal_texture;
    id::id_type orm_texture;
};

struct material_param_update_c
{
    u64 entity_id;
    u32 param_id;    // 0=technique, 1=roughness, 2=metallic, 3=alpha_cutoff, 4-7=base_color[0-3]
    u32 _pad;
    f32 value;
};

material::component component_from_entity(id::id_type entity_id)
{
    game_entity::entity entity{ game_entity::entity_id{entity_id} };
    if (!entity.is_valid()) return {};
    component_mask mask{ game_entity::get_component_mask(game_entity::entity_id{entity_id}) };
    if (!(mask & bit_mask(component_bit::Material))) return {};
    return material::component{ material::material_component_id{entity_id} };
}

} // anonymous namespace

// ============================================================================
// Material Lifecycle
// ============================================================================

EDITOR_INTERFACE void AddEntityMaterial(id::id_type entity_id, material_descriptor* desc)
{
    assert(desc);
    game_entity::entity entity{ game_entity::entity_id{entity_id} };
    if (!entity.is_valid()) return;

    material::init_info info{};
    info.technique = static_cast<graphics::ShaderTechnique>(
        std::min(desc->technique, static_cast<u32>(graphics::ShaderTechnique::Count) - 1));
    memcpy(info.base_color, desc->base_color, sizeof(info.base_color));
    info.roughness = desc->roughness;
    info.metallic = desc->metallic;
    info.alpha_cutoff = desc->alpha_cutoff;
    info.albedo_texture = desc->albedo_texture;
    info.normal_texture = desc->normal_texture;
    info.orm_texture = desc->orm_texture;

    material::component mc = material::create(info, entity);
    if (mc.is_valid())
    {
        game_entity::set_component_bit(entity.get_id(), static_cast<u8>(component_bit::Material));
    }
}

EDITOR_INTERFACE void RemoveEntityMaterial(id::id_type entity_id)
{
    game_entity::entity entity{ game_entity::entity_id{entity_id} };
    if (!entity.is_valid()) return;

    material::component mc{ material::material_component_id{entity_id} };
    if (!mc.is_valid()) return;

    material::remove(mc);
    game_entity::clear_component_bit(game_entity::entity_id{entity_id}, static_cast<u8>(component_bit::Material));
}

EDITOR_INTERFACE u32 HasEntityMaterial(id::id_type entity_id)
{
    component_mask mask{ game_entity::get_component_mask(game_entity::entity_id{entity_id}) };
    return (mask & bit_mask(component_bit::Material)) ? 1u : 0u;
}

// ============================================================================
// Material Getters
// ============================================================================

EDITOR_INTERFACE u32 GetMaterialTechnique(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0;
    return static_cast<u32>(mc.technique());
}

EDITOR_INTERFACE f32 GetMaterialRoughness(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0.5f;
    return mc.roughness();
}

EDITOR_INTERFACE f32 GetMaterialMetallic(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0.0f;
    return mc.metallic();
}

EDITOR_INTERFACE f32 GetMaterialAlphaCutoff(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0.5f;
    return mc.alpha_cutoff();
}

EDITOR_INTERFACE void GetMaterialBaseColor(id::id_type entity_id, f32* out_rgba)
{
    if (!out_rgba) return;
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.base_color(out_rgba);
}

// ============================================================================
// Material Setters
// ============================================================================

EDITOR_INTERFACE void SetMaterialTechnique(id::id_type entity_id, u32 technique)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_technique(static_cast<graphics::ShaderTechnique>(
        std::min(technique, static_cast<u32>(graphics::ShaderTechnique::Count) - 1)));
}

EDITOR_INTERFACE void SetMaterialRoughness(id::id_type entity_id, f32 value)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_roughness(value);
}

EDITOR_INTERFACE void SetMaterialMetallic(id::id_type entity_id, f32 value)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_metallic(value);
}

EDITOR_INTERFACE void SetMaterialAlphaCutoff(id::id_type entity_id, f32 value)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_alpha_cutoff(value);
}

EDITOR_INTERFACE void SetMaterialBaseColor(id::id_type entity_id, const f32* rgba)
{
    if (!rgba) return;
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_base_color(rgba);
}

// ============================================================================
// Material Texture Getters
// ============================================================================

EDITOR_INTERFACE u64 GetMaterialAlbedoTexture(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0;
    return static_cast<u64>(mc.albedo_texture());
}

EDITOR_INTERFACE u64 GetMaterialNormalTexture(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0;
    return static_cast<u64>(mc.normal_texture());
}

EDITOR_INTERFACE u64 GetMaterialOrmTexture(id::id_type entity_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return 0;
    return static_cast<u64>(mc.orm_texture());
}

// ============================================================================
// Material Texture Setters
// ============================================================================

EDITOR_INTERFACE void SetMaterialAlbedoTexture(id::id_type entity_id, u64 texture_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_albedo_texture(static_cast<id::id_type>(texture_id));
}

EDITOR_INTERFACE void SetMaterialNormalTexture(id::id_type entity_id, u64 texture_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_normal_texture(static_cast<id::id_type>(texture_id));
}

EDITOR_INTERFACE void SetMaterialOrmTexture(id::id_type entity_id, u64 texture_id)
{
    material::component mc = component_from_entity(entity_id);
    if (!mc.is_valid()) return;
    mc.set_orm_texture(static_cast<id::id_type>(texture_id));
}

// ============================================================================
// Entity Enumeration
// ============================================================================

EDITOR_INTERFACE u32 GetEntityCountWithMaterial()
{
    const u32 count = game_entity::entity_count();
    u32 result = 0;
    for (u32 i = 0; i < count; ++i)
    {
        game_entity::entity_id eid{ static_cast<id::id_type>(i) };
        if (!game_entity::is_alive(eid)) continue;
        component_mask mask{ game_entity::get_component_mask(eid) };
        if (mask & bit_mask(component_bit::Material)) ++result;
    }
    return result;
}

EDITOR_INTERFACE u32 GetEntityIdWithMaterial(u32 index)
{
    const u32 count = game_entity::entity_count();
    u32 match = 0;
    for (u32 i = 0; i < count; ++i)
    {
        game_entity::entity_id eid{ static_cast<id::id_type>(i) };
        if (!game_entity::is_alive(eid)) continue;
        component_mask mask{ game_entity::get_component_mask(eid) };
        if (mask & bit_mask(component_bit::Material))
        {
            if (match == index)
                return static_cast<u32>(eid);
            ++match;
        }
    }
    return 0;
}

// ============================================================================
// Batch Update
// ============================================================================

EDITOR_INTERFACE void ApplyMaterialParamBatch(const material_param_update_c* updates, u32 count)
{
    if (!updates || count == 0) return;
    for (u32 i = 0; i < count; ++i)
    {
        const material_param_update_c& u = updates[i];
        material::component mc = component_from_entity(u.entity_id);
        if (!mc.is_valid()) continue;
        switch (u.param_id)
        {
        case 0: // technique
            mc.set_technique(static_cast<graphics::ShaderTechnique>(
                std::min(static_cast<u32>(u.value), static_cast<u32>(graphics::ShaderTechnique::Count) - 1)));
            break;
        case 1: // roughness
            mc.set_roughness(u.value);
            break;
        case 2: // metallic
            mc.set_metallic(u.value);
            break;
        case 3: // alpha_cutoff
            mc.set_alpha_cutoff(u.value);
            break;
        case 4: // base_color[0]
        {
            f32 c[4]; mc.base_color(c); c[0] = u.value; mc.set_base_color(c);
            break;
        }
        case 5: // base_color[1]
        {
            f32 c[4]; mc.base_color(c); c[1] = u.value; mc.set_base_color(c);
            break;
        }
        case 6: // base_color[2]
        {
            f32 c[4]; mc.base_color(c); c[2] = u.value; mc.set_base_color(c);
            break;
        }
        case 7: // base_color[3]
        {
            f32 c[4]; mc.base_color(c); c[3] = u.value; mc.set_base_color(c);
            break;
        }
        default:
            break;
        }
    }
}
