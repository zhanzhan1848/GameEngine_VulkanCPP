#include <metal_stdlib>
using namespace metal;

// GPU-Driven Nanite Rendering using Meshlet Storage Buffers
// Data flow: Meshlet Buffers -> Regular Draw (384 vertices, meshletCount instances)
// Reference: GeometryDebugPass.cpp lines 716-774

// 🔥 NEW: InstanceData structure (matches CPU-side InstanceData)
// 192 bytes total, 16-byte aligned
struct InstanceData {
    float4x4 world_matrix;              // 64 bytes - offsets 0-63
    float4x4 inverse_world_matrix;      // 64 bytes - offsets 64-127
    uint geometry_id;                   // 4 bytes - offset 128
    uint material_id;                   // 4 bytes - offset 132 🔥 Material ID!
    uint cluster_start;                 // 4 bytes - offset 136
    uint cluster_count;                 // 4 bytes - offset 140
    uint cluster_map_base;              // 4 bytes - offset 144
    uint padding;                       // 4 bytes - offset 148
    uint padding1;                      // 4 bytes - offset 152
    uint padding2;                      // 4 bytes - offset 156
    float3 bounds_center;               // 12 bytes - offsets 160-171
    float bounds_radius;                // 4 bytes - offset 172
    uint bounds_padding[2];             // 12 bytes - offsets 176-187
    uint bounds_padding2;               // 4 bytes - offset 188
};

// 🔥 NEW: Material data structure (matches CPU-side MaterialData)
struct ClusterMaterial {
    uint albedo_texture_idx;
    uint normal_texture_idx;
    uint orm_texture_idx;
    float albedo_tint[3];
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float uv_scale[2];  // 🔥 NEW: UV scaling for texture repetition
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
    uint materialID;  // 🔥 NEW: Per-cluster material ID
    uint padding;     // Maintain 16-byte alignment
};

// 🔥 NEW: Helper to unpack normal/tangent from uint
// Matches original format
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

// 🔥 Vertex element structure - 24 bytes with padding
struct VertexElement {
    uint colorTSign;     // 4 bytes
    uint normal;         // 4 bytes (stored as uint, unpacked to vec2)
    uint tangent;        // 4 bytes (stored as uint, unpacked to vec2)
    uint padding;        // 4 bytes (padding for alignment)
    float2 uv;           // 8 bytes
    // Total: 4+4+4+4+8 = 24 bytes
};

struct VertexOut {
    float4 position [[position]];
    float3 color;

    // 🔥 NEW: Material sampling fields
    float2 uv;
    float3 normal;   // World-space normal
    float3 tangent;  // World-space tangent
    float3 bitangent;// World-space bitangent (calculated)

