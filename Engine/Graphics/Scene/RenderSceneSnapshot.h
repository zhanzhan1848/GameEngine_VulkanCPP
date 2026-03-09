#pragma once

#include "../../Common/PrimitiveTypes.h"
#include "../../Common/Id.h"
#include "../../Utilities/Vector.h"
#include "../RHI/Core/RHITypes.h"
#include "../../EngineAPI/GameEntity.h"
#include <mutex>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics {

class RenderScene;

// GPU memory layout: 144 bytes total, 16-byte aligned for optimal GPU access
struct InstanceData {
    math::m4x4 world_matrix;              // 64 bytes
    math::m4x4 inverse_world_matrix;      // 64 bytes
    id::id_type geometry_id;              // 4 bytes - references NaniteResourceManager
    id::id_type material_id;              // 4 bytes - material instance ID
    u32 cluster_start;                    // 4 bytes - start index in cluster ref buffer
    u32 cluster_count;                    // 4 bytes - number of clusters for this instance
};

static_assert(sizeof(InstanceData) == 144, "InstanceData must be 144 bytes for GPU alignment");

struct ClusterRef {
    id::id_type geometry_id;              // 4 bytes
    u32 cluster_index;                    // 4 bytes
    u32 padding[2];                       // 8 bytes - pad to 16-byte alignment
};

static_assert(sizeof(ClusterRef) == 16, "ClusterRef must be 16 bytes for GPU alignment");

class RenderSceneSnapshot {
public:
    RenderSceneSnapshot() = default;
    ~RenderSceneSnapshot() = default;
    
    RenderSceneSnapshot(const RenderSceneSnapshot&) = delete;
    RenderSceneSnapshot& operator=(const RenderSceneSnapshot&) = delete;
    
    bool Initialize(rhi::RHIDeviceBase* device, 
                    u32 initial_instance_capacity = 1000,
                    u32 initial_cluster_capacity = 10000);
    
    void Shutdown();
    
    bool Rebind(const RenderScene& scene);
    bool PartialUpdate(const RenderScene& scene, const utl::vector<game_entity::entity_id>& dirty_entities);
    
    rhi::ResourceHandle GetInstanceBuffer() const { return instance_buffer_; }
    rhi::ResourceHandle GetClusterRefBuffer() const { return cluster_ref_buffer_; }
    u32 GetInstanceCount() const { return instance_count_; }
    u32 GetClusterRefCount() const { return cluster_ref_count_; }
    bool NeedsFullRebuild() const { return needs_full_rebuild_; }
    void ClearFullRebuildFlag() { needs_full_rebuild_ = false; }

private:
    rhi::RHIDeviceBase* device_{ nullptr };
    rhi::ResourceHandle instance_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_ref_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle instance_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_ref_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    u32 instance_count_{ 0 };
    u32 cluster_ref_count_{ 0 };
    u32 instance_capacity_{ 0 };
    u32 cluster_ref_capacity_{ 0 };
    
    bool needs_full_rebuild_{ true };
    bool initialized_{ false };
    std::recursive_mutex mutex_;
    
    bool AllocateBuffers(u32 instance_capacity, u32 cluster_capacity);
    bool ResizeBuffersIfNeeded(u32 required_instances, u32 required_cluster_refs);
    bool UploadInstanceData(const InstanceData* data, u32 count);
    bool UploadClusterRefs(const ClusterRef* refs, u32 count);
    bool ExtractSceneData(const RenderScene& scene,
                         utl::vector<InstanceData>& out_instances,
                         utl::vector<ClusterRef>& out_cluster_refs);
    bool UpdateInstances(const RenderScene& scene,
                        const utl::vector<game_entity::entity_id>& dirty_entities,
                        utl::vector<InstanceData>& instances,
                        utl::vector<ClusterRef>& cluster_refs);
};

} // namespace primal::graphics
