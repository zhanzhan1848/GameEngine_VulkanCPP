#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHITypes.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace primal::graphics::nanite {

class NaniteResourceManager;

struct NaniteStreamingConfig {
    u64 page_pool_size_bytes{ 256 * 1024 * 1024 };
    u32 page_size_bytes{ 256 * 1024 };
    u32 max_requests_per_frame{ 100 };
    f32 eviction_threshold{ 0.8f };
    bool enable_prefetching{ true };
};

struct ClusterPageInfo {
    u32 page_index{ UINT32_MAX };
    u64 last_access_frame{ 0 };
    u32 priority{ 0 };
    bool is_resident{ false };
};

struct StreamingRequest {
    id::id_type geometry_id;
    u32 cluster_index;
    u32 priority;
    math::v3 camera_position;
};

struct StreamingStats {
    u32 total_clusters_streamed{ 0 };
    u32 total_clusters_evicted{ 0 };
    u32 current_resident_clusters{ 0 };
    u32 page_pool_usage{ 0 };
    u32 pending_requests_count{ 0 };
    f32 avg_streaming_latency_ms{ 0.0f };
    u32 eviction_count{ 0 };
};

class NaniteStreamingManager {
public:
    NaniteStreamingManager() = default;
    ~NaniteStreamingManager() = default;
    
    NaniteStreamingManager(const NaniteStreamingManager&) = delete;
    NaniteStreamingManager& operator=(const NaniteStreamingManager&) = delete;
    
    bool Initialize(rhi::RHIDeviceBase* device, const NaniteStreamingConfig& config);
    void Shutdown();
    
    void ProcessRequests(u64 current_frame);
    void UpdateLRU(u64 current_frame, const math::v3& camera_position);
    
    bool IsClusterResident(id::id_type geometry_id, u32 cluster_index) const;
    bool RequestCluster(id::id_type geometry_id, u32 cluster_index, u32 priority, const math::v3& camera_position);
    
    rhi::ResourceHandle GetResidencyBuffer() const { return residency_buffer_; }
    rhi::ResourceHandle GetRequestBuffer() const { return request_buffer_; }
    rhi::ResourceHandle GetFeedbackBuffer() const { return feedback_buffer_; }
    rhi::ResourceHandle GetPagePoolBuffer() const { return page_pool_buffer_; }
    
    const StreamingStats& GetStats() const { return stats_; }
    const NaniteStreamingConfig& GetConfig() const { return config_; }
    
    void SetNaniteResourceManager(NaniteResourceManager* manager) { resource_manager_ = manager; }
    
private:
    bool AllocatePagePool();
    bool AllocateBuffers();
    
    u32 AllocatePage();
    void FreePage(u32 page_index);
    
    void EvictLRUClusters();
    bool LoadClusterToGPU(id::id_type geometry_id, u32 cluster_index);
    void UpdateResidency(id::id_type geometry_id, u32 cluster_index, bool is_resident);
    
    rhi::RHIDeviceBase* device_{ nullptr };
    NaniteResourceManager* resource_manager_{ nullptr };
    
    NaniteStreamingConfig config_;
    
    rhi::ResourceHandle page_pool_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle residency_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle request_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle feedback_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    rhi::ResourceHandle request_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle feedback_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    utl::vector<u32> free_pages_;
    std::unordered_map<u64, ClusterPageInfo> cluster_page_info_;
    
    utl::vector<StreamingRequest> pending_requests_;
    utl::vector<StreamingRequest> gpu_requests_;
    
    std::unordered_map<u64, u64> cluster_last_access_;
    
    StreamingStats stats_;
    
    std::atomic<u32> current_resident_count_{ 0 };
    std::recursive_mutex mutable mutex_;
    bool initialized_{ false };
    
    u64 MakeClusterKey(id::id_type geometry_id, u32 cluster_index) const {
        return (static_cast<u64>(geometry_id) << 32) | cluster_index;
    }
};

} // namespace primal::graphics
