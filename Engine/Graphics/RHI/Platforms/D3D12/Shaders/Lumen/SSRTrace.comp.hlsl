// Vulkan reference: Lumen/SSRTrace.comp (main).
#include "../Common/TextureDimensions.hlsli"

struct TraceResult
{
    bool hit;
    float2 hitUV;
    float hitDist;
};

cbuffer SSRTraceParams : register(b5, space0)
{
    row_major float4x4 params_invProj : packoffset(c0);
    row_major float4x4 params_proj : packoffset(c4);
    float4 params_screenSize : packoffset(c8);
    float4 params_halfScreenSize : packoffset(c9);
    float params_maxDistance : packoffset(c10);
    float params_thickness : packoffset(c10.y);
    float params_nearPlane : packoffset(c10.z);
    float params_farPlane : packoffset(c10.w);
    uint params_hzbMipLevels : packoffset(c11);
    uint params_frameIndex : packoffset(c11.y);
    uint params_pad0 : packoffset(c11.z);
    uint params_pad1 : packoffset(c11.w);
};

Texture2D<float4> depthTex : register(t0, space0);
Texture2D<float4> colorTex : register(t2, space0);
Texture2D<float4> hzbTex : register(t1, space0);
RWTexture2D<float4> ssrOutput : register(u4, space0);
Texture2D<float4> ormTex : register(t3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float ndcDepthToLinear(float ndcDepth)
{
    return ((2.0f * params_nearPlane) * params_farPlane) / ((params_farPlane + params_nearPlane) - (ndcDepth * (params_farPlane - params_nearPlane)));
}

float3 reconstructViewPos(float2 uv, float ndcDepth)
{
    float ndcX = (uv.x * 2.0f) - 1.0f;
    float ndcY = 1.0f - (uv.y * 2.0f);
    float4 clipPos = float4(ndcX, ndcY, ndcDepth, 1.0f);
    float4 viewPos4 = mul(clipPos, params_invProj);
    float3 vp = viewPos4.xyz / viewPos4.w.xxx;
    return float3(vp.x, vp.y, -vp.z);
}

float3 reconstructViewNormal(int2 pixel, float2 uv, float depth)
{
    float2 ts = params_screenSize.zw;
    int px = clamp(pixel.x, 1, int(params_screenSize.x) - 2);
    int py = clamp(pixel.y, 1, int(params_screenSize.y) - 2);
    float dL = depthTex.Load(int3(int2(px - 1, py), 0)).x;
    float dR = depthTex.Load(int3(int2(px + 1, py), 0)).x;
    float dU = depthTex.Load(int3(int2(px, py - 1), 0)).x;
    float dD = depthTex.Load(int3(int2(px, py + 1), 0)).x;
    float2 param = float2(uv.x - ts.x, uv.y);
    float param_1 = dL;
    float3 pL = reconstructViewPos(param, param_1);
    float2 param_2 = float2(uv.x + ts.x, uv.y);
    float param_3 = dR;
    float3 pR = reconstructViewPos(param_2, param_3);
    float2 param_4 = float2(uv.x, uv.y - ts.y);
    float param_5 = dU;
    float3 pU = reconstructViewPos(param_4, param_5);
    float2 param_6 = float2(uv.x, uv.y + ts.y);
    float param_7 = dD;
    float3 pD = reconstructViewPos(param_6, param_7);
    float3 n = normalize(cross(pD - pU, pR - pL));
    if (n.z > 0.0f)
    {
        n = -n;
    }
    return n;
}

float3 viewToScreen(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos.x, viewPos.y, -viewPos.z, 1.0f), params_proj);
    float3 ndc = clipPos.xyz / clipPos.w.xxx;
    return float3((ndc.x * 0.5f) + 0.5f, 0.5f - (0.5f * ndc.y), ndc.z);
}

