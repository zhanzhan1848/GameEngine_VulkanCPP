#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/SceneDataAdapter.h"
#include "Graphics/Passes/ParticlePass.h"
#include "Utilities/Math.h"

namespace primal::graphics {

class ForwardSceneRenderer {
public:
    ForwardSceneRenderer() = default;
    ~ForwardSceneRenderer();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();

    // Scene loading — uses SceneDataAdapter internally
    bool LoadScene(const std::string& model_path);
    void UnloadScene();

    // Async texture loading (called each frame until all textures loaded)
    void UpdateAsyncTextures(u32 frame_index);

    // IBL environment setup
    void SetIBLMaps(rhi::ResourceHandle irradiance, rhi::ResourceHandle prefiltered,
                    rhi::ResourceHandle brdf_lut);

    // Main render entry point
    void Render(rhi::RHICommandBuffer* cmd,
                const math::m4x4& view_matrix,
                const math::m4x4& proj_matrix,
                const math::v3& camera_position,
                rhi::ResourceHandle backbuffer,
                u32 frame_index);

    void SetLightDirection(math::v3 dir);
    void SetLightColor(math::v4 color);
    ParticlePass* GetParticlePass();

private:
    // Initialization sub-methods
    void CreateShaders();
    void CreatePipelines();
    void CreateDescriptorLayouts();
    void CreatePersistentResources();
    void CreateSamplers();

    // Per-mesh material descriptor sets
    void CreateMaterialDescriptorSets();

    // Shadow VP computation
    math::m4x4 ComputeCascadeVP(math::v3 light_dir, math::v3 camera_pos,
                                float ortho_extent, u32 shadow_map_size, u32 cascade,
                                float near_plane = 0.1f, float far_plane = 1000.0f);

    // Render sub-passes
    void RenderShadowPass(rhi::RHICommandBuffer* cmd, u32 cascade, u32 frame_index);
    void RenderGBufferPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderSkyboxPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderLightingPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderParticlePass(rhi::RHICommandBuffer* cmd, u32 frame_index,
                            const math::m4x4& view_matrix, const math::m4x4& proj_matrix);
    void RenderBlit(rhi::RHICommandBuffer* cmd, rhi::ResourceHandle backbuffer, u32 frame_index);

    // Shader loading helper
    rhi::ShaderHandle LoadShader(const char* filename, const char* entry_point, rhi::ShaderStage stage);

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    bool initialized_ = false;

    // Scene data (loaded via SceneDataAdapter)
    SceneDataAdapter scene_adapter_;
    utl::vector<SceneDataMeshInfo> mesh_infos_;
    utl::vector<rhi::ResourceHandle> material_textures_; // albedo, normal, ORM per mesh
    bool scene_loaded_ = false;

    // IBL
    rhi::ResourceHandle irradiance_map_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle prefiltered_map_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle brdf_lut_{rhi::handles::INVALID_RESOURCE};
    bool ibl_ready_ = false;

    // Shaders
    rhi::ShaderHandle gbuffer_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle gbuffer_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle shadow_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle lighting_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle lighting_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle skybox_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle skybox_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_ps_{rhi::handles::INVALID_SHADER};

    // Pipelines
    rhi::PipelineHandle gbuffer_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle shadow_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle lighting_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle skybox_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle blit_pipeline_{rhi::handles::INVALID_PIPELINE};

    // Pipeline layouts
    rhi::PipelineLayoutHandle gbuffer_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle shadow_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle lighting_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle skybox_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle blit_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle global_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle material_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle lighting_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle skybox_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle blit_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle global_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle lighting_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle skybox_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle blit_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    // Per-mesh material descriptor sets
    utl::vector<rhi::DescriptorSetHandle> material_ds_;

    // Persistent GPU resources
    rhi::ResourceHandle shadow_map_[2]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_albedo_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                            rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_normal_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                            rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_orm_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                         rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_velocity_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                              rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_depth_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                           rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle lighting_output_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                             rhi::handles::INVALID_RESOURCE};

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle view_cb_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                     rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle scene_cb_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                      rhi::handles::INVALID_RESOURCE};

    // Samplers
    rhi::SamplerHandle default_sampler_{rhi::handles::INVALID_SAMPLER};
    rhi::SamplerHandle brdf_sampler_{rhi::handles::INVALID_SAMPLER};

    // Particle pass (owned)
    ParticlePass particle_pass_;

    // Lighting state
    math::v3 light_dir_{-0.9f, 1.5f, -0.8f};
    math::v4 light_color_{20.0f, 20.0f, 20.0f, 1.0f};

    // Shadow VP caching
    math::m4x4 cached_shadow_vp_[2]{};

    // Fallback textures
    rhi::ResourceHandle white_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle flat_normal_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle black_cube_texture_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
