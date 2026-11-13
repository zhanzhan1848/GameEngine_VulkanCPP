static const uint3 gl_WorkGroupSize = uint3(16u, 16u, 1u);

cbuffer AdaptParamsBlock : register(b3, space0)
{
    float adaptParams_rateUp : packoffset(c0);
    float adaptParams_rateDown : packoffset(c0.y);
    float adaptParams_minLogLum : packoffset(c0.z);
    float adaptParams_maxLogLum : packoffset(c0.w);
    float adaptParams_brightClip : packoffset(c1);
    float adaptParams_cutSnap : packoffset(c1.y);
    float2 adaptParams_pad : packoffset(c1.z);
};

Texture2D<float4> hdr_scene : register(t0, space0);
Texture2D<float4> prev_state : register(t1, space0);
RWTexture2D<float> new_state : register(u2, space0);

static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

groupshared float sLogLum[256];

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

float luminance(float3 c)
{
    bool _17 = any(isnan(c));
    bool _24;
    if (!_17)
    {
        _24 = any(isinf(c));
    }
    else
    {
        _24 = _17;
    }
    if (_24)
    {
        return 0.0f;
    }
    return dot(max(c, 0.0f.xxx), float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
}

void comp_main()
{
    int2 gid = int2(gl_LocalInvocationID.xy);
    uint _57_dummy_parameter;
    int2 dims = int2(spvTextureSize(hdr_scene, uint(0), _57_dummy_parameter));
    int2 tileSize = max((dims / int2(16, 16)), int2(1, 1));
    int2 tileOrigin = gid * tileSize;
    float sumLog = 0.0f;
    uint count = 0u;
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            int2 p = (tileOrigin + (int2(x, y) * int2(8, 8))) + (tileSize / int2(8, 8));
            p = clamp(p, int2(0, 0), dims - int2(1, 1));
            float3 param = hdr_scene.Load(int3(p, 0)).xyz;
            float l = luminance(param);
            l = min(l, adaptParams_brightClip);
            sumLog += log2(max(l, 9.9999997473787516355514526367188e-05f));
            count++;
        }
    }
    sLogLum[(gid.y * 16) + gid.x] = sumLog / float(count);
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s = s >> 1u)
    {
        if (uint((gid.y * 16) + gid.x) < s)
        {
            int _184 = (gid.y * 16) + gid.x;
            sLogLum[_184] += sLogLum[uint((gid.y * 16) + gid.x) + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (all(bool2(gid.x == int2(0, 0).x, gid.y == int2(0, 0).y)))
    {
        float avgLogLum = sLogLum[0] / 256.0f;
        float prev = prev_state.Load(int3(int2(0, 0), 0)).x;
        if (!((prev > (-64.0f)) && (prev < 64.0f)))
        {
            prev = 0.0f;
        }
        if (!((avgLogLum > (-64.0f)) && (avgLogLum < 64.0f)))
        {
            avgLogLum = prev;
        }
        float delta = avgLogLum - prev;
        float _244;
        if (delta > 0.0f)
        {
            _244 = adaptParams_rateUp;
        }
        else
        {
            _244 = adaptParams_rateDown;
        }
        float rate = _244;
        if (abs(delta) > adaptParams_cutSnap)
        {
            rate = 1.0f;
        }
        float adapted = lerp(prev, avgLogLum, clamp(rate, 0.0f, 1.0f));
        adapted = clamp(adapted, adaptParams_minLogLum, adaptParams_maxLogLum);
        new_state[int2(0, 0)] = adapted.x;
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
