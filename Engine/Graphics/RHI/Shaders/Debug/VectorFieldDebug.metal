#include <metal_stdlib>
using namespace metal;

struct VFVertexOut {
    float4 position [[position]];
    float3 color;
};

struct DebugUniforms {
    float4x4 viewProjection;
    float4x4 model;
    struct {
        float scale;
        float step;
        float padding[2];
        uint32_t resolution[3];
        uint32_t padding2;
    } vf;
};

vertex VFVertexOut vector_field_debug_vs(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]], // Not used, we draw many lines in one go? No, draw (total*2) vertices
    constant DebugUniforms& uniforms [[buffer(1)]], // Binding 1
    texture3d<float> vfTexture [[texture(0)]]       // Binding 0
) {
    VFVertexOut out;
    
    // VertexID: 0,1 -> Line 0. 2,3 -> Line 1.
    uint lineIndex = vertexID / 2;
    uint endpoint = vertexID % 2; // 0 = start, 1 = end
    
    // Apply step
    uint actualLineIndex = lineIndex * uint(uniforms.vf.step);

    uint3 res = uint3(uniforms.vf.resolution[0], uniforms.vf.resolution[1], uniforms.vf.resolution[2]);
    
    if (res.x == 0 || res.y == 0 || res.z == 0) {
        out.position = float4(0.0);
        return out;
    }

    // Decode actualLineIndex
    uint z = actualLineIndex / (res.x * res.y);
    uint temp = actualLineIndex % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;
    
    // Safety check
    if (z >= res.z) {
        out.position = float4(0.0);
        return out;
    }
    
    // Normalized coordinates [0, 1]
    float3 uvw = (float3(x, y, z) + 0.5) / float3(res);
    
    // Sample Vector Field
    // Note: Vertex Shader sampling requires texture(0) binding in Vertex Stage
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float3 vector = vfTexture.sample(s, uvw, level(0)).rgb;
    
    if (isnan(vector.x) || isnan(vector.y) || isnan(vector.z) || length(vector) < 0.001) {
        out.position = float4(0.0); // Safe fallback, collapse line
        return out;
    }
    
    // Remap vector from [0,1] to [-1,1] if encoded? 
    // Assume texture contains raw direction for now.
    
    // Position calculation
    // Model maps [-0.5, 0.5] to World
    // Transform [0,1] UVW to [-0.5, 0.5] Model Space
    float3 modelSpacePos = uvw - 0.5;
    
    if (endpoint == 1) {
        modelSpacePos += vector * uniforms.vf.scale;
    }
    
    float4 worldPos = uniforms.model * float4(modelSpacePos, 1.0);
    out.position = uniforms.viewProjection * worldPos;
    
    // Color direction
    out.color = abs(vector);
    
    return out;
}

fragment float4 vector_field_debug_fs(
    VFVertexOut in [[stage_in]]
) {
    return float4(in.color, 1.0);
}
