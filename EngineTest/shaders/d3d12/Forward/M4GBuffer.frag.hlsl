Texture2D<float4> albedoTexture : register(t0, space1);
SamplerState albedoSampler : register(s1, space1);

static float2 inUV;
static float4 inBaseColor;
static float4 outAlbedo;
static float3 inWorldNormal;
static float2 outNormal;
static float4 outMaterial;
static float3 inWorldPos;

struct SPIRV_Cross_Input
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float4 inBaseColor : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float4 outAlbedo : SV_Target0;
    float2 outNormal : SV_Target1;
    float4 outMaterial : SV_Target2;
};

void frag_main()
{
    float3 albedo = albedoTexture.Sample(albedoSampler, inUV).xyz * inBaseColor.xyz;
    outAlbedo = float4(albedo, 1.0f);
    float3 N = normalize(inWorldNormal);
    outNormal = N.xy;
    outMaterial = float4(1.0f, 0.5f, inUV.x, float(N.z >= 0.0f));
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUV = stage_input.inUV;
    inBaseColor = stage_input.inBaseColor;
    inWorldNormal = stage_input.inWorldNormal;
    inWorldPos = stage_input.inWorldPos;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outAlbedo = outAlbedo;
    stage_output.outNormal = outNormal;
    stage_output.outMaterial = outMaterial;
    return stage_output;
}
