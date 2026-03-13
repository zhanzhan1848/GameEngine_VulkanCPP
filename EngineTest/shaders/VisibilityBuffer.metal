#include <metal_stdlib>
using namespace metal;

// Visibility Buffer Shader
// Renders geometry to a visibility buffer storing cluster + primitive IDs
// This is the FIRST PASS of GPU-driven rendering

struct MeshletData {
    uint center_offset;
    uint radius;
    uint primitive_offset;
    uint primitive_count;
};

struct VertexOut {
    float4 position [[position]];
    float depth [[depth_any]];
    uint meshlet_id [[flat]];
    uint primitive_id [[flat]];
};

struct DrawConstants {
    float4x4 view_matrix;
    float4x4 proj_matrix;
    float4x4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
};

// Vertex shader for visibility buffer generation
vertex VertexOut visibility_vertex_shader(
    const device float3* positions [[buffer(4)]],           // Position buffer
    const device uint* meshlet_vertices [[buffer(2)]],      // Meshlet vertices
    const device uint* meshlet_triangles [[buffer(3)]],     // Meshlet triangles
    constant DrawConstants& constants [[buffer(0)]],        // Camera constants
    constant MeshletData* meshlets [[buffer(1)]],           // Meshlet data
    uint vid [[vertex_id]],
    uint instance_id [[instance_id]])
{
    VertexOut out;

    // Calculate which meshlet and primitive we're rendering
    uint meshlet_id = instance_id;
    uint vertex_in_meshlet = vid / 3;  // Which vertex in the meshlet
    uint vertex_in_triangle = vid % 3; // Which vertex in the triangle

    // Get primitive data for this meshlet
    uint primitive_idx = (vertex_in_meshlet / 3) + (meshlet_id * 128); // Approximate
    if (primitive_idx >= (meshlet_id * 128 + 128)) {
        primitive_idx = meshlet_id * 128; // Safety clamp
    }

    // Get actual vertex index from meshlet data
    uint vertex_idx = vid;

    // Read position and transform
    float3 world_pos = positions[vertex_idx];
    float4 clip_pos = constants.proj_matrix * constants.view_matrix * constants.world_matrix * float4(world_pos, 1.0);

    out.position = clip_pos;
    out.depth = clip_pos.z / clip_pos.w;
    out.meshlet_id = meshlet_id;
    out.primitive_id = primitive_idx;

    return out;
}

// Fragment shader for visibility buffer generation
fragment uint visibility_fragment_shader(
    VertexOut in [[stage_in]],
    constant DrawConstants& constants [[buffer(0)]])
{
    // Pack meshlet_id and primitive_id into single uint
    // High 8 bits: meshlet_id (255 meshlets max), Low 24 bits: primitive_id (16M primitives max)
    uint meshlet_part = (in.meshlet_id & 0xFF) << 24;
    uint primitive_part = in.primitive_id & 0x00FFFFFF;
    uint visibility_value = meshlet_part | primitive_part;

    return visibility_value;
}