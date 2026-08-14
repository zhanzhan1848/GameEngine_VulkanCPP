#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "../RHI/Core/RHIMeshAsset.h"
#include "GPUCullingPipeline.h"
#include <mutex>
#include <iostream>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics {
    class RenderScene;
}

namespace primal::graphics::nanite {

class HZBSystem;
class VisibilityBufferSystem;

struct BinningConfig {
    u32 bin_size{ 64 };
    u32 max_bins_per_frame{ 4096 };
    u32 max_clusters_per_bin{ 256 };
    bool enable_spatial_sorting{ true };
};

struct VisibilityBufferConfig {
    u32 width{ 1920 };
    u32 height{ 1080 };
    rhi::DataFormat format{ rhi::DataFormat::R32_UInt };
    bool enable_depth{ true };
    bool enable_conservative_rasterization{ false };
};

struct IndirectDrawCommand {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    u32 vertexOffset;
    u32 firstInstance;
};

struct GPUDrawResults {
    u32 total_draw_calls{ 0 };
    u32 total_primitives_rendered{ 0 };
    u32 total_clusters_rendered{ 0 };
    u32 bin_count{ 0 };

    rhi::ResourceHandle indirect_draw_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle visibility_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle bin_counter_buffer{ rhi::handles::INVALID_RESOURCE };
};

class GPUDrivenDrawPipeline {
public:
    static GPUDrivenDrawPipeline& Get();
    ~GPUDrivenDrawPipeline();

    GPUDrivenDrawPipeline(const GPUDrivenDrawPipeline&) = delete;
    GPUDrivenDrawPipeline& operator=(const GPUDrivenDrawPipeline&) = delete;

    bool Initialize(rhi::RHIDeviceBase* device,
                   const BinningConfig& binning_config = BinningConfig{},
                   const VisibilityBufferConfig& visibility_config = VisibilityBufferConfig{});
    void Shutdown();

    bool Execute(rhi::RHICommandBuffer* cmd_buffer,
                 const RenderSceneSnapshot& scene_snapshot,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix,
                 const CullingResults& culling_results,
                 u32 frame_index = 0,
                 u32 buffer_index = 0);

    // Phase 9.3b: Issue one DrawIndirect per visible, non-tombstoned streaming
    // mesh. Must be called inside a render pass with a graphics pipeline bound.
    // v1 relies on whatever material/pipeline is currently bound (Task 12 will
    // validate visually and wire a default material if needed).
    void DrawStreamingMeshes(rhi::RHICommandBuffer* cmd_buffer);

    const GPUDrawResults& GetResults() const { return results_; }
    const BinningConfig& GetBinningConfig() const { return binning_config_; }
    const VisibilityBufferConfig& GetVisibilityConfig() const { return visibility_config_; }
    bool IsInitialized() const { return initialized_; }

    void SetHZBSystem(HZBSystem* hzb_system) { hzb_system_ = hzb_system; }
    void SetVisibilityBufferSystem(VisibilityBufferSystem* vis_system) { visibility_buffer_system_ = vis_system; }
    void SetCullingPipeline(class GPUCullingPipeline* culling_pipeline) { culling_pipeline_ = culling_pipeline; }

    // Debug visualization mode for the meshlet GBuffer pass.
    // 0=off, 1=meshlet_id, 2=triangle_id, 3=mesh_id (instance).
    // Written into DrawConstants.debug_mode each frame.
    void SetDebugMode(u32 mode) { meshlet_debug_mode_ = mode; }

    // Phase 9.3b: Streaming terrain — RenderScene accessor for iterating
    // GetStreamingMeshes() and issuing one DrawIndirect per visible mesh.
    void SetRenderScene(RenderScene* scene) { render_scene_ = scene; }

    void UpdateGeometryData(const RenderSceneSnapshot& scene_snapshot);
    bool CreateGeometryBuffers(u32 vertex_count, u32 index_count);
    void UploadGeometryData(const RenderSceneSnapshot& scene_snapshot);

    void SetMaterialDataBuffer(rhi::ResourceHandle material_buffer) {
        global_material_data_buffer_ = material_buffer;
    }

    void SetTextureArrays(rhi::ResourceHandle albedo_array,
                          rhi::ResourceHandle normal_array,
                          rhi::ResourceHandle orm_array,
                          rhi::SamplerHandle sampler) {
        albedo_texture_array_ = albedo_array;
        normal_texture_array_ = normal_array;
        orm_texture_array_ = orm_array;
        texture_sampler_ = sampler;
    }

