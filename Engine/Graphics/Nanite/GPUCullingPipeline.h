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
class GPUDrivenDrawPipeline;

struct CullingConfig {
    u32 max_clusters_per_dispatch{ 100000 };
    u32 max_instances_per_dispatch{ 10000 };
    float lod_bias{ 1.0f };
    bool enable_occlusion_culling{ true };
    bool enable_lod_selection{ true };
    bool enable_small_object_culling{ false }; // Keep disabled for now
    bool enable_streaming_feedback{ true };
    bool debug_visualization{ false };
    bool enable_gpu_culling{ true };
    float small_object_threshold{ 0.01f }; // Screen space threshold
    uint32_t max_lod_levels{ 4 };
    bool enable_debug_output{ false }; // Enable culling debug output
};

struct ClusterBounds {
    math::v3 min{ 0.0f, 0.0f, 0.0f };
    math::v3 max{ 0.0f, 0.0f, 0.0f };
    float screen_space_error{ 1.0f };
};

// Debug culling data structure
struct CullingDebugData {
    float view_space_z;           // Z value in view space
    float bounds_radius;          // Object bounding radius
    float distance_to_camera;     // Distance from camera
    u32 is_visible;              // Visibility result
    u32 instance_id;             // Instance identifier
    u32 cluster_id;              // Cluster identifier
    u32 culling_reason;          // Why it was culled (0=frustum, 1=distance, 2=none, 3=backface)
    u32 culling_plane;           // 🔥 NEW: Which plane caused culling (0=left, 1=right, 2=bottom, 3=top, 4=near, 5=far, 0xFFFFFFFF=none)
    float plane_distances[6];     // 🔥 NEW: Distance to each frustum plane for debugging

    // 🔥 NEW: Backface culling specific data
    u32 meshlet_id;              // Meshlet ID for backface culling
    float backface_cos_angle;     // Cosine of angle between view dir and cone axis
    float backface_cutoff;        // Cone cutoff value for debugging
    u32 is_backface_culled;      // Whether backface culling was triggered (0=no, 1=yes)
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
                u32 buffer_index = 0);  // Changed: Use pre-calculated buffer index instead of frame_index
    
    const CullingResults& GetResults() {
        // Try to update results from GPU if readback is needed
        const_cast<GPUCullingPipeline*>(this)->UpdateResults();
        return results_;
    }
    const CullingConfig& GetConfig() const { return config_; }
    
    void SetConfig(const CullingConfig& config) { config_ = config; }
    void SetLODBias(float bias) { config_.lod_bias = bias; }

    // Debug bypass for the gray-white bug investigation. When true, the next
    // Execute() call will set CullingConstants.force_pass_all = 1, which stage4
    // of GPUCullingPipeline.wgsl consumes to skip the instance-visibility check
    // and the far-plane cluster cull. Effect: all clusters from all instances
    // survive the culling pipeline and reach DrawIndirect. Use to bisect whether
    // the gray-white bug originates in stage4's visibility/far-plane logic or
    // downstream (stage5 occlusion, stage6/7 compact+indirect, or post-cull).
    void SetForcePassAll(bool enabled) { force_pass_all_debug_ = enabled; }
    bool IsForcePassAll() const { return force_pass_all_debug_; }

    // Set HZB System for occlusion culling
    void SetHZBSystem(class HZBSystem* hzb_system) {
        hzb_system_ = hzb_system;
        // Re-create descriptor sets to use the new HZB texture
        if (initialized_) {
            UpdateHZBBindings();
        }
    }

    // Set GPU Draw Pipeline for accessing global meshlet buffer (for backface culling)
    void SetGPUDrawPipeline(GPUDrivenDrawPipeline* pipeline) { gpuDrawPipeline_ = pipeline; }

    // Debug methods
    void EnableDebugOutput(bool enable) { config_.enable_debug_output = enable; }
    bool ReadDebugData(utl::vector<primal::graphics::nanite::CullingDebugData>& out_debug_data);
    
    bool IsInitialized() const { return initialized_; }

    // Helper to get frame resources for external systems (like RenderGraph)
    rhi::ResourceHandle GetIndirectBuffer(u32 frame_index) const {
        return frame_resources_[frame_index % 3].indirect_args_buffer;
    }

    rhi::ResourceHandle GetVisibleClusterListBuffer(u32 frame_index) const {
        return frame_resources_[frame_index % 3].visible_cluster_list_buffer;
    }

