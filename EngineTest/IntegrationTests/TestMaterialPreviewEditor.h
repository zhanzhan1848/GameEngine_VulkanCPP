#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"

// MaterialPreview C API (defined in EngineDLL/MaterialPreviewAPI.cpp).
// Forward-declared here so this test does not need to ship a shared header.
extern "C" {
    u32 CreateMaterialPreview(u64 device_handle, u32 width, u32 height);
    void DestroyMaterialPreview(u32 preview_id);
    void DestroyAllMaterialPreviews();
    u32  SetMaterialPreviewGraph(u32 preview_id, const char* graph_json);
    void SetMaterialPreviewModel(u32 preview_id, u32 model_type);
    u32  SetMaterialPreviewMesh(u32 preview_id, u64 geometry_content_id);
    void ClearMaterialPreviewMesh(u32 preview_id);
    void SetMaterialPreviewMode2D(u32 preview_id, u32 enable);
    u32  IsMaterialPreviewMode2D(u32 preview_id);
    void ResizeMaterialPreview(u32 preview_id, u32 width, u32 height);
    u64  RenderMaterialPreview(u32 preview_id, u64 cmd_handle, u32 frame_index, f32 dt);
    void BlitMaterialPreviewToBackbuffer(u32 preview_id, u64 cmd_handle,
                                          u64 dst_texture_handle,
                                          u32 dst_width, u32 dst_height);
    u64  GetMaterialPreviewTexture(u32 preview_id);
    u32  GetMaterialPreviewWidth(u32 preview_id);
    u32  GetMaterialPreviewHeight(u32 preview_id);

    // Camera
    void SetMaterialPreviewCameraPosition(u32 preview_id, const f32* xyz);
    void GetMaterialPreviewCameraPosition(u32 preview_id, f32* out_xyz);
    void SetMaterialPreviewCameraTarget(u32 preview_id, const f32* xyz);
    void SetMaterialPreviewCameraFov(u32 preview_id, f32 radians);
    void SetMaterialPreviewRotationSpeed(u32 preview_id, f32 rad_per_sec);
    // Light
    void SetMaterialPreviewLightDirection(u32 preview_id, const f32* normalized_dir);
    void SetMaterialPreviewLightColor(u32 preview_id, const f32* rgb);
    void SetMaterialPreviewLightIntensity(u32 preview_id, f32 intensity);
    // Background
    void SetMaterialPreviewBackgroundMode(u32 preview_id, u32 mode);
    void SetMaterialPreviewBackgroundColors(u32 preview_id, const f32* top_rgb, const f32* bot_rgb);
}

class MaterialPreviewEditorTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;

    u32  preview_id_{0};
    u64  frame_count_{0};
    f32  total_time_{0.0f};

    // Orbit camera state (spherical coords around target).
    f32  orbit_yaw_{0.0f};      // radians, around Y
    f32  orbit_pitch_{0.0f};    // radians, around X
    f32  orbit_radius_{4.5f};   // distance from target
    f32  cam_fov_{0.7853981633974483f}; // PI/4

    // Light preset cycling.
    u32  light_preset_{0};

    // Edge detection for digit/letter keys.
    bool prev_key_down_[256]{};

    bool JustPressed(u32 code_index);
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
