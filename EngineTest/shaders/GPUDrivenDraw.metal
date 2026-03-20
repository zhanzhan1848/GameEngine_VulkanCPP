#include <metal_stdlib>
using namespace metal;

// GPU-Driven Nanite Rendering using Meshlet Storage Buffers
// Data flow: Meshlet Buffers -> Regular Draw (384 vertices, meshletCount instances)
// Reference: GeometryDebugPass.cpp lines 716-774

// Constants per draw
struct DrawConstants {
    float4x4 view_matrix;
    float4x4 proj_matrix;
    float4x4 world_matrix; // Unused in indirect path (fetched from buffer)
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
};

struct ClusterMap {
    uint globalMeshletIndex;
    uint instanceIndex;
};

// Meshlet structure (matches RHIMeshlet)
struct Meshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    
    // Additional fields matching C++ rhi::RHIMeshlet (64 bytes total)
    packed_float3 cone_apex;
    packed_float3 cone_axis;
    float cone_cutoff;
    packed_float3 center;
    float radius;
    uint padding;
};

struct VertexOut {
    float4 position [[position]];
    float3 color;
};

// GPU-Driven Vertex Shader - reads from storage buffers
// Buffer layout: 
// 0=DrawConstants
// 1=Meshlets
// 2=MeshletVertices
// 3=MeshletTriangles
// 4=Positions
// 5=CompactClusterIDs (From Culling)
// 6=ClusterMap (ClusterID -> MeshletID/InstanceID)
// 7=InstanceMatrices (InstanceID -> WorldMatrix)
vertex VertexOut gpu_driven_vertex_shader(
    constant DrawConstants& uniforms [[buffer(0)]],           // Camera
    constant Meshlet* meshlets [[buffer(1)]],               // Global Meshlet data
    constant uint* meshlet_vertices [[buffer(2)]],         // Global vertex indices
    device const uchar* meshlet_triangles [[buffer(3)]],       // Local vertex indices (uchar)
    device const packed_float3* positions [[buffer(4)]],        // Global Position data
    device const uint* compact_cluster_ids [[buffer(5)]],       // Visible cluster IDs
    constant ClusterMap* cluster_map [[buffer(6)]],         // Cluster mapping
    constant float4x4* instance_matrices [[buffer(7)]],     // Instance transforms
    uint vertexID [[vertex_id]],                            // Vertex ID within meshlet (0..383)
    uint instanceID [[instance_id]])                         // Indirect Draw Instance ID (0..VisibleClusterCount)
{
    VertexOut out;
    
    // CRITICAL FIX: Don't check instanceID bounds here!
    // The indirect buffer's instanceCount already guarantees instanceID < visibleClusterCount
    // Checking against uniforms.meshlet_count (total meshlets) incorrectly rejects valid instances
    // The hardware provides the guarantee via the indirect draw command
    
    // 1. Get Global Cluster ID from Compact List
    // Note: instanceID corresponds to the index in the compacted list
    uint globalClusterID = compact_cluster_ids[instanceID];

    // DEBUG: Detect if compact_cluster_ids contains unstable data
    // CRITICAL FIX: Cluster 0 is valid! Only check for 0xFFFFFFFF (invalid marker)
    if (globalClusterID == 0xFFFFFFFF) {
        // Invalid cluster ID - corrupted data
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(1.0, 0.0, 1.0); // DEBUG: Magenta for invalid ID
        return out;
    }

    // ADDITIONAL SAFETY: If cluster index seems unreasonably large, skip it
    if (globalClusterID >= 1000000) { // Arbitrary large number indicating corruption
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(1.0, 1.0, 0.0); // Yellow for extremely large ID
        return out;
    }

    // 2. Map to Global Meshlet and Instance Index
    ClusterMap map = cluster_map[globalClusterID];
    uint globalMeshletIndex = map.globalMeshletIndex;
    uint instanceIndex = map.instanceIndex;

    // DEBUG: Check if cluster_map data is valid
    if (globalMeshletIndex == 0xFFFFFFFF || instanceIndex == 0xFFFFFFFF) {
        // Invalid cluster map entry - show as green
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(0.0, 1.0, 0.0); // DEBUG: Green for invalid map
        return out;
    }

    // Additional safety check for meshlet bounds
    if (globalMeshletIndex >= uniforms.meshlet_count) { // Use exact meshlet count
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(1.0, 0.5, 0.0); // Orange for meshlet index out of bounds
        return out;
    }

    Meshlet meshlet = meshlets[globalMeshletIndex];

    // 3. Vertex Pulling with bounds checking
    // We draw exactly 384 vertices per instance (128 triangles * 3 vertices)
    // But the actual meshlet may have fewer triangles
    uint maxTriangles = 128; // Max triangles we allocate per meshlet draw
    uint actualTriangles = meshlet.triangle_count;

    // Calculate actual vertex count for this meshlet
    uint actualVertexCount = actualTriangles * 3;

    // Check if this vertexID is valid for the actual meshlet size
    if (vertexID >= actualVertexCount) {
        // Emit degenerate geometry for padding vertices
        out.position = float4(0.0, 0.0, 0.0, 0.0); // w=0 ensures clipping
        out.color = float3(0.1, 0.1, 0.1); // Dark gray for padding
        return out;
    }

    // Safe array access - get local vertex index from meshlet triangles
    uchar localVertIdx = meshlet_triangles[meshlet.triangle_offset + vertexID];

    // Get global vertex index from meshlet_vertices buffer
    uint vertIdx = meshlet_vertices[meshlet.vertex_offset + localVertIdx];

    // Get position
    float3 pos = positions[vertIdx];

    // DEBUG: Check if position is valid
    if (isnan(pos.x) || isnan(pos.y) || isnan(pos.z)) {
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(1.0, 0.0, 1.0); // Magenta for NaN position
        return out;
    }

    // 4. Transform
    float4x4 worldMatrix = instance_matrices[instanceIndex];
    float4 worldPos = worldMatrix * float4(pos, 1.0);
    float4 viewPos = uniforms.view_matrix * worldPos;
    out.position = uniforms.proj_matrix * viewPos;

    // ENHANCED DEBUG: Multiple visualization modes
    // Use instance-based coloring - each instance gets unique color for better debugging
    uint clusterHash = globalClusterID * 73856093; // Prime number for good distribution
    float r = float((clusterHash >> 0) & 0xFF) / 255.0;
    float g = float((clusterHash >> 8) & 0xFF) / 255.0;
    float b = float((clusterHash >> 16) & 0xFF) / 255.0;

    out.color = float3(r, g, b);

    // DEBUG: Check for problematic transformations
    if (abs(worldPos.x) > 1000.0 || abs(worldPos.y) > 1000.0 || abs(worldPos.z) > 1000.0) {
        out.color = float3(1.0, 0.0, 0.0); // Red for extreme positions
    }

    return out;
}

fragment float4 gpu_driven_fragment_shader(
    VertexOut in [[stage_in]])
{
    return float4(in.color, 1.0);
}