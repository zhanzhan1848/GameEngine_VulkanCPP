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
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff;
    float center[3];
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
    constant uchar* meshlet_triangles [[buffer(3)]],       // Local vertex indices (uchar)
    constant packed_float3* positions [[buffer(4)]],        // Global Position data
    constant uint* compact_cluster_ids [[buffer(5)]],       // Visible cluster IDs
    constant ClusterMap* cluster_map [[buffer(6)]],         // Cluster mapping
    constant float4x4* instance_matrices [[buffer(7)]],     // Instance transforms
    uint vertexID [[vertex_id]],                            // Vertex ID within meshlet (0..383)
    uint instanceID [[instance_id]])                         // Indirect Draw Instance ID (0..VisibleClusterCount)
{
    VertexOut out;
    
    // 1. Get Global Cluster ID from Compact List
    // Note: instanceID corresponds to the index in the compacted list
    // Check bounds just in case
    if (instanceID >= uniforms.meshlet_count) { // meshlet_count here is actually visible_cluster_count
        out.position = float4(0, 0, 10000.0, 1.0); // Move far away (behind camera) instead of w=0
        out.color = float3(0, 0, 0);
        return out;
    }
    
    uint globalClusterID = compact_cluster_ids[instanceID];
    
    // 2. Map to Global Meshlet and Instance Index
    ClusterMap map = cluster_map[globalClusterID];
    uint globalMeshletIndex = map.globalMeshletIndex;
    uint instanceIndex = map.instanceIndex;
    
    Meshlet meshlet = meshlets[globalMeshletIndex];
    
    // 3. Vertex Pulling
    // Each triangle has 3 vertices - if vertexID >= triangle_count * 3, discard
    if (vertexID >= meshlet.triangle_count * 3) {
        out.position = float4(0, 0, 10000.0, 1.0); // Move far away (behind camera) instead of w=0
        out.color = float3(0, 0, 0);
        return out;
    }
    
    // Get local vertex index from meshlet triangles (uchar array)
    // Offsets are already patched on CPU to be global offsets
    uchar localVertIdx = meshlet_triangles[meshlet.triangle_offset + vertexID];
    
    // Get global vertex index from meshlet_vertices buffer
    uint vertIdx = meshlet_vertices[meshlet.vertex_offset + localVertIdx];
    
    // Get position
    float3 pos = positions[vertIdx];

    // 4. Transform
    float4x4 worldMatrix = instance_matrices[instanceIndex];
    float4 worldPos = worldMatrix * float4(pos, 1.0);
    float4 viewPos = uniforms.view_matrix * worldPos;
    out.position = uniforms.proj_matrix * viewPos;

    // Create a completely stable color based on the Global Cluster ID
    // This ensures each cluster has a unique, random color that is stable across frames
    uint stable_seed = globalClusterID;

    // Use a hash function for better color distribution
    stable_seed = (stable_seed ^ 61) ^ (stable_seed >> 16);
    stable_seed *= 9;
    stable_seed = stable_seed ^ (stable_seed >> 4);
    stable_seed *= 0x27d4eb2d;
    stable_seed = stable_seed ^ (stable_seed >> 15);

    float r = fract(float(stable_seed) * (1.0 / 4294967296.0));
    float g = fract(float(stable_seed * 16807) * (1.0 / 4294967296.0));
    float b = fract(float(stable_seed * 48271) * (1.0 / 4294967296.0));
    out.color = float3(r, g, b);
    
    return out;
}

fragment float4 gpu_driven_fragment_shader(
    VertexOut in [[stage_in]])
{
    return float4(in.color, 1.0);
}