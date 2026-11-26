// Vulkan reference: Debug/ProbeSphereWRC.frag (main).
#include "../Common/HalfPacking.hlsli"

ByteAddressBuffer wrc : register(t1, space0);

struct StageInput
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
    nointerpolation float vState : TEXCOORD4;
    nointerpolation float vMode : TEXCOORD5;
    nointerpolation float vTotal : TEXCOORD6;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 N = normalize(input.vNormal);
    uint metaBase = (uint(input.vTotal) * 128u) + (input.vProbe * 4u);
    uint rbase = input.vProbe * 128u;
    uint state = wrc.Load((metaBase + 2u) * 4);
    bool valid = (state & 1u) != 0u;
    bool inGeo = (state & 2u) != 0u;
    if (((input.vMode > 1.5f) && (input.vMode < 2.5f)))
    {
        if (!valid)
        {
            output.outColor = float4(0.1f, 0.1f, 0.11f, 1.0f);
            return output;
        }
        uint consume = (state >> 16u) & 65535u;
        float t = clamp(float(consume) / 64.0f, 0.0f, 1.0f);
        float3 heat = lerp(float3(0.15f, 0.25f, 0.85f), float3(0.95f, 0.85f, 0.2f), t.xxx);
        output.outColor = float4(heat, 1.0f);
        return output;
    }
    float3 acc = 0.0f.xxx;
    float wsum = 0.0f;
    float lumSum = 0.0f;
    float lumSqSum = 0.0f;
    for (uint d = 0u; d < 64u; d++)
    {
        float y = 1.0f - ((2.0f * (float(d) + 0.5f)) / 64.0f);
        float r = sqrt(max(1.0f - (y * y), 0.0f));
        float a = 2.3999631f * float(d);
        float3 dir = float3(cos(a) * r, y, sin(a) * r);
        uint lo = wrc.Load(((rbase + (d * 2u)) + 0u) * 4);
        uint hi = wrc.Load(((rbase + (d * 2u)) + 1u) * 4);
        float3 rad = float3(unpackHalf2(lo), unpackHalf2(hi).x);
        float lum = dot(rad, float3(0.2126f, 0.7152f, 0.0722f));
        lumSum += lum;
        lumSqSum += (lum * lum);
        float w = max(dot(N, dir), 0.0f);
        if (w <= 0.0f)
        {
            continue;
        }
        w *= w;
        acc += (rad * w);
        wsum += w;
    }
    if (input.vMode > 2.5f)
    {
        if (!valid)
        {
            output.outColor = float4(0.1f, 0.1f, 0.11f, 1.0f);
            return output;
        }
        float meanL = lumSum / 64.0f;
        float varL = max((lumSqSum / 64.0f) - (meanL * meanL), 0.0f);
        float _247;
        if (meanL > 0.0001f)
        {
            _247 = varL / (meanL * meanL);
        }
        else
        {
            _247 = 0.0f;
        }
        float ratio = _247;
        float t_1 = clamp(ratio / 0.5f, 0.0f, 1.0f);
        float3 heat_1 = lerp(float3(0.35f, 0.4f, 0.55f), float3(0.15f, 0.85f, 0.3f), t_1.xxx);
        if (inGeo)
        {
            heat_1 = lerp(heat_1, float3(0.55f, 0.08f, 0.05f), 0.5f.xxx);
        }
        output.outColor = float4(heat_1, 1.0f);
        return output;
    }
    float3 _289;
    if (wsum > 0.0f)
    {
        _289 = acc / wsum.xxx;
    }
    else
    {
        _289 = 0.0f.xxx;
    }
    float3 rgb = _289;
    rgb /= (1.0f + max(max(rgb.x, rgb.y), rgb.z)).xxx;
    if (!valid)
    {
        rgb = float3(0.1f, 0.1f, 0.11f);
    }
    else
    {
        bool _321;
        if (inGeo)
        {
            _321 = input.vMode < 0.5f;
        }
        else
        {
            _321 = inGeo;
        }
        if (_321)
        {
            rgb = lerp(rgb, float3(0.55f, 0.08f, 0.05f), 0.5f.xxx);
        }
    }
    output.outColor = float4(clamp(rgb, 0.0f.xxx, 1.0f.xxx), 1.0f);
    return output;
}
