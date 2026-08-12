#include <metal_stdlib>
using namespace metal;

// Visibility Buffer Resolve Compute Shader
// This is the CRITICAL step that makes GPU-driven rendering work

struct VisibilityEntry {
    uint triangle_id;    // 24 bits for triangle primitive ID
    uint material_id;    // 8 bits for material ID
};

struct ClusterData {
    uint cluster_id;
    float3 center;
    float radius;
    uint primitive_count;
    uint vertex_offset;
    uint index_offset;
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

// Main compute shader entry point for visibility buffer resolve
kernel void ComputeMain(
    texture2d<uint> visibility_buffer [[texture(0)]],
    texture2d<float> depth_buffer [[texture(1)]],
    texture2d<float, access::write> output_color [[texture(2)]],
    constant DrawConstants& constants [[buffer(0)]],
    device const ClusterData* clusters [[buffer(1)]],
    uint2 global_id [[thread_position_in_grid]])
{
    uint2 texture_size = output_color.get_width();
    if (global_id.x >= texture_size.x || global_id.y >= texture_size.y) {
        return;
    }

    // Read visibility buffer entry
    uint visibility_data = visibility_buffer.read(global_id).r;

    // Extract triangle_id and material_id
    uint triangle_id = visibility_data & 0x00FFFFFF;      // Lower 24 bits
    uint material_id = (visibility_data >> 24) & 0xFF;     // Upper 8 bits

    // Read depth
    float depth = depth_buffer.read(global_id).r;

    // Default background color (blue for debugging)
    float4 final_color = float4(0.0, 0.2, 0.4, 1.0);

    if (triangle_id != 0 && depth < 1.0) {
        // Valid visible pixel - reconstruct geometry and apply shading

        // TODO: Implement proper geometry reconstruction
        // 1. Use triangle_id to find actual triangle data
        // 2. Fetch vertex positions, normals, UVs
        // 3. Apply proper material based on material_id
        // 4. Calculate lighting
        // 5. Write final color

        // TEMPORARY: Simple color based on material ID for debugging
        float material_color = float(material_id) / 255.0;
        final_color = float4(material_color, 0.5, 1.0 - material_color, 1.0);

        // Add some depth-based shading
        float depth_shading = 1.0 - depth;
        final_color.rgb *= depth_shading;
    }

    // Write final color
    output_color.write(final_color, global_id);
}