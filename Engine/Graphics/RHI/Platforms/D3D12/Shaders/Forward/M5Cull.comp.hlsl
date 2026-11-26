// Vulkan reference: Forward/M5Cull.comp (main).
struct M5Instance
{
    float4 sphere;
    uint4 meta;
};

cbuffer CullFlags : register(b4, space0)
{
    uint4 flags_f : packoffset(c0);
};

globallycoherent RWByteAddressBuffer counter : register(u3, space0);
cbuffer CullParams : register(b1, space0)
{
    float4 params_planes[6] : packoffset(c0);
    float4 params_camMaxDist : packoffset(c6);
    float4 params_counts : packoffset(c7);
};

ByteAddressBuffer instBuf : register(t0, space0);
RWByteAddressBuffer argsBuf : register(u2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    uint m = input.dispatchThreadID.x;
    if (flags_f.x == 1u)
    {
        if (m == 0u)
        {
            counter.Store(0, 0u);
        }
        return;
    }
    if (m >= uint(params_counts.x))
    {
        return;
    }
    float4 s = asfloat(instBuf.Load4(m * 32));
    bool _pass = true;
    for (int p = 0; p < 6; p++)
    {
        float d = dot(params_planes[p].xyz, s.xyz) + params_planes[p].w;
        if (d < (-s.w))
        {
            _pass = false;
            break;
        }
    }
    if (_pass)
    {
        float3 dv = s.xyz - params_camMaxDist.xyz;
        float dist2 = dot(dv, dv);
        float rmax = params_camMaxDist.w + s.w;
        if (dist2 > (rmax * rmax))
        {
            _pass = false;
        }
    }
    uint4 meta = instBuf.Load4(m * 32 + 16);
    uint base = m * 5u;
    argsBuf.Store((base + 0u) * 4 + 0, meta.x);
    argsBuf.Store((base + 1u) * 4 + 0, uint(_pass));
    argsBuf.Store((base + 2u) * 4 + 0, meta.y);
    argsBuf.Store((base + 3u) * 4 + 0, meta.z);
    argsBuf.Store((base + 4u) * 4 + 0, meta.w);
    if (_pass)
    {
        uint _180;
        counter.InterlockedAdd(0, 1u, _180);
    }
}
