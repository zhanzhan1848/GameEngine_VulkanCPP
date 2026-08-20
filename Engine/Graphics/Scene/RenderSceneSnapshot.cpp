#include "RenderSceneSnapshot.h"
#include "../RenderScene.h"
#include "../RenderProxy.h"
#include "../../Components/Cluster.h"
#include "../../Graphics/Nanite/NaniteResourceManager.h"
#include "../../Graphics/RHI/Core/RHIDevice.h"
#include "../../Graphics/RHI/Core/RHICommand.h"
#include "../../Graphics/RHI/Core/RHIGpuMesh.h"
#include "../../Graphics/RHI/Core/RHIMeshAsset.h"
#include "../../Content/ContentToEngine.h"
#ifdef __APPLE__
#if defined(__APPLE__)
#include <simd/simd.h>
#endif
#endif
#include <iostream>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace primal::graphics {

namespace {

// Cache of local-space AABB (center + extent) keyed by geometry_id.
// Position-buffer-derived bounds only change when the mesh asset reloads,
// which doesn't happen at runtime for Sponza. Avoids recomputing bounds
// per-instance per-frame for 393+ meshes.
struct LocalBounds {
    math::v3 center;
    math::v3 extent;
    bool valid;
};

std::mutex& bounds_cache_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<id::id_type, LocalBounds>& bounds_cache() {
    static std::unordered_map<id::id_type, LocalBounds> c;
    return c;
}

// Computes local-space AABB for a geometry. Uses cached result when available.
// gpu_mesh may be null on WASM (Sponza) — falls back to position_buffer.
LocalBounds ComputeLocalBounds(nanite::NaniteRuntimeResource* resource,
                                id::id_type geometry_id) {
    {
        std::lock_guard<std::mutex> lock(bounds_cache_mutex());
        auto it = bounds_cache().find(geometry_id);
        if (it != bounds_cache().end()) {
            return it->second;
        }
    }

    LocalBounds result{};

    const f32* boundsMin = nullptr;
    const f32* boundsMax = nullptr;
    if (resource && resource->gpu_mesh) {
        boundsMin = resource->gpu_mesh->GetBoundsMin();
        boundsMax = resource->gpu_mesh->GetBoundsMax();
    }

    auto is_finite_bounds = [](const f32* p) -> bool {
        if (!p) return false;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(p[i])) return false;
            if (std::abs(p[i]) > 1.0e6f) return false;
        }
        return true;
    };

    // Reject zero/degenerate bounds: gpu_mesh->GetBoundsMin/Max returns
    // asset.sdf.bounds_min/max which are zero-initialized for meshes that
    // weren't SDF-baked (common for test scenes). Trusting zeros makes
    // bounds_radius=0, which causes stage1 to cull every instance whenever
    // the world origin is even slightly behind the camera plane.
    auto has_meaningful_extent = [](const f32* mn, const f32* mx) -> bool {
        for (int i = 0; i < 3; ++i) {
            if (mx[i] - mn[i] < 1e-6f) return false;
        }
        return true;
    };

    if (is_finite_bounds(boundsMin) && is_finite_bounds(boundsMax)
        && has_meaningful_extent(boundsMin, boundsMax)) {
        result.center = {
            (boundsMin[0] + boundsMax[0]) * 0.5f,
            (boundsMin[1] + boundsMax[1]) * 0.5f,
            (boundsMin[2] + boundsMax[2]) * 0.5f
        };
        result.extent = {
            (boundsMax[0] - boundsMin[0]) * 0.5f,
            (boundsMax[1] - boundsMin[1]) * 0.5f,
            (boundsMax[2] - boundsMin[2]) * 0.5f
        };
        result.valid = true;
    } else {
        // Recompute from raw vertex positions in the mesh asset.
        // Runs when gpu_mesh is null (WASM Sponza) OR when gpu_mesh
        // exists but its bounds are garbage (SDF-baked without bounds
        // metadata).
        graphics::rhi::RHIMeshAsset asset;
        if (primal::content::get_rhi_mesh_asset(geometry_id, asset)
            && asset.num_vertices > 0
            && asset.position_buffer.size() >= asset.num_vertices * sizeof(f32) * 3) {
            const f32* pos = reinterpret_cast<const f32*>(asset.position_buffer.data());
            f32 mn[3] = { std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max() };
            f32 mx[3] = { std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest() };
            for (u32 vi = 0; vi < asset.num_vertices; ++vi) {
                for (int k = 0; k < 3; ++k) {
                    f32 v = pos[vi * 3 + k];
                    if (v < mn[k]) mn[k] = v;
                    if (v > mx[k]) mx[k] = v;
                }
            }
            result.center = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f };
            result.extent = { (mx[0] - mn[0]) * 0.5f, (mx[1] - mn[1]) * 0.5f, (mx[2] - mn[2]) * 0.5f };
            result.valid = true;
        } else {
            result.center = { 0, 0, 0 };
            result.extent = { 1, 1, 1 };
            result.valid = false;
            std::cerr << "[BOUNDS_FALLBACK] geometry_id=" << geometry_id
                      << " no gpu_mesh AND no position_buffer — using (0,0,0)/1.0" << std::endl;
        }
    }

    std::lock_guard<std::mutex> lock(bounds_cache_mutex());
    bounds_cache()[geometry_id] = result;
    return result;
}

} // anonymous namespace

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

    // std::cout << "[RenderSceneSnapshot] Creating instance buffer: capacity=" << instance_capacity
    //           << " size=" << instance_buffer_size << " bytes" << std::endl;

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

    // std::cout << "[RenderSceneSnapshot] ExtractSceneData: " << proxies.size() << " proxies" << std::endl;

    out_instances.clear();
    out_cluster_refs.clear();

    out_instances.reserve(proxies.size());

    auto& resource_manager = nanite::NaniteResourceManager::Get();

    for (const RenderProxy& proxy : proxies) {
        const cluster::component_cache* cluster_cache =
            cluster::get(proxy.meshId);

        // std::cout << "[RenderSceneSnapshot]   Proxy[" << proxyIndex << "] meshId=" << proxy.meshId
        //           << ", entity_id=" << proxy.entityId << std::endl;

        if (!cluster_cache || !cluster_cache->exists) {
            // std::cout << "[RenderSceneSnapshot]     -> No cluster component or not exists" << std::endl;
            continue;
        }

        // std::cout << "[RenderSceneSnapshot]     -> Cluster found, geometry_content_id=" 
        //           << cluster_cache->geometry_content_id 
        //           << " (valid=" << (cluster_cache->geometry_content_id != id::invalid_id) << ")" << std::endl;

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

            // CRITICAL: Initialize cluster_map_base to match cluster_start for compact layout
            // This assumes we are doing a full rebuild where cluster_refs is contiguous
            instance.cluster_map_base = instance.cluster_start;

            // Local-space AABB cached by geometry_id (computed once).
            LocalBounds lb = ComputeLocalBounds(resource, instance.geometry_id);
            const math::v3& localCenter = lb.center;
            const math::v3& localExtent = lb.extent;

            // Transform to world space
            math::v4 worldCenter4 = proxy.transform * math::v4{localCenter.x, localCenter.y, localCenter.z, 1.0f};
            instance.bounds_center = {worldCenter4.x, worldCenter4.y, worldCenter4.z};

            // Calculate radius as max extent scaled by transform
            f32 maxLocalExtent = std::max({localExtent.x, localExtent.y, localExtent.z});

            const math::m4x4& transform = proxy.transform;
            f32 maxScale = std::max({
                std::abs(transform.columns[0].x), std::abs(transform.columns[0].y), std::abs(transform.columns[0].z),
                std::abs(transform.columns[1].x), std::abs(transform.columns[1].y), std::abs(transform.columns[1].z),
                std::abs(transform.columns[2].x), std::abs(transform.columns[2].y), std::abs(transform.columns[2].z)
            });
            instance.bounds_radius = maxLocalExtent * maxScale;

            instance.padding = 0;

            // std::cout << "[RenderSceneSnapshot]   geometry_id " << instance.geometry_id
            //           << " -> " << instance.cluster_count << " clusters" << std::endl;

            for (u32 i = 0; i < resource->cluster_data.cluster_count; ++i) {
                ClusterRef ref{};
                ref.geometry_id = instance.geometry_id;
                ref.cluster_index = instance.cluster_start + i;

                // Set meshlet_id: simplified 1:1 mapping with cluster_index
                // TODO: Implement proper meshlet_id mapping when needed
                if (resource->gpu_mesh && i < resource->gpu_mesh->GetMeshletCount()) {
                    ref.meshlet_id = i;  // Direct cluster->meshlet mapping
                } else {
                    ref.meshlet_id = 0;  // Fallback to first meshlet
                }

                out_cluster_refs.push_back(ref);
            }
        } else {
            instance.cluster_count = 0;
            instance.bounds_center = {0, 0, 0};
            instance.bounds_radius = 1.0f;
            instance.padding = 0;
        }

        out_instances.push_back(instance);
    }

    instance_data_cpu_ = out_instances;

    // std::cout << "[RenderSceneSnapshot] ExtractSceneData complete:" << std::endl;
    // std::cout << "  Valid cluster resources: " << valid_cluster_count << std::endl;
    // std::cout << "  Missing resources: " << missing_resource_count << std::endl;
    // std::cout << "  Total instances: " << out_instances.size() << std::endl;
    // std::cout << "  Total cluster refs: " << out_cluster_refs.size() << std::endl;

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

    // CRITICAL: Track global cluster offset for cluster_map indexing
    u32 global_cluster_offset = 0;

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
        instance.cluster_map_base = global_cluster_offset; // CRITICAL: Set cluster_map base index
        instance.padding = 0;


        nanite::NaniteRuntimeResource* resource =
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);

        if (resource) {
            instance.cluster_count = resource->cluster_data.cluster_count;

            // Local-space AABB cached by geometry_id (computed once).
            LocalBounds lb = ComputeLocalBounds(resource, instance.geometry_id);
            const math::v3& localCenter = lb.center;
            const math::v3& localExtent = lb.extent;

            // Transform to world space
            math::v4 worldCenter4 = proxy.transform * math::v4{localCenter.x, localCenter.y, localCenter.z, 1.0f};
            instance.bounds_center = {worldCenter4.x, worldCenter4.y, worldCenter4.z};

            // Calculate radius as max extent scaled by transform
            f32 maxLocalExtent = std::max({localExtent.x, localExtent.y, localExtent.z});

            const math::m4x4& transform = proxy.transform;
            f32 maxScale = std::max({
                std::abs(transform.columns[0].x), std::abs(transform.columns[0].y), std::abs(transform.columns[0].z),
                std::abs(transform.columns[1].x), std::abs(transform.columns[1].y), std::abs(transform.columns[1].z),
                std::abs(transform.columns[2].x), std::abs(transform.columns[2].y), std::abs(transform.columns[2].z)
            });
            instance.bounds_radius = maxLocalExtent * maxScale;

            // DEBUG: Print cluster index assignment
            std::cout << "[RenderSceneSnapshot] Instance " << instances.size()
                      << " geometry_id=" << instance.geometry_id
                      << " cluster_map_base=" << instance.cluster_map_base
                      << " cluster_count=" << instance.cluster_count
                      << " range=[" << instance.cluster_map_base
                      << "-" << (instance.cluster_map_base + instance.cluster_count - 1) << "]" << std::endl;

            for (u32 i = 0; i < instance.cluster_count; ++i) {
                ClusterRef ref{};
                ref.geometry_id = instance.geometry_id;
                ref.cluster_index = i;

                // Set meshlet_id: simplified 1:1 mapping with cluster_index
                // TODO: Implement proper meshlet_id mapping when needed
                const auto& proxy = scene.GetProxies()[instance.geometry_id];
                auto* resource = nanite::NaniteResourceManager::Get().GetOrCreateResource(proxy.meshId);
                if (resource && resource->gpu_mesh && i < resource->gpu_mesh->GetMeshletCount()) {
                    ref.meshlet_id = i;  // Direct cluster->meshlet mapping
                } else {
                    ref.meshlet_id = 0;  // Fallback to first meshlet
                }

                cluster_refs.push_back(ref);
            }
        } else {
            instance.cluster_count = 0;
            instance.bounds_center = {0, 0, 0};
            instance.bounds_radius = 1.0f;
        }

        instances.push_back(instance);

        // CRITICAL: Update global cluster offset for next instance
        global_cluster_offset += instance.cluster_count;
    }
    
    return true;
}

