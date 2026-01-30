#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"
#include "IBL_Hammersley.metal"
#include "RHIShaderPBR.metal"

/**
 * @file IBL_SpecularPrefilter.metal
 * @brief 生成 Prefiltered Environment Map (Specular IBL) 的计算着色器
 */

struct PrefilterParams {
    uint faceIndex;
    float roughness;
    float padding[2];
};

kernel void CS_SpecularPrefilter(
    uint2 gid [[thread_position_in_grid]],
    texturecube<float, access::sample> envMap [[texture(0)]],
    texture2d<float, access::write> output [[texture(1)]],
    sampler smp [[sampler(2)]],
    constant PrefilterParams& params [[buffer(3)]]
) {
    if (gid.x >= output.get_width() || gid.y >= output.get_height()) return;

    float2 uv = (float2(gid) + 0.5) / float2(output.get_width(), output.get_height());
    
    // Calculate direction (same as Irradiance)
    float u = 2.0 * uv.x - 1.0;
    float v = 2.0 * uv.y - 1.0; 
    v = -v; // Flip Y
    
    float3 N;
    switch(params.faceIndex) {
        case 0: N = float3(1.0,  v, -u); break;
        case 1: N = float3(-1.0, v,  u); break;
        case 2: N = float3(u, 1.0, -v); break;
        case 3: N = float3(u, -1.0, v); break;
        case 4: N = float3(u, v, 1.0); break;
        case 5: N = float3(-u, v, -1.0); break;
    }
    N = normalize(N);
    
    float3 R = N;
    float3 V = R;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0);

    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, params.roughness);
        
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
            // Simple sampling for now. 
            // For production quality, we should use mip level selection based on PDF to reduce fireflies.
            // But Metal's sample with gradient/level requires some care.
            
            // Using mip level 0 or a bias could be better. 
            // Here we rely on the importance sampling to pick the right directions.
            // But accessing high freq details in env map with low PDF samples causes noise.
            
            // Calculating mip level:
            float D   = DistributionGGX(N, H, params.roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001; 

            float resolution = 512.0; // Resolution of source cubemap face
            float saTexel  = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            float mipLevel = params.roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel); 
            
            prefilteredColor += envMap.sample(smp, L, level(mipLevel)).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    
    prefilteredColor = prefilteredColor / totalWeight;

    output.write(float4(prefilteredColor, 1.0), gid);
}
