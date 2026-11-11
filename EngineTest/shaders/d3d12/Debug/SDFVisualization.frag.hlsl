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

static float2 inUv;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float fetchCascade(int cascade, float3 uvw)
{
    bool _43 = cascade < 0;
    bool _65;
    if (!_43)
    {
        _65 = cascade >= int(uniforms_SdfResolutionsAndCount.w);
    }
    else
    {
        _65 = _43;
    }
    if (_65)
    {
        return 1000000000.0f;
    }
    float res = float(uniforms_SdfResolutionsAndCount[cascade]);
    if (res <= 0.0f)
    {
        return 1000000000.0f;
    }
    float3 clamped = clamp(uvw, 0.0f.xxx, 1.0f.xxx);
    bool _90 = any(isnan(clamped));
    bool _97;
    if (!_90)
    {
        _97 = any(isinf(clamped));
    }
    else
    {
        _97 = _90;
    }
    if (_97)
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
    bool _159 = extent.x <= 0.0f;
    bool _167;
    if (!_159)
    {
        _167 = extent.y <= 0.0f;
    }
    else
    {
        _167 = _159;
    }
    bool _175;
    if (!_167)
    {
        _175 = extent.z <= 0.0f;
    }
    else
    {
        _175 = _167;
    }
    if (_175)
    {
        return 1000000000.0f;
    }
    float3 local = (p - origin) / extent;
    bool _187 = any(isnan(local));
    bool _194;
    if (!_187)
    {
        _194 = any(isinf(local));
    }
    else
    {
        _194 = _187;
    }
    if (_194)
    {
        return 1000000000.0f;
    }
    bool _200 = local.x < 0.0f;
    bool _207;
    if (!_200)
    {
        _207 = local.x > 1.0f;
    }
    else
    {
        _207 = _200;
    }
    bool _214;
    if (!_207)
    {
        _214 = local.y < 0.0f;
    }
    else
    {
        _214 = _207;
    }
    bool _221;
    if (!_214)
    {
        _221 = local.y > 1.0f;
    }
    else
    {
        _221 = _214;
    }
    bool _228;
    if (!_221)
    {
        _228 = local.z < 0.0f;
    }
    else
    {
        _228 = _221;
    }
    bool _235;
    if (!_228)
    {
        _235 = local.z > 1.0f;
    }
    else
    {
        _235 = _228;
    }
    if (_235)
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
    float e = max(voxel * 2.0f, length(extent0) * 0.00200000009499490261077880859375f);
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

float3 shade(float3 normal, float3 baseColor, float3 lightDir, float intensity)
{
    float diff = max(dot(normal, -lightDir), 0.0f);
    float3 ambient = baseColor * 0.25f;
    return ambient + ((baseColor * diff) * intensity);
}

float3 depthHeat(inout float t)
{
    t = clamp(t, 0.0f, 1.0f);
    float3 c0 = float3(1.0f, 0.0f, 0.0f);
    float3 c1 = float3(1.0f, 1.0f, 0.0f);
    float3 c2 = float3(0.0f, 1.0f, 0.0f);
    float3 c3 = float3(0.0f, 1.0f, 1.0f);
    float3 c4 = float3(0.0f, 0.0f, 1.0f);
    if (t < 0.25f)
    {
        return lerp(c0, c1, (t / 0.25f).xxx);
    }
    else
    {
        if (t < 0.5f)
        {
            return lerp(c1, c2, ((t - 0.25f) / 0.25f).xxx);
        }
        else
        {
            if (t < 0.75f)
            {
                return lerp(c2, c3, ((t - 0.5f) / 0.25f).xxx);
            }
            else
            {
                return lerp(c3, c4, ((t - 0.75f) / 0.25f).xxx);
            }
        }
    }
}

void frag_main()
{
    float3 camPos = uniforms_CameraPosAndMaxDist.xyz;
    float maxDist = uniforms_CameraPosAndMaxDist.w;
    float2 ndc = (inUv * 2.0f) - 1.0f.xx;
    float4 nearH = mul(float4(ndc, 0.0f, 1.0f), uniforms_InvViewProj);
    float4 farH = mul(float4(ndc, 1.0f, 1.0f), uniforms_InvViewProj);
    bool _464 = abs(nearH.w) < 9.9999999747524270787835121154785e-07f;
    bool _472;
    if (!_464)
    {
        _472 = abs(farH.w) < 9.9999999747524270787835121154785e-07f;
    }
    else
    {
        _472 = _464;
    }
    if (_472)
    {
        outColor = float4(0.5f, 0.0f, 0.0f, 1.0f);
        return;
    }
    float3 nearPos = nearH.xyz / nearH.w.xxx;
    float3 farPos = farH.xyz / farH.w.xxx;
    float3 rayDir = normalize(farPos - nearPos);
    bool _500 = any(isnan(rayDir));
    bool _509;
    if (!_500)
    {
        _509 = dot(rayDir, rayDir) < 9.9999999600419720025001879548654e-13f;
    }
    else
    {
        _509 = _500;
    }
    if (_509)
    {
        outColor = float4(0.5f, 0.0f, 0.0f, 1.0f);
        return;
    }
    float t = 0.0f;
    bool hit = false;
    for (uint i = 0u; i < 3u; i++)
    {
        float3 p = camPos + (rayDir * t);
        float3 param = p;
        float d = mapSDF(param);
        bool _536 = isnan(d);
        bool _542;
        if (!_536)
        {
            _542 = isinf(d);
        }
        else
        {
            _542 = _536;
        }
        if (_542)
        {
            break;
        }
        if (d < 0.00999999977648258209228515625f)
        {
            hit = true;
            break;
        }
        if (d > 100000000.0f)
        {
            break;
        }
        t += (d * 0.89999997615814208984375f);
        if (t > maxDist)
        {
            break;
        }
    }
    if (!hit)
    {
        float skyT = clamp((rayDir.y * 0.5f) + 0.5f, 0.0f, 1.0f);
        float3 sky = lerp(float3(0.0500000007450580596923828125f, 0.0500000007450580596923828125f, 0.07999999821186065673828125f), float3(0.25f, 0.2800000011920928955078125f, 0.3499999940395355224609375f), skyT.xxx);
        outColor = float4(sky, 1.0f);
        return;
    }
    float3 hitPos = camPos + (rayDir * t);
    float3 param_1 = hitPos;
    float3 normal = estimateNormal(param_1);
    bool _610 = any(isnan(normal));
    bool _619;
    if (!_610)
    {
        _619 = dot(normal, normal) < 9.9999997473787516355514526367188e-05f;
    }
    else
    {
        _619 = _610;
    }
    if (_619)
    {
        normal = float3(0.0f, 1.0f, 0.0f);
    }
    float3 lightDir = normalize(uniforms_LightDirAndIntensity.xyz);
    float lightI = uniforms_LightDirAndIntensity.w;
    float3 normalColor = (normal * 0.5f) + 0.5f.xxx;
    float3 param_2 = normal;
    float3 param_3 = normalColor * 0.60000002384185791015625f;
    float3 param_4 = lightDir;
    float param_5 = lightI;
    float3 lit = shade(param_2, param_3, param_4, param_5);
    float depthT = t / maxDist;
    float param_6 = depthT;
    float3 _655 = depthHeat(param_6);
    float3 heat = _655 * 0.3499999940395355224609375f;
    outColor = float4(lit + heat, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUv = stage_input.inUv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
