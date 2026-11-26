// Vulkan reference: Volume/FroxelDensityInject.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer FroxelParamsBlock : register(b6, space0)
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

ByteAddressBuffer _990 : register(t9, space0);
Texture3D<float4> particle_atlas : register(t8, space0);
RWTexture3D<float> froxel_density : register(u0, space0);
Texture3D<float4> cloud_volume : register(t7, space0);
Texture3D<float4> sdf_cascade_0 : register(t2, space0);
Texture3D<float4> sdf_cascade_1 : register(t3, space0);
Texture3D<float4> sdf_cascade_2 : register(t4, space0);
Texture2D<float4> depth_buffer : register(t5, space0);
Texture3D<float4> noise_tex : register(t1, space0);

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

float froxelTrilinearClamp(Texture3D<float4> tex, float3 uvw, float3 dims)
{
    float3 param = uvw;
    float3 param_1 = dims;
    return froxelTrilinearClamp4(tex, param, param_1).x;
}

float froxelSampleSdf(Texture3D<float4> sdfTex, float3 pos, float4 origin, float4 voxelSize, float4 extent, float3 dims)
{
    float3 uvw = (pos - origin.xyz) / extent.xyz;
    if (((((((uvw.x < 0.0f) || (uvw.x > 1.0f)) || (uvw.y < 0.0f)) || (uvw.y > 1.0f)) || (uvw.z < 0.0f)) || (uvw.z > 1.0f)))
    {
        return 10000000000.0f;
    }
    float3 param = uvw;
    float3 param_1 = dims;
    return froxelTrilinearClamp(sdfTex, param, param_1);
}

float froxelSampleBestSdf(float3 pos, Texture3D<float4> sdf0, Texture3D<float4> sdf1, Texture3D<float4> sdf2)
{
    uint count = uint(params_SdfResolutions.w);
    if (count >= 1u)
    {
        float3 param = pos;
        float4 param_1 = params_SdfOrigins[0];
        float4 param_2 = params_SdfVoxelSizes[0];
        float4 param_3 = params_SdfExtents[0];
        float3 param_4 = params_SdfResolutions.x.xxx;
        float d = froxelSampleSdf(sdf0, param, param_1, param_2, param_3, param_4);
        if (d < 1000000000.0f)
        {
            return d;
        }
    }
    if (count >= 2u)
    {
        float3 param_5 = pos;
        float4 param_6 = params_SdfOrigins[1];
        float4 param_7 = params_SdfVoxelSizes[1];
        float4 param_8 = params_SdfExtents[1];
        float3 param_9 = params_SdfResolutions.y.xxx;
        float d_1 = froxelSampleSdf(sdf1, param_5, param_6, param_7, param_8, param_9);
        if (d_1 < 1000000000.0f)
        {
            return d_1;
        }
    }
    if (count >= 3u)
    {
        float3 param_10 = pos;
        float4 param_11 = params_SdfOrigins[2];
        float4 param_12 = params_SdfVoxelSizes[2];
        float4 param_13 = params_SdfExtents[2];
        float d_2 = froxelSampleSdf(sdf2, param_10, param_11, param_12, param_13, params_SdfResolutions.z.xxx);
        if (d_2 < 1000000000.0f)
        {
            return d_2;
        }
    }
    return 10000000000.0f;
}

bool sparseLess(int3 a, int3 b)
{
    bool _939 = a.x < b.x;
    bool _973;
    if (!_939)
    {
        bool _947 = a.x == b.x;
        bool _972;
        if (_947)
        {
            bool _954 = a.y < b.y;
            bool _971;
            if (!_954)
            {
                bool _970 = (a.y == b.y) && (a.z < b.z);
                _971 = _970;
            }
            else
            {
                _971 = _954;
            }
            _972 = _971;
        }
        else
        {
            _972 = _947;
        }
        _973 = _972;
    }
    else
    {
        _973 = _939;
    }
    return _973;
}

float sparseVoxel(int3 v)
{
    int3 b = int3(floor(float3(v) / 8.0f.xxx));
    int lo = 0;
    int hi = int(_990.Load(0));
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        int3 param = int4(_990.Load4((4 + mid) * 16)).xyz;
        int3 param_1 = b;
        if (sparseLess(param, param_1))
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (((lo >= int(_990.Load(0))) || (any(bool3(int4(_990.Load4((4 + lo) * 16)).xyz.x != b.x, int4(_990.Load4((4 + lo) * 16)).xyz.y != b.y, int4(_990.Load4((4 + lo) * 16)).xyz.z != b.z)))))
    {
        return 0.0f;
    }
    int n = int(_990.Load(4));
    int t = int(_990.Load((4 + lo) * 16 + 12));
    int3 tile = int3(t % n, (t / n) % n, t / (n * n));
    return particle_atlas.Load(int4(((tile * int3(8, 8, 8)) + v) - (b * int3(8, 8, 8)), 0)).x;
}

