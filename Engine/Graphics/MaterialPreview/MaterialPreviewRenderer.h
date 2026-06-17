#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics {

class RenderMesh;
namespace material_graph { class MaterialGraph; }

enum class PreviewModelType : u8 {
    Sphere = 0,
    Box,
    Cylinder,
    Cone,
    Torus,
    Capsule,
    Teapot,
    Plane,
    Count
};

enum class PreviewBackgroundMode : u8 {
    Gradient = 0,
    SolidColor = 1
};

class MaterialPreviewRenderer {
public:
    MaterialPreviewRenderer() = default;
    ~MaterialPreviewRenderer() { Shutdown(); }

    bool Initialize(rhi::RHIDeviceBase* device, u32 width = 512, u32 height = 512);
    void Shutdown();

    bool SetMaterialGraph(const material_graph::MaterialGraph& graph);
    void SetPreviewModel(PreviewModelType model);

    // 2D mode: orthographic projection, no rotation, renders a full-viewport XY quad.
    // Independent of PreviewModelType (toggle affects projection + rotation only).
    void SetMode2D(bool enable) { mode_2d_ = enable; }
    bool IsMode2D() const { return mode_2d_; }

    // External mesh override: takes precedence over PreviewModelType selection.
    // Loads a geometry asset by id; Render() will use it until cleared.
    bool SetPreviewMeshFromAsset(id::id_type geometry_content_id);
    void ClearPreviewMeshOverride();

    rhi::ResourceHandle Render(rhi::RHICommandBuffer* cmd, u32 frame_index, f32 dt);

    // Copy color_target_ into dst_texture via a fragment shader. Required because swap chain
    // backbuffers are framebufferOnly and reject MTLBlitCommandEncoder writes — a fullscreen
    // triangle render pass is the canonical Metal path for this.
    void BlitToTexture(rhi::RHICommandBuffer* cmd,
                       rhi::ResourceHandle dst_texture,
                       u32 dst_width, u32 dst_height);

    rhi::ResourceHandle GetOutputTexture() const { return color_target_; }
    void Resize(u32 w, u32 h);

    u32 GetWidth() const { return width_; }
    u32 GetHeight() const { return height_; }

    // --- Camera configuration ---
    void SetCameraPosition(f32 x, f32 y, f32 z);
    void SetCameraTarget(f32 x, f32 y, f32 z);
    void SetCameraFov(f32 radians);
    void SetRotationSpeed(f32 rad_per_sec);
    rhi::math::v3 GetCameraPosition() const { return camera_position_; }
    rhi::math::v3 GetCameraTarget() const { return camera_target_; }
    f32 GetCameraFov() const { return camera_fov_radians_; }
    f32 GetRotationSpeed() const { return rotation_speed_; }

    // --- Light configuration ---
    void SetLightDirection(f32 x, f32 y, f32 z);
    void SetLightColor(f32 r, f32 g, f32 b);
    void SetLightIntensity(f32 intensity);
    rhi::math::v3 GetLightDirection() const { return light_direction_; }
    rhi::math::v3 GetLightColor() const { return light_color_; }
    f32 GetLightIntensity() const { return light_intensity_; }

    // --- Background configuration ---
    void SetBackgroundMode(PreviewBackgroundMode mode);
    void SetBackgroundColors(f32 top_r, f32 top_g, f32 top_b,
                             f32 bot_r, f32 bot_g, f32 bot_b);
    PreviewBackgroundMode GetBackgroundMode() const { return bg_mode_; }
    rhi::math::v3 GetBackgroundTopColor() const { return bg_top_color_; }
    rhi::math::v3 GetBackgroundBotColor() const { return bg_bot_color_; }

private:
    struct ViewData {
        rhi::math::m4x4 viewProj;
        rhi::math::m4x4 invViewProj;
        rhi::math::m4x4 prevViewProj;
    };

