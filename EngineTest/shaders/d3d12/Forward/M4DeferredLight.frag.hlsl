cbuffer DeferredViewData : register(b0, space0)
{
    row_major float4x4 viewData_invViewProjection : packoffset(c0);
    float4 viewData_cameraPos : packoffset(c4);
};

cbuffer LightShadowData : register(b1, space0)
{
    float4 lightShadowData_dirAndIntensity : packoffset(c0);
    float4 lightShadowData_color : packoffset(c1);
    row_major float4x4 lightShadowData_lightViewProjection : packoffset(c2);
    float4 lightShadowData_shadowParams : packoffset(c6);
};

cbuffer GIParams : register(b10, space0)
{
    float4 giParams_gridMinAndScale : packoffset(c0);
    float4 giParams_gridInvExtent : packoffset(c1);
};

Texture2D<float4> depthTexture : register(t2, space0);
Texture2D<float4> gbufferAlbedo : register(t3, space0);
Texture2D<float4> gbufferNormal : register(t4, space0);
Texture2D<float4> gbufferMaterial : register(t5, space0);
Texture2D<float4> shadowMap : register(t6, space0);
SamplerComparisonState shadowSampler : register(s7, space0);
Texture3D<float4> giProbeTexture : register(t8, space0);
SamplerState giProbeSampler : register(s9, space0);

static float4 gl_FragCoord;
static float4 outColor;
static float2 inUV;

struct SPIRV_Cross_Input
{
    float2 inUV : TEXCOORD0;
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    int2 texel = int2(gl_FragCoord.xy);
    float depth = depthTexture.Load(int3(texel, 0)).x;
    float3 albedo = gbufferAlbedo.Load(int3(texel, 0)).xyz;
    float2 nxy = gbufferNormal.Load(int3(texel, 0)).xy;
    float4 mat = gbufferMaterial.Load(int3(texel, 0));
    if (depth >= 1.0f)
    {
        outColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }
    float2 ndcXY = float2((inUV.x * 2.0f) - 1.0f, (inUV.y * 2.0f) - 1.0f);
    float4 ndc = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(ndc, viewData_invViewProjection);
    float3 worldPos = worldPosH.xyz / max(worldPosH.w, 9.9999999747524270787835121154785e-07f).xxx;
    float zSign = (mat.w * 2.0f) - 1.0f;
    float3 N = normalize(float3(nxy, zSign * sqrt(max(0.0f, 1.0f - dot(nxy, nxy)))));
    float3 L = normalize(lightShadowData_dirAndIntensity.xyz);
    float3 V = normalize(viewData_cameraPos.xyz - worldPos);
    float3 H = normalize(L + V);
    float intensity = lightShadowData_dirAndIntensity.w;
    float3 lightColor = lightShadowData_color.xyz * intensity;
    float ndl = max(dot(N, L), 0.0f);
    float ndh = max(dot(N, H), 0.0f);
    float ndlRaw = dot(N, L);
    float slope = clamp(sqrt(max(0.0f, 1.0f - (ndlRaw * ndlRaw))) / max(abs(ndlRaw), 0.25f), 0.0f, 4.0f);
    float3 offsetPos = worldPos + ((N * lightShadowData_shadowParams.x) * min(1.0f + slope, 4.0f));
    float bias = (lightShadowData_shadowParams.x * (0.5f + (2.0f * min(slope, 3.0f)))) / lightShadowData_shadowParams.y;
    float4 clip = mul(float4(offsetPos, 1.0f), lightShadowData_lightViewProjection);
    float3 sc = clip.xyz / max(clip.w, 9.9999999747524270787835121154785e-07f).xxx;
    float2 shadowUV = (sc.xy * 0.5f) + 0.5f.xx;
    float ref = sc.z - bias;
    float shadow = 1.0f;
    bool _240 = shadowUV.x >= 0.0f;
    bool _246;
    if (_240)
    {
        _246 = shadowUV.x <= 1.0f;
    }
    else
    {
        _246 = _240;
    }
    bool _252;
    if (_246)
    {
        _252 = shadowUV.y >= 0.0f;
    }
    else
    {
        _252 = _246;
    }
    bool _258;
    if (_252)
    {
        _258 = shadowUV.y <= 1.0f;
    }
    else
    {
        _258 = _252;
    }
    bool _264;
    if (_258)
    {
        _264 = sc.z >= 0.0f;
    }
    else
    {
        _264 = _258;
    }
    bool _270;
    if (_264)
    {
        _270 = sc.z <= 1.0f;
    }
    else
    {
        _270 = _264;
    }
    if (_270)
    {
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                float2 tapUV = shadowUV + (float2(float(dx), float(dy)) * lightShadowData_shadowParams.z);
                float3 _317 = float3(tapUV, ref);
                sum += shadowMap.SampleCmp(shadowSampler, _317.xy, _317.z);
            }
        }
        shadow = sum / 9.0f;
    }
    float3 giUVW = clamp((worldPos - giParams_gridMinAndScale.xyz) * giParams_gridInvExtent.xyz, 0.0f.xxx, 1.0f.xxx);
    float3 giAmbient = (albedo * giProbeTexture.Sample(giProbeSampler, giUVW).xyz) * giParams_gridMinAndScale.w;
    float3 diffuse = albedo * ndl;
    float3 specular = (0.2199999988079071044921875f * pow(ndh, 48.0f)).xxx * ndl;
    float3 ambient = (albedo * 0.1500000059604644775390625f) + giAmbient;
    outColor = float4(ambient + ((lightColor * (diffuse + specular)) * shadow), 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    inUV = stage_input.inUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
