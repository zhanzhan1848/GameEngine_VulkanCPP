// Vulkan reference: Forward/M5Mipmap.comp (main).
cbuffer MipParams : register(b3, space0)
{
    uint4 p_srcSize : packoffset(c0);
};

Texture2D<uint4> srcTex : register(t0, space0);
SamplerState srcSamp : register(s1, space0);
RWTexture2D<uint4> dstMip : register(u2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 dst = input.dispatchThreadID.xy;
    uint w = max(p_srcSize.x, 1u);
    uint h = max(p_srcSize.y, 1u);
    if (((dst.x >= ((w + 1u) / 2u)) || (dst.y >= ((h + 1u) / 2u))))
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