    // Get the final output texture that was rendered to
    rhi::ResourceHandle GetFinalOutputTexture() const { return final_color_texture_; }

    // Get the final depth texture for HZB generation
    rhi::ResourceHandle GetFinalDepthTexture() const { return final_depth_texture_; }

    // ---- Shadow Mapping ----

    struct alignas(16) DirectionalLightData {
        math::v4 direction;        // xyz = normalized light dir (toward light), w = 0
        math::v4 color;            // rgb = light intensity, a = unused
        math::v4 viewPos;          // xyz = camera position (cascade selection), w = unused
        math::m4x4 shadowMatrix0;  // Light VP matrix for cascade 0
        math::m4x4 shadowMatrix1;  // Light VP matrix for cascade 1
        math::v4 cascadeSplits;    // x = cascade 0 max distance, y = cascade 1 max distance
    };

    struct ShadowFrameResources {
        rhi::ResourceHandle shadow_depth_rt_0{ rhi::handles::INVALID_RESOURCE };  // D32_Float, 2048x2048
        rhi::ResourceHandle shadow_depth_rt_1{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle shadow_map_0{ rhi::handles::INVALID_RESOURCE };       // R32_Float sampleable
        rhi::ResourceHandle shadow_map_1{ rhi::handles::INVALID_RESOURCE };
        
        // Per-cascade resources to avoid CPU/GPU data races and descriptor set overwrite issues
        rhi::ResourceHandle visible_clusters_buffer[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle visible_counter_buffer[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };  // atomic counter per cascade
        rhi::ResourceHandle indirect_draw_buffer[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle light_frustum_cb[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle shadow_depth_cb[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };    // ShadowDepthUniforms for raster pass
        rhi::ResourceHandle blit_resolution_cb[2]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE }; // BlitResolution for depth blit
        
        rhi::DescriptorSetHandle shadow_cull_descriptor_set[2]{ rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET };
        rhi::DescriptorSetHandle shadow_depth_descriptor_set[2]{ rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET };  // 8 bindings for vertex pulling
        rhi::DescriptorSetHandle shadow_blit_descriptor_set[2]{ rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET };   // 3 bindings for D32→R32 blit
    };

    bool InitializeShadowResources(u32 num_instances, u32 max_clusters);
    void ShutdownShadowResources();

    bool ExecuteShadowCulling(rhi::RHICommandBuffer* cmd_buffer,
                              const RenderSceneSnapshot& scene_snapshot,
                              const DirectionalLightData& light_data,
                              u32 cascade_index,
                              u32 buffer_index);

    bool ExecuteShadowRaster(rhi::RHICommandBuffer* cmd_buffer,
                             const math::m4x4& light_view_projection,
                             u32 cascade_index,
                             u32 buffer_index);

    bool ExecuteShadowDepthBlit(rhi::RHICommandBuffer* cmd_buffer,
                                u32 cascade_index,
                                u32 buffer_index);

    // GBuffer depth blit: D32_Float -> R32_Float via compute shader (ShadowBlit.metal)
    // Metal TBDR cannot sample D32_Float; this uses the same compute pipeline as shadow blit.
    bool ExecuteGBufferDepthBlit(rhi::RHICommandBuffer* cmd_buffer);

    const ShadowFrameResources& GetShadowFrameResources(u32 buffer_index) const {
        return shadow_frames_[buffer_index % 3];
    }

    rhi::ResourceHandle GetShadowMap(u32 cascade_index, u32 buffer_index) const {
        return (cascade_index == 0)
            ? shadow_frames_[buffer_index % 3].shadow_map_0
            : shadow_frames_[buffer_index % 3].shadow_map_1;
    }

    // GBuffer texture accessors for downstream passes (SSGI, DDGI, etc.)
    rhi::ResourceHandle GetGBufferAlbedo() const { return gbuffer_albedo_texture_; }
    rhi::ResourceHandle GetGBufferNormal() const { return gbuffer_normal_texture_; }
    rhi::ResourceHandle GetGBufferORM() const { return gbuffer_orm_texture_; }
    rhi::ResourceHandle GetGBufferVelocity() const { return gbuffer_velocity_texture_; }
    // Sampleable depth for DeferredLighting — returns D32 depth texture directly.
    // Fragment shaders read it via depth2d<float> (Metal supports sampling depth textures directly).
    rhi::ResourceHandle GetGBufferDepthSampleable() const { return final_depth_texture_; }

    // Get global meshlet buffer for backface culling
    rhi::ResourceHandle GetGlobalMeshletBuffer() const { return global_meshlet_buffer_; }

    // Geometry buffer accessors for GlobalSDF voxelization
    rhi::ResourceHandle GetGlobalVertexBuffer() const { return global_vertex_buffer_; }
    rhi::ResourceHandle GetGlobalMeshletVerticesBuffer() const { return global_meshlet_vertices_buffer_; }
    rhi::ResourceHandle GetGlobalMeshletTrianglesBuffer() const { return global_meshlet_triangles_buffer_; }
    rhi::ResourceHandle GetClusterMapBuffer() const { return cluster_map_buffer_; }
    rhi::ResourceHandle GetGlobalInstanceDataBuffer() const { return global_instance_data_buffer_; }

private:
    GPUDrivenDrawPipeline() = default;

    bool CreatePipelines();
    bool CreateResources();
    bool CreateRenderPasses();
    bool CreateDescriptorSets();

    bool Stage1_ClusterBinning(rhi::RHICommandBuffer* cmd_buffer,
                               const RenderSceneSnapshot& scene_snapshot,
                               const CullingResults& culling_results,
                               u32 frame_index);

    bool Stage2_VisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer,
                                  const RenderSceneSnapshot& scene_snapshot,
                                  const math::m4x4& view_matrix,
                                  const math::m4x4& projection_matrix,
                                  u32 frame_index);

    bool Stage3_GPUDrawCalls(rhi::RHICommandBuffer* cmd_buffer,
                             const RenderSceneSnapshot& scene_snapshot,
                             const CullingResults& culling_results,
                             const GPUDrawResults& intermediate_results,
                             u32 frame_index,
                             u32 buffer_index);

    void SetupVisibilityBufferPipeline(rhi::RHICommandBuffer* cmd_buffer);
    void ResolveVisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer);
    rhi::ResourceHandle GetPreviousFrameDepth();

