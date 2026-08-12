#pragma once

#include "RenderSceneSnapshot.h"
#include "../../Utilities/Vector.h"
#include "../../Components/Transform.h"

namespace primal::graphics {

class RenderScene;

struct SceneExtractionStats {
    u32 total_entities{ 0 };
    u32 dirty_entities{ 0 };
    u32 extracted_instances{ 0 };
    u32 extracted_cluster_refs{ 0 };
    u32 instance_capacity{ 0 };
    u32 cluster_ref_capacity{ 0 };
    f32 extraction_time_ms{ 0.0f };
    u32 buffer_resizes{ 0 };
    bool needs_full_rebuild{ false };
    bool last_update_was_partial{ false };
};

class SceneExtractionSystem {
public:
    SceneExtractionSystem() = default;
    ~SceneExtractionSystem() = default;
    
    SceneExtractionSystem(const SceneExtractionSystem&) = delete;
    SceneExtractionSystem& operator=(const SceneExtractionSystem&) = delete;
    
    bool Initialize(rhi::RHIDeviceBase* device,
                   u32 initial_instance_capacity = 1000,
                   u32 initial_cluster_capacity = 10000);
    
    void Shutdown();
    
    bool ExtractScene(const RenderScene& scene, bool force_full_rebuild = false);
    
    bool UpdateDirtyEntities(const RenderScene& scene,
                            const utl::vector<game_entity::entity_id>& dirty_entities);
    
    const RenderSceneSnapshot& GetSnapshot() const { return snapshot_; }
    RenderSceneSnapshot& GetSnapshot() { return snapshot_; }
    
    u32 GetInstanceCount() const { return snapshot_.GetInstanceCount(); }
    u32 GetClusterRefCount() const { return snapshot_.GetClusterRefCount(); }
    bool NeedsFullRebuild() const { return snapshot_.NeedsFullRebuild(); }
    
    void SetDirtyEntities(const utl::vector<game_entity::entity_id>& entities);
    void ClearDirtyEntities();
    
    void QueryDirtyTransforms(const RenderScene& scene);
    
    const SceneExtractionStats& GetStats() const { return stats_; }
    SceneExtractionStats& GetStats() { return stats_; }

private:
    RenderSceneSnapshot snapshot_;
    utl::vector<game_entity::entity_id> dirty_entities_;
    utl::vector<game_entity::entity_id> entity_ids_cache_;
    utl::vector<u8> transform_flags_cache_;
    SceneExtractionStats stats_;
    bool initialized_{ false };
};

} // namespace primal::graphics
