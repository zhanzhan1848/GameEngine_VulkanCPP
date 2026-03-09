#include "NaniteResourceManager.h"
#include "../RHI/Core/RHIDevice.h"
#include <cassert>

namespace primal::graphics::nanite {

NaniteRuntimeResource::NaniteRuntimeResource(id::id_type geo_id)
    : geometry_id(geo_id)
    , ref_count(0)
{
    cluster_data.bounds_buffer = rhi::handles::INVALID_RESOURCE;
    cluster_data.meshlet_buffer = rhi::handles::INVALID_RESOURCE;
    cluster_data.sdf_texture = rhi::handles::INVALID_RESOURCE;
    cluster_data.cluster_count = 0;
    cluster_data.meshlet_count = 0;

    streaming_data.residency_buffer = rhi::handles::INVALID_RESOURCE;
    streaming_data.request_buffer = rhi::handles::INVALID_RESOURCE;
    streaming_data.last_access_frame = 0;
    streaming_data.is_resident = false;
}

NaniteRuntimeResource::~NaniteRuntimeResource() {
    assert(cluster_data.bounds_buffer == rhi::handles::INVALID_RESOURCE);
    assert(cluster_data.meshlet_buffer == rhi::handles::INVALID_RESOURCE);
    assert(cluster_data.sdf_texture == rhi::handles::INVALID_RESOURCE);
    assert(streaming_data.residency_buffer == rhi::handles::INVALID_RESOURCE);
    assert(streaming_data.request_buffer == rhi::handles::INVALID_RESOURCE);
}

bool NaniteResourceManager::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    device_ = device;
    return true;
}

void NaniteResourceManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : resources_) {
        if (pair.second) {
            NaniteRuntimeResource* res = pair.second.get();
            if (res->cluster_data.bounds_buffer != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(res->cluster_data.bounds_buffer);
            }
            if (res->cluster_data.meshlet_buffer != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(res->cluster_data.meshlet_buffer);
            }
            if (res->cluster_data.sdf_texture != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyTexture(res->cluster_data.sdf_texture);
            }
            if (res->streaming_data.residency_buffer != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(res->streaming_data.residency_buffer);
            }
            if (res->streaming_data.request_buffer != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(res->streaming_data.request_buffer);
            }
        }
    }

    resources_.clear();
    ref_counts_.clear();
    destroyed_resources_.clear();
    page_pool_.clear();
    device_ = nullptr;
}

NaniteRuntimeResource* NaniteResourceManager::GetOrCreateResource(id::id_type geometry_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (destroyed_resources_.count(geometry_id) > 0) {
        return nullptr;
    }

    auto it = resources_.find(geometry_id);
    if (it != resources_.end()) return it->second.get();

    auto resource = NaniteRuntimeResource::Create(geometry_id);
    if (!resource) return nullptr;

    resource->cluster_data.cluster_count = 1;
    resource->cluster_data.meshlet_count = 1;
    
    UploadClusterData(resource.get());

    auto* ptr = resource.get();
    resource->ref_count.store(1, std::memory_order_relaxed);
    resources_[geometry_id] = std::move(resource);
    ref_counts_[geometry_id] = 1;

    return ptr;
}

void NaniteResourceManager::DestroyResource(NaniteRuntimeResource* resource) {
    if (!resource || !device_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (resource->cluster_data.bounds_buffer != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(resource->cluster_data.bounds_buffer);
        resource->cluster_data.bounds_buffer = rhi::handles::INVALID_RESOURCE;
    }
    
    if (resource->cluster_data.meshlet_buffer != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(resource->cluster_data.meshlet_buffer);
        resource->cluster_data.meshlet_buffer = rhi::handles::INVALID_RESOURCE;
    }
    
    if (resource->cluster_data.sdf_texture != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(resource->cluster_data.sdf_texture);
        resource->cluster_data.sdf_texture = rhi::handles::INVALID_RESOURCE;
    }
    
    if (resource->streaming_data.residency_buffer != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(resource->streaming_data.residency_buffer);
        resource->streaming_data.residency_buffer = rhi::handles::INVALID_RESOURCE;
    }
    
    if (resource->streaming_data.request_buffer != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(resource->streaming_data.request_buffer);
        resource->streaming_data.request_buffer = rhi::handles::INVALID_RESOURCE;
    }
    
    resources_.erase(resource->geometry_id);
    ref_counts_.erase(resource->geometry_id);
}

void NaniteResourceManager::AddGeometryRef(id::id_type geometry_id) {
    if (geometry_id == id::invalid_id) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = resources_.find(geometry_id);
    if (it == resources_.end()) {
        return;
    }
    
    it->second->AddRef();
    ref_counts_[geometry_id]++;
}


void NaniteResourceManager::ReleaseGeometryRef(id::id_type geometry_id) {
    if (geometry_id == id::invalid_id) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = resources_.find(geometry_id);
    if (it == resources_.end()) {
        return;
    }
    
        if (ref_counts_[geometry_id] > 0) {
            ref_counts_[geometry_id]--;
            
            it->second->ref_count.fetch_sub(1, std::memory_order_relaxed);
            
            if (ref_counts_[geometry_id] == 0) {
                DestroyResource(it->second.get());
                resources_.erase(it);
                ref_counts_.erase(geometry_id);
                destroyed_resources_.insert(geometry_id);
            }
        }
}



void NaniteResourceManager::RequestClusterResidency(id::id_type geometry_id, u32 cluster_index) {
    std::lock_guard<std::mutex> lock(mutex_);
}

void NaniteResourceManager::UpdateResidency() {
    std::lock_guard<std::mutex> lock(mutex_);
}

void NaniteResourceManager::OnFrameEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
}

