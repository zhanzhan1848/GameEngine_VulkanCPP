struct M5Instance
{
    float4 sphere;
    uint4 meta;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

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

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint m = gl_GlobalInvocationID.x;
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
    float4 s = asfloat(instBuf.Load4(m * 32 + 0));
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

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
