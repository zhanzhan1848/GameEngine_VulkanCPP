cbuffer EnvironmentParams : register(b10, space0)
{
    float4 _196_environmentParams : packoffset(c0);
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

static float2 inUv;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float2 envBRDFApproxAB(float roughness, float NoV)
{
    float4 r = (roughness.xxxx * float4(-1.0f, -0.0274999998509883880615234375f, -0.572000026702880859375f, 0.02199999988079071044921875f)) + float4(1.0f, 0.0425000004470348358154296875f, 1.03999996185302734375f, -0.039999999105930328369140625f);
    float a004 = (min(r.x * r.x, exp2((-9.27999973297119140625f) * NoV)) * r.x) + r.y;
    return (float2(-1.03999996185302734375f, 1.03999996185302734375f) * a004) + r.zw;
}

void frag_main()
{
    float3 scene = sceneColor.Sample(defaultSampler, inUv).xyz;
    float3 indirect = indirectColor.Sample(defaultSampler, inUv).xyz;
    float4 vol = volumeScatter.Sample(defaultSampler, inUv);
    float4 ssr = ssrColor.Sample(defaultSampler, inUv);
    float4 orm = ormTex.Sample(defaultSampler, inUv);
    float3 albedo = albedoTex.Sample(defaultSampler, inUv).xyz;
    float4 ddgiSpec = ddgiSpecular.Sample(defaultSampler, inUv);
    float4 reflOff = reflSpecular.Sample(defaultSampler, inUv);
    float4 environment = environmentSpecular.Sample(defaultSampler, inUv);
    float roughness = clamp(orm.y, 0.039999999105930328369140625f, 1.0f);
    float metallic = orm.z;
    float NoV = clamp(ddgiSpec.w, 0.0500000007450580596923828125f, 1.0f);
    float param = roughness;
    float param_1 = NoV;
    float2 ab = envBRDFApproxAB(param, param_1);
    float3 F0 = lerp(0.039999999105930328369140625f.xxx, albedo, metallic.xxx);
    float3 ddgiGlossy = ddgiSpec.xyz * ((F0 * ab.x) + ab.y.xxx);
    ddgiGlossy *= ((1.0f - (roughness * roughness)) * 0.75f);
    float ssrW = clamp(ssr.w, 0.0f, 1.0f);
    float offW = clamp(reflOff.w, 0.0f, 1.0f);
    float environmentWeight = _196_environmentParams.x * clamp(environment.w, 0.0f, 1.0f);
    float _212;
    if (_196_environmentParams.w > 0.5f)
    {
        _212 = clamp(environmentVisibility.Sample(defaultSampler, inUv).x, 0.0f, 1.0f);
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
    float3 fallbackReflection = lerp(ddgiGlossy, environment.xyz * _196_environmentParams.y, environmentWeight.xxx);
    float3 reflection = (ssr.xyz + ((reflOff.xyz * offW) * (1.0f - ssrW))) + ((fallbackReflection * (1.0f - ssrW)) * (1.0f - offW));
    float3 F = F0 + ((1.0f.xxx - F0) * pow(1.0f - NoV, 5.0f));
    float kD = 1.0f - metallic;
    if (_196_environmentParams.z > 0.5f)
    {
        indirect = 0.0f.xxx;
        reflection = (environment.xyz * _196_environmentParams.y) * environmentWeight;
    }
    float3 result = (((scene + ((indirect * kD) * (1.0f.xxx - F))) + reflection) * vol.w) + vol.xyz;
    result = clamp(result, 0.0f.xxx, 64.0f.xxx);
    outColor = float4(result, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUv = stage_input.inUv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
