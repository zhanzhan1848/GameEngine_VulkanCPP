static const uint3 gl_WorkGroupSize = uint3(4u, 4u, 4u);

cbuffer AccumParams : register(b4, space0)
{
    float4 params_flags : packoffset(c0);
};

RWTexture3D<float4> newGrid : register(u0, space0);
Texture3D<float4> histTexture : register(t1, space0);
SamplerState histSampler : register(s2, space0);
RWTexture3D<float4> outGrid : register(u3, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    int3 c = int3(gl_GlobalInvocationID);
    float4 newV = newGrid[c];
    float4 _38;
    if (params_flags.x > 0.5f)
    {
        _38 = 0.0f.xxxx;
    }
    else
    {
        _38 = histTexture.Load(int4(c, 0));
    }
    float4 histV = _38;
    outGrid[c] = (histV * 0.89999997615814208984375f) + (newV * 0.100000001490116119384765625f);
}

[numthreads(4, 4, 4)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
