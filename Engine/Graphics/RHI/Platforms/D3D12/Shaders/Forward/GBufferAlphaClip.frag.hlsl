// Vulkan reference: Forward/GBufferAlphaClip.frag (main).
Texture2D<float4> albedoMap : register(t0, space1);
SamplerState defaultSampler : register(s3, space1);
Texture2D<float4> ormMap : register(t2, space1);
Texture2D<float4> normalMap : register(t1, space1);

struct StageInput
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float3 inWorldTangent : TEXCOORD2;
    float2 inUV : TEXCOORD3;
    float4 inCurrentPos : TEXCOORD4;
    float4 inPreviousPos : TEXCOORD5;
    float4 inInstanceBaseColor : TEXCOORD6;
    float inInstanceRoughness : TEXCOORD7;
    float inInstanceMetallic : TEXCOORD8;
    float inInstanceTransmission : TEXCOORD9;
};

struct StageOutput
{
    float4 outAlbedo : SV_Target0;
    float4 outNormal : SV_Target1;
    float4 outORM : SV_Target2;
    float2 outVelocity : SV_Target3;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4 albedoSample = albedoMap.Sample(defaultSampler, input.inUV);
    output.outAlbedo = albedoSample * input.inInstanceBaseColor;
    if (output.outAlbedo.w < (input.inInstanceBaseColor.w * 0.5f))
    {
        discard;
    }
    float4 ormSample = ormMap.Sample(defaultSampler, input.inUV);
    if (length(ormSample.xyz) < 0.01f)
    {
        output.outORM = float4(1.0f, input.inInstanceRoughness, input.inInstanceMetallic, input.inInstanceTransmission);
    }
    else
    {
        output.outORM = float4(ormSample.x, ormSample.y * input.inInstanceRoughness, ormSample.z * input.inInstanceMetallic, input.inInstanceTransmission);
        output.outORM.x = max(output.outORM.x, 0.1f);
    }
    float3 normalSample = normalMap.Sample(defaultSampler, input.inUV).xyz;
    if (length(normalSample) > 0.1f)
    {
        float3 normal = (normalSample * 2.0f) - 1.0f.xxx;
        float3 N = normalize(input.inWorldNormal);
        float3 T = normalize(input.inWorldTangent);
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(float3(T), float3(B), float3(N));
        output.outNormal = float4((normalize(mul(normal, TBN)) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    else
    {
        output.outNormal = float4((normalize(input.inWorldNormal) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    float2 currentNDC = input.inCurrentPos.xy / input.inCurrentPos.w.xx;
    float2 previousNDC = input.inPreviousPos.xy / input.inPreviousPos.w.xx;
    output.outVelocity = (currentNDC - previousNDC) * 0.5f;
    return output;
}
