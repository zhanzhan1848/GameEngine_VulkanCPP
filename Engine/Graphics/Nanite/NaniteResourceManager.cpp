#include "NaniteResourceManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHIGpuMesh.h"
#include "../../Content/ContentToEngine.h"
#include <iostream>
#include <cassert>

namespace primal::graphics::nanite {

NaniteRuntimeResource::NaniteRuntimeResource(id::id_type geo_id)
    : geometry_id(geo_id)
    , ref_count(0)
    , gpu_mesh(nullptr)
{
    cluster_data.cluster_count = 0;
    cluster_data.meshlet_count = 0;

    streaming_data.residency_buffer = rhi::handles::INVALID_RESOURCE;
    streaming_data.request_buffer = rhi::handles::INVALID_RESOURCE;
    streaming_data.last_access_frame = 0;
    streaming_data.is_resident = false;
}

NaniteRuntimeResource::~NaniteRuntimeResource() {
    // Do not delete gpu_mesh - it's owned by rhi_gpu_meshes in ContentToEngine.cpp
    // This is just a non-owning pointer to avoid circular dependencies
    gpu_mesh = nullptr;

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
        // gpu_mesh is owned by ContentToEngine, do NOT delete it here
        if (pair.second) {
            pair.second->gpu_mesh = nullptr;
        }

        if (pair.second && pair.second->streaming_data.residency_buffer != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(pair.second->streaming_data.residency_buffer);
            pair.second->streaming_data.residency_buffer = rhi::handles::INVALID_RESOURCE;
        }
        if (pair.second && pair.second->streaming_data.request_buffer != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(pair.second->streaming_data.request_buffer);
            pair.second->streaming_data.request_buffer = rhi::handles::INVALID_RESOURCE;
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

    // std::cout << "[NaniteResourceManager] GetOrCreateResource called for geometry_id: " << geometry_id << std::endl;

    if (!device_) {
        std::cerr << "[NaniteResourceManager] ERROR: ResourceManager not initialized! device_ is null" << std::endl;
        return nullptr;
    }

    if (destroyed_resources_.count(geometry_id) > 0) {
        // std::cout << "[NaniteResourceManager]   Resource was previously destroyed, returning nullptr" << std::endl;
        return nullptr;
    }

    auto it = resources_.find(geometry_id);
    if (it != resources_.end()) {
        // std::cout << "[NaniteResourceManager]   Resource already exists, returning cached instance" << std::endl;
        return it->second.get();
    }

    // std::cout << "[NaniteResourceManager]   Creating new resource..." << std::endl;

    auto resource = NaniteRuntimeResource::Create(geometry_id);
    if (!resource) {
        std::cerr << "[NaniteResourceManager]   Failed to create resource object!" << std::endl;
        return nullptr;
    }

    graphics::rhi::RHIMeshAsset meshAsset;
    bool hasMeshletData = primal::content::get_rhi_mesh_asset(geometry_id, meshAsset);

    // std::cout << "[NaniteResourceManager]   get_rhi_mesh_asset returned: " << hasMeshletData << std::endl;

    if (hasMeshletData) {
        // std::cout << "[NaniteResourceManager]   MeshAsset meshlets count: " << meshAsset.meshlets.size() << std::endl;
        // std::cout << "[NaniteResourceManager]   MeshAsset meshlet_vertices count: " << meshAsset.meshlet_vertices.size() << std::endl;
        // std::cout << "[NaniteResourceManager]   MeshAsset meshlet_triangles count: " << meshAsset.meshlet_triangles.size() << std::endl;
    } else {
        // std::cout << "[NaniteResourceManager]   get_rhi_mesh_asset returned false!" << std::endl;
    }

    if (hasMeshletData && !meshAsset.meshlets.empty()) {
        resource->cluster_data.cluster_count = static_cast<u32>(meshAsset.meshlets.size());
        resource->cluster_data.meshlet_count = static_cast<u32>(meshAsset.meshlets.size());

        // std::cout << "[NaniteResourceManager]   Created resource with " << resource->cluster_data.cluster_count
        //           << " meshlets for geometry_id: " << geometry_id << std::endl;
    } else {
        resource->cluster_data.cluster_count = 1;
        resource->cluster_data.meshlet_count = 1;

        // std::cout << "[NaniteResourceManager]   No meshlet data found for geometry_id: " << geometry_id
        //           << ", using single cluster fallback" << std::endl;
    }

    resource->gpu_mesh = primal::content::get_rhi_gpu_mesh(geometry_id);
    
    if (resource->gpu_mesh) {
        // std::cout << "[NaniteResourceManager]   RHIGpuMesh obtained successfully" << std::endl;
        // std::cout << "[NaniteResourceManager]   GPU mesh has "
        //           << resource->gpu_mesh->GetMeshletCount() << " meshlets" << std::endl;
        // std::cout << "[NaniteResourceManager]   GPU mesh has "
        //           << resource->gpu_mesh->GetVertexCount() << " vertices" << std::endl;
        // std::cout << "[NaniteResourceManager]   GPU mesh has "
        //           << resource->gpu_mesh->GetIndexCount() << " indices" << std::endl;
    } else {
        // std::cout << "[NaniteResourceManager]   WARNING: get_rhi_gpu_mesh returned null!" << std::endl;
    }

    auto* ptr = resource.get();
    resource->ref_count.store(1, std::memory_order_relaxed);
    resources_[geometry_id] = std::move(resource);
    ref_counts_[geometry_id] = 1;

    // std::cout << "[NaniteResourceManager]   Resource created successfully with "
    //           << ptr->cluster_data.cluster_count << " clusters" << std::endl;

    return ptr;
}

void NaniteResourceManager::DestroyResource(NaniteRuntimeResource* resource) {
    if (!resource || !device_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (resource->gpu_mesh) {
        // delete resource->gpu_mesh;
        resource->gpu_mesh = nullptr;
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

void NaniteResourceManager::EvictPages(u64 target_memory) {
}

} // namespace primal::graphics::nanite