bool RenderSceneSnapshot::UploadToGPUBuffers(rhi::RHICommandBuffer* cmd_buffer) {
    if (!cmd_buffer) {
        std::cerr << "[RenderSceneSnapshot] UploadToGPUBuffers failed: null command buffer" << std::endl;
        return false;
    }

    if (!initialized_) {
        std::cerr << "[RenderSceneSnapshot] UploadToGPUBuffers failed: not initialized" << std::endl;
        return false;
    }

    // Copy instance data from staging buffer to GPU buffer
    if (instance_staging_buffer_ != rhi::handles::INVALID_RESOURCE &&
        instance_buffer_ != rhi::handles::INVALID_RESOURCE &&
        instance_count_ > 0) {

        u64 instance_data_size = instance_count_ * sizeof(InstanceData);
        u64 instance_buffer_capacity = instance_capacity_ * sizeof(InstanceData);

        /*
        std::cout << "[RenderSceneSnapshot] Buffer Check: count=" << instance_count_
                  << " capacity=" << instance_capacity_
                  << " needed=" << instance_data_size
                  << " available=" << instance_buffer_capacity << std::endl;
        */

        if (instance_data_size > instance_buffer_capacity) {
            std::cerr << "[RenderSceneSnapshot] ERROR: Instance buffer too small! "
                      << "Need " << instance_data_size << " bytes but only have "
                      << instance_buffer_capacity << " bytes" << std::endl;
            return false;
        }

        cmd_buffer->CopyBuffer(instance_staging_buffer_, instance_buffer_, 0, 0, instance_data_size);

        // std::cout << "[RenderSceneSnapshot] Uploaded " << instance_count_
        //           << " instances (" << instance_data_size << " bytes) to GPU" << std::endl;
    }

    // Copy cluster ref data from staging buffer to GPU buffer
    if (cluster_ref_staging_buffer_ != rhi::handles::INVALID_RESOURCE &&
        cluster_ref_buffer_ != rhi::handles::INVALID_RESOURCE &&
        cluster_ref_count_ > 0) {

        u64 cluster_ref_size = cluster_ref_count_ * sizeof(ClusterRef);
        cmd_buffer->CopyBuffer(cluster_ref_staging_buffer_, cluster_ref_buffer_, 0, 0, cluster_ref_size);

        // std::cout << "[RenderSceneSnapshot] Uploaded " << cluster_ref_count_
        //           << " cluster refs (" << cluster_ref_size << " bytes) to GPU" << std::endl;
    }

    return true;
}

} // namespace primal::graphics
