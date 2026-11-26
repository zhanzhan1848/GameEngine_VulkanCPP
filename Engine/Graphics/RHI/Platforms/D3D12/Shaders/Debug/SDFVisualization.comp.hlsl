// Vulkan reference: Debug/SDFVisualization.comp (main).
#include "../Common/TextureDimensions.hlsli"

struct HitResult
{
    bool hit;
    float3 position;
    float _distance;
};

cbuffer SDFVizUniforms : register(b4, space0)
{
    float4 uniforms_SdfOrigins[3] : packoffset(c0);
    float4 uniforms_SdfExtents[3] : packoffset(c3);
    float4 uniforms_SdfVoxelSizes[3] : packoffset(c6);
    uint4 uniforms_SdfResolutionsAndCount : packoffset(c9);
    float4 uniforms_CameraPosAndMaxDist : packoffset(c10);
    float4 uniforms_CameraForward : packoffset(c11);
    float4 uniforms_CameraRight : packoffset(c12);
    float4 uniforms_CameraUp : packoffset(c13);
    float4 uniforms_LightDirAndIntensity : packoffset(c14);
    float4 uniforms_FovAndAspect : packoffset(c15);
};

Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);
RWTexture2D<unorm float4> output_image : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _185 = (((((local.x >= 0.0f) && (local.x < extent.x)) && (local.y >= 0.0f)) && (local.y < extent.y)) && (local.z >= 0.0f)) && (local.z < extent.z);
    return _185;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (((((((uvw.x < 0.0f) || (uvw.x > 1.0f)) || (uvw.y < 0.0f)) || (uvw.y > 1.0f)) || (uvw.z < 0.0f)) || (uvw.z > 1.0f)))
    {
        return 10000000000.0f;
    }
    int resI = int(resolution);
    int tx = clamp(int(uvw.x * float(resI)), 0, resI - 1);
    int ty = clamp(int(uvw.y * float(resI)), 0, resI - 1);
    int tz = clamp(int(uvw.z * float(resI)), 0, resI - 1);
    float d = sdfTexture.Load(int4(int3(tx, ty, tz), 0)).x;
    return d;
}

HitResult traceSDF(float3 rayOrigin, float3 rayDir, float maxDist)
{
    HitResult r;
    r.hit = false;
    r.position = rayOrigin;
    r._distance = maxDist;
    float t = max(uniforms_SdfVoxelSizes[0].x * 2.0f, 0.5f);
    uint cc = uniforms_SdfResolutionsAndCount.w;
    float prevDist = 10000000000.0f;
    for (uint _step = 0u; _step < 48u; _step++)
    {
        float3 pos = rayOrigin + (rayDir * t);
        float minDist = 10000000000.0f;
        bool insideAny = false;
        bool _351 = cc > 0u;
        bool _365;
        if (_351)
        {
            float3 param = pos;
            float3 param_1 = uniforms_SdfOrigins[0].xyz;
            float3 param_2 = uniforms_SdfExtents[0].xyz;
            _365 = isInsideCascade(param, param_1, param_2);
        }
        else
        {
            _365 = _351;
        }
        if (_365)
        {
            float3 param_3 = pos;
            float3 param_4 = uniforms_SdfOrigins[0].xyz;
            float3 param_5 = uniforms_SdfExtents[0].xyz;
            uint param_6 = uniforms_SdfResolutionsAndCount.x;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6));
            insideAny = true;
        }
        bool _386 = cc > 1u;
        bool _400;
        if (_386)
        {
            float3 param_7 = pos;
            float3 param_8 = uniforms_SdfOrigins[1].xyz;
            float3 param_9 = uniforms_SdfExtents[1].xyz;
            _400 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _400 = _386;
        }
        if (_400)
        {
            float3 param_10 = pos;
            float3 param_11 = uniforms_SdfOrigins[1].xyz;
            float3 param_12 = uniforms_SdfExtents[1].xyz;
            uint param_13 = uniforms_SdfResolutionsAndCount.y;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13));
            insideAny = true;
        }
        bool _420 = cc > 2u;
        bool _434;
        if (_420)
        {
            float3 param_14 = pos;
            float3 param_15 = uniforms_SdfOrigins[2].xyz;
            float3 param_16 = uniforms_SdfExtents[2].xyz;
            _434 = isInsideCascade(param_14, param_15, param_16);
        }
        else
        {
            _434 = _420;
        }
        if (_434)
        {
            float3 param_17 = pos;
            float3 param_18 = uniforms_SdfOrigins[2].xyz;
            float3 param_19 = uniforms_SdfExtents[2].xyz;
            uint param_20 = uniforms_SdfResolutionsAndCount.z;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_2, param_17, param_18, param_19, param_20));
            insideAny = true;
        }
        if (!insideAny)
        {
            uint coarsest = min(2u, (cc - 1u));
            t += (uniforms_SdfExtents[coarsest].x * 0.1f);
            if (t > maxDist)
            {
                break;
            }
            continue;
        }
        float hitThreshold = max(uniforms_SdfVoxelSizes[0].x * 0.5f, 0.01f);
        if (minDist < hitThreshold)
        {
            r.hit = true;
            r.position = pos;
            r._distance = t;
            return r;
        }
        t += max(minDist, 0.05f);
        if (t > maxDist)
        {
            break;
        }
    }
    return r;
}

