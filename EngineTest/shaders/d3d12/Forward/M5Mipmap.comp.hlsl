static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer MipParams : register(b3, space0)
{
    uint4 p_srcSize : packoffset(c0);
};

Texture2D<uint4> srcTex : register(t0, space0);
SamplerState srcSamp : register(s1, space0);
RWTexture2D<uint4> dstMip : register(u2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint2 dst = gl_GlobalInvocationID.xy;
    uint w = max(p_srcSize.x, 1u);
    uint h = max(p_srcSize.y, 1u);
    bool _40 = dst.x >= ((w + 1u) / 2u);
    bool _50;
    if (!_40)
    {
        _50 = dst.y >= ((h + 1u) / 2u);
    }
    else
    {
        _50 = _40;
    }
    if (_50)
    {
        return;
    }
    uint2 srcBase = dst * uint2(2u, 2u);
    uint x1 = min((srcBase.x + 1u), (w - 1u));
    uint y1 = min((srcBase.y + 1u), (h - 1u));
    uint4 a = srcTex.Load(int3(int2(int(srcBase.x), int(srcBase.y)), 0));
    uint4 b = srcTex.Load(int3(int2(int(x1), int(srcBase.y)), 0));
    uint4 c = srcTex.Load(int3(int2(int(srcBase.x), int(y1)), 0));
    uint4 d = srcTex.Load(int3(int2(int(x1), int(y1)), 0));
    uint4 sum = ((a + b) + c) + d;
    uint4 avg = (sum + uint4(2u, 2u, 2u, 2u)) / uint4(4u, 4u, 4u, 4u);
    dstMip[int2(int(dst.x), int(dst.y))] = avg;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
