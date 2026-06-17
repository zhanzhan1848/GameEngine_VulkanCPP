// CameraAPI.cpp - ECS Camera component C ABI for UI layer.
//
// Phase 5: Exposes Phase 3 ECS Camera component CRUD + scalar setters/getters.
//
// IMPORTANT LIMITATION: ECS Camera component does not yet feed the renderer.
// ForwardSceneRenderer's view/projection currently come from the legacy
// graphics::create_camera path (see ResourceAPI::CreateCamera + EngineAPI
// frame_info.camera_id). To make ECS Camera drive rendering, we need a
// CameraSyncSystem that mirrors LightSyncSystem:
//   - reads ECS Camera params + Transform
//   - computes view & projection matrices
//   - calls RenderView::SetViewMatrix/SetProjectionMatrix
//
// Until CameraSyncSystem lands, Editor code using this API can populate camera
// data but it won't affect what's drawn. Use ResourceAPI::CreateCamera for
// active camera control in the meantime.
//
// Matrix getters (view/projection) are NOT exposed here because the underlying
// component::view() returns identity — would mislead callers.

#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "Components/Camera.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "EngineAPI/Camera.h"
#include "Graphics/RHI/Core/RHIMath.h"

using namespace primal;
using primal::component::Camera;

namespace {

game_entity::entity entity_from_id(id::id_type id)
{
    return game_entity::entity{ game_entity::entity_id{id} };
}

} // anonymous namespace

// --- Lifecycle ---

EDITOR_INTERFACE u32 AddEntityCamera(id::id_type entity_id,
                                     u32 projection_type,
                                     f32 field_of_view,
                                     f32 aspect_ratio,
                                     f32 near_z,
                                     f32 far_z,
                                     const f32* up_vector)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || e.Has<Camera>()) return 0;

    camera::init_info info{};
    if (projection_type <= static_cast<u32>(graphics::camera::orthographic)) {
        info.type = static_cast<graphics::camera::type>(projection_type);
    }
    info.field_of_view = field_of_view;
    info.aspect_ratio  = aspect_ratio;
    info.near_z        = near_z;
    info.far_z         = far_z;
    if (up_vector) {
        info.up = math::v3{up_vector[0], up_vector[1], up_vector[2]};
    }

    e.Add<Camera>(info);
    return e.Has<Camera>() ? 1u : 0u;
}

EDITOR_INTERFACE void RemoveEntityCamera(id::id_type entity_id)
{
    if (!id::is_valid(entity_id)) return;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return;
    e.Remove<Camera>();
}

EDITOR_INTERFACE u32 HasEntityCamera(id::id_type entity_id)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid()) return 0;
    return e.Has<Camera>() ? 1u : 0u;
}

// --- Getters ---

EDITOR_INTERFACE u32 GetEntityCameraProjectionType(id::id_type entity_id, u32* out)
{
    if (!id::is_valid(entity_id) || !out) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    *out = static_cast<u32>(e.Get<Camera>().projection_type());
    return 1;
}

EDITOR_INTERFACE u32 GetEntityCameraFov(id::id_type entity_id, f32* out)
{
    if (!id::is_valid(entity_id) || !out) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    *out = e.Get<Camera>().field_of_view();
    return 1;
}

EDITOR_INTERFACE u32 GetEntityCameraAspectRatio(id::id_type entity_id, f32* out)
{
    if (!id::is_valid(entity_id) || !out) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    *out = e.Get<Camera>().aspect_ratio();
    return 1;
}

EDITOR_INTERFACE u32 GetEntityCameraRange(id::id_type entity_id, f32* out_near, f32* out_far)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    auto cc = e.Get<Camera>();
    if (out_near) *out_near = cc.near_z();
    if (out_far)  *out_far  = cc.far_z();
    return 1;
}

EDITOR_INTERFACE u32 GetEntityCameraUp(id::id_type entity_id, f32* out_up)
{
    if (!id::is_valid(entity_id) || !out_up) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    math::v3 up = e.Get<Camera>().up_vector();
    out_up[0] = up.x; out_up[1] = up.y; out_up[2] = up.z;
    return 1;
}

// --- Setters ---

EDITOR_INTERFACE u32 SetEntityCameraProjectionType(id::id_type entity_id, u32 projection_type)
{
    if (!id::is_valid(entity_id)) return 0;
    // component::set_projection_type 不存在，需要先 remove + add 重置。
    // 当前 Camera::component 只暴露 set_field_of_view/set_aspect_ratio/
    // set_view_width/set_view_height/set_range/set_up_vector。
    // projection_type 不可热切换 —— 通过返回 0 表示未支持。
    (void)projection_type;
    return 0;
}

EDITOR_INTERFACE u32 SetEntityCameraFov(id::id_type entity_id, f32 fov)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    e.Get<Camera>().set_field_of_view(fov);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityCameraAspectRatio(id::id_type entity_id, f32 aspect)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    e.Get<Camera>().set_aspect_ratio(aspect);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityCameraRange(id::id_type entity_id, f32 near_z, f32 far_z)
{
    if (!id::is_valid(entity_id)) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    e.Get<Camera>().set_range(near_z, far_z);
    return 1;
}

EDITOR_INTERFACE u32 SetEntityCameraUp(id::id_type entity_id, const f32* up)
{
    if (!id::is_valid(entity_id) || !up) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;
    e.Get<Camera>().set_up_vector(math::v3{up[0], up[1], up[2]});
    return 1;
}

// --- Matrix readback (computed on-demand from Transform + Camera params) ---
//
// Mirrors CameraSyncSystem's construction: view = Inverse(world), projection =
// MatrixPerspective or createOrthographicLH depending on projection_type.
// Lets UI query any entity's camera matrices (not just the active main camera),
// useful for editor viewports, thumbnail rendering, frustum culling UI, etc.

EDITOR_INTERFACE u32 GetEntityCameraViewMatrix(id::id_type entity_id, f32* out_16)
{
    if (!id::is_valid(entity_id) || !out_16) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;

    math::m4x4 world, inv_unused;
    transform::get_transform_matrices(game_entity::entity_id{entity_id}, world, inv_unused);
    const math::m4x4 view_matrix = graphics::rhi::math::Inverse(world);

    std::memcpy(out_16, &view_matrix, sizeof(math::m4x4));
    return 1u;
}

EDITOR_INTERFACE u32 GetEntityCameraProjectionMatrix(id::id_type entity_id, f32* out_16)
{
    if (!id::is_valid(entity_id) || !out_16) return 0;
    game_entity::entity e = entity_from_id(entity_id);
    if (!e.is_valid() || !e.Has<Camera>()) return 0;

    auto cam = e.Get<Camera>();
    math::m4x4 proj_matrix;
    if (cam.projection_type() == graphics::camera::perspective) {
        proj_matrix = graphics::rhi::math::MatrixPerspective(cam.field_of_view(),
                                                             cam.aspect_ratio(),
                                                             cam.near_z(),
                                                             cam.far_z());
    } else {
        proj_matrix = math::createOrthographicLH(cam.view_width(),
                                                 cam.view_height(),
                                                 cam.near_z(),
                                                 cam.far_z());
    }

    std::memcpy(out_16, &proj_matrix, sizeof(math::m4x4));
    return 1u;
}