private:
    rhi::RHIDeviceBase* device_{ nullptr };
    CullingConfig config_;
    CullingResults results_;
    
    rhi::ResourceHandle reset_buffers_pipeline_{ rhi::handles::INVALID_RESOURCE };
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

    // Triple-buffered frame resources for Culling-Draw synchronization
    struct FrameResources {
        rhi::ResourceHandle instance_bounds_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle instance_visibility_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle cluster_visibility_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle visible_counter_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle cluster_visibility_counter_buffer{ rhi::handles::INVALID_RESOURCE }; // NEW: Atomic counter for cluster expansion
        rhi::ResourceHandle visible_cluster_list_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle indirect_args_buffer{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle culling_constants_buffer{ rhi::handles::INVALID_RESOURCE };
        u32 frame_index{ 0 };
        bool in_use{ false };
    };
    std::array<FrameResources, 3> frame_resources_;
    u32 current_frame_resource_{ 0 };

    rhi::ResourceHandle hiz_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle occlusion_query_buffer_{ rhi::handles::INVALID_RESOURCE };

    // Pointer to HZB System for occlusion culling
    class HZBSystem* hzb_system_{ nullptr };

    // Pointer to GPU Draw Pipeline for accessing global meshlet buffer
    GPUDrivenDrawPipeline* gpuDrawPipeline_{ nullptr };

    // Triple-buffered debug buffers to avoid data races
    std::array<rhi::ResourceHandle, 3> culling_debug_buffers_{ rhi::handles::INVALID_RESOURCE };
    static constexpr u32 MAX_DEBUG_ENTRIES = 1000; // Limit debug data size

    rhi::PipelineLayoutHandle culling_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle culling_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    // Note: brace-init with a single value only initializes element 0 — the rest
    // get value-initialized to 0, which is a VALID DescriptorSetHandle. UpdateHZBBindings
    // would then write to descriptor sets belonging to OTHER layouts (silent corruption).
    std::array<rhi::DescriptorSetHandle, 3> culling_descriptor_sets_{ rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET };
    
    rhi::PipelineLayoutHandle streaming_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle streaming_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle streaming_descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    bool initialized_{ false };
    std::mutex mutex_;

    // Execution state tracking (replaces static locals in Execute())
    // These must be member variables to reset properly across Initialize/Shutdown cycles
    u32 execute_call_count_{ 0 };
    bool basic_descriptor_sets_created_{ false };
    bool backface_descriptor_sets_created_{ false };
    bool hzb_bindings_updated_{ false };
    u32 matrix_print_count_{ 0 };

    // Reflects CullingConstants.force_pass_all for the diagnostic toggle.
    // Default false so production behavior is unchanged.
    bool force_pass_all_debug_{false};

    // Culling constants structure matching Metal shader layout
    struct CullingConstants {
        math::m4x4 view_matrix;
        math::m4x4 projection_matrix;
        math::m4x4 view_projection_matrix;
        math::v4 camera_position;
        float near_plane;
        float far_plane;
        u32 frame_index;
        u32 enable_occlusion_culling;
        u32 enable_lod_selection;
        u32 enable_small_object_culling;
        float small_object_threshold;
        float lod_bias;
        u32 max_lod_levels;
        u32 instance_count;
        u32 cluster_count;
        u32 force_pass_all; // 🔥 DEBUG: Force all geometry to pass culling
        u32 enable_debug_output; // 🔥 DEBUG: Enable debug output
    };

    bool CreatePipelines();
    bool CreateBuffers();
    bool CreateDescriptorSets(const RenderSceneSnapshot& snapshot);
    bool UpdateHZBBindings();  // Update HZB texture binding after HZB system is set
    bool UpdateCullingConstants(u32 frame_index, const CullingConstants& constants);

    bool Stage0_ResetBuffers(rhi::RHICommandBuffer* cmd_buffer,
                            const RenderSceneSnapshot& snapshot,
                            u32 frame_index);

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

    // Helper function to get current frame's descriptor set
    rhi::DescriptorSetHandle GetCurrentFrameDescriptorSet(u32 frame_index) const {
        return culling_descriptor_sets_[frame_index % 3];
    }

    math::v3 extract_camera_position(const math::m4x4& view_matrix);
    void ReadbackResults(const RenderSceneSnapshot& snapshot, u32 frame_index);
    
    bool CompactResults(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index);
    
    void StreamingFeedback(rhi::RHICommandBuffer* cmd_buffer,
                           NaniteStreamingManager* streaming_manager,
                           u32 frame_index);
    
    void UpdateResults();
};

} // namespace primal::graphics::nanite
