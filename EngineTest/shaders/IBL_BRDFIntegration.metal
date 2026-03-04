#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"
#include "IBL_Hammersley.metal"
#include "RHIShaderPBR.metal"

/**
 * @file IBL_BRDFIntegration.metal
 * @brief 生成 BRDF Integration LUT (Specular IBL) 的计算着色器
 */

kernel void CS_BRDFIntegration(
    uint2 gid [[thread_position_in_grid]],
    texture2d<float, access::write> output [[texture(0)]]
) {
    if (gid.x >= output.get_width() || gid.y >= output.get_height()) return;

    float NdotV = (float(gid.x) + 0.5) / float(output.get_width());
    float roughness = (float(gid.y) + 0.5) / float(output.get_height());

    float3 V;
    V.x = sqrt(1.0 - NdotV*NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    float3 N = float3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if(NdotL > 0.0)
        {
            float G = GeometrySmith_IBL(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    
    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    output.write(float4(A, B, 0.0, 0.0), gid);
}
