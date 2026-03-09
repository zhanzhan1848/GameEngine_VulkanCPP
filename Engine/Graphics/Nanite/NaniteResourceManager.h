#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::nanite {

struct ClusterBounds {
    math::v3 min;
    math::v3 max;
    f32 screen_space_error;
};

struct MeshletData {
    u32 vertex_offset;
    u32 index_offset;
    u32 triangle_count;
    u32 padding;
};

class NaniteRuntimeResource {
public:
    struct ClusterData {
        rhi::ResourceHandle bounds_buffer;
        rhi::ResourceHandle meshlet_buffer;
        rhi::ResourceHandle sdf_texture;
        u32 cluster_count;
        u32 meshlet_count;
    };
    
    struct StreamingData {
        rhi::ResourceHandle residency_buffer;
        rhi::ResourceHandle request_buffer;
        u64 last_access_frame;
        bool is_resident;
    };
    
    id::id_type geometry_id;
    ClusterData cluster_data;
    StreamingData streaming_data;
    
    std::atomic<u32> ref_count{ 0 };

    ~NaniteRuntimeResource();
    
    void AddRef() { 
        ref_count.fetch_add(1, std::memory_order_relaxed); 
    }
    
    void Release();
    
private:
    friend class NaniteResourceManager;
    
    NaniteRuntimeResource(id::id_type geometry_id);
    
    static std::unique_ptr<NaniteRuntimeResource> Create(id::id_type geometry_id) {
        return std::unique_ptr<NaniteRuntimeResource>(new NaniteRuntimeResource(geometry_id));
    }
};

class NaniteResourceManager {
public:
    static NaniteResourceManager& Get() {
        static NaniteResourceManager instance;
        return instance;
    }
    
    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();
    
    NaniteRuntimeResource* GetOrCreateResource(id::id_type geometry_id);
    void DestroyResource(NaniteRuntimeResource* resource);
    
    void AddGeometryRef(id::id_type geometry_id);
    void ReleaseGeometryRef(id::id_type geometry_id);
    
    void RequestClusterResidency(id::id_type geometry_id, u32 cluster_index);
    void UpdateResidency();
    
    void OnFrameEnd();
    
private:
    rhi::RHIDeviceBase* device_{ nullptr };
    std::mutex mutex_;
    
    std::unordered_map<id::id_type, std::unique_ptr<NaniteRuntimeResource>> resources_;
    std::unordered_map<id::id_type, u32> ref_counts_;
    std::unordered_set<id::id_type> destroyed_resources_;
    
    struct Page {
        rhi::ResourceHandle gpu_buffer;
        u64 size;
        bool resident;
        u64 last_access_frame;
    };
    utl::vector<Page> page_pool_;
    
    void UploadClusterData(NaniteRuntimeResource* resource);
    void EvictPages(u64 target_memory);
};

inline void NaniteRuntimeResource::Release() { 
    if (ref_count.fetch_sub(1, std::memory_order_relaxed) == 0) {
        NaniteResourceManager::Get().DestroyResource(this);
    }
}

} // namespace primal::graphics::nanite
