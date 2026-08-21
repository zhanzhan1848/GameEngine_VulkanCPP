// === Phase 4.5: ECS Light → RenderScene 同步层 ===
// 把 entity 上挂的 Light 组件每帧同步到 RenderScene::lights_，
// 让 ForwardSceneRenderer 的 GetLights() 能拿到真实数据。
//
// 设计选择：
//   - 用路径 B（独立同步系统）而非路径 A（在 component::create 里调 graphics::create_light）。
//     原因：ForwardSceneRenderer 走的是 RenderScene::GetLights() 路径，
//     而不是旧的 light_set_key + MetalLight 后端路径。
//     把 ECS Light 直接 push 到 RenderScene 才能让 Phase 4 真正闭环。
//   - Camera 同步暂未实现 —— 现有 graphics::create_camera 路径（Track A CreateCamera
//     或 frame_info.camera）仍然工作。CameraSyncSystem 留作后续 sub-step。

#include "LightSyncSystem.h"
#include "../RenderScene.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Light.h"
#include "Components/ComponentTraits.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/Light.h"
#include "EngineAPI/LightComponent.h"

#include <cmath>

namespace primal::graphics::scene_sync {

namespace {

// graphics::light::type (EngineAPI/Light.h) → RenderScene::LightType
LightType MapLightType(graphics::light::type t) {
    switch (t) {
        case graphics::light::directional: return LightType::Directional;
        case graphics::light::point:       return LightType::Point;
        case graphics::light::spot:        return LightType::Spot;
        default:                           return LightType::Directional;
    }
}

} // anonymous namespace

void SyncLightsFromECS(RenderScene& scene) {
    // Clear last frame's snapshot before re-pushing current ECS state.
    // RenderScene::AddLight locks per-call, but ClearLights is one lock.
    scene.ClearLights();

    const u64 count = game_entity::entity_count();
    const component_mask light_mask = bit_mask(component_bit::Light);

    for (u64 i = 0; i < count; ++i) {
        const game_entity::entity_id eid = game_entity::entity_id_from_index(static_cast<u32>(i));
        if (!game_entity::is_alive(eid)) continue;

        const component_mask mask = game_entity::get_component_mask(eid);
        if ((mask & light_mask) == 0) continue;

        // Build Light component handle from entity_id.
        // light_component_id wraps entity.get_id() (see Light.cpp create()).
        primal::light::component lc{ primal::light::light_component_id{ static_cast<id::id_type>(eid) } };
        if (!lc.is_valid()) continue;
        if (!lc.is_enabled()) continue;

        // Pull world matrix from Transform for position + forward direction.
        math::m4x4 world, inv_world;
        transform::get_transform_matrices(eid, world, inv_world);

        // Position = translation column of world matrix.
        const math::v3 position{
            world.columns[3][0],
            world.columns[3][1],
            world.columns[3][2]
        };

        // Forward = Z axis of world matrix (column 2).
        // Includes scale; users should keep Light entity scale=(1,1,1).
        // Normalize defensively.
        math::v3 direction{
            world.columns[2][0],
            world.columns[2][1],
            world.columns[2][2]
        };
        const f32 dlen = std::sqrt(direction.x * direction.x +
                                   direction.y * direction.y +
                                   direction.z * direction.z);
        if (dlen > 1e-5f) {
            direction.x /= dlen;
            direction.y /= dlen;
            direction.z /= dlen;
        } else {
            // Degenerate — fall back to "straight down" so the renderer doesn't NaN.
            direction = math::v3{ 0.f, -1.f, 0.f };
        }

        RenderLight rl{};
        rl.entityId    = static_cast<id::id_type>(eid);
        rl.type        = MapLightType(lc.light_type());
        rl.color       = lc.color();
        rl.intensity   = lc.intensity();
        rl.position    = position;
        rl.range       = lc.range();
        rl.direction   = direction;
        rl.innerCone   = std::cos(lc.umbra());
        rl.outerCone   = std::cos(lc.penumbra());
        rl.castShadow  = false;  // Light component doesn't expose castShadow yet.

        scene.AddLight(rl);
    }
}

} // namespace primal::graphics::scene_sync
