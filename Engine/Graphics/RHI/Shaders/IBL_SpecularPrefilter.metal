#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"
#include "IBL_Hammersley.metal"

/**
 * @file IBL_SpecularPrefilter.metal
 * @brief 生成 Prefiltered Environment Map (Specular IBL) 的计算着色器
 */

// Matches the frozen IBLPrecomputer 16-byte upload (roughness at byte 8).
struct PrefilterParams {
    uint faceSize;
    uint _pad0;
    float roughness;
    float srcResolution;
};
static_assert(sizeof(PrefilterParams) == 16, "PrefilterParams upload size");
static_assert(__builtin_offsetof(PrefilterParams, roughness) == 8, "PrefilterParams roughness offset");

kernel void CS_SpecularPrefilter(
    uint3 gid [[thread_position_in_grid]],
    texturecube<float, access::sample> envMap [[texture(0)]],
    texture2d_array<float, access::write> output [[texture(1)]],
    sampler smp [[sampler(2)]],
    constant PrefilterParams& params [[buffer(3)]]
) {
    if (gid.x >= params.faceSize || gid.y >= params.faceSize || gid.z >= 6u) return;

    float2 uv = (float2(gid.xy) + 0.5) / float2(output.get_width(), output.get_height());
    
    // Calculate direction (same as Irradiance)
    float u = 2.0 * uv.x - 1.0;
    float v = 2.0 * uv.y - 1.0; 
    v = -v; // Flip Y
    
    float3 N;
    switch(gid.z) {
        case 0: N = float3(1.0,  v, -u); break;
        case 1: N = float3(-1.0, v,  u); break;
        case 2: N = float3(u, 1.0, -v); break;
        case 3: N = float3(u, -1.0, v); break;
        case 4: N = float3(u, v, 1.0); break;
        case 5: N = float3(-u, v, -1.0); break;
    }
    N = normalize(N);
    
    const float roughness = clamp(params.roughness, 0.0f, 1.0f);
    float3 R = N;
    float3 V = R;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0);

    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        
        // H is in Tangent Space, rotate to World Space
        float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
        float3 tangentX = normalize(cross(up, N));
        float3 tangentY = cross(N, tangentX);
        
        float3 H_World = tangentX * H.x + tangentY * H.y + N * H.z;
        H = normalize(H_World);
        
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if(NdotL > 0.0)
        {
            // Preserve the frozen Vulkan prefilter NDF/PDF convention. The
            // direct-light PBR helper uses a different roughness exponent.
            float a2 = roughness * roughness;
            float NdotH = max(dot(N, H), 0.0f);
            float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
            float D = a2 / (PI * d * d + 0.0001f);
            float HdotV = max(dot(H, V), 0.0f);
            float pdf = D * NdotH / (4.0f * HdotV + 0.0001f);
            float saTexel = 4.0f * PI / (6.0f * params.srcResolution * params.srcResolution);
            float saSample = 1.0f / (float(SAMPLE_COUNT) * pdf + 0.0001f);
            float mipLevel = roughness > 0.0f ? 0.5f * log2(saSample / saTexel) : 0.0f;

            prefilteredColor += envMap.sample(smp, L, level(mipLevel)).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    
    prefilteredColor = prefilteredColor / totalWeight;

    output.write(float4(prefilteredColor, 1.0), gid.xy, gid.z);
}
