/**
 * @file ShadowDepth.metal
 * @brief Depth-only vertex pulling shader for shadow map rasterization.
 *
 * This shader renders visible clusters (produced by ShadowCulling.metal)
 * into a shadow depth map using vertex pulling. It reads the visible_cluster
 * list, looks up geometry data, and outputs transformed positions using the
 * light's view-projection matrix.
 *
 * Called via DrawIndirect with:
 *   - vertex_count = 126 * 3 (max triangles per cluster)
 *   - instance_count = visible_cluster_count
 *
 * No fragment shader needed (depth-only pass).
 */

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Struct definitions matching GlobalSDFVoxelization.metal
// ============================================================================

struct InstanceData {
    float4x4 world_matrix;
    float4x4 inverse_world_matrix;
    uint geometry_id;
    uint material_id;
    uint cluster_start;
    uint cluster_count;
    uint cluster_map_base;
    uint _pad0;
    uint _pad1;
    uint _pad2;
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
// Uniforms
// ============================================================================

struct ShadowDepthUniforms {
    float4x4 light_view_projection;
    uint visible_cluster_count;
    uint padding[3];
};

// ============================================================================
// Vertex Shader
// ============================================================================

struct ShadowDepthVertexOut {
    float4 position [[position]];
};

vertex ShadowDepthVertexOut shadow_depth_vs(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],

    constant ShadowDepthUniforms& uniforms [[buffer(0)]],
    device const uint* visible_clusters [[buffer(1)]],
    device const ClusterMap* cluster_map [[buffer(2)]],
    device const InstanceData* instances [[buffer(3)]],
    device const MeshletData* meshlets [[buffer(4)]],
    device const uint* meshlet_vertex_indices [[buffer(5)]],
    device const uchar* meshlet_triangle_indices [[buffer(6)]],
    device const packed_float3* vertex_positions [[buffer(7)]]
) {
    ShadowDepthVertexOut out;

    // Decode triangle from vertexID
    uint triangle_index = vertexID / 3;
    uint vertex_in_triangle = vertexID % 3;

    // instanceID is the index into visible_clusters[].
    // DrawIndirect already limits instance_count to the actual visible count,
    // so instanceID is always in bounds — no bounds check needed.
    uint global_cluster_idx = visible_clusters[instanceID];

    // Thread-local copy (NOT 'constant' reference to 'device' data)
    ClusterMap cmap = cluster_map[global_cluster_idx];
    uint meshlet_idx = cmap.globalMeshletIndex;
    uint inst_idx = cmap.instanceIndex;

    // Thread-local copy of instance data
    InstanceData inst = instances[inst_idx];

    // Thread-local copy of meshlet data
    MeshletData meshlet = meshlets[meshlet_idx];

    // Check triangle index bounds
    if (triangle_index >= meshlet.triangle_count) {
        // Out of bounds - output position behind camera
        out.position = float4(0.0, 0.0, 2.0, 1.0);
        return out;
    }

    // Read meshlet triangle indices (uchar) → get 3 local vertex indices
    uint tri_offset = meshlet.triangle_offset + triangle_index * 3;
    uint local_vertex_idx = meshlet_triangle_indices[tri_offset + vertex_in_triangle];

    // Map local→global via meshlet_vertex_indices
    uint global_vertex_idx = meshlet_vertex_indices[meshlet.vertex_offset + local_vertex_idx];

    // Get local-space position
    float3 local_pos = float3(vertex_positions[global_vertex_idx]);

    // Transform: local → world → light clip space
    float4 world_pos = inst.world_matrix * float4(local_pos, 1.0f);
    out.position = uniforms.light_view_projection * world_pos;

    // Depth bias: push shadow map depth slightly farther from the light.
    // In Metal Z=[0,1] space, adding to Z makes depth larger (= farther from light).
    // This gives front-facing surfaces a margin in the shadow comparison,
    // preventing self-shadowing (shadow acne) without requiring back-face culling.
    out.position.z += 0.001;

    return out;
}
