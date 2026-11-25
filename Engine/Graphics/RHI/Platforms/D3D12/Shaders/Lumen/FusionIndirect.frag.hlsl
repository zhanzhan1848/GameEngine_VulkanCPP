// Combine full-resolution SPGI/SSGI and low-frequency DDGI as irradiance.
// Albedo / pi converts irradiance to outgoing diffuse radiance exactly once.
Texture2D<float4> ssgiColor : register(t0);
Texture2D<float4> ddgiColor : register(t1);
Texture2D<float4> spgiColor : register(t2);
Texture2D<float4> albedoTex : register(t3);
SamplerState defaultSampler : register(s5);
cbuffer GiGainUBO : register(b6) {
    float giIndirectGain;
    float fusionMode;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    float3 ddgi = 0;
    float4 spgi = 0, ssgi = 0;
#if !defined(GI_NO_DDGI) && !defined(GI_NO_ALL)
    ddgi = ddgiColor.Sample(defaultSampler, uv).rgb;
#endif
#if !defined(GI_NO_SPGI) && !defined(GI_NO_ALL)
    spgi = spgiColor.Sample(defaultSampler, uv);
#endif
#if !defined(GI_NO_SSGI) && !defined(GI_NO_ALL)
    ssgi = ssgiColor.Sample(defaultSampler, uv);
#endif

    float probeWeight = clamp(spgi.a, 0.0, 0.9);
    float3 irradiance = ddgi;
    float screenWeight = 0.25;
    if (fusionMode < 1.5) {
        irradiance = lerp(ddgi, spgi.rgb, probeWeight);
        screenWeight = lerp(0.25, 0.15, probeWeight);
    }
    float hitDistance = clamp(ssgi.a, 0.0, 30.0) / 30.0;
    float confidence = clamp(1.0 - hitDistance * 0.7, 0.3, 1.0);
    irradiance += ssgi.rgb * confidence * screenWeight;
    float3 albedo = saturate(albedoTex.Sample(defaultSampler, uv).rgb);
    return float4(albedo * irradiance * giIndirectGain * 0.31830988, 1);
}
