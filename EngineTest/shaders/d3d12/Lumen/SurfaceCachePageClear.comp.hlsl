static const uint3 gl_WorkGroupSize = uint3(256u, 1u, 1u);

ByteAddressBuffer rb : register(t6, space0);
RWTexture2D<float4> lighting0 : register(u0, space0);
RWTexture2D<float4> lighting1 : register(u1, space0);
RWTexture2D<float4> lighting2 : register(u2, space0);
RWTexture2D<float4> nodirect0 : register(u3, space0);
RWTexture2D<float4> nodirect1 : register(u4, space0);
RWTexture2D<float4> nodirect2 : register(u5, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint tid = gl_GlobalInvocationID.x;
    uint count = rb.Load(0);
    for (uint r = 0u; r < count; r++)
    {
        uint base = 1u + (r * 4u);
        uint area = rb.Load((base + 2u) * 4 + 0) * rb.Load((base + 3u) * 4 + 0);
        if (tid >= area)
        {
            tid -= area;
            continue;
        }
        uint lx = tid % rb.Load((base + 2u) * 4 + 0);
        uint ly = tid / rb.Load((base + 2u) * 4 + 0);
        int2 uv = int2(int(rb.Load((base + 0u) * 4 + 0) + lx), int(rb.Load((base + 1u) * 4 + 0) + ly));
        lighting0[uv] = 0.0f.xxxx;
        lighting1[uv] = 0.0f.xxxx;
        lighting2[uv] = 0.0f.xxxx;
        nodirect0[uv] = 0.0f.xxxx;
        nodirect1[uv] = 0.0f.xxxx;
        nodirect2[uv] = 0.0f.xxxx;
        return;
    }
}

[numthreads(256, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