float sparseDensity(float3 world)
{
    if (int(_990.Load(0)) == 0)
    {
        return 0.0f;
    }
    float3 p = (world / asfloat(int(_990.Load(16))).xxx) - 0.5f.xxx;
    float3 _1105 = (float3(int4(_990.Load4(32)).xyz) * 8.0f) - 1.0f.xxx;
    bool _1107 = any(bool3(p.x < _1105.x, p.y < _1105.y, p.z < _1105.z));
    bool _1121;
    if (!_1107)
    {
        float3 _1118 = float3(int4(_990.Load4(48)).xyz + int3(1, 1, 1)) * 8.0f;
        _1121 = any(bool3(p.x > _1118.x, p.y > _1118.y, p.z > _1118.z));
    }
    else
    {
        _1121 = _1107;
    }
    if (_1121)
    {
        return 0.0f;
    }
    int3 v = int3(floor(p));
    float3 f = frac(p);
    float d = 0.0f;
    for (int k = 0; k < 8; k++)
    {
        int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float3 w = lerp(1.0f.xxx - f, f, float3(o));
        int3 param = v + o;
        d += (((sparseVoxel(param) * w.x) * w.y) * w.z);
    }
    return d;
}

float ndcToViewZ(float ndcZ)
{
    float n = params_FrustumParams2.x;
    float f = params_FrustumParams2.y;
    return (n * f) / (f - (ndcZ * (f - n)));
}

float froxelZToDepth(float slice)
{
    float zFrac = (slice + 0.5f) * params_FrustumParams.w;
    return params_FrustumParams.x * pow(params_FrustumParams.y / params_FrustumParams.x, zFrac);
}

float froxelTrilinearRepeat(Texture3D<float4> tex, float3 uvw, float3 dims)
{
    float3 p = (frac(uvw) * dims) - 0.5f.xxx;
    int3 c0 = int3(floor(p));
    float3 t = frac(p);
    int3 d = int3(dims);
    int3 c1 = (c0 + int3(1, 1, 1)) % d;
    c0 = ((c0 % d) + d) % d;
    float v000 = tex.Load(int4(int3(c0.x, c0.y, c0.z), 0)).x;
    float v100 = tex.Load(int4(int3(c1.x, c0.y, c0.z), 0)).x;
    float v010 = tex.Load(int4(int3(c0.x, c1.y, c0.z), 0)).x;
    float v110 = tex.Load(int4(int3(c1.x, c1.y, c0.z), 0)).x;
    float v001 = tex.Load(int4(int3(c0.x, c0.y, c1.z), 0)).x;
    float v101 = tex.Load(int4(int3(c1.x, c0.y, c1.z), 0)).x;
    float v011 = tex.Load(int4(int3(c0.x, c1.y, c1.z), 0)).x;
    float v111 = tex.Load(int4(int3(c1.x, c1.y, c1.z), 0)).x;
    return lerp(lerp(lerp(v000, v100, t.x), lerp(v010, v110, t.x), t.y), lerp(lerp(v001, v101, t.x), lerp(v011, v111, t.x), t.y), t.z);
}

float froxelHash(float3 p) {
    p = frac(p * float3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yxz + 19.19);
    return frac((p.x + p.y) * p.z);
}

float froxelWorley(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    float minDist = 1.0;
    for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
    for (int z = -1; z <= 1; ++z) {
        float3 neighbor = float3(float(x), float(y), float(z));
        float3 pointValue = float3(froxelHash(i + neighbor + float3(0.0, 0.0, 0.0)), froxelHash(i + neighbor + float3(37.0, 0.0, 0.0)), froxelHash(i + neighbor + float3(0.0, 0.0, 74.0)));
        minDist = min(minDist, length(neighbor + pointValue - f));
    }
    return minDist;
}

float froxelSampleDensity(float3 worldPos, Texture3D<float4> noiseTex, float3 noiseDims, Texture3D<float4> sdf0, Texture3D<float4> sdf1, Texture3D<float4> sdf2)
{
    if (params_FrustumParams2.z > 0.0f)
    {
        return params_FrustumParams2.z;
    }
    float heightGrad = 1.0f;
    if (params_VolumeParams2.y > 0.0f)
    {
        float h = smoothstep(params_VolumeParams2.x - params_VolumeParams2.y, params_VolumeParams2.x + params_VolumeParams2.y, worldPos.y);
        heightGrad = 1.0f - h;
    }
    float3 noiseUvw = worldPos * params_VolumeParams3.y;
    float3 param = noiseUvw;
    float3 param_1 = noiseDims;
    float baseShape = froxelTrilinearRepeat(noiseTex, param, param_1);
    float3 param_2 = worldPos * 2.0f;
    float detail = froxelWorley(param_2);
    float density = ((baseShape * 0.6f) + (detail * 0.4f)) * heightGrad;
    float3 param_3 = worldPos * 2.5f;
    float erosion = froxelWorley(param_3);
    density *= smoothstep(0.0f, 0.35f, erosion);
    uint cascadeCount = uint(params_SdfResolutions.w);
    if (cascadeCount > 0u)
    {
        float3 param_4 = worldPos;
        float sdf = froxelSampleBestSdf(param_4, sdf0, sdf1, sdf2);
        float sdfGate;
        if (sdf > 1000000000.0f)
        {
            sdfGate = 0.25f;
        }
        else
        {
            float air = 1.0f - smoothstep(0.0f, 10.0f, max(sdf, 0.0f));
            float notSolid = smoothstep(-1.5f, -0.1f, sdf);
            sdfGate = lerp(0.25f, 1.0f, air) * notSolid;
        }
        density *= sdfGate;
    }
    float dist = length(worldPos - params_CameraPos.xyz);
    if (params_VolumeParams1.w > 0.0f)
    {
        float fade = 1.0f - smoothstep(0.0f, params_VolumeParams1.w, dist - params_VolumeParams1.w);
        density *= fade;
    }
    return max(0.0f, density);
}

