// Vulkan reference: Debug/SDFVisualization.frag (main).
cbuffer SDFVizUniforms : register(b3, space0)
{
    float4 uniforms_SdfOrigins[3] : packoffset(c0);
    float4 uniforms_SdfExtents[3] : packoffset(c3);
    float4 uniforms_SdfVoxelSizes[3] : packoffset(c6);
    uint4 uniforms_SdfResolutionsAndCount : packoffset(c9);
    float4 uniforms_CameraPosAndMaxDist : packoffset(c10);
    float4 uniforms_Pad0 : packoffset(c11);
    float4 uniforms_Pad1 : packoffset(c12);
    float4 uniforms_Pad2 : packoffset(c13);
    float4 uniforms_LightDirAndIntensity : packoffset(c14);
    row_major float4x4 uniforms_InvViewProj : packoffset(c15);
};

Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);

struct StageInput
{
    float2 inUv : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float fetchCascade(int cascade, float3 uvw)
{
    if (((cascade < 0) || (cascade >= int(uniforms_SdfResolutionsAndCount.w))))
    {
        return 1000000000.0f;
    }
    float res = float(uniforms_SdfResolutionsAndCount[cascade]);
    if (res <= 0.0f)
    {
        return 1000000000.0f;
    }
    float3 clamped = clamp(uvw, 0.0f.xxx, 1.0f.xxx);
    if (((any(isnan(clamped))) || (any(isinf(clamped)))))
    {
        return 1000000000.0f;
    }
    float3 coord = clamped * (res - 1.0f);
    int3 ic = int3(clamp(coord, 0.0f.xxx, (res - 1.0f).xxx));
    if (cascade == 0)
    {
        return sdf_cascade_0.Load(int4(ic, 0)).x;
    }
    if (cascade == 1)
    {
        return sdf_cascade_1.Load(int4(ic, 0)).x;
    }
    return sdf_cascade_2.Load(int4(ic, 0)).x;
}

float sampleSDF(int cascade, float3 p)
{
    float3 origin = uniforms_SdfOrigins[cascade].xyz;
    float3 extent = uniforms_SdfExtents[cascade].xyz;
    if ((((extent.x <= 0.0f) || (extent.y <= 0.0f)) || (extent.z <= 0.0f)))
    {
        return 1000000000.0f;
    }
    float3 local = (p - origin) / extent;
    if (((any(isnan(local))) || (any(isinf(local)))))
    {
        return 1000000000.0f;
    }
    if (((((((local.x < 0.0f) || (local.x > 1.0f)) || (local.y < 0.0f)) || (local.y > 1.0f)) || (local.z < 0.0f)) || (local.z > 1.0f)))
    {
        return 1000000000.0f;
    }
    int param = cascade;
    float3 param_1 = local;
    return fetchCascade(param, param_1);
}

float mapSDF(float3 p)
{
    float d = 1000000000.0f;
    uint count = uniforms_SdfResolutionsAndCount.w;
    for (int c = 0; c < int(count); c++)
    {
        int param = c;
        float3 param_1 = p;
        float dc = sampleSDF(param, param_1);
        d = min(d, dc);
    }
    return d;
}

float3 estimateNormal(float3 p)
{
    float voxel = uniforms_SdfVoxelSizes[0].x;
    float3 extent0 = uniforms_SdfExtents[0].xyz;
    float e = max(voxel * 2.0f, length(extent0) * 0.002f);
    float2 h = float2(0.5f, -0.5f) * e;
    int param = 0;
    float3 param_1 = p + h.xyy;
    int param_2 = 0;
    float3 param_3 = p + h.yyx;
    int param_4 = 0;
    float3 param_5 = p + h.yxy;
    int param_6 = 0;
    float3 param_7 = p + h.xxx;
    return normalize((((h.xyy * sampleSDF(param, param_1)) + (h.yyx * sampleSDF(param_2, param_3))) + (h.yxy * sampleSDF(param_4, param_5))) + (h.xxx * sampleSDF(param_6, param_7)));
}

float3 shade(float3 normal, float3 baseColor, float3 lightDir, float intensity) {
    float diff = max(dot(normal, -lightDir), 0.0);
    float3 ambient = baseColor * 0.25;
    return ambient + baseColor * diff * intensity;
}

float3 depthHeat(float t) {
    t = clamp(t, 0.0, 1.0);
    float3 c0 = float3(1.0, 0.0, 0.0);
    float3 c1 = float3(1.0, 1.0, 0.0);
    float3 c2 = float3(0.0, 1.0, 0.0);
    float3 c3 = float3(0.0, 1.0, 1.0);
    float3 c4 = float3(0.0, 0.0, 1.0);
    if (t < 0.25)       return lerp(c0, c1, t / 0.25);
    else if (t < 0.50)  return lerp(c1, c2, (t - 0.25) / 0.25);
    else if (t < 0.75)  return lerp(c2, c3, (t - 0.50) / 0.25);
    else                return lerp(c3, c4, (t - 0.75) / 0.25);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 camPos = uniforms_CameraPosAndMaxDist.xyz;
    float maxDist = uniforms_CameraPosAndMaxDist.w;
    float2 ndc = (input.inUv * 2.0f) - 1.0f.xx;
    float4 nearH = mul(float4(ndc, 0.0f, 1.0f), uniforms_InvViewProj);
    float4 farH = mul(float4(ndc, 1.0f, 1.0f), uniforms_InvViewProj);
    if (((abs(nearH.w) < 1e-06f) || (abs(farH.w) < 1e-06f)))
    {
        output.outColor = float4(0.5f, 0.0f, 0.0f, 1.0f);
        return output;
    }
    float3 nearPos = nearH.xyz / nearH.w.xxx;
    float3 farPos = farH.xyz / farH.w.xxx;
    float3 rayDir = normalize(farPos - nearPos);
    if (((any(isnan(rayDir))) || (dot(rayDir, rayDir) < 1e-12f)))
    {
        output.outColor = float4(0.5f, 0.0f, 0.0f, 1.0f);
        return output;
    }
    float t = 0.0f;
    bool hit = false;
    for (uint i = 0u; i < 3u; i++)
    {
        float3 p = camPos + (rayDir * t);
        float3 param = p;
        float d = mapSDF(param);
        if (((isnan(d)) || (isinf(d))))
        {
            break;
        }
        if (d < 0.01f)
        {
            hit = true;
            break;
        }
        if (d > 100000000.0f)
        {
            break;
        }
        t += (d * 0.9f);
        if (t > maxDist)
        {
            break;
        }
    }
    if (!hit)
    {
        float skyT = clamp((rayDir.y * 0.5f) + 0.5f, 0.0f, 1.0f);
        float3 sky = lerp(float3(0.05f, 0.05f, 0.08f), float3(0.25f, 0.28f, 0.35f), skyT.xxx);
        output.outColor = float4(sky, 1.0f);
        return output;
    }
    float3 hitPos = camPos + (rayDir * t);
    float3 param_1 = hitPos;
    float3 normal = estimateNormal(param_1);
    if (((any(isnan(normal))) || (dot(normal, normal) < 0.0001f)))
    {
        normal = float3(0.0f, 1.0f, 0.0f);
    }
    float3 lightDir = normalize(uniforms_LightDirAndIntensity.xyz);
    float lightI = uniforms_LightDirAndIntensity.w;
    float3 normalColor = (normal * 0.5f) + 0.5f.xxx;
    float3 param_2 = normal;
    float3 param_3 = normalColor * 0.6f;
    float3 param_4 = lightDir;
    float param_5 = lightI;
    float3 lit = shade(param_2, param_3, param_4, param_5);
    float depthT = t / maxDist;
    float param_6 = depthT;
    float3 _655 = depthHeat(param_6);
    float3 heat = _655 * 0.35f;
    output.outColor = float4(lit + heat, 1.0f);
    return output;
}