TraceResult traceRayHZB(float3 rayOriginView, float3 rayDirView, float2 sourceUV)
{
    TraceResult result;
    result.hit = false;
    float stepSize = params_maxDistance / 128.0f;
    float t = 0.0f;
    uint mip = 1u;
    for (uint step_i = 0u; step_i < 128u; step_i++)
    {
        t += stepSize;
        if (t > params_maxDistance)
        {
            break;
        }
        float3 rayPos = rayOriginView + (rayDirView * t);
        float3 param = rayPos;
        float3 screen = viewToScreen(param);
        float2 sampleUV = screen.xy;
        if (((((sampleUV.x < 0.0f) || (sampleUV.x > 1.0f)) || (sampleUV.y < 0.0f)) || (sampleUV.y > 1.0f)))
        {
            break;
        }
        if (rayPos.z < params_nearPlane)
        {
            continue;
        }
        float dpx = abs(sampleUV.x - sourceUV.x) * params_screenSize.x;
        float dpy = abs(sampleUV.y - sourceUV.y) * params_screenSize.y;
        if (max(dpx, dpy) < 4.0f)
        {
            continue;
        }

        int2 mipDims = int2(textureDimensions(hzbTex, uint(int(mip))));
        int2 mipCoord = clamp(int2(sampleUV * float2(mipDims)), int2(0, 0), mipDims - int2(1, 1));
        float sceneDepthNDC = hzbTex.Load(int3(mipCoord, int(mip))).x;
        float param_1 = sceneDepthNDC;
        float sceneDepthLinear = ndcDepthToLinear(param_1);
        float rayDepthLinear = rayPos.z;
        if (rayDepthLinear > sceneDepthLinear)
        {
            if (mip == 0u)
            {
                if (rayDepthLinear <= (sceneDepthLinear + params_thickness))
                {
                    result.hit = true;
                    result.hitUV = sampleUV;
                    result.hitDist = t;
                    break;
                }
            }
            else
            {
                t -= stepSize;
                mip--;
            }
        }
        else
        {
            mip = min((mip + 1u), (params_hzbMipLevels - 1u));
        }
    }
    return result;
}

