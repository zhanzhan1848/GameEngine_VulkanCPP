Texture2D<float4> giDirect : register(t0, space0);
Texture2D<float4> giIndirect : register(t1, space0);

static float4 gl_FragCoord;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    int2 t = int2(gl_FragCoord.xy);
    outColor = float4(giDirect.Load(int3(t, 0)).xyz + giIndirect.Load(int3(t, 0)).xyz, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
