static const uint3 gl_WorkGroupSize = uint3(4u, 4u, 4u);

cbuffer BakeParams : register(b1, space0)
{
    float4 params_dirAndIntensity : packoffset(c0);
    float4 params_lightColor : packoffset(c1);
};

RWTexture3D<float4> outGrid : register(u0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint pcg1d(inout uint v)
{
    v = (v * 747796405u) + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

void comp_main()
{
    int3 c = int3(gl_GlobalInvocationID);
    float3 dir = normalize(float3(c) - 3.5f.xxx);
    float t = float(c.y) / 7.0f;
    float3 sky = lerp(float3(0.100000001490116119384765625f, 0.12999999523162841796875f, 0.2199999988079071044921875f), float3(0.37999999523162841796875f, 0.4799999892711639404296875f, 0.680000007152557373046875f), t.xxx);
    float3 L = normalize(params_dirAndIntensity.xyz);
    float ndl = max(dot(dir, L), 0.0f);
    float3 flood = (params_lightColor.xyz * params_dirAndIntensity.w) * (0.25f + ((0.75f * ndl) * ndl));
    uint param = ((uint(c.x) * 4096u) + (uint(c.y) * 64u)) + uint(c.z);
    uint _130 = pcg1d(param);
    uint h = _130;
    float dither = float(h >> uint(28)) * 0.00133333331905305385589599609375f;
    float3 val = (sky + flood) + dither.xxx;
    outGrid[c] = float4(val, 1.0f);
}

[numthreads(4, 4, 4)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