    rhi::RHIDeviceBase* device_{ nullptr };
    BinningConfig binning_config_;
    VisibilityBufferConfig visibility_config_;
    GPUDrawResults results_;

    HZBSystem* hzb_system_{ nullptr };
    VisibilityBufferSystem* visibility_buffer_system_{ nullptr };
    class GPUCullingPipeline* culling_pipeline_{ nullptr };

    // Phase 9.3b: Non-owning pointer to the RenderScene, set by the render
    // pipeline each frame. Used by DrawStreamingMeshes to iterate streaming meshes.
    RenderScene* render_scene_{ nullptr };

    rhi::PipelineLayoutHandle binning_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle binning_pipeline_{ rhi::handles::INVALID_PIPELINE };

    rhi::PipelineLayoutHandle visibility_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle visibility_pipeline_{ rhi::handles::INVALID_PIPELINE };

    rhi::PipelineLayoutHandle draw_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle draw_pipeline_{ rhi::handles::INVALID_PIPELINE };

    struct FrameResource {
        rhi::ResourceHandle camera_constants_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::DescriptorSetHandle global_draw_descriptor_set{ rhi::handles::INVALID_DESCRIPTOR_SET };
    };

    utl::vector<FrameResource> frame_resources_;

    // Single buffers (read-only or static)
    rhi::ResourceHandle bin_data_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle bin_counter_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle visibility_buffer_{ rhi::handles::INVALID_RESOURCE }; // Texture
    rhi::ResourceHandle indirect_draw_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    // Global Geometry Buffers (Static/Append only)

    rhi::ResourceHandle vertex_position_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle vertex_normal_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle vertex_uv_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle index_buffer_{ rhi::handles::INVALID_RESOURCE };

    // Global Buffers for GPU-Driven Rendering (Merged Scene Data)
    rhi::ResourceHandle global_meshlet_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle global_meshlet_vertices_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle global_meshlet_triangles_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle global_vertex_buffer_{ rhi::handles::INVALID_RESOURCE }; // Merged Positions
    rhi::ResourceHandle global_element_buffer_{ rhi::handles::INVALID_RESOURCE }; // Merged Vertex Elements (Normal, Tangent, UV)
    rhi::ResourceHandle cluster_map_buffer_{ rhi::handles::INVALID_RESOURCE }; // Cluster ID -> (MeshletID, InstanceID)
    rhi::ResourceHandle global_instance_data_buffer_{ rhi::handles::INVALID_RESOURCE }; // Instance ID -> World Matrix
    rhi::ResourceHandle global_material_data_buffer_{ rhi::handles::INVALID_RESOURCE }; // Material Data (for material sampling)

