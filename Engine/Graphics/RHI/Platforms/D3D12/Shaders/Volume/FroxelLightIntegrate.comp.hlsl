// Vulkan reference: Volume/FroxelLightIntegrate.comp (main).
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

ByteAddressBuffer _588 : register(t11, space0);
Texture3D<float4> particle_atlas : register(t10, space0);
Texture2D<float4> shadow_moments_c0 : register(t4, space0);
Texture2D<float4> shadow_moments_c1 : register(t5, space0);
Texture2D<float4> shadow_moments_c2 : register(t6, space0);
Texture2D<float4> shadow_moments_c3 : register(t7, space0);
Texture3D<float4> froxel_density : register(t0, space0);
Texture2D<float4> spgi_ambient : register(t8, space0);
Texture3D<float4> cloud_volume : register(t9, space0);
RWTexture3D<float4> froxel_scatter : register(u1, space0);
Texture2D<float4> shadow_map_legacy : register(t2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 froxelCameraForward()
{
    float2 centerUv = 0.5f.xx;
    float2 ndc = float2((centerUv.x * 2.0f) - 1.0f, 1.0f - (2.0f * centerUv.y));
    float4 far4 = (((params_InvViewProj[0] * ndc.x) + (params_InvViewProj[1] * ndc.y)) + (params_InvViewProj[2] * 1.0f)) + params_InvViewProj[3];
    far4 /= far4.w.xxxx;
    return normalize(far4.xyz - params_CameraPos.xyz);
}

float froxelZToDepth(float slice)
{
    float zFrac = (slice + 0.5f) * params_FrustumParams.w;
    return params_FrustumParams.x * pow(params_FrustumParams.y / params_FrustumParams.x, zFrac);
}

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

float3 froxelWorldToUvw(float3 worldPos)
{
    float4 clip = (((params_ViewProj[0] * worldPos.x) + (params_ViewProj[1] * worldPos.y)) + (params_ViewProj[2] * worldPos.z)) + params_ViewProj[3];
    float3 sc = clip.xyz / clip.w.xxx;
    float2 uv = float2((sc.x * 0.5f) + 0.5f, 0.5f - (sc.y * 0.5f));
    float zView = max(dot(worldPos - params_CameraPos.xyz, froxelCameraForward()), params_FrustumParams.x);
    float w = clamp(log2(zView / params_FrustumParams.x) / params_FrustumParams.z, 0.0f, 1.0f);
    return float3(uv, w);
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

float3 froxelShadowProject(float3 worldPos, float4 col0, float4 col1, float4 col2, float4 col3) {
    float4 clip = col0 * worldPos.x
              + col1 * worldPos.y
              + col2 * worldPos.z
              + col3;
    return clip.xyz / clip.w;
}

float2 froxelShadowNdcToUv(float3 ndc) {
    return float2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}

float froxelDepthCascade(float3 ndc, Texture2D<float4> depthMap)
{
    if (((((((ndc.x < (-1.0f)) || (ndc.x > 1.0f)) || (ndc.y < (-1.0f))) || (ndc.y > 1.0f)) || (ndc.z < 0.0f)) || (ndc.z > 1.0f)))
    {
        return -1.0f;
    }
    float3 param = ndc;
    float2 uv = froxelShadowNdcToUv(param);

    int2 dims = int2(textureDimensions(depthMap, 0u));
    float2 p = (clamp(uv, 0.0f.xx, 1.0f.xx) * float2(dims)) - 0.5f.xx;
    int2 c0 = int2(floor(p));
    float2 t = frac(p);
    int2 c1 = min((c0 + int2(1, 1)), (dims - int2(1, 1)));
    c0 = max(c0, int2(0, 0));
    float lit = 0.0f;
    lit += (((ndc.z - 0.002f) > depthMap.Load(int3(c0, 0)).x) ? 0.0f : 1.0f);
    lit += (((ndc.z - 0.002f) > depthMap.Load(int3(int2(c1.x, c0.y), 0)).x) ? 0.0f : 1.0f);
    lit += (((ndc.z - 0.002f) > depthMap.Load(int3(int2(c0.x, c1.y), 0)).x) ? 0.0f : 1.0f);
    lit += (((ndc.z - 0.002f) > depthMap.Load(int3(int2(c1.x, c1.y), 0)).x) ? 0.0f : 1.0f);
    return lit * 0.25f;
}

float2 froxelShadowBilinear2(Texture2D<float4> tex, float2 uv)
{

    int2 dims = int2(textureDimensions(tex, 0u));
    float2 p = (clamp(uv, 0.0f.xx, 1.0f.xx) * float2(dims)) - 0.5f.xx;
    int2 c0 = int2(floor(p));
    float2 t = frac(p);
    int2 c1 = min((c0 + int2(1, 1)), (dims - int2(1, 1)));
    c0 = max(c0, int2(0, 0));
    float2 v00 = tex.Load(int3(c0, 0)).xy;
    float2 v10 = tex.Load(int3(int2(c1.x, c0.y), 0)).xy;
    float2 v01 = tex.Load(int3(int2(c0.x, c1.y), 0)).xy;
    float2 v11 = tex.Load(int3(int2(c1.x, c1.y), 0)).xy;
    return lerp(lerp(v00, v10, t.x.xx), lerp(v01, v11, t.x.xx), t.y.xx);
}

float froxelChebyshevVisibility(float2 moments, float t) {
    if (t <= moments.x) return 1.0;
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, 0.00002);
    float d = t - moments.x;
    float pMax = variance / (variance + d * d);
    return smoothstep(0.05, 1.0, pMax);
}

float froxelVsmCascade(float3 ndc, Texture2D<float4> moments)
{
    if (((((((ndc.x < (-1.0f)) || (ndc.x > 1.0f)) || (ndc.y < (-1.0f))) || (ndc.y > 1.0f)) || (ndc.z < 0.0f)) || (ndc.z > 1.0f)))
    {
        return -1.0f;
    }
    float3 param = ndc;
    float2 uv = froxelShadowNdcToUv(param);
    float2 param_1 = uv;
    float2 m = froxelShadowBilinear2(moments, param_1);
    float2 param_2 = m;
    float param_3 = ndc.z;
    return froxelChebyshevVisibility(param_2, param_3);
}

float froxelShadowQuery(float3 worldPos, int mode)
{
    float v = -1.0f;
    if (mode == 2)
    {
        float3 param = worldPos;
        float4 param_1 = params_ShadowVP[0][0];
        float4 param_2 = params_ShadowVP[0][1];
        float4 param_3 = params_ShadowVP[0][2];
        float4 param_4 = params_ShadowVP[0][3];
        float3 param_5 = froxelShadowProject(param, param_1, param_2, param_3, param_4);
        v = froxelDepthCascade(param_5, shadow_moments_c0);
        if (v < 0.0f)
        {
            float3 param_6 = worldPos;
            float4 param_7 = params_ShadowVP[1][0];
            float4 param_8 = params_ShadowVP[1][1];
            float4 param_9 = params_ShadowVP[1][2];
            float4 param_10 = params_ShadowVP[1][3];
            float3 param_11 = froxelShadowProject(param_6, param_7, param_8, param_9, param_10);
            v = froxelDepthCascade(param_11, shadow_moments_c1);
        }
        if (v < 0.0f)
        {
            float3 param_12 = worldPos;
            float4 param_13 = params_ShadowVP[2][0];
            float4 param_14 = params_ShadowVP[2][1];
            float4 param_15 = params_ShadowVP[2][2];
            float4 param_16 = params_ShadowVP[2][3];
            float3 param_17 = froxelShadowProject(param_12, param_13, param_14, param_15, param_16);
            v = froxelDepthCascade(param_17, shadow_moments_c2);
        }
        if (v < 0.0f)
        {
            float3 param_18 = worldPos;
            float4 param_19 = params_ShadowVP[3][0];
            float4 param_20 = params_ShadowVP[3][1];
            float4 param_21 = params_ShadowVP[3][2];
            float3 param_23 = froxelShadowProject(param_18, param_19, param_20, param_21, params_ShadowVP[3][3]);
            v = froxelDepthCascade(param_23, shadow_moments_c3);
        }
    }
    else
    {
        float3 param_24 = worldPos;
        float4 param_25 = params_ShadowVP[0][0];
        float4 param_26 = params_ShadowVP[0][1];
        float4 param_27 = params_ShadowVP[0][2];
        float3 param_29 = froxelShadowProject(param_24, param_25, param_26, param_27, params_ShadowVP[0][3]);
        v = froxelVsmCascade(param_29, shadow_moments_c0);
        if (v < 0.0f)
        {
            float3 param_30 = worldPos;
            float4 param_31 = params_ShadowVP[1][0];
            float4 param_32 = params_ShadowVP[1][1];
            float4 param_33 = params_ShadowVP[1][2];
            float3 param_35 = froxelShadowProject(param_30, param_31, param_32, param_33, params_ShadowVP[1][3]);
            v = froxelVsmCascade(param_35, shadow_moments_c1);
        }
        if (v < 0.0f)
        {
            float3 param_36 = worldPos;
            float4 param_37 = params_ShadowVP[2][0];
            float4 param_38 = params_ShadowVP[2][1];
            float4 param_39 = params_ShadowVP[2][2];
            float3 param_41 = froxelShadowProject(param_36, param_37, param_38, param_39, params_ShadowVP[2][3]);
            v = froxelVsmCascade(param_41, shadow_moments_c2);
        }
        if (v < 0.0f)
        {
            float3 param_42 = worldPos;
            float4 param_43 = params_ShadowVP[3][0];
            float4 param_44 = params_ShadowVP[3][1];
            float4 param_45 = params_ShadowVP[3][2];
            float3 param_47 = froxelShadowProject(param_42, param_43, param_44, param_45, params_ShadowVP[3][3]);
            v = froxelVsmCascade(param_47, shadow_moments_c3);
        }
    }
    float _1261;
    if (v < 0.0f)
    {
        _1261 = 1.0f;
    }
    else
    {
        _1261 = clamp(v, 0.0f, 1.0f);
    }
    return _1261;
}

float3 froxelSpgiBilinear(Texture2D<float4> tex, float2 uv)
{

    int2 dims = int2(textureDimensions(tex, 0u));
    float2 p = (clamp(uv, 0.0f.xx, 1.0f.xx) * float2(dims)) - 0.5f.xx;
    int2 c0 = int2(floor(p));
    float2 t = frac(p);
    int2 c1 = min((c0 + int2(1, 1)), (dims - int2(1, 1)));
    c0 = max(c0, int2(0, 0));
    float3 v00 = tex.Load(int3(c0, 0)).xyz;
    float3 v10 = tex.Load(int3(int2(c1.x, c0.y), 0)).xyz;
    float3 v01 = tex.Load(int3(int2(c0.x, c1.y), 0)).xyz;
    float3 v11 = tex.Load(int3(int2(c1.x, c1.y), 0)).xyz;
    return lerp(lerp(v00, v10, t.x.xxx), lerp(v01, v11, t.x.xxx), t.y.xxx);
}

bool sparseLess(int3 a, int3 b)
{
    bool _537 = a.x < b.x;
    bool _571;
    if (!_537)
    {
        bool _545 = a.x == b.x;
        bool _570;
        if (_545)
        {
            bool _552 = a.y < b.y;
            bool _569;
            if (!_552)
            {
                bool _568 = (a.y == b.y) && (a.z < b.z);
                _569 = _568;
            }
            else
            {
                _569 = _552;
            }
            _570 = _569;
        }
        else
        {
            _570 = _545;
        }
        _571 = _570;
    }
    else
    {
        _571 = _537;
    }
    return _571;
}

float sparseVoxel(int3 v)
{
    int3 b = int3(floor(float3(v) / 8.0f.xxx));
    int lo = 0;
    int hi = int(_588.Load(0));
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        int3 param = int4(_588.Load4((4 + mid) * 16)).xyz;
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
    if (((lo >= int(_588.Load(0))) || (any(bool3(int4(_588.Load4((4 + lo) * 16)).xyz.x != b.x, int4(_588.Load4((4 + lo) * 16)).xyz.y != b.y, int4(_588.Load4((4 + lo) * 16)).xyz.z != b.z)))))
    {
        return 0.0f;
    }
    int n = int(_588.Load(4));
    int t = int(_588.Load((4 + lo) * 16 + 12));
    int3 tile = int3(t % n, (t / n) % n, t / (n * n));
    return particle_atlas.Load(int4(((tile * int3(8, 8, 8)) + v) - (b * int3(8, 8, 8)), 0)).x;
}

float sparseDensity(float3 world)
{
    if (int(_588.Load(0)) == 0)
    {
        return 0.0f;
    }
    float3 p = (world / asfloat(int(_588.Load(16))).xxx) - 0.5f.xxx;
    float3 _703 = (float3(int4(_588.Load4(32)).xyz) * 8.0f) - 1.0f.xxx;
    bool _705 = any(bool3(p.x < _703.x, p.y < _703.y, p.z < _703.z));
    bool _719;
    if (!_705)
    {
        float3 _716 = float3(int4(_588.Load4(48)).xyz + int3(1, 1, 1)) * 8.0f;
        _719 = any(bool3(p.x > _716.x, p.y > _716.y, p.z > _716.z));
    }
    else
    {
        _719 = _705;
    }
    if (_719)
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

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    int2 gid = int2(input.dispatchThreadID.xy);
    if (((gid.x >= int(params_FroxelDims.x)) || (gid.y >= int(params_FroxelDims.y))))
    {
        return;
    }
    float3 totalScatter = 0.0f.xxx;
    float transmittance = 1.0f;
    float extinctionScale = params_VolumeParams1.x;
    float scatteringAlbedo = params_VolumeParams1.y;
    float ambient = params_VolumeParams2.z;
    float3 lightColor = params_LightColor.xyz;
    float3 lightDir = normalize(params_LightDirection.xyz);
    float2 screenUv = (float2(gid) + 0.5f.xx) / params_FroxelDims.xy;
    float2 ndc = float2((screenUv.x * 2.0f) - 1.0f, 1.0f - (2.0f * screenUv.y));
    float4 near4 = (((params_InvViewProj[0] * ndc.x) + (params_InvViewProj[1] * ndc.y)) + (params_InvViewProj[2] * 0.0f)) + params_InvViewProj[3];
    float3 nearWorld = near4.xyz / near4.w.xxx;
    float3 rayDir = normalize(nearWorld - params_CameraPos.xyz);
    float cosView = max(dot(rayDir, froxelCameraForward()), 0.001f);
    int depthCount = int(params_FroxelDims.z);
    float2 colScreenUv = (float2(gid) + 0.5f.xx) / params_FroxelDims.xy;
    float phase;
    float _1938;
    for (int z = 0; z < depthCount; z++)
    {
        float density = froxel_density.Load(int4(int3(gid, z), 0)).x;
        float param = float(z) - 0.5f;
        float z0 = froxelZToDepth(param);
        float param_1 = float(z) + 0.5f;
        float z1 = froxelZToDepth(param_1);
        float ds = (z1 - z0) / cosView;
        if (density > 0.001f)
        {
            float sigmaT = density * extinctionScale;
            float sigmaS = (density * scatteringAlbedo) * extinctionScale;
            float3 param_2 = float3(float2(gid), float(z));
            float3 worldPos = froxelToWorld(param_2);
            float shadow = 1.0f;
            float mediumVisibility = 1.0f;
            if (params_VolumeParams3.w > 0.5f)
            {
                int marchSteps = int(params_VolumeParams4.x + 0.5f);
                if (marchSteps > 0)
                {
                    float stepLen = params_VolumeParams4.y / float(marchSteps);
                    float3 lp = worldPos - (lightDir * (0.5f * stepLen));
                    float beer1 = 1.0f;
                    float beer2 = 1.0f;
                    float beer3 = 1.0f;
                    float geo = 1.0f;
                    bool uniformMedium = params_FrustumParams2.z > 0.0f;
                    for (int s = 0; s < marchSteps; s++)
                    {
                        float beer = 0.0f;
                        if (uniformMedium)
                        {
                            beer = sigmaT * stepLen;
                        }
                        else
                        {
                            float3 param_3 = lp;
                            float3 uvw = froxelWorldToUvw(param_3);
                            float3 param_4 = uvw;
                            float3 param_5 = params_FroxelDims.xyz;
                            float d = froxelTrilinearClamp(froxel_density, param_4, param_5);
                            beer = (d * extinctionScale) * stepLen;
                        }
                        beer1 *= exp(-beer);
                        beer2 *= exp((-beer) * 0.25f);
                        beer3 *= exp((-beer) * 0.0625f);
                        float3 param_6 = lp;
                        int param_7 = int(params_VolumeParams3.w + 0.5f);
                        geo *= froxelShadowQuery(param_6, param_7);
                        lp -= (lightDir * stepLen);
                    }
                    mediumVisibility = ((beer1 + (0.7f * beer2)) + (0.4f * beer3)) / 2.1f;
                    shadow = clamp(geo * mediumVisibility, 0.0f, 1.0f);
                }
                else
                {
                    float3 param_8 = worldPos;
                    int param_9 = int(params_VolumeParams3.w + 0.5f);
                    shadow = froxelShadowQuery(param_8, param_9);
                }
            }
            float g = clamp(params_VolumeParams3.x, -0.95f, 0.95f);
            if (abs(g) < 0.0001f)
            {
                phase = 0.07957747f;
            }
            else
            {
                float g2 = g * g;
                float3 viewDir = normalize(worldPos - params_CameraPos.xyz);
                float cosTheta = dot(-viewDir, lightDir);
                float denom = (1.0f + g2) - ((2.0f * g) * cosTheta);
                phase = (1.0f - g2) / ((12.566371f * denom) * sqrt(denom));
            }
            float3 ambientTerm = float3(0.4f, 0.45f, 0.6f) * ambient;
            if (params_VolumeParams4.z > 0.5f)
            {
                float2 param_10 = colScreenUv;
                float3 spgiTerm = froxelSpgiBilinear(spgi_ambient, param_10) * (ambient * 9.55f);
                ambientTerm = max(spgiTerm, float3(0.4f, 0.45f, 0.6f) * (ambient * 0.55f));
            }
            bool _1739 = params_CloudA.x > 0.5f;
            bool _1754;
            if (_1739)
            {
                float3 _1748 = abs(worldPos - params_CloudB.xyz);
                float3 _1751 = params_CloudA.y.xxx;
                _1754 = all(bool3(_1748.x < _1751.x, _1748.y < _1751.y, _1748.z < _1751.z));
            }
            else
            {
                _1754 = _1739;
            }
            if (_1754)
            {

                float3 cloudDims = float3(int3(textureDimensions(cloud_volume, 0u)));
                float3 cloudUv = ((worldPos - params_CloudB.xyz) / (2.0f * params_CloudA.y).xxx) + 0.5f.xxx;
                float3 texel = 1.0f.xxx / cloudDims;
                float3 grad = 0.0f.xxx;
                for (int axis = 0; axis < 3; axis++)
                {
                    float3 offset = 0.0f.xxx;
                    offset[axis] = 2.0f * texel[axis];
                    float3 param_11 = cloudUv + offset;
                    float3 param_12 = cloudDims;
                    float3 param_13 = cloudUv - offset;
                    float3 param_14 = cloudDims;
                    grad[axis] = froxelTrilinearClamp(cloud_volume, param_11, param_12) - froxelTrilinearClamp(cloud_volume, param_13, param_14);
                }
                float skyFacing = 0.65f;
                if (dot(grad, grad) > 1e-10f)
                {
                    float3 outward = -normalize(grad);
                    float3 skyDirection = float3(-0.4000988f, 0.8891084f, -0.2222771f);
                    float directional = 0.15f + (0.85f * max(dot(outward, skyDirection), 0.0f));
                    skyFacing = lerp(0.65f, directional, smoothstep(0.002f, 0.04f, length(grad)));
                }
                ambientTerm *= (skyFacing * (0.35f + (0.65f * mediumVisibility)));
            }
            float3 param_15 = worldPos;
            if (sparseDensity(param_15) > 0.001f)
            {
                float h = asfloat(int(_588.Load(16)));
                float3 grad_1 = 0.0f.xxx;
                for (int a = 0; a < 3; a++)
                {
                    float3 off = 0.0f.xxx;
                    off[a] = h;
                    float3 param_16 = worldPos + off;
                    float3 param_17 = worldPos - off;
                    grad_1[a] = sparseDensity(param_16) - sparseDensity(param_17);
                }
                float facing = 0.65f;
                if (dot(grad_1, grad_1) > 1e-10f)
                {
                    facing = lerp(0.65f, 0.15f + (0.85f * max(dot(-normalize(grad_1), float3(-0.4000988f, 0.8891084f, -0.2222771f)), 0.0f)), smoothstep(0.002f, 0.04f, length(grad_1)));
                }
                ambientTerm *= (facing * (0.35f + (0.65f * mediumVisibility)));
            }
            float3 sourceJ = (((lightColor * shadow) * phase) + ambientTerm) * sigmaS;
            float tr = exp((-sigmaT) * ds);
            if (sigmaT > 1e-05f)
            {
                _1938 = (1.0f - tr) / sigmaT;
            }
            else
            {
                _1938 = ds;
            }
            float segW = _1938;
            totalScatter += ((sourceJ * transmittance) * segW);
            transmittance *= tr;
        }
        froxel_scatter[int3(gid, z)] = float4(totalScatter, transmittance);
    }
}
