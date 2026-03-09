#include "RenderSceneSnapshot.h"
#include "../RenderScene.h"
#include "../RenderProxy.h"
#include "../../Components/Cluster.h"
#include "../../Graphics/Nanite/NaniteResourceManager.h"
#include "../../Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics {

bool RenderSceneSnapshot::Initialize(rhi::RHIDeviceBase* device,
                                     u32 initial_instance_capacity,
                                     u32 initial_cluster_capacity) {
    if (!device || initialized_) {
        return false;
    }
    
    device_ = device;
    
    if (!AllocateBuffers(initial_instance_capacity, initial_cluster_capacity)) {
        return false;
    }
    
    initialized_ = true;
    needs_full_rebuild_ = true;
    
    return true;
}

void RenderSceneSnapshot::Shutdown() {
    if (!initialized_) {
        return;
    }
    
    if (instance_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_buffer_);
        instance_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    if (cluster_ref_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(cluster_ref_buffer_);
        cluster_ref_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    if (instance_staging_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_staging_buffer_);
        instance_staging_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    if (cluster_ref_staging_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(cluster_ref_staging_buffer_);
        cluster_ref_staging_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    instance_count_ = 0;
    cluster_ref_count_ = 0;
    instance_capacity_ = 0;
    cluster_ref_capacity_ = 0;
    initialized_ = false;
}

bool RenderSceneSnapshot::AllocateBuffers(u32 instance_capacity, u32 cluster_capacity) {
    const u64 instance_buffer_size = instance_capacity * sizeof(InstanceData);
    const u64 cluster_buffer_size = cluster_capacity * sizeof(ClusterRef);
    
    rhi::BufferDesc instance_desc{};
    instance_desc.size = instance_buffer_size;
    instance_desc.type = rhi::BufferType::Structured;
    instance_desc.usage = rhi::GPUMemoryUsage::Static;
    instance_desc.structured.elementCount = instance_capacity;
    instance_desc.structured.elementStride = sizeof(InstanceData);
    instance_desc.name = "SceneSnapshot_InstanceBuffer";
    
    instance_buffer_ = device_->CreateBuffer(instance_desc);
    if (instance_buffer_ == rhi::handles::INVALID_RESOURCE) {
        return false;
    }
    
    rhi::BufferDesc cluster_desc{};
    cluster_desc.size = cluster_buffer_size;
    cluster_desc.type = rhi::BufferType::Structured;
    cluster_desc.usage = rhi::GPUMemoryUsage::Static;
    cluster_desc.structured.elementCount = cluster_capacity;
    cluster_desc.structured.elementStride = sizeof(ClusterRef);
    cluster_desc.name = "SceneSnapshot_ClusterRefBuffer";
    
    cluster_ref_buffer_ = device_->CreateBuffer(cluster_desc);
    if (cluster_ref_buffer_ == rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_buffer_);
        instance_buffer_ = rhi::handles::INVALID_RESOURCE;
        return false;
    }
    
    rhi::BufferDesc instance_staging_desc{};
    instance_staging_desc.size = instance_buffer_size;
    instance_staging_desc.type = rhi::BufferType::Raw;
    instance_staging_desc.usage = rhi::GPUMemoryUsage::Staging;
    instance_staging_desc.name = "SceneSnapshot_InstanceStagingBuffer";
    
    instance_staging_buffer_ = device_->CreateBuffer(instance_staging_desc);
    if (instance_staging_buffer_ == rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_buffer_);
        device_->DestroyBuffer(cluster_ref_buffer_);
        instance_buffer_ = rhi::handles::INVALID_RESOURCE;
        cluster_ref_buffer_ = rhi::handles::INVALID_RESOURCE;
        return false;
    }
    
    rhi::BufferDesc cluster_staging_desc{};
    cluster_staging_desc.size = cluster_buffer_size;
    cluster_staging_desc.type = rhi::BufferType::Raw;
    cluster_staging_desc.usage = rhi::GPUMemoryUsage::Staging;
    cluster_staging_desc.name = "SceneSnapshot_ClusterRefStagingBuffer";
    
    cluster_ref_staging_buffer_ = device_->CreateBuffer(cluster_staging_desc);
    if (cluster_ref_staging_buffer_ == rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(instance_buffer_);
        device_->DestroyBuffer(cluster_ref_buffer_);
        device_->DestroyBuffer(instance_staging_buffer_);
        instance_buffer_ = rhi::handles::INVALID_RESOURCE;
        cluster_ref_buffer_ = rhi::handles::INVALID_RESOURCE;
        instance_staging_buffer_ = rhi::handles::INVALID_RESOURCE;
        return false;
    }
    
    instance_capacity_ = instance_capacity;
    cluster_ref_capacity_ = cluster_capacity;
    
    return true;
}

