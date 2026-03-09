#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "../Scene/RenderSceneSnapshot.h"
#include <mutex>
#include <atomic>

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
};

struct GlobalSDFStats {
    u32 cascade_count{ 0 };
    u32 total_memory_mb{ 1 };
    u32 active_cascades{ 1 };
    u32 updates_this_frame{ 1 };
    f32 update_time_ms{ 0.0f };
    u32 skipped_updates{ 1 };
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
    
    bool IsInitialized() const { return initialized_; }
    
private:
    GlobalSDF() = default;
    
    rhi::RHIDeviceBase* device_{ nullptr };
    GlobalSDFConfig config_;
    utl::vector<SDFCascade> cascades_;
    rhi::ResourceHandle global_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    GlobalSDFStats stats_;
    
    std::mutex mutex_;
    std::atomic<bool> initialized_{ false };
    
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
