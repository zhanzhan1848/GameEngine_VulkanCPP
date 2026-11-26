// Vulkan reference: Lumen/TAA.comp (taa_main).
#include "../Common/TextureDimensions.hlsli"

cbuffer TAAGlobals : register(b4, space0)
{
    float4 globals_screenSize : packoffset(c0);
    float globals_invHistoryValid : packoffset(c1);
    uint globals_depthValid : packoffset(c1.y);
    uint globals_pad1 : packoffset(c1.z);
    uint globals_pad2 : packoffset(c1.w);
    row_major float4x4 globals_currInvViewProj : packoffset(c2);
    row_major float4x4 globals_prevViewProj : packoffset(c6);
};

Texture2D<float4> currentColorTex : register(t0, space0);
Texture2D<float4> velocityTex : register(t2, space0);
Texture2D<float4> currDepthTex : register(t5, space0);
Texture2D<float4> prevDepthTex : register(t6, space0);
Texture2D<float4> historyColorTex : register(t1, space0);
RWTexture2D<float4> outputTex : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 sampleBilinear(Texture2D<float4> tex, float2 uv, int2 dims)
{
    float2 coord = (uv * float2(dims)) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 frac_ = frac(coord);
    int2 maxCoord = int2(dims.x - 1, dims.y - 1);
    int2 c00 = clamp(base, int2(0, 0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1), int2(0, 0), maxCoord);
    float3 h00 = tex.Load(int3(c00, 0)).xyz;
    float3 h10 = tex.Load(int3(c10, 0)).xyz;
    float3 h01 = tex.Load(int3(c01, 0)).xyz;
    float3 h11 = tex.Load(int3(c11, 0)).xyz;
    float fx = clamp(frac_.x, 0.0f, 1.0f);
    float fy = clamp(frac_.y, 0.0f, 1.0f);
    return ((((h00 * (1.0f - fx)) * (1.0f - fy)) + ((h10 * fx) * (1.0f - fy))) + ((h01 * (1.0f - fx)) * fy)) + ((h11 * fx) * fy);
}

float3 rgbToYCoCg(float3 c) {
    float y  = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float co = 0.5 * c.r - 0.5 * c.b;
    float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}

float3 clipToAABB(float3 aabbMin, float3 aabbMax, float3 p) {
    float3 center  = 0.5 * (aabbMin + aabbMax);
    float3 extents = 0.5 * (aabbMax - aabbMin);
    float3 v = p - center;
    float3 unit = v / max(extents, (float3)(0.0001));
    float3 absUnit = abs(unit);
    float maxComp = max(absUnit.x, max(absUnit.y, absUnit.z));
    if (maxComp <= 1.0) {
        return p;  // inside AABB
    }
    return center + v / maxComp;
}

float3 yCoCgToRGB(float3 c) {
    float y  = c.x;
    float co = c.y;
    float cg = c.z;
    float tmp = y - cg;
    return float3(tmp + co, y + cg, tmp - co);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;
    int2 dims = int2(globals_screenSize.xy);
    if (((gid.x >= uint(dims.x)) || (gid.y >= uint(dims.y))))
    {
        return;
    }
    int2 pixel = int2(gid);
    float3 currColor = currentColorTex.Load(int3(pixel, 0)).xyz;
    float2 velocity = velocityTex.Load(int3(pixel, 0)).xy;
    float2 currUV = (float2(pixel) + 0.5f.xx) * globals_screenSize.zw;
    float2 historyUV = currUV - (float2(velocity.x, -velocity.y) * 0.5f);
    bool disoccluded = false;
    if (globals_depthValid > 0u)
    {
        float currDepth = currDepthTex.Load(int3(pixel, 0)).x;
        float3 ndc = float3((currUV.x * 2.0f) - 1.0f, 1.0f - (currUV.y * 2.0f), currDepth);
        float4 worldH = mul(float4(ndc, 1.0f), globals_currInvViewProj);
        float3 world = worldH.xyz / max(worldH.w, 1e-06f).xxx;
        float4 prevClip = mul(float4(world, 1.0f), globals_prevViewProj);
        float2 prevNDC = prevClip.xy / prevClip.w.xx;
        float expectedPrevDepth = prevClip.z / prevClip.w;
        historyUV = float2((prevNDC.x * 0.5f) + 0.5f, 0.5f - (prevNDC.y * 0.5f));

        int2 prevDims = int2(textureDimensions(prevDepthTex, 0u));
        int2 prevPixel = int2(historyUV * float2(prevDims));
        float closestPrev = 1.0f;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int2 sp = clamp(prevPixel + int2(dx, dy), int2(0, 0), prevDims - int2(1, 1));
                closestPrev = min(closestPrev, prevDepthTex.Load(int3(sp, 0)).x);
            }
        }
        disoccluded = expectedPrevDepth > (closestPrev + max(closestPrev * 0.1f, 0.002f));
    }
    float3 historyColor = currColor;
    if ((((((globals_invHistoryValid < 0.5f) && (historyUV.x >= 0.0f)) && (historyUV.x <= 1.0f)) && (historyUV.y >= 0.0f)) && (historyUV.y <= 1.0f)))
    {

        int2 histDims = int2(textureDimensions(historyColorTex, 0u));
        float2 param = historyUV;
        int2 param_1 = histDims;
        historyColor = sampleBilinear(historyColorTex, param, param_1);
    }
    float3 neighborMin = currColor;
    float3 neighborMax = currColor;
    for (int dy_1 = -1; dy_1 <= 1; dy_1++)
    {
        for (int dx_1 = -1; dx_1 <= 1; dx_1++)
        {
            if ((dx_1 == 0) && (dy_1 == 0))
            {
                continue;
            }
            int2 npixel = pixel + int2(dx_1, dy_1);
            if (((((npixel.x < 0) || (npixel.x >= dims.x)) || (npixel.y < 0)) || (npixel.y >= dims.y)))
            {
                continue;
            }
            float3 ncolor = currentColorTex.Load(int3(npixel, 0)).xyz;
            neighborMin = min(neighborMin, ncolor);
            neighborMax = max(neighborMax, ncolor);
        }
    }
    float3 aabbMinYC = rgbToYCoCg(neighborMin);
    float3 aabbMaxYC = rgbToYCoCg(neighborMax);
    float3 aabbCenter = (aabbMinYC + aabbMaxYC) * 0.5f;
    float3 aabbExtents = ((aabbMaxYC - aabbMinYC) * 0.5f) * 1.25f;
    aabbMinYC = aabbCenter - aabbExtents;
    aabbMaxYC = aabbCenter + aabbExtents;
    float3 param_4 = historyColor;
    float3 param_5 = aabbMinYC;
    float3 param_6 = aabbMaxYC;
    float3 param_7 = rgbToYCoCg(param_4);
    float3 historyYC = clipToAABB(param_5, param_6, param_7);
    float3 historyClipped = yCoCgToRGB(historyYC);
    float alpha = 0.1f;
    if ((globals_invHistoryValid > 0.5f) || disoccluded)
    {
        alpha = 1.0f;
    }
    float3 result = lerp(historyClipped, currColor, alpha.xxx);
    outputTex[pixel] = float4(result, 1.0f);
}
