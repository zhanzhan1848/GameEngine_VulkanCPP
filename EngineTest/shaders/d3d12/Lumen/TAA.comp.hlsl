static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

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

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

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

float3 rgbToYCoCg(float3 c)
{
    float y = ((0.25f * c.x) + (0.5f * c.y)) + (0.25f * c.z);
    float co = (0.5f * c.x) - (0.5f * c.z);
    float cg = (((-0.25f) * c.x) + (0.5f * c.y)) - (0.25f * c.z);
    return float3(y, co, cg);
}

float3 clipToAABB(float3 aabbMin, float3 aabbMax, float3 p)
{
    float3 center = (aabbMin + aabbMax) * 0.5f;
    float3 extents = (aabbMax - aabbMin) * 0.5f;
    float3 v = p - center;
    float3 unit = v / max(extents, 9.9999997473787516355514526367188e-05f.xxx);
    float3 absUnit = abs(unit);
    float maxComp = max(absUnit.x, max(absUnit.y, absUnit.z));
    if (maxComp <= 1.0f)
    {
        return p;
    }
    return center + (v / maxComp.xxx);
}

float3 yCoCgToRGB(float3 c)
{
    float y = c.x;
    float co = c.y;
    float cg = c.z;
    float tmp = y - cg;
    return float3(tmp + co, y + cg, tmp - co);
}

void comp_main()
{
    uint2 gid = gl_GlobalInvocationID.xy;
    int2 dims = int2(globals_screenSize.xy);
    bool _292 = gid.x >= uint(dims.x);
    bool _302;
    if (!_292)
    {
        _302 = gid.y >= uint(dims.y);
    }
    else
    {
        _302 = _292;
    }
    if (_302)
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
        float3 world = worldH.xyz / max(worldH.w, 9.9999999747524270787835121154785e-07f).xxx;
        float4 prevClip = mul(float4(world, 1.0f), globals_prevViewProj);
        float2 prevNDC = prevClip.xy / prevClip.w.xx;
        float expectedPrevDepth = prevClip.z / prevClip.w;
        historyUV = float2((prevNDC.x * 0.5f) + 0.5f, 0.5f - (prevNDC.y * 0.5f));
        uint _425_dummy_parameter;
        int2 prevDims = int2(spvTextureSize(prevDepthTex, uint(0), _425_dummy_parameter));
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
        disoccluded = expectedPrevDepth > (closestPrev + max(closestPrev * 0.100000001490116119384765625f, 0.00200000009499490261077880859375f));
    }
    float3 historyColor = currColor;
    bool _484 = globals_invHistoryValid < 0.5f;
    bool _490;
    if (_484)
    {
        _490 = historyUV.x >= 0.0f;
    }
    else
    {
        _490 = _484;
    }
    bool _496;
    if (_490)
    {
        _496 = historyUV.x <= 1.0f;
    }
    else
    {
        _496 = _490;
    }
    bool _502;
    if (_496)
    {
        _502 = historyUV.y >= 0.0f;
    }
    else
    {
        _502 = _496;
    }
    bool _508;
    if (_502)
    {
        _508 = historyUV.y <= 1.0f;
    }
    else
    {
        _508 = _502;
    }
    if (_508)
    {
        uint _514_dummy_parameter;
        int2 histDims = int2(spvTextureSize(historyColorTex, uint(0), _514_dummy_parameter));
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
            bool _556 = npixel.x < 0;
            bool _565;
            if (!_556)
            {
                _565 = npixel.x >= dims.x;
            }
            else
            {
                _565 = _556;
            }
            bool _572;
            if (!_565)
            {
                _572 = npixel.y < 0;
            }
            else
            {
                _572 = _565;
            }
            bool _581;
            if (!_572)
            {
                _581 = npixel.y >= dims.y;
            }
            else
            {
                _581 = _572;
            }
            if (_581)
            {
                continue;
            }
            float3 ncolor = currentColorTex.Load(int3(npixel, 0)).xyz;
            neighborMin = min(neighborMin, ncolor);
            neighborMax = max(neighborMax, ncolor);
        }
    }
    float3 param_2 = neighborMin;
    float3 aabbMinYC = rgbToYCoCg(param_2);
    float3 param_3 = neighborMax;
    float3 aabbMaxYC = rgbToYCoCg(param_3);
    float3 aabbCenter = (aabbMinYC + aabbMaxYC) * 0.5f;
    float3 aabbExtents = ((aabbMaxYC - aabbMinYC) * 0.5f) * 1.25f;
    aabbMinYC = aabbCenter - aabbExtents;
    aabbMaxYC = aabbCenter + aabbExtents;
    float3 param_4 = historyColor;
    float3 param_5 = aabbMinYC;
    float3 param_6 = aabbMaxYC;
    float3 param_7 = rgbToYCoCg(param_4);
    float3 historyYC = clipToAABB(param_5, param_6, param_7);
    float3 param_8 = historyYC;
    float3 historyClipped = yCoCgToRGB(param_8);
    float alpha = 0.100000001490116119384765625f;
    if ((globals_invHistoryValid > 0.5f) || disoccluded)
    {
        alpha = 1.0f;
    }
    float3 result = lerp(historyClipped, currColor, alpha.xxx);
    outputTex[pixel] = float4(result, 1.0f);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
