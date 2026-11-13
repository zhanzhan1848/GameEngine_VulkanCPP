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

Texture2D<float4> depthTexture : register(t2, space0);
Texture2D<float4> gbufferAlbedo : register(t3, space0);
Texture2D<float4> gbufferNormal : register(t4, space0);
Texture2D<float4> gbufferMaterial : register(t5, space0);
Texture2D<float4> shadowMap : register(t6, space0);
SamplerComparisonState shadowSampler : register(s7, space0);

static float4 gl_FragCoord;
static float4 outDirect;
static float4 outDirectDiffuse;
static float2 inUV;

struct SPIRV_Cross_Input
{
    float2 inUV : TEXCOORD0;
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 outDirect : SV_Target0;
    float4 outDirectDiffuse : SV_Target1;
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
        outDirect = 0.0f.xxxx;
        outDirectDiffuse = 0.0f.xxxx;
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
    bool _241 = shadowUV.x >= 0.0f;
    bool _247;
    if (_241)
    {
        _247 = shadowUV.x <= 1.0f;
    }
    else
    {
        _247 = _241;
    }
    bool _253;
    if (_247)
    {
        _253 = shadowUV.y >= 0.0f;
    }
    else
    {
        _253 = _247;
    }
    bool _259;
    if (_253)
    {
        _259 = shadowUV.y <= 1.0f;
    }
    else
    {
        _259 = _253;
    }
    bool _265;
    if (_259)
    {
        _265 = sc.z >= 0.0f;
    }
    else
    {
        _265 = _259;
    }
    bool _271;
    if (_265)
    {
        _271 = sc.z <= 1.0f;
    }
    else
    {
        _271 = _265;
    }
    if (_271)
    {
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                float2 tapUV = shadowUV + (float2(float(dx), float(dy)) * lightShadowData_shadowParams.z);
                float3 _318 = float3(tapUV, ref);
                sum += shadowMap.SampleCmp(shadowSampler, _318.xy, _318.z);
            }
        }
        shadow = sum / 9.0f;
    }
    float3 diffuse = albedo * ndl;
    float3 specular = (0.2199999988079071044921875f * pow(ndh, 48.0f)).xxx * ndl;
    outDirect = float4((lightColor * (diffuse + specular)) * shadow, ndl);
    outDirectDiffuse = float4((lightColor * diffuse) * shadow, shadow);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    inUV = stage_input.inUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outDirect = outDirect;
    stage_output.outDirectDiffuse = outDirectDiffuse;
    return stage_output;
}
