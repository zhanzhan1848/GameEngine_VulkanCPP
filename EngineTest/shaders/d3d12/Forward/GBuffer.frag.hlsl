Texture2D<float4> albedoMap : register(t0, space1);
SamplerState defaultSampler : register(s3, space1);
Texture2D<float4> ormMap : register(t2, space1);
Texture2D<float4> normalMap : register(t1, space1);

static float2 inUV;
static float4 outAlbedo;
static float4 inInstanceBaseColor;
static float4 outORM;
static float inInstanceRoughness;
static float inInstanceMetallic;
static float inInstanceTransmission;
static float3 inWorldNormal;
static float3 inWorldTangent;
static float4 outNormal;
static float4 inCurrentPos;
static float4 inPreviousPos;
static float2 outVelocity;
static float3 inWorldPos;

struct SPIRV_Cross_Input
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

struct SPIRV_Cross_Output
{
    float4 outAlbedo : SV_Target0;
    float4 outNormal : SV_Target1;
    float4 outORM : SV_Target2;
    float2 outVelocity : SV_Target3;
};

void frag_main()
{
    float4 albedoSample = albedoMap.Sample(defaultSampler, inUV);
    outAlbedo = float4(albedoSample.xyz * inInstanceBaseColor.xyz, albedoSample.w * inInstanceBaseColor.w);
    float4 ormSample = ormMap.Sample(defaultSampler, inUV);
    if (length(ormSample.xyz) < 0.00999999977648258209228515625f)
    {
        outORM = float4(1.0f, inInstanceRoughness, inInstanceMetallic, inInstanceTransmission);
    }
    else
    {
        outORM = float4(ormSample.x, ormSample.y * inInstanceRoughness, ormSample.z * inInstanceMetallic, inInstanceTransmission);
        outORM.x = max(outORM.x, 0.100000001490116119384765625f);
    }
    float3 normalSample = normalMap.Sample(defaultSampler, inUV).xyz;
    if (length(normalSample) > 0.100000001490116119384765625f)
    {
        float3 normal = (normalSample * 2.0f) - 1.0f.xxx;
        float3 N = normalize(inWorldNormal);
        float3 T = normalize(inWorldTangent);
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(float3(T), float3(B), float3(N));
        outNormal = float4((normalize(mul(normal, TBN)) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    else
    {
        outNormal = float4((normalize(inWorldNormal) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    float2 currentNDC = inCurrentPos.xy / inCurrentPos.w.xx;
    float2 previousNDC = inPreviousPos.xy / inPreviousPos.w.xx;
    outVelocity = (currentNDC - previousNDC) * 0.5f;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUV = stage_input.inUV;
    inInstanceBaseColor = stage_input.inInstanceBaseColor;
    inInstanceRoughness = stage_input.inInstanceRoughness;
    inInstanceMetallic = stage_input.inInstanceMetallic;
    inInstanceTransmission = stage_input.inInstanceTransmission;
    inWorldNormal = stage_input.inWorldNormal;
    inWorldTangent = stage_input.inWorldTangent;
    inCurrentPos = stage_input.inCurrentPos;
    inPreviousPos = stage_input.inPreviousPos;
    inWorldPos = stage_input.inWorldPos;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outAlbedo = outAlbedo;
    stage_output.outORM = outORM;
    stage_output.outNormal = outNormal;
    stage_output.outVelocity = outVelocity;
    return stage_output;
}
