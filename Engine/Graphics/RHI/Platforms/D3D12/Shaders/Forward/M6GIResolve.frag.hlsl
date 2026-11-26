// Vulkan reference: Forward/M6GIResolve.frag (main).
cbuffer DeferredViewData : register(b0, space0)
{
    row_major float4x4 viewData_invViewProjection : packoffset(c0);
    float4 viewData_cameraPos : packoffset(c4);
};

cbuffer ResolveParams : register(b7, space0)
{
    float4 resolveParams_gridMin : packoffset(c0);
    float4 resolveParams_gridInvExtent : packoffset(c1);
    float4 resolveParams_gridDims : packoffset(c2);
};

Texture2D<float4> depthTexture : register(t1, space0);
Texture2D<float4> gbufferAlbedo : register(t2, space0);
Texture2D<float4> gbufferNormal : register(t3, space0);
Texture2D<float4> gbufferMaterial : register(t4, space0);
Texture3D<float4> giProbeSH : register(t5, space0);
SamplerState giProbeSampler : register(s6, space0);

static float4 fragCoord;

struct StageInput
{
    float2 inUV : TEXCOORD0;
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float4 outIndirect : SV_Target0;
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
        output.outIndirect = 0.0f.xxxx;
        return output;
    }
    float2 ndcXY = float2((input.inUV.x * 2.0f) - 1.0f, (input.inUV.y * 2.0f) - 1.0f);
    float4 ndc = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(ndc, viewData_invViewProjection);
    float3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-06f).xxx;
    float zSign = (mat.w * 2.0f) - 1.0f;
    float3 N = normalize(float3(nxy, zSign * sqrt(max(0.0f, 1.0f - dot(nxy, nxy)))));
    float3 P = worldPos + (N * resolveParams_gridMin.w);
    float3 uvw = clamp((P - resolveParams_gridMin.xyz) * resolveParams_gridInvExtent.xyz, 0.0f.xxx, 1.0f.xxx);
    float Nz = resolveParams_gridDims.z;
    float zt = clamp(uvw.z * Nz, 0.5f, Nz - 0.5f);
    float4 shR = giProbeSH.Sample(giProbeSampler, float3(uvw.xy, (zt + (0.0f * Nz)) / (3.0f * Nz)));
    float4 shG = giProbeSH.Sample(giProbeSampler, float3(uvw.xy, (zt + (1.0f * Nz)) / (3.0f * Nz)));
    float4 shB = giProbeSH.Sample(giProbeSampler, float3(uvw.xy, (zt + (2.0f * Nz)) / (3.0f * Nz)));
    float4 basis = float4(0.282095f, 0.325735f * N.y, 0.325735f * N.z, 0.325735f * N.x);
    float3 E = max(float3(dot(shR, basis), dot(shG, basis), dot(shB, basis)), 0.0f.xxx);
    output.outIndirect = float4(albedo * E, 1.0f);
    return output;
}
