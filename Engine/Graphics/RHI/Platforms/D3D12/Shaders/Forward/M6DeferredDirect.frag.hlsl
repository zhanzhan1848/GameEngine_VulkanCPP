// Vulkan reference: Forward/M6DeferredDirect.frag (main).
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

static float4 fragCoord;

struct StageInput
{
    float2 inUV : TEXCOORD0;
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float4 outDirect : SV_Target0;
    float4 outDirectDiffuse : SV_Target1;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    fragCoord = input.fragCoord;
    fragCoord.w = 1.0 / fragCoord.w;
    int2 texel = int2(fragCoord.xy);
    float depth = depthTexture.Load(int3(texel, 0)).x;
    float3 albedo = gbufferAlbedo.Load(int3(texel, 0)).xyz;
    float2 nxy = gbufferNormal.Load(int3(texel, 0)).xy;
    float4 mat = gbufferMaterial.Load(int3(texel, 0));
    if (depth >= 1.0f)
    {
        output.outDirect = 0.0f.xxxx;
        output.outDirectDiffuse = 0.0f.xxxx;
        return output;
    }
    float2 ndcXY = float2((input.inUV.x * 2.0f) - 1.0f, (input.inUV.y * 2.0f) - 1.0f);
    float4 ndc = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(ndc, viewData_invViewProjection);
    float3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-06f).xxx;
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
    float3 sc = clip.xyz / max(clip.w, 1e-06f).xxx;
    float2 shadowUV = (sc.xy * 0.5f) + 0.5f.xx;
    float ref = sc.z - bias;
    float shadow = 1.0f;
    if (((((((shadowUV.x >= 0.0f) && (shadowUV.x <= 1.0f)) && (shadowUV.y >= 0.0f)) && (shadowUV.y <= 1.0f)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f)))
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
    float3 specular = (0.22f * pow(ndh, 48.0f)).xxx * ndl;
    output.outDirect = float4((lightColor * (diffuse + specular)) * shadow, ndl);
    output.outDirectDiffuse = float4((lightColor * diffuse) * shadow, shadow);
    return output;
}
