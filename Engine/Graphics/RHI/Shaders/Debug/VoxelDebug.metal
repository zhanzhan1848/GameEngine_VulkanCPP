#include <metal_stdlib>
using namespace metal;

struct VoxelVertexOut {
    float4 position [[position]];
    float3 uvw;
    float3 color;
};

struct DebugUniforms {
    float4x4 viewProjection;
    float4x4 model;
    struct {
        float scale;
        float threshold;
        float step;
        float padding;
        uint32_t resolution[3];
        uint32_t padding2;
    } voxel;
};

// Cube vertices (triangle list, 36 vertices)
constant float3 cube_tris[36] = {
    // Front
    float3(-0.5, -0.5,  0.5), float3( 0.5, -0.5,  0.5), float3( 0.5,  0.5,  0.5),
    float3(-0.5, -0.5,  0.5), float3( 0.5,  0.5,  0.5), float3(-0.5,  0.5,  0.5),
    // Back
    float3( 0.5, -0.5, -0.5), float3(-0.5, -0.5, -0.5), float3(-0.5,  0.5, -0.5),
    float3( 0.5, -0.5, -0.5), float3(-0.5,  0.5, -0.5), float3( 0.5,  0.5, -0.5),
    // Top
    float3(-0.5,  0.5,  0.5), float3( 0.5,  0.5,  0.5), float3( 0.5,  0.5, -0.5),
    float3(-0.5,  0.5,  0.5), float3( 0.5,  0.5, -0.5), float3(-0.5,  0.5, -0.5),
    // Bottom
    float3(-0.5, -0.5, -0.5), float3( 0.5, -0.5, -0.5), float3( 0.5, -0.5,  0.5),
    float3(-0.5, -0.5, -0.5), float3( 0.5, -0.5,  0.5), float3(-0.5, -0.5,  0.5),
    // Right
    float3( 0.5, -0.5,  0.5), float3( 0.5, -0.5, -0.5), float3( 0.5,  0.5, -0.5),
    float3( 0.5, -0.5,  0.5), float3( 0.5,  0.5, -0.5), float3( 0.5,  0.5,  0.5),
    // Left
    float3(-0.5, -0.5, -0.5), float3(-0.5, -0.5,  0.5), float3(-0.5,  0.5,  0.5),
    float3(-0.5, -0.5, -0.5), float3(-0.5,  0.5,  0.5), float3(-0.5,  0.5, -0.5)
};

vertex VoxelVertexOut voxel_debug_vs(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],
    constant DebugUniforms& uniforms [[buffer(1)]], // Binding 1
    texture3d<float> voxelTexture [[texture(0)]]    // Binding 0 (Used for dimensions if needed, or just assume resolution)
) {
    VoxelVertexOut out;
    
    uint3 res = uint3(uniforms.voxel.resolution[0], uniforms.voxel.resolution[1], uniforms.voxel.resolution[2]);
    
    if (res.x == 0 || res.y == 0 || res.z == 0) {
        out.position = float4(0.0, 0.0, 0.0, 1.0); // Safe fallback
        return out;
    }
    
    // Apply step to instanceID
    uint actualID = instanceID * uint(uniforms.voxel.step);
    
    // Decode actualID to x,y,z
    uint z = actualID / (res.x * res.y);
    uint temp = actualID % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;
    
    // Safety check for bounds
    if (z >= res.z) {
        out.position = float4(0.0);
        return out;
    }
    
    // Normalized coordinates [0, 1]
    float3 uvw = (float3(x, y, z) + 0.5) / float3(res);
    out.uvw = uvw;
    
    // Sample Voxel Texture to discard empty voxels
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float density = voxelTexture.sample(s, uvw, level(0)).r;
    
    if (isnan(density) || isinf(density) || density < uniforms.voxel.threshold) {
        // Collapsing to a single point is safer than NaN
        out.position = float4(0.0);
        return out;
    }
    
    // World position of the voxel center
    // Model matrix transforms from Volume Space [0,1] to World Bounds
    // Volume Model Scale is set to Bounds Size.
    // So we need to map [0,1] to [-0.5, 0.5] to match the scale/center logic.
    
    // Voxel Size in Volume Space
    float3 voxelSize = 1.0 / float3(res);
    
    // Vertex Offset
    float3 vertPos = cube_tris[vertexID]; // [-0.5, 0.5]
    
    // Final Position in Volume Space [0, 1]
    // (x+0.5, y+0.5, z+0.5) * voxelSize + vertPos * voxelSize * scale
    float3 center = (float3(x, y, z) + 0.5) * voxelSize;
    float3 pos = center + vertPos * voxelSize * uniforms.voxel.scale;
    
    // Transform to Model Space [-0.5, 0.5]
    float3 modelSpacePos = pos - 0.5; 
    
    float4 worldPos = uniforms.model * float4(modelSpacePos, 1.0);
    out.position = uniforms.viewProjection * worldPos;
    
    // Basic color based on position
    out.color = float3(density); // Visualize density
    
    return out;
}

fragment float4 voxel_debug_fs(
    VoxelVertexOut in [[stage_in]],
    texture3d<float> voxelTexture [[texture(0)]],
    constant DebugUniforms& uniforms [[buffer(1)]] // Binding 1
) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);
    
    float density = voxelTexture.sample(s, in.uvw).r;
    
    if (density < uniforms.voxel.threshold) {
        discard_fragment();
    }
    
    return float4(in.color, 1.0);
}
