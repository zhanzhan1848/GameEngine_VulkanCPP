// Vulkan reference: Volume/FroxelReproject.comp (main).
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

RWTexture3D<float4> froxel_scatter_out : register(u2, space0);
Texture3D<float4> froxel_scatter_curr : register(t0, space0);
Texture3D<float4> froxel_scatter_prev : register(t1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 froxelToWorld(float3 froxelXyz)
{
    float zFrac = (froxelXyz.z + 0.5f) * params_FrustumParams.w;
    float zView = params_FrustumParams.x * pow(params_FrustumParams.y / params_FrustumParams.x, zFrac);
    float2 screenUv = (froxelXyz.xy + 0.5f.xx) / params_FroxelDims.xy;
    float2 ndc = float2((screenUv.x * 2.0f) - 1.0f, 1.0f - (2.0f * screenUv.y));
    float4 near4 = (((params_InvViewProj[0] * ndc.x) + (params_InvViewProj[1] * ndc.y)) + (params_InvViewProj[2] * 0.0f)) + params_InvViewProj[3];
    near4 /= near4.w.xxxx;
    float3 camToNear = near4.xyz - params_CameraPos.xyz;
    float scale = zView / params_FrustumParams2.x;
    return params_CameraPos.xyz + (camToNear * scale);
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

[numthreads(4, 4, 4)]
void main(StageInput input)
{
    int3 gid = int3(input.dispatchThreadID);

    int3 dims = int3(imageDimensions(froxel_scatter_out));
    if ((((gid.x >= dims.x) || (gid.y >= dims.y)) || (gid.z >= dims.z)))
    {
        return;
    }
    float4 cur = froxel_scatter_curr.Load(int4(gid, 0));
    float blend = params_VolumeParams4.w;
    if (blend <= 0.0f)
    {
        froxel_scatter_out[gid] = cur;
        return;
    }
    float3 worldPos = froxelToWorld(float3(gid));
    float4 pclip = (((params_PrevInvViewProj[0] * worldPos.x) + (params_PrevInvViewProj[1] * worldPos.y)) + (params_PrevInvViewProj[2] * worldPos.z)) + params_PrevInvViewProj[3];
    float3 pndc = pclip.xyz / pclip.w.xxx;
    if (((((((pndc.x < (-1.0f)) || (pndc.x > 1.0f)) || (pndc.y < (-1.0f))) || (pndc.y > 1.0f)) || (pndc.z < 0.0f)) || (pndc.z > 1.0f)))
    {
        froxel_scatter_out[gid] = cur;
        return;
    }
    float3 puvw = float3((pndc.x * 0.5f) + 0.5f, 0.5f - (pndc.y * 0.5f), pndc.z);
    float3 fdims = params_FroxelDims.xyz;
    float3 param_1 = puvw;
    float3 param_2 = fdims;
    float4 hist = froxelTrilinearClamp4(froxel_scatter_prev, param_1, param_2);
    float3 pcoord = (puvw * fdims) - 0.5f.xxx;
    int3 c0 = int3(floor(pcoord));
    float4 mn = 1000000015047466219876688855040.0f.xxxx;
    float4 mx = (-1000000015047466219876688855040.0f).xxxx;
    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int3 c = clamp(c0 + int3(dx, dy, dz), int3(0, 0, 0), int3(fdims) - int3(1, 1, 1));
                float4 v = froxel_scatter_prev.Load(int4(c, 0));
                mn = min(mn, v);
                mx = max(mx, v);
            }
        }
    }
    float4 histClamped = clamp(hist, mn, mx);
    froxel_scatter_out[gid] = lerp(cur, histClamped, blend.xxxx);
}