float3 sampleColorDepthAware(float2 uv)
{

    int2 dims = int2(textureDimensions(colorTex, 0u));
    float2 coord = (uv * float2(dims)) - 0.5f.xx;
    int bx = clamp(int(floor(coord.x)), 0, dims.x - 1);
    int by = clamp(int(floor(coord.y)), 0, dims.y - 1);
    int bx1 = min((bx + 1), (dims.x - 1));
    int by1 = min((by + 1), (dims.y - 1));
    int2 p00 = int2(bx, by);
    int2 p10 = int2(bx1, by);
    int2 p01 = int2(bx, by1);
    int2 p11 = int2(bx1, by1);
    float param = depthTex.Load(int3(p00, 0)).x;
    float d00 = ndcDepthToLinear(param);
    float param_1 = depthTex.Load(int3(p10, 0)).x;
    float d10 = ndcDepthToLinear(param_1);
    float param_2 = depthTex.Load(int3(p01, 0)).x;
    float d01 = ndcDepthToLinear(param_2);
    float param_3 = depthTex.Load(int3(p11, 0)).x;
    float d11 = ndcDepthToLinear(param_3);
    float dMin = min(min(d00, d10), min(d01, d11));
    float dMax = max(max(d00, d10), max(d01, d11));
    if ((dMax - dMin) > params_thickness)
    {
        return 0.0f.xxx;
    }
    float3 c00 = colorTex.Load(int3(p00, 0)).xyz;
    float3 c10 = colorTex.Load(int3(p10, 0)).xyz;
    float3 c01 = colorTex.Load(int3(p01, 0)).xyz;
    float3 c11 = colorTex.Load(int3(p11, 0)).xyz;
    float fx = clamp(frac(coord.x), 0.0f, 1.0f);
    float fy = clamp(frac(coord.y), 0.0f, 1.0f);
    return lerp(lerp(c00, c10, fx.xxx), lerp(c01, c11, fx.xxx), fy.xxx);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint3 gid = input.dispatchThreadID;
    uint2 halfDims = uint2(params_halfScreenSize.xy);
    if (((gid.x >= halfDims.x) || (gid.y >= halfDims.y)))
    {
        return;
    }
    uint2 halfPos = gid.xy;
    int2 fullPos = int2(halfPos) * int2(2, 2);
    int2 fullDims = int2(params_screenSize.xy);
    if (((fullPos.x >= fullDims.x) || (fullPos.y >= fullDims.y)))
    {
        ssrOutput[int2(halfPos)] = 0.0f.xxxx;
        return;
    }
    float depth = depthTex.Load(int3(fullPos, 0)).x;
    float param = depth;
    float linearDepth = ndcDepthToLinear(param);
    if ((((depth >= 0.9999f) || (linearDepth < params_nearPlane)) || (linearDepth > (params_farPlane * 0.999f))))
    {
        ssrOutput[int2(halfPos)] = 0.0f.xxxx;
        return;
    }
    float roughness = ormTex.Load(int3(fullPos, 0)).y;
    if (roughness > 0.98f)
    {
        ssrOutput[int2(halfPos)] = 0.0f.xxxx;
        return;
    }
    float2 pixelUV = (float2(fullPos) + 0.5f.xx) / params_screenSize.xy;
    float2 param_1 = pixelUV;
    float param_2 = depth;
    float3 viewPos = reconstructViewPos(param_1, param_2);
    int2 param_3 = fullPos;
    float2 param_4 = pixelUV;
    float param_5 = depth;
    float3 viewNormal = reconstructViewNormal(param_3, param_4, param_5);
    float3 toCamera = normalize(-viewPos);
    float3 reflDir = reflect(-toCamera, viewNormal);
    float NdotV = dot(reflDir, viewNormal);
    if (NdotV <= 0.0f)
    {
        ssrOutput[int2(halfPos)] = 0.0f.xxxx;
        return;
    }
    float rayOffset = max(0.05f, 0.05f / max(NdotV, 0.3f));
    float3 rayOrigin = viewPos + (viewNormal * rayOffset);
    float3 param_6 = rayOrigin;
    float3 param_7 = reflDir;
    TraceResult tr = traceRayHZB(param_6, param_7, pixelUV);
    if (tr.hit)
    {
        uint hitPxX = clamp(uint(tr.hitUV.x * params_screenSize.x), 1u, uint(params_screenSize.x) - 2u);
        uint hitPxY = clamp(uint(tr.hitUV.y * params_screenSize.y), 1u, uint(params_screenSize.y) - 2u);
        int2 hitPx = int2(int(hitPxX), int(hitPxY));
        float hitDepth = depthTex.Load(int3(hitPx, 0)).x;
        int2 param_9 = hitPx;
        float2 param_10 = tr.hitUV;
        float3 hitNormal = reconstructViewNormal(param_9, param_10, hitDepth);
        if (dot(hitNormal, -reflDir) <= 0.0f)
        {
            ssrOutput[int2(halfPos)] = 0.0f.xxxx;
            return;
        }
        float3 hitColor = sampleColorDepthAware(tr.hitUV);
        float distFade = 1.0f - smoothstep(params_maxDistance * 0.6f, params_maxDistance, tr.hitDist);
        float2 edgeDist = min(tr.hitUV, 1.0f.xx - tr.hitUV);
        float edgeFade = smoothstep(0.0f, 0.1f, min(edgeDist.x, edgeDist.y));
        float grazingFade = smoothstep(0.2f, 0.5f, NdotV);
        float strength = (distFade * edgeFade) * grazingFade;
        ssrOutput[int2(halfPos)] = float4(hitColor * strength, strength);
    }
    else
    {
        ssrOutput[int2(halfPos)] = 0.0f.xxxx;
    }
}
