// Vulkan reference: Lumen/FusionComposite.frag (fusion_composite).
cbuffer EnvironmentParams : register(b10, space0)
{
    float4 environmentParams : packoffset(c0);
};

Texture2D<float4> sceneColor : register(t0, space0);
SamplerState defaultSampler : register(s7, space0);
Texture2D<float4> indirectColor : register(t1, space0);
Texture2D<float4> volumeScatter : register(t2, space0);
Texture2D<float4> ssrColor : register(t3, space0);
Texture2D<float4> ormTex : register(t4, space0);
Texture2D<float4> albedoTex : register(t5, space0);
Texture2D<float4> ddgiSpecular : register(t6, space0);
Texture2D<float4> reflSpecular : register(t8, space0);
Texture2D<float4> environmentSpecular : register(t9, space0);
Texture2D<float4> environmentVisibility : register(t11, space0);

struct StageInput
{
    float2 inUv : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float2 envBRDFApproxAB(float roughness, float NoV) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = (float4)(roughness) * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 scene = sceneColor.Sample(defaultSampler, input.inUv).xyz;
    float3 indirect = indirectColor.Sample(defaultSampler, input.inUv).xyz;
    float4 vol = volumeScatter.Sample(defaultSampler, input.inUv);
    float4 ssr = ssrColor.Sample(defaultSampler, input.inUv);
    float4 orm = ormTex.Sample(defaultSampler, input.inUv);
    float3 albedo = albedoTex.Sample(defaultSampler, input.inUv).xyz;
    float4 ddgiSpec = ddgiSpecular.Sample(defaultSampler, input.inUv);
    float4 reflOff = reflSpecular.Sample(defaultSampler, input.inUv);
    float4 environment = environmentSpecular.Sample(defaultSampler, input.inUv);
    float roughness = clamp(orm.y, 0.04f, 1.0f);
    float metallic = orm.z;
    float NoV = clamp(ddgiSpec.w, 0.05f, 1.0f);
    float param = roughness;
    float param_1 = NoV;
    float2 ab = envBRDFApproxAB(param, param_1);
    float3 F0 = lerp(0.04f.xxx, albedo, metallic.xxx);
    float3 ddgiGlossy = ddgiSpec.xyz * ((F0 * ab.x) + ab.y.xxx);
    ddgiGlossy *= ((1.0f - (roughness * roughness)) * 0.75f);
    float ssrW = clamp(ssr.w, 0.0f, 1.0f);
    float offW = clamp(reflOff.w, 0.0f, 1.0f);
    float environmentWeight = environmentParams.x * clamp(environment.w, 0.0f, 1.0f);
    float _212;
    if (environmentParams.w > 0.5f)
    {
        _212 = clamp(environmentVisibility.Sample(defaultSampler, input.inUv).x, 0.0f, 1.0f);
    }
    else
    {
        _212 = 1.0f;
    }
    float environmentGeometryVisibility = _212;
    float4 _226 = environment;
    float3 _228 = _226.xyz * environmentGeometryVisibility;
    environment.x = _228.x;
    environment.y = _228.y;
    environment.z = _228.z;
    float3 fallbackReflection = lerp(ddgiGlossy, environment.xyz * environmentParams.y, environmentWeight.xxx);
    float3 reflection = (ssr.xyz + ((reflOff.xyz * offW) * (1.0f - ssrW))) + ((fallbackReflection * (1.0f - ssrW)) * (1.0f - offW));
    float3 F = F0 + ((1.0f.xxx - F0) * pow(1.0f - NoV, 5.0f));
    float kD = 1.0f - metallic;
    if (environmentParams.z > 0.5f)
    {
        indirect = 0.0f.xxx;
        reflection = (environment.xyz * environmentParams.y) * environmentWeight;
    }
    float3 result = (((scene + ((indirect * kD) * (1.0f.xxx - F))) + reflection) * vol.w) + vol.xyz;
    result = clamp(result, 0.0f.xxx, 64.0f.xxx);
    output.outColor = float4(result, 1.0f);
    return output;
}
