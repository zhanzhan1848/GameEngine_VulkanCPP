// Vulkan reference: Volume/FroxelResolve.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer FroxelParamsBlock : register(b3, space0)
{
    float4 params_CameraPos : packoffset(c0);
    float4 params_InvViewProj[4] : packoffset(c1);
    float4 params_FrustumParams : packoffset(c5);
    float4 params_FroxelDims : packoffset(c6);
    float4 params_FroxelDims2 : packoffset(c7);
    float4 params_VolumeParams1 : packoffset(c8);
    float4 params_VolumeParams2 : packoffset(c9);
    float4 params_VolumeParams3 : packoffset(c10);
    float4 params_ScreenParams : packoffset(c11);
    float4 params_SdfOrigins[3] : packoffset(c12);
    float4 params_SdfVoxelSizes[3] : packoffset(c15);
    float4 params_SdfExtents[3] : packoffset(c18);
    float4 params_SdfResolutions : packoffset(c21);
    float4 params_LightDirection : packoffset(c22);
    float4 params_LightColor : packoffset(c23);
    float4 params_ShadowVP[4][4] : packoffset(c24);
    float4 params_FrustumParams2 : packoffset(c40);
    float4 params_VolumeParams4 : packoffset(c41);
    float4 params_ViewProj[4] : packoffset(c42);
    float4 params_PrevInvViewProj[4] : packoffset(c46);
    float4 params_CloudA : packoffset(c50);
    float4 params_CloudB : packoffset(c51);
};

RWTexture2D<float4> output_color : register(u2, space0);
Texture2D<float4> depth_buffer : register(t1, space0);
Texture3D<float4> froxel_scatter : register(t0, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float ndcToViewZ(float ndcZ)
{
    float n = params_FrustumParams2.x;
    float f = params_FrustumParams2.y;
    return (n * f) / (f - (ndcZ * (f - n)));
}

float4 froxelTrilinearClamp4(Texture3D<float4> tex, float3 uvw, float3 dims)
{
    float3 p = (clamp(uvw, 0.0f.xxx, 1.0f.xxx) * dims) - 0.5f.xxx;
    int3 c0 = int3(floor(p));
    float3 t = frac(p);
    int3 c1 = min((c0 + int3(1, 1, 1)), (int3(dims) - int3(1, 1, 1)));
    c0 = max(c0, int3(0, 0, 0));
    float4 v000 = tex.Load(int4(int3(c0.x, c0.y, c0.z), 0));
    float4 v100 = tex.Load(int4(int3(c1.x, c0.y, c0.z), 0));
    float4 v010 = tex.Load(int4(int3(c0.x, c1.y, c0.z), 0));
    float4 v110 = tex.Load(int4(int3(c1.x, c1.y, c0.z), 0));
    float4 v001 = tex.Load(int4(int3(c0.x, c0.y, c1.z), 0));
    float4 v101 = tex.Load(int4(int3(c1.x, c0.y, c1.z), 0));
    float4 v011 = tex.Load(int4(int3(c0.x, c1.y, c1.z), 0));
    float4 v111 = tex.Load(int4(int3(c1.x, c1.y, c1.z), 0));
    return lerp(lerp(lerp(v000, v100, t.x.xxxx), lerp(v010, v110, t.x.xxxx), t.y.xxxx), lerp(lerp(v001, v101, t.x.xxxx), lerp(v011, v111, t.x.xxxx), t.y.xxxx), t.z.xxxx);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    int2 gid = int2(input.dispatchThreadID.xy);

    int2 outDims = int2(imageDimensions(output_color));
    if (((gid.x >= outDims.x) || (gid.y >= outDims.y)))
    {
        return;
    }
    int2 fullResPixel = min(((gid * int2(2, 2)) + int2(1, 1)), (int2(params_ScreenParams.xy) - int2(1, 1)));
    float depth = depth_buffer.Load(int3(fullResPixel, 0)).x;
    bool isSky = (depth <= 0.0f) || (depth >= 1.0f);
    float _283;
    if (isSky)
    {
        _283 = params_FrustumParams.y;
    }
    else
    {
        _283 = ndcToViewZ(depth);
    }
    float zView = _283;
    float f = clamp(log2(zView / params_FrustumParams.x) / params_FrustumParams.z, 0.0f, 1.0f);
    float sliceCoord = (f * params_FroxelDims.z) - 1.0f;
    float2 screenUv = (float2(gid) + 0.5f.xx) / float2(outDims);

    float3 dims = float3(int3(textureDimensions(froxel_scatter, 0u)));
    float3 param_1 = float3(screenUv, sliceCoord / params_FroxelDims.z);
    float3 param_2 = dims;
    float4 result = froxelTrilinearClamp4(froxel_scatter, param_1, param_2);
    output_color[gid] = result;
}