    struct SceneData {
        rhi::math::m4x4 model;
        rhi::math::v4   lightDir;
        rhi::math::v4   lightColor;
        f32        lightIntensity;
        f32        time;
        f32        _pad[2];
        rhi::math::v3   cameraPos;
        f32        _pad2;
        // Background colors for gradient mode (read by PreviewVertex.metal gradientFragment)
        rhi::math::v4   bgTopColor;   // rgb + unused
        rhi::math::v4   bgBotColor;   // rgb + unused
        f32        bgMode;       // 0 = gradient, 1 = solid color
        f32        _pad3[3];
    };

    bool CreateSamplers();
    bool CreateDescriptorLayouts();
    bool CreateShaders();
    bool CreateBackgroundPipeline();
    bool CreateRenderTargets();
    bool CreateConstantBuffers();
    bool CreateMeshes();
    bool CreateMaterialPipeline(const std::string& metal_source);
    bool CreateBlitPipeline();
    void UpdateGlobalDescriptorSets();
    void UpdateBlitDescriptorSet();

    rhi::RHIDeviceBase* device_{nullptr};

    u32 width_{512};
    u32 height_{512};

    // Samplers
    rhi::SamplerHandle linear_sampler_{rhi::handles::INVALID_RESOURCE};

    // Descriptor layouts
    rhi::DescriptorSetLayoutHandle global_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineLayoutHandle global_pipeline_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle material_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineLayoutHandle material_pipeline_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle background_pipeline_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle blit_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineLayoutHandle blit_pipeline_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    // Shaders
    rhi::ShaderHandle preview_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle background_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle background_fs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle material_fs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_fs_{rhi::handles::INVALID_SHADER};

    // Pipelines
    rhi::PipelineHandle background_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle material_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle blit_pipeline_{rhi::handles::INVALID_PIPELINE};

    // Render targets
    rhi::ResourceHandle color_target_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle depth_target_{rhi::handles::INVALID_RESOURCE};

    // Constant buffers (triple-buffered)
    static constexpr u32 FRAME_COUNT = rhi::MAX_FRAMES_IN_FLIGHT;
    rhi::ResourceHandle view_cb_[FRAME_COUNT]{};
    void* view_cb_mapped_[FRAME_COUNT]{};
    rhi::ResourceHandle scene_cb_[FRAME_COUNT]{};
    void* scene_cb_mapped_[FRAME_COUNT]{};

    // Descriptor sets
    rhi::DescriptorSetHandle global_set_[FRAME_COUNT]{};
    rhi::DescriptorSetHandle material_set_{rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle blit_set_{rhi::handles::INVALID_DESCRIPTOR_SET};

    // Meshes
    RenderMesh* meshes_[static_cast<u8>(PreviewModelType::Count)]{};
    RenderMesh* override_mesh_{nullptr};  // external mesh; Render() prefers this when set
    RenderMesh* quad_xy_mesh_{nullptr};   // for SetMode2D(true)
    PreviewModelType current_model_{PreviewModelType::Sphere};

    // Animation
    f32 rotation_angle_{0.0f};

    bool material_graph_set_{false};
    bool mode_2d_{false};

    // --- Configurable camera/light/background state ---
    // Defaults match the original hardcoded values for zero behavioral change.
    rhi::math::v3 camera_position_{0.0f, 0.0f, 4.5f};
    rhi::math::v3 camera_target_{0.0f, 0.0f, 0.0f};
    f32 camera_fov_radians_{3.14159265358979323846f / 4.0f};  // PI/4 = 45 degrees
    f32 rotation_speed_{0.5f};  // matches original hardcoded value
    rhi::math::v3 light_direction_{0.5f, 0.8f, 0.3f};   // matches original lightDir.xyz
    rhi::math::v3 light_color_{1.0f, 1.0f, 0.95f};       // matches original lightColor.rgb
    f32 light_intensity_{1.0f};
    PreviewBackgroundMode bg_mode_{PreviewBackgroundMode::Gradient};
    // Gradient colors match PreviewVertex.metal gradientFragment defaults.
    rhi::math::v3 bg_top_color_{0.45f, 0.65f, 0.92f};
    rhi::math::v3 bg_bot_color_{0.12f, 0.12f, 0.18f};
};

} // namespace primal::graphics