    // 🎨 Texture arrays for material sampling
    rhi::ResourceHandle albedo_texture_array_{ rhi::handles::INVALID_RESOURCE };    // Albedo texture array
    rhi::ResourceHandle normal_texture_array_{ rhi::handles::INVALID_RESOURCE };    // Normal texture array
    rhi::ResourceHandle orm_texture_array_{ rhi::handles::INVALID_RESOURCE };       // ORM texture array
    rhi::SamplerHandle texture_sampler_{ rhi::handles::INVALID_SAMPLER };           // Texture sampler

    // Total counts for global buffers
    u32 total_meshlet_count_{ 0 };
    u32 total_meshlet_vertex_count_{ 0 };
    u32 total_meshlet_triangle_count_{ 0 };
    u32 total_vertex_count_{ 0 };

    // CPU-side mirror of global_meshlet_buffer_. Dawn Storage buffers are
    // GPU-only: MapBuffer returns zeroed staging, so reads must come from CPU.
    utl::vector<rhi::RHIMeshlet> cpu_meshlet_cache_;

    math::m4x4 cached_view_matrix_;
    math::m4x4 cached_proj_matrix_;
    math::m4x4 prev_view_matrix_;   // Previous frame view matrix for velocity
    math::m4x4 prev_proj_matrix_;   // Previous frame proj matrix for velocity
    bool has_prev_frame_{ false };   // Whether previous frame data is available
    u32 meshlet_debug_mode_{ 0 };   // 0=off, 1=meshlet, 2=triangle, 3=mesh (mirrors DrawConstants.debug_mode)
    u32 vertex_count_{ 0 };
    u32 index_count_{ 0 };
    rhi::DataFormat index_format_{ rhi::DataFormat::R32_UInt };

    rhi::DescriptorSetLayoutHandle global_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle global_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    rhi::DescriptorSetLayoutHandle draw_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle draw_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    rhi::RenderPassHandle visibility_render_pass_{ rhi::handles::INVALID_RENDER_PASS };
    rhi::RenderPassHandle final_render_pass_{ rhi::handles::INVALID_RENDER_PASS };
    rhi::ResourceHandle final_color_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle final_depth_texture_{ rhi::handles::INVALID_RESOURCE };

    // GBuffer render targets (created in CreateRenderPasses)
    rhi::ResourceHandle gbuffer_albedo_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_normal_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_orm_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_velocity_texture_{ rhi::handles::INVALID_RESOURCE };

    // Sampleable depth (R32_Float) for GTAO, SSDO, DeferredLighting
    rhi::ResourceHandle gbuffer_depth_sampleable_{ rhi::handles::INVALID_RESOURCE };

    rhi::PipelineLayoutHandle resolve_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::DescriptorSetLayoutHandle resolve_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle resolve_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    rhi::ResourceHandle resolve_output_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle resolve_sampler_{ rhi::handles::INVALID_SAMPLER };

    // ---- Shadow Mapping Resources ----
    ShadowFrameResources shadow_frames_[3];                         // Triple-buffered per-frame
    rhi::PipelineHandle shadow_cull_pipeline_{ rhi::handles::INVALID_PIPELINE };    // Compute: cluster culling
    rhi::PipelineHandle shadow_depth_pipeline_{ rhi::handles::INVALID_PIPELINE };   // Graphics: depth-only raster
    rhi::PipelineHandle shadow_finalize_pipeline_{ rhi::handles::INVALID_PIPELINE }; // Compute: finalize indirect args
    rhi::PipelineHandle shadow_blit_pipeline_{ rhi::handles::INVALID_PIPELINE };    // Compute: D32→R32 blit
    rhi::PipelineLayoutHandle shadow_cull_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle shadow_depth_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle shadow_blit_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle shadow_cull_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle shadow_depth_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle shadow_blit_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    u32 shadow_max_clusters_{ 100000 };
    bool shadow_initialized_{ false };

    // GBuffer depth blit resources (reuses shadow_blit_pipeline_/layout/set_layout)
    rhi::DescriptorSetHandle gbuffer_depth_blit_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::ResourceHandle gbuffer_depth_blit_cb_{ rhi::handles::INVALID_RESOURCE };  // Resolution uniform buffer

    bool initialized_{ false };
    std::mutex mutex_;
};

} // namespace primal::graphics::nanite
