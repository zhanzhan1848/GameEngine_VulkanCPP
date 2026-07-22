#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "Graphics/Field/FieldDescriptor.h"
#include "ISDFDataProvider.h"
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>

namespace primal::graphics::nanite {

struct GlobalSDFConfig {
    u32 cascade_count{ 3 };
    u32 base_resolution{ 128 };
    u32 cascade_scale_factor{ 2 };
    f32 voxel_size_base{ 1.0f };
    f32 update_budget_ms{ 2.0f };
    f32 max_cascade_distance{ 1000.0f };
    bool enable_adaptive_quality{ true };
};

struct SDFCascade {
    rhi::ResourceHandle sdf_texture{ rhi::handles::INVALID_RESOURCE };
    math::v3 origin{ 0.0f, 1.0f, 1.0f };
    math::v3 extent{ 10.0f, 10.0f, 10.0f };
    f32 voxel_size{ 1.0f };
    u32 resolution{ 128 };
    u32 mip_levels{ 1 };
    u64 last_update_frame{ 0 };
    u32 cascade_index{ 0 };
    bool needs_update{ false };
    bool is_valid{ false };
    // True when origin snapped to a new grid cell this frame (or first-ever update).
    // Voxelization dispatch is gated on this — static scene + static camera → 0
    // dispatches after the initial fill.
    bool needs_voxelization{ true };
    bool ever_voxelized{ false };
};

struct GlobalSDFStats {
    u32 cascade_count{ 0 };
    u32 total_memory_mb{ 1 };
    u32 active_cascades{ 1 };
    u32 updates_this_frame{ 1 };
    f32 update_time_ms{ 0.0f };
    u32 skipped_updates{ 1 };
};

/// GPU buffer handles needed for SDF voxelization.
struct SDFVoxelizationResources {
    rhi::ResourceHandle vertex_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle meshlet_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle meshlet_vertices_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle meshlet_triangles_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_map_buffer{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle instance_data_buffer{ rhi::handles::INVALID_RESOURCE };
    u32 num_instances{ 0 };
};

class GlobalSDF {
public:
    static GlobalSDF& Get();

    GlobalSDF(const GlobalSDF&) = delete;
    GlobalSDF& operator=(const GlobalSDF&) = delete;

    bool Initialize(rhi::RHIDeviceBase* device, const GlobalSDFConfig& config = GlobalSDFConfig{});
    void Shutdown();

    void Update(const RenderSceneSnapshot& snapshot, u64 current_frame, const math::v3& camera_position);

    const SDFCascade& GetCascade(u32 index) const;
    const rhi::ResourceHandle GetGlobalTexture() const { return global_sdf_texture_; }
    const GlobalSDFConfig& GetConfig() const { return config_; }
    const GlobalSDFStats& GetStats() const { return stats_; }

    /// True iff this cascade's origin snapped to a new grid cell on the most
    /// recent Update(), or it has never been voxelized yet. Callers should
    /// gate DispatchVoxelization on this to avoid re-sweeping static geometry.
    bool CascadeNeedsVoxelization(u32 index) const;

    bool IsInitialized() const {
        return initialized_ && init_resources_valid_;
    }

    // Field System integration
    using CascadeDescriptors = std::array<field::FieldDescriptor, 4>;
    CascadeDescriptors GetCascadeDescriptors() const;
    void RegisterToFieldRegistry();
    void UnregisterFromFieldRegistry();

    /// Initialize the voxelization compute pipeline. Call once after Initialize().
    bool InitVoxelization(const SDFVoxelizationResources& resources);

    /// Update buffer handles (geometry may be uploaded after init).
    void SetVoxelizationResources(const SDFVoxelizationResources& resources) {
        vox_resources_ = resources;
    }

    /// Dispatch voxelization for a specific cascade. Call from render graph compute pass.
    /// If a data_provider_ is set (Strategy pattern), uses it instead of the legacy
    /// Nanite-meshlet vox_pipeline_. This is the path editor_mode uses.
    void DispatchVoxelization(rhi::RHICommandBuffer* cmd, u32 cascade_index);

    bool IsVoxelizationReady() const;

    /// Strategy pattern: plug in an alternative SDF data source (e.g. analytic,
    /// render-mesh rasterizer). When non-null, DispatchVoxelization routes through
    /// the provider instead of the legacy vox_pipeline_.
    void SetDataProvider(std::unique_ptr<ISDFDataProvider> provider) {
        data_provider_ = std::move(provider);
    }
    ISDFDataProvider* GetDataProvider() const { return data_provider_.get(); }

    /// Test/debug-only: fill all cascade textures by sampling sdf_fn on the CPU,
    /// uploading via staging buffer + CopyBufferToTexture. Production path uses
    /// DispatchVoxelization; this exists so headless tests can populate cascades
    /// without a Nanite mesh source. Blocking (creates+submits its own CB).
    /// Returns true if all cascades filled successfully.
    bool DebugFill(std::function<f32(const math::v3&)> sdf_fn);

private:
    GlobalSDF() = default;

    rhi::RHIDeviceBase* device_{ nullptr };
    GlobalSDFConfig config_;
    utl::vector<SDFCascade> cascades_;
    rhi::ResourceHandle global_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    GlobalSDFStats stats_;

    std::mutex mutex_;
    std::atomic<bool> initialized_{ false };
    bool init_resources_valid_{ false };  // F4: true only if all GPU resources non-INVALID

    // Voxelization pipeline resources
    bool voxelization_ready_{ false };
    SDFVoxelizationResources vox_resources_;
    rhi::PipelineHandle vox_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle vox_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle vox_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle vox_descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle vox_cascade_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };

    // Strategy-pattern provider. When non-null, DispatchVoxelization routes here
    // instead of the legacy vox_pipeline_ path. Allows editor_mode / tests to
    // populate the cascade without Nanite meshlet buffers.
    std::unique_ptr<ISDFDataProvider> data_provider_{nullptr};

    // Per-frame counter for provider triple-buffer slot indexing. Older code
    // passed cascade_index%3 to DispatchCascade, which is always 0 when only
    // cascade 0 is dispatched — this caused params_cb_[0] to be rewritten every
    // frame while the previous frame's GPU read was still in flight (CPU-GPU
    // race → corrupted SDF → unstable SurfaceNets mesh after a few frames).
    u32 provider_frame_counter_{0};

    bool CreateCascades();
    bool CreateGlobalTexture();
    void UpdateCascade(SDFCascade& cascade, const RenderSceneSnapshot& snapshot, const math::v3& camera_position);
    void MergeCascades();
    void UpdateStats(u64 current_frame);

    bool AllocateTexture(rhi::ResourceHandle& handle, u32 resolution, u32 mip_levels);
    void FreeTexture(rhi::ResourceHandle& handle);

    u32 CalculateRequiredResolution(f32 distance, u32 cascade_index) const;
    math::v3 CalculateCascadeOrigin(u32 cascade_index, const math::v3& camera_position, f32 voxel_size) const;
    f32 CalculateCascadeVoxelSize(u32 cascade_index) const;
};

} // namespace primal::graphics::nanite