    // 🎨 Material ID (for material sampling)
    uint materialID; // Will be used to index material data
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
// 7=InstanceData (InstanceID -> Full Instance Data including material_id) 🔥 CHANGED
// 8=Elements (Normal, Tangent, UV)
// 9=MaterialData (MaterialID -> Texture indices)
vertex VertexOut gpu_driven_vertex_shader(
    constant DrawConstants& uniforms [[buffer(0)]],           // Camera
    constant Meshlet* meshlets [[buffer(1)]],               // Global Meshlet data
    constant uint* meshlet_vertices [[buffer(2)]],         // Global vertex indices
    device const uchar* meshlet_triangles [[buffer(3)]],       // Local vertex indices (uchar)
    device const packed_float3* positions [[buffer(4)]],        // Global Position data
    device const uint* compact_cluster_ids [[buffer(5)]],       // Visible cluster IDs
    constant ClusterMap* cluster_map [[buffer(6)]],         // Cluster mapping
    constant InstanceData* instance_data [[buffer(7)]],     // 🔥 CHANGED: Full Instance Data
    device const VertexElement* elements [[buffer(8)]],      // Vertex elements (Normal, Tangent, UV)
    constant ClusterMaterial* material_data [[buffer(9)]],   // Material data per material
    uint vertexID [[vertex_id]],                            // Vertex ID within meshlet (0..383)
    uint instanceID [[instance_id]])                         // Indirect Draw Instance ID (0..VisibleClusterCount)
{
    VertexOut out;
    
    // 1. Get Global Cluster ID from Compact List
    uint globalClusterID = compact_cluster_ids[instanceID];

    // 2. Map to Global Meshlet and Instance Index
    ClusterMap map = cluster_map[globalClusterID];
    uint globalMeshletIndex = map.globalMeshletIndex;
    uint instanceIndex = map.instanceIndex;
    uint materialID = map.materialID;

    // Get InstanceData for transform
    InstanceData instance = instance_data[instanceIndex];

    Meshlet meshlet = meshlets[globalMeshletIndex];

    // 3. Vertex Pulling
    uint actualTriangles = meshlet.triangle_count;
    uint actualVertexCount = actualTriangles * 3;

    // Check if this vertexID is valid for the actual meshlet size
    if (vertexID >= actualVertexCount) {
        // Emit degenerate geometry for padding vertices
        out.position = float4(0.0, 0.0, 0.0, 0.0);
        out.color = float3(0.0, 0.0, 0.0);
        return out;
    }

    // Safe array access - get local vertex index from meshlet triangles
    uchar localVertIdx = meshlet_triangles[meshlet.triangle_offset + vertexID];

    // Get global vertex index from meshlet_vertices buffer
    uint vertIdx = meshlet_vertices[meshlet.vertex_offset + localVertIdx];

    // Get position and element data
    float3 pos = positions[vertIdx];
    VertexElement element = elements[vertIdx];

    // Unpack Normal and Tangent
    float3 normal = UnpackNormal(element.normal);
    float3 tangent = UnpackNormal(element.tangent);

    // UV coordinates (flip Y axis for Metal texture coordinate system)
    float2 uv = float2(element.uv.x, 1.0 - element.uv.y);

    // 🔧 DEBUG: Visualize UV coordinates to diagnose texture stretching
    // Mode 1: UV gradient visualization (red = U, green = V)
    // out.color = float3(uv.x, uv.y, 0.0);

    // Normal rendering
    out.color = float3(1.0, 1.0, 1.0); // Default white

    // 4. Transform using instance world matrix
    float4 worldPos = instance.world_matrix * float4(pos, 1.0);
    float4 viewPos = uniforms.view_matrix * worldPos;
    out.position = uniforms.proj_matrix * viewPos;

    // 🔥 NEW: Transform normal/tangent to world space
    float3x3 normalMatrix = float3x3(
        instance.world_matrix[0].xyz,
        instance.world_matrix[1].xyz,
        instance.world_matrix[2].xyz
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

    // Output material ID and data
    out.materialID = materialID;
    out.uv = uv;
    out.normal = worldNormal;
    out.tangent = worldTangent;
    out.bitangent = worldBitangent;
    out.color = float3(1.0, 1.0, 1.0); // Default white

    return out;
}

// 🔥 NEW: Fragment shader with texture sampling and normal mapping
fragment GBufferOutput gpu_driven_fragment_shader(
    VertexOut in [[stage_in]],
    constant DrawConstants& uniforms [[buffer(0)]],
    constant ClusterMaterial* material_data [[buffer(9)]],      // Material data buffer (binding 9)
    texture2d_array<float> albedo_textures [[texture(10)]],     // Albedo texture array (binding 10)
    texture2d_array<float> normal_textures [[texture(11)]],     // Normal texture array (binding 11)
    texture2d_array<float> orm_textures [[texture(12)]],        // ORM texture array (binding 12)
    sampler texture_sampler [[sampler(13)]])                     // Texture sampler (binding 13)
{
    GBufferOutput out;

    // 🔧 DEBUG: Visualize which material ID is being used
    // This helps identify if stretched textures correlate with specific materials
    ClusterMaterial mat = material_data[in.materialID];

    // Sample textures
    constexpr sampler linear_sampler(mip_filter::linear, mag_filter::linear, min_filter::linear);

    // Get texture array size
    uint albedo_array_size = albedo_textures.get_array_size();

    // 🔥 NEW: Apply UV scaling for texture repetition
    float2 scaled_uv = in.uv * float2(mat.uv_scale[0], mat.uv_scale[1]);

    // Sample albedo texture
    float4 albedo_sample = float4(1.0, 1.0, 1.0, 1.0);

    if (mat.albedo_texture_idx != 0xFFFFFFFF && mat.albedo_texture_idx < albedo_array_size) {
        albedo_sample = albedo_textures.sample(linear_sampler, scaled_uv, mat.albedo_texture_idx);
    }

    // Apply albedo tint
    float3 baseColor = albedo_sample.rgb * float3(mat.albedo_tint[0], mat.albedo_tint[1], mat.albedo_tint[2]);

    // 🔧 DEBUG: Mix material ID color with texture to identify which material is which
    // Make material ID 0-15 visible as color overlay
    float matIDOverlay = (in.materialID < 16) ? (float(in.materialID) / 16.0) : 0.0;
    baseColor = mix(baseColor, float3(matIDOverlay, 0.0, 0.0), 0.3);  // Red tint for material ID

    // For now, just output albedo with material ID overlay
    out.albedo = float4(baseColor, 1.0);
    out.normal = float4(in.normal * 0.0 + 0.5, 1.0);
    out.orm = float4(1.0, 1.0, 1.0, 1.0);
    out.velocity = float2(0.0, 0.0);

    return out;
}