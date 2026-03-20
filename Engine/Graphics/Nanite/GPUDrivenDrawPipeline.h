#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "GPUCullingPipeline.h"
#include <mutex>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
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

    const GPUDrawResults& GetResults() const { return results_; }
    const BinningConfig& GetBinningConfig() const { return binning_config_; }
    const VisibilityBufferConfig& GetVisibilityConfig() const { return visibility_config_; }
    bool IsInitialized() const { return initialized_; }

    void SetHZBSystem(HZBSystem* hzb_system) { hzb_system_ = hzb_system; }
    void SetVisibilityBufferSystem(VisibilityBufferSystem* vis_system) { visibility_buffer_system_ = vis_system; }
    void SetCullingPipeline(class GPUCullingPipeline* culling_pipeline) { culling_pipeline_ = culling_pipeline; }

    void UpdateGeometryData(const RenderSceneSnapshot& scene_snapshot);
    bool CreateGeometryBuffers(u32 vertex_count, u32 index_count);
    void UploadGeometryData(const RenderSceneSnapshot& scene_snapshot);

    // Get the final output texture that was rendered to
    rhi::ResourceHandle GetFinalOutputTexture() const { return final_color_texture_; }

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
    rhi::ResourceHandle cluster_map_buffer_{ rhi::handles::INVALID_RESOURCE }; // Cluster ID -> (MeshletID, InstanceID)
    rhi::ResourceHandle global_instance_data_buffer_{ rhi::handles::INVALID_RESOURCE }; // Instance ID -> World Matrix
    
    // Total counts for global buffers
    u32 total_meshlet_count_{ 0 };
    u32 total_meshlet_vertex_count_{ 0 };
    u32 total_meshlet_triangle_count_{ 0 };
    u32 total_vertex_count_{ 0 };

    math::m4x4 cached_view_matrix_;
    math::m4x4 cached_proj_matrix_;
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

    rhi::PipelineLayoutHandle resolve_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::DescriptorSetLayoutHandle resolve_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle resolve_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    rhi::ResourceHandle resolve_output_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle resolve_sampler_{ rhi::handles::INVALID_SAMPLER };

    bool initialized_{ false };
    std::mutex mutex_;
};

} // namespace primal::graphics::nanite
