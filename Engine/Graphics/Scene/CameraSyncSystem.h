#pragma once

#include "CommonHeaders.h"

namespace primal::graphics {
    class RenderView;

    namespace scene_sync {

        // === Phase 3: ECS Camera → RenderView 同步 ===
        // 每帧调用一次（在 StandardRenderPipeline::UpdatePerFrame 读 view.GetViewMatrix() 之前）。
        // 内部：
        //   1) 遍历所有 alive entity，挑出第一个带 Camera 组件的（"main camera" 语义）
        //   2) 从 Camera 组件读 fov/aspect/near/far/projection_type，从 Transform 读 world matrix
        //   3) view = Inverse(world), projection = MatrixPerspective 或 MatrixOrthographic
        //   4) 调 view.SetViewMatrix / view.SetProjectionMatrix 覆盖上游传入的矩阵
        //
        // "Main camera" 选取规则：第一个 alive 的、带 component_bit::Camera 的 entity（按 entity_index 升序）。
        // 后续如需显式 main camera 标记，可扩展为 SetMainCamera(entity_id) 注册表 + CameraSyncSystem 优先级查询。
        //
        // Fallback 行为：如果场景里没有 Camera entity，RenderView 保持上游 caller 设置的矩阵不变。
        // 这样既不破坏旧的 graphics::create_camera 路径，也允许 Editor 用 ECS Camera 组件驱动渲染。
        //
        // 注意：本函数**不是线程安全**的 —— 必须在渲染线程的同一帧内调用，与 SyncLightsFromECS 同一时机。
        void SyncCamerasFromECS(RenderView& view);

    } // namespace scene_sync
} // namespace primal::graphics
