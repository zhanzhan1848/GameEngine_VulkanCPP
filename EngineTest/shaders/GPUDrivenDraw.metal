#include <metal_stdlib>
using namespace metal;

// GPU-Driven Nanite Rendering using Meshlet Storage Buffers
// Data flow: Meshlet Buffers -> Regular Draw (384 vertices, meshletCount instances)
// Reference: GeometryDebugPass.cpp lines 716-774

// 🔥 NEW: Material data structure (matches CPU-side MaterialData)
struct ClusterMaterial {
    uint albedo_texture_idx;
    uint normal_texture_idx;
    uint orm_texture_idx;
    float albedo_tint[3];
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    uint flags;
};

// GBuffer output structure
struct GBufferOutput {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
    float4 orm    [[color(2)]];
    float2 velocity [[color(3)]];
};

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

// 🔥 NEW: Helper to unpack normal/tangent from packed_ushort2
// Matches GBuffer.metal UnpackNormal function
float3 UnpackNormal(uint packed) {
    float2 f = float2((packed >> 16) & 0xFFFF, packed & 0xFFFF);
    f = f / 32767.0 - 1.0;  // Convert from [0, 65535] to [-1, 1]
    float d = dot(f, f);
    if (d > 1.0f) {
        return float3(0.0f, 0.0f, 1.0f);  // Default normal
    }
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z);
}

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

// 🔥 NEW: Vertex element structure (matches C++ layout)
// C++ uses math::v2 (simd_float2) which is 8-byte aligned, resulting in 24-byte total
struct VertexElement {
    uint colorTSign;     // 4 bytes
    uint normal;         // 4 bytes (packed_ushort2)
    uint tangent;        // 4 bytes (packed_ushort2)
    uint padding;        // 4 bytes padding to align uv to 8-byte boundary
    float2 uv;           // 8 bytes
    // Total: 4+4+4+4+8 = 24 bytes (matches C++ simd_float2 alignment)
};

struct VertexOut {
    float4 position [[position]];
    float3 color;

    // 🔥 NEW: Material sampling fields
    float2 uv;
    float3 normal;   // World-space normal
    float3 tangent;  // World-space tangent
    float3 bitangent;// World-space bitangent (calculated)
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
// 8=Elements (Normal, Tangent, UV) - 🔥 NEW
vertex VertexOut gpu_driven_vertex_shader(
    constant DrawConstants& uniforms [[buffer(0)]],           // Camera
    constant Meshlet* meshlets [[buffer(1)]],               // Global Meshlet data
    constant uint* meshlet_vertices [[buffer(2)]],         // Global vertex indices
    device const uchar* meshlet_triangles [[buffer(3)]],       // Local vertex indices (uchar)
    device const packed_float3* positions [[buffer(4)]],        // Global Position data
    device const uint* compact_cluster_ids [[buffer(5)]],       // Visible cluster IDs
    constant ClusterMap* cluster_map [[buffer(6)]],         // Cluster mapping
    constant float4x4* instance_matrices [[buffer(7)]],     // Instance transforms
    device const VertexElement* elements [[buffer(8)]],      // 🔥 NEW: Vertex elements (Normal, Tangent, UV)
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

    // 🔥 NEW: Get element data (Normal, Tangent, UV)
    VertexElement element = elements[vertIdx];

    // Unpack Normal
    float3 normal = UnpackNormal(element.normal);

    // Unpack Tangent
    float3 tangent = UnpackNormal(element.tangent);

    // Get UV (flip Y to match GBuffer.metal)
    float2 uv = float2(element.uv.x, 1.0 - element.uv.y);

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

    // 🔥 NEW: Transform normal/tangent to world space
    float3x3 normalMatrix = float3x3(
        worldMatrix[0].xyz,
        worldMatrix[1].xyz,
        worldMatrix[2].xyz
    );

    // Transform Normal and Tangent
    float3 worldNormal = normalize(normalMatrix * normal);
    float3 worldTangent = normalize(normalMatrix * tangent);

    // Calculate Bitangent
    float3 worldBitangent = cross(worldNormal, worldTangent);

    // Output material data
    out.uv = uv;
    out.normal = worldNormal;
    out.tangent = worldTangent;
    out.bitangent = worldBitangent;

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

// 🔥 NEW: Fragment shader with GBuffer output
fragment GBufferOutput gpu_driven_fragment_shader(
    VertexOut in [[stage_in]])
{
    GBufferOutput out;

    // DEBUG: Visualize UV coordinates to verify vertex data flow
    // This confirms UVs are being passed correctly from vertex shader
    out.albedo = float4(in.uv, 0.0, 1.0);

    // DEBUG: Visualize normals in RGB
    // out.albedo = float4(in.normal * 0.5 + 0.5, 1.0);

    // Placeholder for normal output (world normal packed to [0,1])
    out.normal = float4(normalize(in.normal) * 0.5 + 0.5, 1.0);

    // Placeholder for ORM (AO=1, Roughness=0.8, Metallic=0)
    out.orm = float4(1.0, 0.8, 0.0, 1.0);

    // Placeholder for velocity
    out.velocity = float2(0.0, 0.0);

    return out;
}