void NaniteResourceManager::UploadClusterData(NaniteRuntimeResource* resource) {
    if (!resource || !device_) return;
    if (resource->cluster_data.cluster_count == 0) return;
    
    rhi::BufferDesc bounds_desc{};
    bounds_desc.type = rhi::BufferType::Structured;
    bounds_desc.usage = rhi::GPUMemoryUsage::Static;
    bounds_desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    bounds_desc.size = sizeof(ClusterBounds) * resource->cluster_data.cluster_count;
    bounds_desc.name = "NaniteClusterBounds";
    
    resource->cluster_data.bounds_buffer = device_->CreateBuffer(bounds_desc);
    if (resource->cluster_data.bounds_buffer == rhi::handles::INVALID_RESOURCE) {
        return;
    }
    
    if (resource->cluster_data.meshlet_count > 1) {
        rhi::BufferDesc meshlet_desc{};
        meshlet_desc.type = rhi::BufferType::Structured;
        meshlet_desc.usage = rhi::GPUMemoryUsage::Static;
        meshlet_desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        meshlet_desc.size = sizeof(MeshletData) * resource->cluster_data.meshlet_count;
        meshlet_desc.name = "NaniteMeshlets";
        
        resource->cluster_data.meshlet_buffer = device_->CreateBuffer(meshlet_desc);
        if (resource->cluster_data.meshlet_buffer == rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(resource->cluster_data.bounds_buffer);
            resource->cluster_data.bounds_buffer = rhi::handles::INVALID_RESOURCE;
            return;
        }
    }
    
    if (resource->cluster_data.cluster_count > 1) {
        rhi::TextureDesc sdf_desc{};
        sdf_desc.size = { 64, 64, 1 };
        sdf_desc.format = rhi::DataFormat::R8_UNorm;
        sdf_desc.type = rhi::TextureType::Texture2DArray;
        sdf_desc.usage = rhi::TextureUsage::ShaderResource;
        sdf_desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        sdf_desc.mipLevels = 1;
        sdf_desc.arraySize = resource->cluster_data.cluster_count;
        sdf_desc.name = "NaniteClusterSDF";
        
        resource->cluster_data.sdf_texture = device_->CreateTexture(sdf_desc);
        if (resource->cluster_data.sdf_texture == rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(resource->cluster_data.meshlet_buffer);
            device_->DestroyBuffer(resource->cluster_data.bounds_buffer);
            resource->cluster_data.meshlet_buffer = rhi::handles::INVALID_RESOURCE;
            resource->cluster_data.bounds_buffer = rhi::handles::INVALID_RESOURCE;
            return;
        }
    }
    
    rhi::BufferDesc residency_desc{};
    residency_desc.type = rhi::BufferType::Structured;
    residency_desc.usage = rhi::GPUMemoryUsage::Static;
    residency_desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    residency_desc.size = resource->cluster_data.cluster_count * sizeof(u32);
    residency_desc.name = "NaniteResidency";
    
    resource->streaming_data.residency_buffer = device_->CreateBuffer(residency_desc);
    if (resource->streaming_data.residency_buffer == rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(resource->cluster_data.sdf_texture);
        device_->DestroyBuffer(resource->cluster_data.meshlet_buffer);
        device_->DestroyBuffer(resource->cluster_data.bounds_buffer);
        resource->cluster_data.sdf_texture = rhi::handles::INVALID_RESOURCE;
        resource->cluster_data.meshlet_buffer = rhi::handles::INVALID_RESOURCE;
        resource->cluster_data.bounds_buffer = rhi::handles::INVALID_RESOURCE;
        return;
    }
    
    resource->streaming_data.is_resident = true;
}

void NaniteResourceManager::EvictPages(u64 target_memory) {
}

} // namespace primal::graphics::nanite
