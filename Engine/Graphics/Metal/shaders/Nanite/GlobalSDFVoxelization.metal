/**
 * @file GlobalSDFVoxelization.metal
 * @brief Compute shader to voxelize scene geometry into GlobalSDF cascade textures.
 *
 * Each thread processes one voxel. For each visible instance:
 *   1. Bounding sphere cull
 *   2. Per-meshlet bounds cull
 *   3. Per-triangle point-to-triangle distance
 * Tracks minimum distance across all triangles → writes to cascade SDF texture.
 *
 * Dispatch: (resolution, resolution, resolution), threadGroupSize = (4, 4, 4)
 */

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Struct definitions matching C++ GPU buffer layouts
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
    uint _pad_bc;
    float bounds_radius;
    uint _pad_br[3];
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

struct CascadeUniforms {
    float4   origin;           // xyz = cascade origin, w = voxel_size — offset 0, 16 bytes
    uint     res_x;            // offset 16
    uint     res_y;            // offset 20
    uint     res_z;            // offset 24
    uint     num_instances;    // offset 28
};

// ============================================================================
// Point-to-triangle distance
// ============================================================================

static float pointToTriangleDistance(
    float3 p,
    float3 a, float3 b, float3 c)
{
    float3 ab = b - a;
    float3 ac = c - a;
    float3 ap = p - a;

    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return length(ap);

    float3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return length(bp);

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return length(ap + v * ab);
    }

    float3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return length(cp);

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return length(ap + w * ac);
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(b + w * (c - b) - p);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return length(ap + ab * v + ac * w - p);
}

// ============================================================================
// Main Kernel
// ============================================================================

kernel void voxelize_sdf(
    uint3 tid [[thread_position_in_grid]],

    // Output: cascade SDF texture (R16_Float)
    texture3d<float, access::write> sdf_output [[texture(0)]],

    // Cascade uniforms (only ~32 bytes — safe for constant address space)
    constant CascadeUniforms& cascade [[buffer(0)]],

    // GPU buffers — use 'device const' (NOT 'constant') because Metal constant
    // address space has a ~64KB limit. With 393+ instances × 192 bytes each,
    // these buffers far exceed that limit and would return garbage data.
    device const packed_float3* vertex_positions  [[buffer(1)]],
    device const MeshletData* meshlets            [[buffer(2)]],
    device const uint* meshlet_vertex_indices     [[buffer(3)]],
    device const uchar* meshlet_triangle_indices  [[buffer(4)]],
    device const ClusterMap* cluster_map          [[buffer(5)]],
    device const InstanceData* instance_data      [[buffer(6)]]
)
{
    uint3 resolution = uint3(cascade.res_x, cascade.res_y, cascade.res_z);
    if (any(tid >= resolution)) return;

    float3 origin = cascade.origin.xyz;
    float voxelSize = cascade.origin.w;
    uint numInstances = cascade.num_instances;

    // World position of this voxel center
    float3 voxelPos = origin + (float3(tid) + 0.5f) * voxelSize;

    float minDist = 1e10f;

    for (uint instIdx = 0; instIdx < numInstances; ++instIdx) {
        // Early out: can't get better than zero
        if (minDist <= 0.0f) break;
        // CRITICAL: Use thread-local copy, NOT 'constant' reference.
        InstanceData inst = instance_data[instIdx];

        // Instance bounding sphere cull
        float3 toCenter = voxelPos - float3(inst.bounds_center);
        float distToBounds = length(toCenter) - inst.bounds_radius;
        // Skip if voxel is more than 4 voxels outside the instance bounds
        if (distToBounds > voxelSize * 4.0f) continue;

        // Iterate clusters (meshlets) for this instance
        uint clusterStart = inst.cluster_start;
        uint clusterCount = inst.cluster_count;

        for (uint c = 0; c < clusterCount; ++c) {
            uint globalClusterIdx = clusterStart + c;
            // Thread-local copies (NOT 'constant' references to 'device' data)
            ClusterMap cmap = cluster_map[globalClusterIdx];
            uint meshletIdx = cmap.globalMeshletIndex;

            MeshletData meshlet = meshlets[meshletIdx];

            // Meshlet bounding sphere cull
            float3 meshletCenter = float3(meshlet.center[0], meshlet.center[1], meshlet.center[2]);
            float meshletRadius = meshlet.radius;

            // Transform meshlet center to world space
            float4 worldCenter = inst.world_matrix * float4(meshletCenter, 1.0f);
            float distToMeshlet = length(voxelPos - worldCenter.xyz) - meshletRadius;
            if (distToMeshlet > voxelSize * 2.0f) continue;

            // Iterate triangles in this meshlet
            uint triOffset = meshlet.triangle_offset;
            uint triCount = meshlet.triangle_count;
            uint vertOffset = meshlet.vertex_offset;

            for (uint t = 0; t < triCount; ++t) {
                // Get 3 local vertex indices for this triangle
                uint i0 = meshlet_triangle_indices[triOffset + t * 3 + 0];
                uint i1 = meshlet_triangle_indices[triOffset + t * 3 + 1];
                uint i2 = meshlet_triangle_indices[triOffset + t * 3 + 2];

                // Map local → global vertex indices
                uint gi0 = meshlet_vertex_indices[vertOffset + i0];
                uint gi1 = meshlet_vertex_indices[vertOffset + i1];
                uint gi2 = meshlet_vertex_indices[vertOffset + i2];

                // Get local-space positions
                float3 lp0 = float3(vertex_positions[gi0]);
                float3 lp1 = float3(vertex_positions[gi1]);
                float3 lp2 = float3(vertex_positions[gi2]);

                // Transform to world space
                float3 wp0 = (inst.world_matrix * float4(lp0, 1.0f)).xyz;
                float3 wp1 = (inst.world_matrix * float4(lp1, 1.0f)).xyz;
                float3 wp2 = (inst.world_matrix * float4(lp2, 1.0f)).xyz;

                float d = pointToTriangleDistance(voxelPos, wp0, wp1, wp2);
                minDist = min(minDist, d);
            }
        }
    }

    sdf_output.write(float4(minDist, 0.0f, 0.0f, 0.0f), tid);
}
