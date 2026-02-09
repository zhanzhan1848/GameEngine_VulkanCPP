#include <metal_stdlib>
using namespace metal;

#include "RHIShaderCommon.metal"

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Simple Post Process Shader: Tone Mapping + Gamma Correction
fragment float4 post_process_ps(
    VertexOut in [[stage_in]],
    texture2d<float> inputTexture [[texture(0)]],
    sampler defaultSampler [[sampler(1)]]
) {
    float4 color = inputTexture.sample(defaultSampler, in.uv);
    
    // Simple Exposure
    float exposure = 1.0;
    float3 exposed = color.rgb * exposure;

    // Reinhard Tone Mapping
    float3 mapped = exposed / (exposed + float3(1.0));
    
    // Gamma Correction
    mapped = pow(mapped, float3(1.0 / 2.2));
    
    return float4(mapped, color.a);
}
