// Vulkan reference: Debug/OfflineSDFViz.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer SDFVizUniforms : register(b4, space0)
{
    float4 uniforms_SdfOrigins[3] : packoffset(c0);
    float4 uniforms_SdfExtents[3] : packoffset(c3);
    float4 uniforms_SdfVoxelSizes[3] : packoffset(c6);
    float4 uniforms_SdfResolutionsAndCount : packoffset(c9);
    float4 uniforms_CameraPosAndMaxDist : packoffset(c10);
    float4 uniforms_CameraForward : packoffset(c11);
    float4 uniforms_CameraRight : packoffset(c12);
    float4 uniforms_CameraUp : packoffset(c13);
    float4 uniforms_LightDirAndIntensity : packoffset(c14);
    float4 uniforms_FovAndAspect : packoffset(c15);
};

Texture3D<float4> sdf_volume : register(t0, space0);
RWTexture2D<float4> output_image : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float sampleSDF(float3 worldPos)
{
    float3 origin = uniforms_SdfOrigins[0].xyz;
    float3 extent = uniforms_SdfExtents[0].xyz;
    float res = uniforms_SdfResolutionsAndCount.x;
    float3 uvw = (worldPos - origin) / extent;
    if (((((((uvw.x < 0.0f) || (uvw.x > 1.0f)) || (uvw.y < 0.0f)) || (uvw.y > 1.0f)) || (uvw.z < 0.0f)) || (uvw.z > 1.0f)))
    {
        return 10000000000.0f;
    }
    float3 gridF = (uvw * res) - 0.5f.xxx;
    int3 i0 = int3(clamp(gridF, 0.0f.xxx, (res - 1.0f).xxx));
    int3 i1 = int3(clamp(gridF + 1.0f.xxx, 0.0f.xxx, (res - 1.0f).xxx));
    float3 f = clamp(gridF - float3(i0), 0.0f.xxx, 1.0f.xxx);
    float c000 = sdf_volume.Load(int4(i0, 0)).x;
    float c100 = sdf_volume.Load(int4(int3(i1.x, i0.y, i0.z), 0)).x;
    float c010 = sdf_volume.Load(int4(int3(i0.x, i1.y, i0.z), 0)).x;
    float c110 = sdf_volume.Load(int4(int3(i1.x, i1.y, i0.z), 0)).x;
    float c001 = sdf_volume.Load(int4(int3(i0.x, i0.y, i1.z), 0)).x;
    float c101 = sdf_volume.Load(int4(int3(i1.x, i0.y, i1.z), 0)).x;
    float c011 = sdf_volume.Load(int4(int3(i0.x, i1.y, i1.z), 0)).x;
    float c111 = sdf_volume.Load(int4(i1, 0)).x;
    return lerp(lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y), lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y), f.z);
}

float3 computeNormal(float3 pos)
{
    float eps = max(uniforms_SdfVoxelSizes[0].x * 1.5f, 0.02f);
    float3 param = pos + float3(eps, 0.0f, 0.0f);
    float3 param_1 = pos - float3(eps, 0.0f, 0.0f);
    float dx = sampleSDF(param) - sampleSDF(param_1);
    float3 param_2 = pos + float3(0.0f, eps, 0.0f);
    float3 param_3 = pos - float3(0.0f, eps, 0.0f);
    float dy = sampleSDF(param_2) - sampleSDF(param_3);
    float3 param_4 = pos + float3(0.0f, 0.0f, eps);
    float3 param_5 = pos - float3(0.0f, 0.0f, eps);
    float dz = sampleSDF(param_4) - sampleSDF(param_5);
    float3 n = float3(dx, dy, dz);
    if (dot(n, n) < 1e-12f)
    {
        return float3(0.0f, 1.0f, 0.0f);
    }
    return normalize(n);
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
    float voxelSize = uniforms_SdfVoxelSizes[0].x;
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(imgSize);
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, ((1.0f - uv.y) * 2.0f) - 1.0f);
    float tanHalf = uniforms_FovAndAspect.x;
    float aspect = uniforms_FovAndAspect.y;
    float3 forward = normalize(uniforms_CameraForward.xyz);
    float3 right = normalize(uniforms_CameraRight.xyz);
    float3 up = normalize(uniforms_CameraUp.xyz);
    float3 rayDir = normalize((forward + (right * ((ndc.x * tanHalf) * aspect))) + (up * (ndc.y * tanHalf)));
    float3 volOrigin = uniforms_SdfOrigins[0].xyz;
    float3 volExtent = uniforms_SdfExtents[0].xyz;
    float3 tMin3 = (volOrigin - camPos) / max(rayDir, 1e-08f.xxx);
    float3 tMax3 = ((volOrigin + volExtent) - camPos) / max(rayDir, 1e-08f.xxx);
    float3 t1 = min(tMin3, tMax3);
    float3 t2 = max(tMin3, tMax3);
    float tNear = max(max(t1.x, t1.y), max(t1.z, 0.0f));
    float tFar = min(min(t2.x, t2.y), t2.z);
    if ((tNear > tFar) || (tFar < 0.0f))
    {
        float3 skyTop = float3(0.35f, 0.45f, 0.65f);
        float3 skyBot = float3(0.7f, 0.75f, 0.85f);
        output_image[pixel] = float4(lerp(skyBot, skyTop, uv.y.xxx), 1.0f);
        return;
    }
    tFar = min(tFar, maxDist);
    float hitThreshold = voxelSize * 0.75f;
    float stepSize = voxelSize * 0.5f;
    float t = tNear;
    bool hit = false;
    float3 hitPos = 0.0f.xxx;
    float prevDist = 10000000000.0f;
    for (int i = 0; (i < 512) && (t <= tFar); i++)
    {
        float3 pos = camPos + (rayDir * t);
        float3 param = pos;
        float d = sampleSDF(param);
        if (d < hitThreshold)
        {
            hit = true;
            hitPos = pos;
            break;
        }
        float _step = min(d * 0.9f, stepSize * 4.0f);
        _step = max(_step, stepSize);
        _step = min(_step, stepSize * 2.0f);
        t += _step;
    }
    if (!hit)
    {
        float3 skyTop_1 = float3(0.35f, 0.45f, 0.65f);
        float3 skyBot_1 = float3(0.7f, 0.75f, 0.85f);
        output_image[pixel] = float4(lerp(skyBot_1, skyTop_1, uv.y.xxx), 1.0f);
        return;
    }
    float3 param_1 = hitPos;
    float distAtHit = sampleSDF(param_1);
    float3 param_2 = hitPos;
    float3 N = computeNormal(param_2);
    if (dot(N, rayDir) > 0.0f)
    {
        N = -N;
    }
    float3 normalColor = (N * 0.5f) + 0.5f.xxx;
    float contour = abs(frac(distAtHit / voxelSize) - 0.5f);
    float contourLine = smoothstep(0.35f, 0.5f, contour);
    float depthT = clamp(t / maxDist, 0.0f, 1.0f);
    float3 depthTint = lerp(float3(1.0f, 0.95f, 0.85f), float3(0.7f, 0.8f, 1.0f), depthT.xxx);
    float3 finalColor = normalColor * depthTint;
    finalColor = lerp(finalColor, 1.0f.xxx, (contourLine * 0.25f).xxx);
    output_image[pixel] = float4(finalColor, 1.0f);
}
