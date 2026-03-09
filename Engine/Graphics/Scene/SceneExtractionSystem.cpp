#include "SceneExtractionSystem.h"
#include "../RenderScene.h"
#include "../RenderProxy.h"
#include <chrono>

namespace primal::graphics {

bool SceneExtractionSystem::Initialize(rhi::RHIDeviceBase* device,
                                        u32 initial_instance_capacity,
                                        u32 initial_cluster_capacity) {
    if (initialized_) {
        return true;
    }
    
    if (!snapshot_.Initialize(device, initial_instance_capacity, initial_cluster_capacity)) {
        return false;
    }
    
    entity_ids_cache_.reserve(initial_instance_capacity);
    transform_flags_cache_.reserve(initial_instance_capacity);
    
    stats_.instance_capacity = initial_instance_capacity;
    stats_.cluster_ref_capacity = initial_cluster_capacity;
    
    initialized_ = true;
    return true;
}

void SceneExtractionSystem::Shutdown() {
    if (!initialized_) {
        return;
    }
    
    snapshot_.Shutdown();
    dirty_entities_.clear();
    entity_ids_cache_.clear();
    transform_flags_cache_.clear();
    
    stats_ = SceneExtractionStats{};
    initialized_ = false;
}

bool SceneExtractionSystem::ExtractScene(const RenderScene& scene, bool force_full_rebuild) {
    if (!initialized_) {
        return false;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    stats_.total_entities = static_cast<u32>(scene.GetProxies().size());
    stats_.dirty_entities = static_cast<u32>(dirty_entities_.size());
    stats_.needs_full_rebuild = snapshot_.NeedsFullRebuild();
    
    bool result = false;
    
    if (force_full_rebuild) {
        snapshot_.ClearFullRebuildFlag();
        result = snapshot_.Rebind(scene);
        stats_.last_update_was_partial = false;
    }
    else if (snapshot_.NeedsFullRebuild() || dirty_entities_.empty()) {
        snapshot_.ClearFullRebuildFlag();
        result = snapshot_.Rebind(scene);
        stats_.last_update_was_partial = false;
    }
    else {
        result = snapshot_.PartialUpdate(scene, dirty_entities_);
        dirty_entities_.clear();
        stats_.last_update_was_partial = true;
    }
    
    if (result) {
        stats_.extracted_instances = snapshot_.GetInstanceCount();
        stats_.extracted_cluster_refs = snapshot_.GetClusterRefCount();
        
        if (stats_.instance_capacity != snapshot_.GetInstanceCount()) {
            stats_.instance_capacity = snapshot_.GetInstanceCount();
            stats_.buffer_resizes++;
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    stats_.extraction_time_ms = static_cast<f32>(duration.count()) / 1000.0f;
    
    return result;
}

bool SceneExtractionSystem::UpdateDirtyEntities(const RenderScene& scene,
                                                const utl::vector<game_entity::entity_id>& dirty_entities) {
    if (!initialized_) {
        return false;
    }
    
    if (dirty_entities.empty()) {
        return true;
    }
    
    return snapshot_.PartialUpdate(scene, dirty_entities);
}

void SceneExtractionSystem::SetDirtyEntities(const utl::vector<game_entity::entity_id>& entities) {
    dirty_entities_ = entities;
}

void SceneExtractionSystem::ClearDirtyEntities() {
    dirty_entities_.clear();
}

void SceneExtractionSystem::QueryDirtyTransforms(const RenderScene& scene) {
    if (!initialized_) {
        return;
    }
    
    const utl::vector<RenderProxy>& proxies = scene.GetProxies();
    const u32 count = static_cast<u32>(proxies.size());
    
    entity_ids_cache_.clear();
    entity_ids_cache_.reserve(count);
    
    for (const RenderProxy& proxy : proxies) {
        entity_ids_cache_.push_back(game_entity::entity_id{ proxy.entityId });
    }
    
    transform_flags_cache_.clear();
    transform_flags_cache_.resize(count);
    
    transform::get_updated_components_flags(
        entity_ids_cache_.data(),
        count,
        transform_flags_cache_.data()
    );
    
    dirty_entities_.clear();
    dirty_entities_.reserve(count);
    
    for (u32 i = 0; i < count; ++i) {
        if (transform_flags_cache_[i] != 0) {
            dirty_entities_.push_back(entity_ids_cache_[i]);
        }
    }
    
    stats_.dirty_entities = static_cast<u32>(dirty_entities_.size());
}

} // namespace primal::graphics
