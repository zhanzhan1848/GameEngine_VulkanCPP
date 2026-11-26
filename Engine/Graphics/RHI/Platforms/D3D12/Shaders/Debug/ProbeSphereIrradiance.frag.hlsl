// Vulkan reference: Debug/ProbeSphereIrradiance.frag (main).
ByteAddressBuffer irradianceSSBO : register(t1, space0);

struct StageInput
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 N = normalize(input.vNormal);
    uint base = input.vProbe * 9u;
    float3 result = 0.0f.xxx;
    result += (asfloat(irradianceSSBO.Load4((base + 0u) * 16)).xyz * 0.8862269f);
    result += (asfloat(irradianceSSBO.Load4((base + 1u) * 16)).xyz * ((-1.0233268f) * N.y));
    result += (asfloat(irradianceSSBO.Load4((base + 2u) * 16)).xyz * (1.0233268f * N.z));
    result += (asfloat(irradianceSSBO.Load4((base + 3u) * 16)).xyz * ((-1.0233268f) * N.x));
    result += (asfloat(irradianceSSBO.Load4((base + 4u) * 16)).xyz * ((0.8580855f * N.x) * N.y));
    result += (asfloat(irradianceSSBO.Load4((base + 5u) * 16)).xyz * (((-0.8580855f) * N.y) * N.z));
    result += (asfloat(irradianceSSBO.Load4((base + 6u) * 16)).xyz * (0.24770795f * N.z));
    result += (asfloat(irradianceSSBO.Load4((base + 7u) * 16)).xyz * ((-0.24770795f) * N.x));
    result += (asfloat(irradianceSSBO.Load4((base + 8u) * 16)).xyz * (0.42904276f * (((3.0f * N.z) * N.z) - 1.0f)));
    float3 rgb = result / (1.0f + max(max(result.x, result.y), result.z)).xxx;
    output.outColor = float4(clamp(rgb, 0.0f.xxx, 1.0f.xxx), 1.0f);
    return output;
}
