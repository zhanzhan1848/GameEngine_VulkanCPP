// LightAPI.cpp - ECS Light component C ABI for UI layer.
//
// Phase 5: Exposes Phase 3 (ECS Light component) + Phase 4.5 (LightSyncSystem)
// capabilities to the Editor. Each function takes an entity_id (from
// EntityAPI::CreateEntity) and forwards to entity.Add<Light> / Get<Light> /
// Remove<Light>.
//
// Data flow:
//   Editor → AddEntityLight → entity.Add<Light>(info)
//   → next frame LightSyncSystem::SyncLightsFromECS copies to RenderScene::lights_
//   → ForwardSceneRenderer reads GetLights() and uploads to GPU
//
// Note on light_set_key: legacy field from the old graphics::create_light path.
// ECS sync ignores it (LightSyncSystem doesn't read light_set_key). Kept here
// for ABI source-compat with EngineAPI/Light.h; can be set to 0.

#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "Components/Light.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "EngineAPI/Light.h"

using namespace primal;
using primal::component::Light;

namespace {

game_entity::entity entity_from_id(id::id_type id)
{
    return game_entity::entity{ game_entity::entity_id{id} };
}

} // anonymous namespace

// --- Lifecycle ---

EDITOR_INTERFACE u32 AddEntityLight(id::id_type entity_id,
                                    u32 light_type,
                                    f32 intensity,
                                    const f32* color_rgb,
                                    f32 range,
                                    f32 umbra_rad,
                                    f32 penumbra_rad,
                                    u32 is_enabled)
{
    if (!id::is_valid(entity_id)) return 0;
    if (light_type > static_cast<u32>(graphics::light::spot)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || e.Has<Light>()) return 0;

    light::init_info info{};
    info.type = static_cast<graphics::light::type>(light_type);
    info.intensity = intensity;
    if (color_rgb) {
        info.color = math::v3{color_rgb[0], color_rgb[1], color_rgb[2]};
    }
    info.range     = range;
    info.umbra     = umbra_rad;
    info.penumbra  = penumbra_rad;
    info.is_enabled = is_enabled != 0;

    e.Add<Light>(info);
    return e.Has<Light>() ? 1u : 0u;
}

EDITOR_INTERFACE void RemoveEntityLight(id::id_type entity_id)
{
    if (!id::is_valid(entity_id)) return;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return;
    e.Remove<Light>();
}

EDITOR_INTERFACE u32 HasEntityLight(id::id_type entity_id)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid()) return 0;
    return e.Has<Light>() ? 1u : 0u;
}

// --- Getters (return 1 on success, 0 on failure) ---

EDITOR_INTERFACE u32 GetEntityLightType(id::id_type entity_id, u32* out_type)
{
    if (!id::is_valid(entity_id) || !out_type) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    auto lc = e.Get<Light>();
    *out_type = static_cast<u32>(lc.light_type());
    return 1;
}

EDITOR_INTERFACE u32 GetEntityLightIntensity(id::id_type entity_id, f32* out)
{
    if (!id::is_valid(entity_id) || !out) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    *out = e.Get<Light>().intensity();
    return 1;
}

EDITOR_INTERFACE u32 GetEntityLightColor(id::id_type entity_id, f32* out_rgb)
{
    if (!id::is_valid(entity_id) || !out_rgb) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    math::v3 c = e.Get<Light>().color();
    out_rgb[0] = c.x; out_rgb[1] = c.y; out_rgb[2] = c.z;
    return 1;
}

EDITOR_INTERFACE u32 GetEntityLightRange(id::id_type entity_id, f32* out)
{
    if (!id::is_valid(entity_id) || !out) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    *out = e.Get<Light>().range();
    return 1;
}

EDITOR_INTERFACE u32 GetEntityLightConeAngles(id::id_type entity_id, f32* out_umbra, f32* out_penumbra)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    auto lc = e.Get<Light>();
    if (out_umbra)    *out_umbra    = lc.umbra();
    if (out_penumbra) *out_penumbra = lc.penumbra();
    return 1;
}

EDITOR_INTERFACE u32 IsEntityLightEnabled(id::id_type entity_id, u32* out_enabled)
{
    if (!id::is_valid(entity_id) || !out_enabled) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    *out_enabled = e.Get<Light>().is_enabled() ? 1u : 0u;
    return 1;
}

// --- Setters (return 1 on success, 0 on failure) ---

EDITOR_INTERFACE u32 SetEntityLightType(id::id_type entity_id, u32 light_type)
{
    if (!id::is_valid(entity_id)) return 0;
    if (light_type > static_cast<u32>(graphics::light::spot)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_light_type(static_cast<graphics::light::type>(light_type));
    return 1;
}

EDITOR_INTERFACE u32 SetEntityLightIntensity(id::id_type entity_id, f32 value)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_intensity(value);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityLightColor(id::id_type entity_id, const f32* rgb)
{
    if (!id::is_valid(entity_id) || !rgb) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_color(math::v3{rgb[0], rgb[1], rgb[2]});
    return 1;
}

EDITOR_INTERFACE u32 SetEntityLightRange(id::id_type entity_id, f32 value)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_range(value);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityLightConeAngles(id::id_type entity_id, f32 umbra_rad, f32 penumbra_rad)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_cone_angles(umbra_rad, penumbra_rad);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityLightEnabled(id::id_type entity_id, u32 enabled)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Light>()) return 0;
    e.Get<Light>().set_enabled(enabled != 0);
    return 1;
}
