#pragma once

#include "CommonHeaders.h"

namespace primal::graphics {
    class RenderScene;

    namespace scene_sync {

        // === Phase 4.5: ECS Light → RenderScene 同步 ===
        // 每帧调用一次（在 ForwardSceneRenderer 读 GetLights() 之前）。
        // 内部：
        //   1) scene.ClearLights() — 清掉上一帧的 RenderLight 快照
        //   2) 遍历所有 alive entity，挑出带 Light 组件 + is_enabled 的
        //   3) 从 Light 组件读 color/intensity/range/type，从 Transform 读 position
        //   4) 构造 RenderLight，调 scene.AddLight(...)
        //
        // Camera 组件同步未实现 —— 现有 graphics::create_camera 路径
        // （Track A CreateCamera / EngineAPI frame_info.camera_id）仍可工作。
        // 后续会补 CameraSyncSystem。
        //
        // 注意：本函数**不是线程安全**的 —— 必须在渲染线程的同一帧内调用。
        // RenderScene::AddLight 内部加锁，但 ECS 状态读取期间不应有并发 entity 创建/删除。
        void SyncLightsFromECS(RenderScene& scene);

    } // namespace scene_sync
} // namespace primal::graphics
