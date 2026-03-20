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

// GPU memory layout: 192 bytes total, 16-byte aligned for optimal GPU access
struct InstanceData {
    math::m4x4 world_matrix;              // 64 bytes - offsets 0-63
    math::m4x4 inverse_world_matrix;      // 64 bytes - offsets 64-127
    id::id_type geometry_id;              // 4 bytes - offset 128
    id::id_type material_id;              // 4 bytes - offset 132
    u32 cluster_start;                    // 4 bytes - offset 136
    u32 cluster_count;                    // 4 bytes - offset 140
    u32 cluster_map_base;                 // 4 bytes - offset 144
    u32 padding;                         // 4 bytes - offset 148
    u32 padding1;                         // 4 bytes - offset 152 (extra padding for 16-byte alignment)
    u32 padding2;                         // 4 bytes - offset 156 (extra padding for alignment)
    math::v3 bounds_center;               // 12 bytes - offsets 160-171 (16-byte aligned!)
    f32 bounds_radius;                    // 4 bytes - offset 172
    u32 bounds_padding[2];                // 12 bytes - offsets 176-187
    u32 bounds_padding2;                  // 4 bytes - offset 188-191 (total 192 bytes)
};

static_assert(sizeof(InstanceData) == 192, "InstanceData must be 192 bytes for GPU alignment");

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
    
    const utl::vector<InstanceData>& GetInstanceData() const { return instance_data_cpu_; }

    // Upload staging data to GPU buffers (requires command buffer for GPU->GPU copy)
    bool UploadToGPUBuffers(rhi::RHICommandBuffer* cmd_buffer);

private:
    rhi::RHIDeviceBase* device_{ nullptr };
    
    rhi::ResourceHandle instance_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_ref_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle instance_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle cluster_ref_staging_buffer_{ rhi::handles::INVALID_RESOURCE };
    
    utl::vector<InstanceData> instance_data_cpu_;
    
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
