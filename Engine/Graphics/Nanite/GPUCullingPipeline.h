#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "../Scene/RenderSceneSnapshot.h"
#include <mutex>
#include <atomic>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::nanite {

class NaniteStreamingManager;
class NaniteResourceManager;

struct CullingConfig {
    u32 max_clusters_per_dispatch{ 100000 };
    u32 max_instances_per_dispatch{ 10000 };
    float lod_bias{ 1.0f };
    bool enable_occlusion_culling{ true };
    bool enable_lod_selection{ true };
    bool enable_small_object_culling{ true };
    bool enable_streaming_feedback{ true };
    bool debug_visualization{ false };
    bool enable_gpu_culling{ true }; // Changed default to true
    float small_object_threshold{ 0.01f }; // Screen space threshold
    uint32_t max_lod_levels{ 4 };
};

struct ClusterBounds {
    math::v3 min{ 0.0f, 0.0f, 0.0f };
    math::v3 max{ 0.0f, 0.0f, 0.0f };
    float screen_space_error{ 1.0f };
};

struct VisibilityResult {
    std::atomic<uint32_t> is_visible{ 0 };
    uint32_t cluster_index{ 0 };
    uint32_t instance_index{ 0 };
    uint32_t padding{ 0 };
};

struct CullingResults {
    u32 visible_cluster_count{ 0 };
    u32 visible_instance_count{ 0 };
    u32 culled_cluster_count{ 0 };
    u32 culled_instance_count{ 0 };
    u32 lod_transitions{ 0 };
    bool needs_readback{ false }; // Flag to indicate when GPU->CPU readback is needed

    rhi::ResourceHandle indirect_args_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle instance_visibility_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_visibility_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle visible_cluster_list_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle draw_command_buffer{ rhi::handles::INVALID_RESOURCE };

    utl::vector<uint32_t> visible_cluster_indices;
};

class GPUCullingPipeline {
public:
    static GPUCullingPipeline& Get();

    GPUCullingPipeline() = default;

    ~GPUCullingPipeline() { Shutdown(); }
    
    GPUCullingPipeline(const GPUCullingPipeline&) = delete;
    GPUCullingPipeline& operator=(const GPUCullingPipeline&) = delete;
    
    bool Initialize(rhi::RHIDeviceBase* device, const CullingConfig& config = CullingConfig{});
    void Shutdown();
    
    bool Execute(rhi::RHICommandBuffer* cmd_buffer,
                const RenderSceneSnapshot& snapshot,
                const math::m4x4& view_matrix,
                const math::m4x4& projection_matrix,
                NaniteStreamingManager* streaming_manager = nullptr,
                u32 frame_index = 0);
    
    const CullingResults& GetResults() const { return results_; }
    const CullingConfig& GetConfig() const { return config_; }
    
    void SetConfig(const CullingConfig& config) { config_ = config; }
    void SetLODBias(float bias) { config_.lod_bias = bias; }
    
    bool IsInitialized() const { return initialized_; }

private:
    rhi::RHIDeviceBase* device_{ nullptr };
    CullingConfig config_;
    CullingResults results_;
    
    rhi::ResourceHandle frustum_culling_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle distance_culling_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle lod_selection_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_expansion_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle occlusion_culling_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle compaction_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_command_pipeline_{ rhi::handles::INVALID_RESOURCE };
    
    rhi::ResourceHandle streaming_feedback_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle update_access_time_pipeline_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle check_residency_pipeline_{ rhi::handles::INVALID_RESOURCE };
    
    rhi::ResourceHandle instance_bounds_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle instance_visibility_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_visibility_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle visible_counter_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle visible_cluster_list_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_args_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle culling_constants_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    rhi::ResourceHandle hiz_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle occlusion_query_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle debug_readback_buffer_{ rhi::handles::INVALID_RESOURCE }; // For GPU-to-CPU debug data transfer

    rhi::PipelineLayoutHandle culling_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle culling_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle culling_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };
    
    rhi::PipelineLayoutHandle streaming_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle streaming_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle streaming_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };
    
    bool initialized_{ false };
    std::mutex mutex_;
    
    bool CreatePipelines();
    bool CreateBuffers();
    bool CreateDescriptorSets(u32 frame_index);
    
    bool Stage1_FrustumCulling(rhi::RHICommandBuffer* cmd_buffer,
                               const RenderSceneSnapshot& snapshot,
                               const math::m4x4& view_projection,
                               u32 frame_index);
    
    bool Stage2_OcclusionCulling(rhi::RHICommandBuffer* cmd_buffer,
                                 const RenderSceneSnapshot& snapshot,
                                 u32 frame_index);
    
    bool Stage3_LODSelection(rhi::RHICommandBuffer* cmd_buffer,
                            const RenderSceneSnapshot& snapshot,
                            const math::m4x4& view_matrix,
                            const math::v3& camera_position,
                            u32 frame_index);
    
    bool Stage4_InstanceCulling(rhi::RHICommandBuffer* cmd_buffer,
                               const RenderSceneSnapshot& snapshot,
                               u32 frame_index);

    bool Stage2_DistanceCulling(rhi::RHICommandBuffer* cmd_buffer,
                               const RenderSceneSnapshot& snapshot,
                               u32 frame_index);

    bool Stage4_ClusterExpansion(rhi::RHICommandBuffer* cmd_buffer,
                                const RenderSceneSnapshot& snapshot,
                                u32 frame_index);

    bool Stage5_OcclusionCulling(rhi::RHICommandBuffer* cmd_buffer,
                                const RenderSceneSnapshot& snapshot,
                                u32 frame_index);

    bool Stage6_Compaction(rhi::RHICommandBuffer* cmd_buffer,
                          const RenderSceneSnapshot& snapshot,
                          u32 frame_index);

    bool Stage7_BuildIndirectCommands(rhi::RHICommandBuffer* cmd_buffer,
                                     const RenderSceneSnapshot& snapshot,
                                     u32 frame_index);

    bool UpdateCullingDescriptorSet(const RenderSceneSnapshot& snapshot,
                                   const math::m4x4& view_matrix,
                                   const math::m4x4& projection_matrix,
                                   const math::m4x4& view_projection,
                                   const math::v3& camera_position,
                                   u32 frame_index);

    math::v3 extract_camera_position(const math::m4x4& view_matrix);
    void ReadbackResults(const RenderSceneSnapshot& snapshot, u32 frame_index);
    
    bool CompactResults(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index);
    
    void StreamingFeedback(rhi::RHICommandBuffer* cmd_buffer,
                           NaniteStreamingManager* streaming_manager,
                           u32 frame_index);
    
    void UpdateResults();
};

} // namespace primal::graphics::nanite