float sampleBestSDF(float3 pos)
{
    float d = 10000000000.0f;
    uint cc = uniforms_SdfResolutionsAndCount.w;
    bool _203 = cc > 0u;
    bool _218;
    if (_203)
    {
        float3 param = pos;
        float3 param_1 = uniforms_SdfOrigins[0].xyz;
        float3 param_2 = uniforms_SdfExtents[0].xyz;
        _218 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _218 = _203;
    }
    if (_218)
    {
        float3 param_3 = pos;
        float3 param_4 = uniforms_SdfOrigins[0].xyz;
        float3 param_5 = uniforms_SdfExtents[0].xyz;
        uint param_6 = uniforms_SdfResolutionsAndCount.x;
        d = min(d, sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6));
    }
    bool _239 = cc > 1u;
    bool _253;
    if (_239)
    {
        float3 param_7 = pos;
        float3 param_8 = uniforms_SdfOrigins[1].xyz;
        float3 param_9 = uniforms_SdfExtents[1].xyz;
        _253 = isInsideCascade(param_7, param_8, param_9);
    }
    else
    {
        _253 = _239;
    }
    if (_253)
    {
        float3 param_10 = pos;
        float3 param_11 = uniforms_SdfOrigins[1].xyz;
        float3 param_12 = uniforms_SdfExtents[1].xyz;
        uint param_13 = uniforms_SdfResolutionsAndCount.y;
        d = min(d, sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13));
    }
    bool _274 = cc > 2u;
    bool _289;
    if (_274)
    {
        float3 param_14 = pos;
        float3 param_15 = uniforms_SdfOrigins[2].xyz;
        float3 param_16 = uniforms_SdfExtents[2].xyz;
        _289 = isInsideCascade(param_14, param_15, param_16);
    }
    else
    {
        _289 = _274;
    }
    if (_289)
    {
        float3 param_17 = pos;
        float3 param_18 = uniforms_SdfOrigins[2].xyz;
        float3 param_19 = uniforms_SdfExtents[2].xyz;
        uint param_20 = uniforms_SdfResolutionsAndCount.z;
        d = min(d, sampleSDFCascade(sdf_cascade_2, param_17, param_18, param_19, param_20));
    }
    return d;
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    int2 pixel = int2(input.dispatchThreadID.xy);

    int2 imgSize = int2(imageDimensions(output_image));
    if (((pixel.x >= imgSize.x) || (pixel.y >= imgSize.y)))
    {
        return;
    }
    float3 camPos = uniforms_CameraPosAndMaxDist.xyz;
    float maxDist = uniforms_CameraPosAndMaxDist.w;
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(imgSize);
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, ((1.0f - uv.y) * 2.0f) - 1.0f);
    float tanHalf = uniforms_FovAndAspect.x;
    float aspect = uniforms_FovAndAspect.y;
    float3 forward = uniforms_CameraForward.xyz;
    float3 right = uniforms_CameraRight.xyz;
    float3 up = uniforms_CameraUp.xyz;
    float3 rayDir = normalize((forward + (right * ((ndc.x * tanHalf) * aspect))) + (up * (ndc.y * tanHalf)));
    float3 param = camPos;
    float3 param_1 = rayDir;
    float param_2 = maxDist;
    HitResult hit = traceSDF(param, param_1, param_2);
    float3 color;
    if (!hit.hit)
    {
        float skyT = clamp((rayDir.y * 0.5f) + 0.5f, 0.0f, 1.0f);
        color = lerp(float3(0.01f, 0.01f, 0.02f), float3(0.08f, 0.1f, 0.15f), skyT.xxx);
    }
    else
    {
        float eps = max(uniforms_SdfVoxelSizes[0].x * 2.0f, 0.05f);
        float3 param_3 = hit.position + float3(eps, 0.0f, 0.0f);
        float3 param_4 = hit.position - float3(eps, 0.0f, 0.0f);
        float3 param_5 = hit.position + float3(0.0f, eps, 0.0f);
        float3 param_6 = hit.position - float3(0.0f, eps, 0.0f);
        float3 param_7 = hit.position + float3(0.0f, 0.0f, eps);
        float3 param_8 = hit.position - float3(0.0f, 0.0f, eps);
        float3 gradient = float3(sampleBestSDF(param_3) - sampleBestSDF(param_4), sampleBestSDF(param_5) - sampleBestSDF(param_6), sampleBestSDF(param_7) - sampleBestSDF(param_8));
        float glen = length(gradient);
        float3 _700;
        if (glen > 1e-08f)
        {
            _700 = gradient / glen.xxx;
        }
        else
        {
            _700 = float3(0.0f, 1.0f, 0.0f);
        }
        float3 normal = _700;
        float3 viewDir = -rayDir;
        float3 keyDir = normalize(uniforms_LightDirAndIntensity.xyz);
        float keyDiff = max(dot(normal, -keyDir), 0.0f);
        float3 fillDir = normalize(float3((-keyDir.x) * 0.5f, keyDir.y * 0.3f, (-keyDir.z) * 0.5f));
        float fillDiff = max(dot(normal, -fillDir), 0.0f) * 0.3f;
        float3 halfV = normalize((-keyDir) + viewDir);
        float spec = pow(max(dot(normal, halfV), 0.0f), 32.0f) * 0.4f;
        float fresnel = pow(1.0f - max(dot(normal, viewDir), 0.0f), 3.0f);
        float3 rimColor = (float3(0.4f, 0.6f, 1.0f) * fresnel) * 0.5f;
        float3 baseColor = (normal * 0.5f) + 0.5f.xxx;
        float ambient = 0.12f;
        color = ((baseColor * ((ambient + (keyDiff * 0.65f)) + fillDiff)) + (float3(1.0f, 0.95f, 0.8f) * spec)) + rimColor;
        float depthT = clamp(hit._distance / max(maxDist, 0.001f), 0.0f, 1.0f);
        float depthInv = 1.0f - depthT;
        float3 depthColor = float3(depthInv, (depthInv * depthInv) * 0.7f, depthT * 0.5f);
        color = (color * 0.55f) + (depthColor * 0.45f);
    }
    output_image[pixel] = float4(color, 1.0f);
}