bool RenderSceneSnapshot::ResizeBuffersIfNeeded(u32 required_instances, u32 required_cluster_refs) {
    bool needs_resize = false;
    u32 new_instance_capacity = instance_capacity_;
    u32 new_cluster_capacity = cluster_ref_capacity_;
    
    if (required_instances > instance_capacity_) {
        new_instance_capacity = std::max(required_instances, instance_capacity_ * 2);
        needs_resize = true;
    }
    
    if (required_cluster_refs > cluster_ref_capacity_) {
        new_cluster_capacity = std::max(required_cluster_refs, cluster_ref_capacity_ * 2);
        needs_resize = true;
    }
    
    if (needs_resize) {
        if (instance_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(instance_buffer_);
            instance_buffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (cluster_ref_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(cluster_ref_buffer_);
            cluster_ref_buffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (instance_staging_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(instance_staging_buffer_);
            instance_staging_buffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (cluster_ref_staging_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(cluster_ref_staging_buffer_);
            cluster_ref_staging_buffer_ = rhi::handles::INVALID_RESOURCE;
        }
        
        if (!AllocateBuffers(new_instance_capacity, new_cluster_capacity)) {
            return false;
        }
        
        needs_full_rebuild_ = true;
    }
    
    return true;
}

bool RenderSceneSnapshot::Rebind(const RenderScene& scene) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    utl::vector<InstanceData> instances;
    utl::vector<ClusterRef> cluster_refs;
    
    if (!ExtractSceneData(scene, instances, cluster_refs)) {
        return false;
    }
    
    if (!ResizeBuffersIfNeeded(static_cast<u32>(instances.size()), 
                               static_cast<u32>(cluster_refs.size()))) {
        return false;
    }
    
    if (!UploadInstanceData(instances.data(), static_cast<u32>(instances.size()))) {
        return false;
    }
    
    if (!UploadClusterRefs(cluster_refs.data(), static_cast<u32>(cluster_refs.size()))) {
        return false;
    }
    
    instance_count_ = static_cast<u32>(instances.size());
    cluster_ref_count_ = static_cast<u32>(cluster_refs.size());
    needs_full_rebuild_ = false;
    
    return true;
}

bool RenderSceneSnapshot::PartialUpdate(const RenderScene& scene,
                                        const utl::vector<game_entity::entity_id>& dirty_entities) {
    if (dirty_entities.empty()) {
        return true;
    }
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (needs_full_rebuild_) {
        return Rebind(scene);
    }
    
    utl::vector<InstanceData> instances;
    utl::vector<ClusterRef> cluster_refs;
    
    if (!UpdateInstances(scene, dirty_entities, instances, cluster_refs)) {
        return false;
    }
    
    if (instances.empty()) {
        return true;
    }
    
    if (!ResizeBuffersIfNeeded(static_cast<u32>(instances.size()),
                               static_cast<u32>(cluster_refs.size()))) {
        return false;
    }
    
    if (!UploadInstanceData(instances.data(), static_cast<u32>(instances.size()))) {
        return false;
    }
    
    if (!UploadClusterRefs(cluster_refs.data(), static_cast<u32>(cluster_refs.size()))) {
        return false;
    }
    
    instance_count_ = static_cast<u32>(instances.size());
    cluster_ref_count_ = static_cast<u32>(cluster_refs.size());
    
    return true;
}


bool RenderSceneSnapshot::UploadInstanceData(const InstanceData* data, u32 count) {
    if (!data || count == 0) {
        return true;
    }
    
    const u64 data_size = count * sizeof(InstanceData);
    
    void* mapped = device_->MapBuffer(instance_staging_buffer_, 0, data_size);
    if (!mapped) {
        return false;
    }
    
    std::memcpy(mapped, data, data_size);
    device_->UnmapBuffer(instance_staging_buffer_);
    
    return true;
}

bool RenderSceneSnapshot::UploadClusterRefs(const ClusterRef* refs, u32 count) {
    if (!refs || count == 0) {
        return true;
    }
    
    const u64 data_size = count * sizeof(ClusterRef);
    
    void* mapped = device_->MapBuffer(cluster_ref_staging_buffer_, 0, data_size);
    if (!mapped) {
        return false;
    }
    
    std::memcpy(mapped, refs, data_size);
    device_->UnmapBuffer(cluster_ref_staging_buffer_);
    
    return true;
}

bool RenderSceneSnapshot::ExtractSceneData(const RenderScene& scene,
                                          utl::vector<InstanceData>& out_instances,
                                          utl::vector<ClusterRef>& out_cluster_refs) {
    const utl::vector<RenderProxy>& proxies = scene.GetProxies();
    
    out_instances.clear();
    out_cluster_refs.clear();
    
    out_instances.reserve(proxies.size());
    
    auto& resource_manager = nanite::NaniteResourceManager::Get();
    
    for (const RenderProxy& proxy : proxies) {
        const cluster::component_cache* cluster_cache = 
            cluster::get(proxy.meshId);
        
        if (!cluster_cache || !cluster_cache->exists) {
            continue;
        }
        
        InstanceData instance{};
        instance.world_matrix = proxy.transform;
        instance.inverse_world_matrix = rhi::math::Inverse(proxy.transform);
        
        instance.geometry_id = cluster_cache->geometry_content_id;
        instance.material_id = proxy.materialId;
        instance.cluster_start = static_cast<u32>(out_cluster_refs.size());
        
        nanite::NaniteRuntimeResource* resource = 
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);
        
        if (resource) {
            instance.cluster_count = resource->cluster_data.cluster_count;
            
            for (u32 i = 0; i < instance.cluster_count; ++i) {
                ClusterRef ref{};
                ref.geometry_id = instance.geometry_id;
                ref.cluster_index = i;
                out_cluster_refs.push_back(ref);
            }
        } else {
            instance.cluster_count = 0;
        }
        
        out_instances.push_back(instance);
    }
    
    return true;
}

bool RenderSceneSnapshot::UpdateInstances(const RenderScene& scene,
                                         const utl::vector<game_entity::entity_id>& dirty_entities,
                                         utl::vector<InstanceData>& instances,
                                         utl::vector<ClusterRef>& cluster_refs) {
    instances.clear();
    instances.reserve(dirty_entities.size());
    
    auto& resource_manager = nanite::NaniteResourceManager::Get();
    
    const utl::vector<RenderProxy>& proxies = scene.GetProxies();
    
    for (game_entity::entity_id entity_id : dirty_entities) {
        auto it = std::find_if(proxies.begin(), proxies.end(),
                              [entity_id](const RenderProxy& p) {
                                  return p.entityId == (id::id_type)entity_id;
                              });
        
        if (it == proxies.end()) {
            continue;
        }
        
        const RenderProxy& proxy = *it;
        
        const cluster::component_cache* cluster_cache = 
            cluster::get(proxy.meshId);
        
        if (!cluster_cache || !cluster_cache->exists) {
            continue;
        }
        
        InstanceData instance{};
        instance.world_matrix = proxy.transform;
        instance.inverse_world_matrix = rhi::math::Inverse(proxy.transform);
        
        instance.geometry_id = cluster_cache->geometry_content_id;
        instance.material_id = proxy.materialId;
        instance.cluster_start = static_cast<u32>(cluster_refs.size());
        
        nanite::NaniteRuntimeResource* resource = 
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);
        
        if (resource) {
            instance.cluster_count = resource->cluster_data.cluster_count;
            
            for (u32 i = 0; i < instance.cluster_count; ++i) {
                ClusterRef ref{};
                ref.geometry_id = instance.geometry_id;
                ref.cluster_index = i;
                cluster_refs.push_back(ref);
            }
        } else {
            instance.cluster_count = 0;
        }
        
        instances.push_back(instance);
    }
    
    return true;
}

} // namespace primal::graphics
