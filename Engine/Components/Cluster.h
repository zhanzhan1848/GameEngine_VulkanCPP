// File: Cluster Component Declaration
// Purpose: Nanite cluster component for GPU-driven virtualized geometry.
// Design: Reference-based resource ownership - entities hold geometry references,
//         not GPU resources. All GPU resource lifecycle is managed by NaniteResourceManager.
//
// Migration Notes (v7.1):
// - Cluster components no longer directly own GPU buffers/textures
// - Resource ownership is managed centrally by NaniteResourceManager
// - Multiple entities with same geometry_content_id share one GPU resource
// - Entity destruction only decrements reference count, doesn't immediately free GPU resources
// - GPU resources are freed when reference count reaches 0

#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"

namespace primal::cluster {

    // Component initialization information
    struct init_info
    {
        // Geometry resource ID (created by ContentTools pipeline)
        // This is a reference only - component does not own GPU resources
        id::id_type geometry_content_id{ id::invalid_id };

        // Optional: LOD bias for this instance (default 0.0)
        f32 lod_bias{ 0.f };

        // Optional: Force specific LOD level (-1 = automatic)
        s32 forced_lod{ -1 };

        // Optional: Visibility flags (bitmask for culling)
        u32 visibility_flags{ 0xFFFFFFFF };
    };

    // Component cache for batch updates
    struct component_cache
    {
        id::id_type id{ id::invalid_id };  // Component identifier
        id::id_type geometry_content_id{ id::invalid_id };

        // Instance-specific parameters
        f32 lod_bias{ 0.f };
        s32 forced_lod{ -1 };
        u32 visibility_flags{ 0xFFFFFFFF };

        // Cluster metadata (read from geometry, not stored per-instance)
        u32 cluster_count{ 0 };

        // State flags
        u32 flags{ 0 }; // bit0: needs rebind, bit1: needs LOD update
        bool exists{ false };
    };

    // Component handle type
    using component = id::id_type;

    // Create a Cluster component for an entity
    // - Calls NaniteResourceManager::AddGeometryRef to increment reference count
    // - Does NOT allocate GPU resources directly
    // - Returns invalid_id on failure
    component create(init_info info, game_entity::entity entity);

    // Remove a Cluster component
    // - Calls NaniteResourceManager::ReleaseGeometryRef to decrement reference count
    // - GPU resources are freed only when reference count reaches 0
    // - Does NOT immediately free GPU resources
    void remove(component c);

    // Get component cache for an existing component
    // Returns nullptr if component doesn't exist
    const component_cache* get(component c);

    // Update geometry binding (rarely needed, geometry is typically set at creation)
    // This will call ReleaseGeometryRef on old geometry and AddGeometryRef on new geometry
    void set_geometry(component c, id::id_type new_geometry_content_id);

    // Update LOD policy for an instance
    void set_lod_policy(component c, f32 lod_bias, s32 forced_lod);

    // Update visibility flags
    void set_visibility_flags(component c, u32 flags);

    // Batch update multiple component caches (for system-level updates)
    void update(const component_cache* caches, u32 count);

} // namespace primal::cluster
