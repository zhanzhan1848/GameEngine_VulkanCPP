// === Phase 3: ECS Camera → RenderView 同步层 ===
// 把 entity 上挂的 Camera 组件每帧同步到 RenderView 的 view/projection 矩阵，
// 让 StandardRenderPipeline 能拿到 ECS 驱动的相机数据。
//
// 设计选择（与 LightSyncSystem 一致）：
//   - 独立同步系统，不在 component::create 里调 graphics::create_camera。
//   - Camera 组件的 view()/projection() 目前返回 identity（stub）；本系统从原始参数
//     (fov/aspect/near/far/projection_type) + Transform world matrix 现场构造矩阵。
//   - "Main camera" = 第一个 alive 的 Camera entity（按 entity_index 升序）。
//     如果场景里没有 Camera entity，本函数 no-op，RenderView 保留上游矩阵。

#include "CameraSyncSystem.h"
#include "../RenderView.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Camera.h"
#include "Components/ComponentTraits.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/CameraComponent.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::scene_sync {

void SyncCamerasFromECS(RenderView& view) {
    const u64 count = game_entity::entity_count();
    const component_mask camera_mask = bit_mask(component_bit::Camera);

    // 选取第一个 alive 的 Camera entity（按 entity_index 升序）作为 "main camera"。
    // 找不到就 no-op — RenderView 保留上游 caller 设置的矩阵。
    for (u64 i = 0; i < count; ++i) {
        const game_entity::entity_id eid = game_entity::entity_id_from_index(static_cast<u32>(i));
        if (!game_entity::is_alive(eid)) continue;

        const component_mask mask = game_entity::get_component_mask(eid);
        if ((mask & camera_mask) == 0) continue;

        // 找到了 — 构造 ECS Camera 组件 handle。
        // 注意：必须是 primal::camera::component（ECS 组件），不是
        // primal::graphics::camera（旧 platform_interface handle）。
        primal::camera::component cc{
            primal::camera::camera_component_id{ static_cast<id::id_type>(eid) }
        };
        if (!cc.is_valid()) return;  // 不太可能，但防御性返回避免后续崩溃

        // View matrix = inverse of camera entity's world matrix.
        // 注意：transform::get_transform_matrices 给出的 inv_world 在某些 entity 状态下
        // 会被填成 NaN（LightSyncSystem 只用 world，没用 inv_world，所以这个 bug 一直没暴露）。
        // 这里我们只取 world，自己算 inverse —— 对正常 Transform 来说 world 是可逆的。
        math::m4x4 world, inv_world_unused;
        transform::get_transform_matrices(eid, world, inv_world_unused);
        const math::m4x4 view_matrix = rhi::math::Inverse(world);

        // Projection matrix 从原始组件参数现场构造。
        math::m4x4 proj_matrix;
        const graphics::camera::type ptype = cc.projection_type();
        if (ptype == graphics::camera::perspective) {
            proj_matrix = rhi::math::MatrixPerspective(cc.field_of_view(),
                                                       cc.aspect_ratio(),
                                                       cc.near_z(),
                                                       cc.far_z());
        } else {
            // Orthographic：Camera.cpp 把 ortho 的 width 存在 fovs[]、height 存在 aspects[]，
            // 通过 view_width()/view_height() 暴露。
            proj_matrix = math::createOrthographicLH(cc.view_width(),
                                                      cc.view_height(),
                                                      cc.near_z(),
                                                      cc.far_z());
        }

        view.SetViewMatrix(view_matrix);
        view.SetProjectionMatrix(proj_matrix);
        return;  // 第一个 Camera 胜出，无需继续遍历
    }
    // 没有 Camera entity — no-op。
}

} // namespace primal::graphics::scene_sync
