// Vulkan reference: Lumen/SSRTemporal.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer SSRTemporalParams : register(b4, space0)
{
    float params_feedback : packoffset(c0);
    uint params_halfWidth : packoffset(c0.y);
    uint params_halfHeight : packoffset(c0.z);
    uint params_fullWidth : packoffset(c0.w);
    uint params_fullHeight : packoffset(c1);
    uint params_pad0 : packoffset(c1.y);
    uint params_pad1 : packoffset(c1.z);
    uint params_pad2 : packoffset(c1.w);
};

Texture2D<float4> velocityTex : register(t2, space0);
Texture2D<float4> ssrTrace : register(t0, space0);
RWTexture2D<float4> ssrOutput : register(u3, space0);
Texture2D<float4> ssrHistory : register(t1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float2 sampleVelocity(float2 fullPixelUV)
{
    int2 dims = int2(int(params_fullWidth), int(params_fullHeight));
    float2 coord = (fullPixelUV * float2(dims)) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 frac_ = frac(coord);
    int2 maxCoord = dims - int2(1, 1);
    int2 c00 = clamp(base, int2(0, 0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1), int2(0, 0), maxCoord);
    float2 h00 = velocityTex.Load(int3(c00, 0)).xy;
    float2 h10 = velocityTex.Load(int3(c10, 0)).xy;
    float2 h01 = velocityTex.Load(int3(c01, 0)).xy;
    float2 h11 = velocityTex.Load(int3(c11, 0)).xy;
    float fx = clamp(frac_.x, 0.0f, 1.0f);
    float fy = clamp(frac_.y, 0.0f, 1.0f);
    return ((((h00 * (1.0f - fx)) * (1.0f - fy)) + ((h10 * fx) * (1.0f - fy))) + ((h01 * (1.0f - fx)) * fy)) + ((h11 * fx) * fy);
}

float4 sampleBilinear(Texture2D<float4> tex, float2 uv, int2 dims)
{
    float2 coord = (uv * float2(dims)) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 frac_ = frac(coord);
    int2 maxCoord = int2(dims.x - 1, dims.y - 1);
    int2 c00 = clamp(base, int2(0, 0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1), int2(0, 0), maxCoord);
    float4 h00 = tex.Load(int3(c00, 0));
    float4 h10 = tex.Load(int3(c10, 0));
    float4 h01 = tex.Load(int3(c01, 0));
    float4 h11 = tex.Load(int3(c11, 0));
    float fx = clamp(frac_.x, 0.0f, 1.0f);
    float fy = clamp(frac_.y, 0.0f, 1.0f);
    return ((((h00 * (1.0f - fx)) * (1.0f - fy)) + ((h10 * fx) * (1.0f - fy))) + ((h01 * (1.0f - fx)) * fy)) + ((h11 * fx) * fy);
}

float3 rgbToYCoCg(float3 c) {
    float Y  =  0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float Co =  0.5  * c.r - 0.5 * c.b;
    float Cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(Y, Co, Cg);
}

float3 ycocgToRgb(float3 c) {
    float R = c.x + c.y - c.z;
    float G = c.x + c.z;
    float B = c.x - c.y - c.z;
    return float3(R, G, B);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint3 gid = input.dispatchThreadID;
    if (((gid.x >= params_halfWidth) || (gid.y >= params_halfHeight)))
    {
        return;
    }
    int2 halfPixel = int2(gid.xy);
    float2 halfUV = (float2(halfPixel) + 0.5f.xx) / float2(float(params_halfWidth), float(params_halfHeight));
    float2 fullUV = (float2(halfPixel * int2(2, 2)) + 1.0f.xx) / float2(float(params_fullWidth), float(params_fullHeight));
    float4 current = ssrTrace.Load(int3(halfPixel, 0));
    float3 currentColor = current.xyz;
    float currentMask = current.w;
    float2 velocity = sampleVelocity(fullUV);
    float2 prevHalfUV = halfUV - velocity;
    bool _435 = (((prevHalfUV.x >= 0.0f) && (prevHalfUV.x <= 1.0f)) && (prevHalfUV.y >= 0.0f)) && (prevHalfUV.y <= 1.0f);
    bool inBounds = _435;
    if (!inBounds)
    {
        ssrOutput[halfPixel] = float4(currentColor, currentMask);
        return;
    }

    int2 histDims = int2(textureDimensions(ssrHistory, 0u));
    float2 param_1 = prevHalfUV;
    int2 param_2 = histDims;
    float4 history = sampleBilinear(ssrHistory, param_1, param_2);
    float3 historyColor = history.xyz;
    float historyMask = history.w;
    float3 nbSum = rgbToYCoCg(currentColor);
    float3 nbSum2 = nbSum * nbSum;
    int nbCount = 1;
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if ((dx == 0) && (dy == 0))
            {
                continue;
            }
            int2 nPos = halfPixel + int2(dx, dy);
            if (((((nPos.x < 0) || (nPos.y < 0)) || (nPos.x >= int(params_halfWidth))) || (nPos.y >= int(params_halfHeight))))
            {
                continue;
            }
            float3 nb = rgbToYCoCg(ssrTrace.Load(int3(nPos, 0)).xyz);
            nbSum += nb;
            nbSum2 += (nb * nb);
            nbCount++;
        }
    }
    float fCount = float(nbCount);
    float3 mean = nbSum / fCount.xxx;
    float3 variance = abs((nbSum2 / fCount.xxx) - (mean * mean));
    float3 sigma = sqrt(max(variance, 0.0f.xxx));
    float3 sigmaFloor = max(abs(mean) * 0.1f, 0.01f.xxx);
    float3 aabbMin = (mean - (sigma * 2.0f)) - sigmaFloor;
    float3 aabbMax = (mean + (sigma * 2.0f)) + sigmaFloor;
    float3 histYCoCg = rgbToYCoCg(historyColor);
    float3 clampedYCoCg = clamp(histYCoCg, aabbMin, aabbMax);
    float3 clampedHistory = ycocgToRgb(clampedYCoCg);
    float confidenceScale = lerp(0.5f, 1.0f, currentMask);
    float effectiveFeedback = params_feedback * confidenceScale;
    float3 resultColor = lerp(currentColor, clampedHistory, effectiveFeedback.xxx);
    float resultMask = lerp(currentMask, historyMask, effectiveFeedback);
    ssrOutput[halfPixel] = float4(resultColor, resultMask);
}
