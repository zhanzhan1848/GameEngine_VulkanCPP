// Vulkan reference: Forward/M6Composite.frag (main).
Texture2D<float4> giDirect : register(t0, space0);
Texture2D<float4> giIndirect : register(t1, space0);

static float4 fragCoord;

struct StageInput
{
    float2 inUV : TEXCOORD0;
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    fragCoord = input.fragCoord;
    fragCoord.w = 1.0 / fragCoord.w;
    int2 t = int2(fragCoord.xy + (input.inUV * 0.0f));
    output.outColor = float4(giDirect.Load(int3(t, 0)).xyz + giIndirect.Load(int3(t, 0)).xyz, 1.0f);
    return output;
}