[numthreads(4, 4, 4)]
void main(StageInput input)
{
    int3 gid = int3(input.dispatchThreadID);

    int3 dims = int3(imageDimensions(froxel_density));
    if ((((gid.x >= dims.x) || (gid.y >= dims.y)) || (gid.z >= dims.z)))
    {
        return;
    }
    float3 param = float3(gid);
    float3 worldPos = froxelToWorld(param);
    float cloudD = 0.0f;
    if (params_CloudA.x > 0.5f)
    {
        float3 cuvw = ((worldPos - params_CloudB.xyz) / (2.0f * params_CloudA.y).xxx) + 0.5f.xxx;
        if (((((((cuvw.x > 0.0f) && (cuvw.x < 1.0f)) && (cuvw.y > 0.0f)) && (cuvw.y < 1.0f)) && (cuvw.z > 0.0f)) && (cuvw.z < 1.0f)))
        {

            float3 param_1 = cuvw;
            float3 param_2 = float3(int3(textureDimensions(cloud_volume, 0u)));
            float cd = froxelTrilinearClamp(cloud_volume, param_1, param_2);
            float3 param_3 = worldPos;
            float sdf = froxelSampleBestSdf(param_3, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2);
            float notSolid = smoothstep(-1.0f, -0.2f, sdf);
            cloudD = (params_CloudA.z * cd) * notSolid;
        }
    }
    float3 param_4 = worldPos;
    float particleD = sparseDensity(param_4);
    if (particleD > 0.0f)
    {
        float3 param_5 = worldPos;
        cloudD = min(60000.0f, cloudD + (particleD * smoothstep(-1.0f, -0.2f, froxelSampleBestSdf(param_5, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2))));
    }
    bool cleared = false;
    float2 pixPerFroxel = params_ScreenParams.xy / params_FroxelDims.xy;
    float2 centerPix = (float2(gid.xy) + 0.5f.xx) * pixPerFroxel;
    float2 halfPix = pixPerFroxel * 0.5f;
    float _1358;
    float _1371;
    for (int q = 0; q < 4; q++)
    {
        if ((q & 1) != 0)
        {
            _1358 = halfPix.x;
        }
        else
        {
            _1358 = -halfPix.x;
        }
        if ((q & 2) != 0)
        {
            _1371 = halfPix.y;
        }
        else
        {
            _1371 = -halfPix.y;
        }
        float2 off = float2(_1358, _1371);
        int2 depthPixel = clamp(int2(centerPix + off), int2(0, 0), int2(params_ScreenParams.xy) - int2(1, 1));
        float sceneDepth = depth_buffer.Load(int3(depthPixel, 0)).x;
        if ((sceneDepth > 0.0f) && (sceneDepth < 1.0f))
        {
            float param_6 = sceneDepth;
            float sceneZ = ndcToViewZ(param_6);
            float param_7 = float(gid.z) + 0.5f;
            float froxelZFar = froxelZToDepth(param_7);
            if (froxelZFar > sceneZ)
            {
                cleared = true;
                break;
            }
        }
    }
    if (cleared && (cloudD <= 0.0f))
    {
        froxel_density[gid] = 0.0f.x;
        return;
    }
    if (params_FrustumParams2.z > 0.0f)
    {
        froxel_density[gid] = params_FrustumParams2.z.x;
        return;
    }

    float3 noiseDims = float3(int3(textureDimensions(noise_tex, 0u)));
    float _1459;
    if (cleared)
    {
        _1459 = 0.0f;
    }
    else
    {
        float3 param_8 = worldPos;
        float3 param_9 = noiseDims;
        _1459 = froxelSampleDensity(param_8, noise_tex, param_9, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2);
    }
    float density = _1459;
    density = max(density, cloudD);
    if (density < params_VolumeParams1.z)
    {
        density = 0.0f;
    }
    froxel_density[gid] = density.x;
}
