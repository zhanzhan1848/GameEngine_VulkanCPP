/**
 * @file ShadowCulling.metal
 * @brief GPU-driven shadow map cluster culling.
 *
 * Culls instances and meshlets against the light frustum for cascaded shadow maps.
 * Reuses the same InstanceData/MeshletData/ClusterMap structs as GlobalSDFVoxelization.metal.
 *
 * Algorithm:
 *   1. Instance bounding sphere vs 6 light frustum planes
 *   2. Per-meshlet bounding sphere vs light frustum (world-space transform)
 *   3. Stream-compact visible clusters via atomic counter → indirect draw args
 *   4. Finalize kernel sorts visible_clusters for deterministic ordering
 *
 * The sort in finalize eliminates non-deterministic ordering from atomic_fetch_add
 * that causes shadow flickering on Apple Silicon TBDR GPUs.
 */

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Struct definitions — MUST match GlobalSDFVoxelization.metal exactly
// ============================================================================

struct InstanceData {
    float4x4 world_matrix;
    float4x4 inverse_world_matrix;
    uint geometry_id;
    uint material_id;
    uint cluster_start;
    uint cluster_count;
    uint cluster_map_base;
    uint _pad0; uint _pad1; uint _pad2;
    packed_float3 bounds_center;
    float bounds_radius;           // offset 172 — matches CPU InstanceData layout
    uint _pad_br[4];               // 16 bytes padding to reach 192 total
};

struct MeshletData {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff;
    float center[3];
    float radius;
    uint _pad;
};

struct ClusterMap {
    uint globalMeshletIndex;
    uint instanceIndex;
    uint materialID;
    uint _pad;
};

// ============================================================================
// Shadow culling uniforms
// ============================================================================

struct ShadowCullUniforms {
    float4   frustum_planes[6];     // 6 light frustum planes (normalized: xyz=normal, w=distance)
    float4x4 light_view_projection; // Light VP matrix (for future use: per-cluster depth bounds)
    uint     num_instances;
    uint     cascade_index;         // 0 or 1
    uint     padding[2];
};

// ============================================================================
// Indirect draw args — matches RHI DrawIndirect command layout
// ============================================================================

struct IndirectDrawArgs {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

// ============================================================================
// Frustum-sphere test
// ============================================================================

static bool sphere_vs_frustum(float3 center, float radius, constant float4 planes[6]) {
    for (int i = 0; i < 6; ++i) {
        float distance = dot(float4(center, 1.0), planes[i]);
        if (distance < -radius) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Main Kernel: Shadow cluster culling
// ============================================================================

kernel void shadow_cluster_culling(
    uint3 tid [[thread_position_in_grid]],

    // Input buffers — shared with GlobalSDFVoxelization
    device const InstanceData* instances    [[buffer(0)]],
    device const MeshletData*  meshlets     [[buffer(1)]],
    device const ClusterMap*   cluster_map  [[buffer(2)]],

    // Uniforms — light frustum + config
    constant ShadowCullUniforms& uniforms   [[buffer(3)]],

    // Output: visible cluster list (compact)
    device atomic_uint*  visible_counter    [[buffer(4)]],
    device uint*         visible_clusters   [[buffer(5)]],

    // Output: indirect draw args (single entry)
    device IndirectDrawArgs* indirect_args  [[buffer(6)]]
)
{
    uint instance_id = tid.x;
    if (instance_id >= uniforms.num_instances) return;

    // Thread-local copy (NOT constant reference — 'device' data exceeds Metal constant limits)
    InstanceData inst = instances[instance_id];

    // ---- Stage 1: Instance bounding sphere vs light frustum ----
    float3 instance_center = float3(inst.bounds_center);
    float instance_radius = inst.bounds_radius;

    if (!sphere_vs_frustum(instance_center, instance_radius, uniforms.frustum_planes)) {
        return;
    }

    // ---- Stage 2: Per-cluster (meshlet) bounding sphere culling ----
    uint cluster_start = inst.cluster_start;
    uint cluster_count = inst.cluster_count;

    for (uint c = 0; c < cluster_count; ++c) {
        uint global_cluster_idx = cluster_start + c;

        // Get meshlet for this cluster
        ClusterMap cmap = cluster_map[global_cluster_idx];
        uint meshlet_idx = cmap.globalMeshletIndex;
        MeshletData meshlet = meshlets[meshlet_idx];

        // Transform meshlet center to world space
        float3 local_center = float3(meshlet.center[0], meshlet.center[1], meshlet.center[2]);
        float4 world_center_4 = inst.world_matrix * float4(local_center, 1.0);
        float3 world_center = world_center_4.xyz;

        // Test against light frustum
        if (!sphere_vs_frustum(world_center, meshlet.radius, uniforms.frustum_planes)) {
            continue;
        }

        // ---- Stage 3: Append to visible list ----
        uint list_idx = atomic_fetch_add_explicit(visible_counter, 1, memory_order_relaxed);
        visible_clusters[list_idx] = global_cluster_idx;
    }

    // ---- Build indirect draw args (only first thread) ----
    if (instance_id == 0) {
        indirect_args[0].vertex_count = 126 * 3;  // Max meshlet triangles * 3 vertices
        indirect_args[0].instance_count = 0;       // Will be set by finalize kernel
        indirect_args[0].first_vertex = 0;
        indirect_args[0].first_instance = 0;
    }
}

// ============================================================================
// Finalize kernel: sort visible clusters for deterministic ordering, then
// copy count to indirect draw args
//
// Single-thread dispatch. Reads the atomic counter, insertion-sorts the
// visible_clusters array, and sets instance_count. This eliminates the
// non-deterministic ordering from atomic_fetch_add.
// ============================================================================

kernel void shadow_finalize_indirect(
    device const InstanceData*   instances         [[buffer(0)]],
    device const MeshletData*    meshlets          [[buffer(1)]],
    device const ClusterMap*     cluster_map       [[buffer(2)]],
    constant ShadowCullUniforms& uniforms          [[buffer(3)]],
    device atomic_uint*          visible_counter   [[buffer(4)]],  // mutable: reset after read
    device uint*                 visible_clusters  [[buffer(5)]],
    device IndirectDrawArgs*     indirect_args     [[buffer(6)]]
)
{
    // Single-thread dispatch
    uint count = atomic_load_explicit(visible_counter, memory_order_relaxed);

    // Insertion sort visible_clusters[0..count-1] for deterministic ordering
    for (uint i = 1; i < count; ++i) {
        uint key = visible_clusters[i];
        int j = (int)(i - 1);
        while (j >= 0 && visible_clusters[j] > key) {
            visible_clusters[j + 1] = visible_clusters[j];
            j--;
        }
        visible_clusters[j + 1] = key;
    }

    indirect_args[0].instance_count = count;

    // Reset counter to 0 for the next cascade's culling dispatch.
    atomic_store_explicit(visible_counter, 0u, memory_order_relaxed);
}
