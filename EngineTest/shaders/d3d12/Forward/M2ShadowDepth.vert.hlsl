cbuffer ShadowDepthData : register(b0, space0)
{
    row_major float4x4 shadowData_lightViewProjection : packoffset(c0);
};

cbuffer M2ShadowPush
{
    row_major float4x4 pc_model : packoffset(c0);
};


static float4 gl_Position;
static float3 in_position;

struct SPIRV_Cross_Input
{
    float3 in_position : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float4 worldPos = mul(float4(in_position, 1.0f), pc_model);
    gl_Position = mul(worldPos, shadowData_lightViewProjection);
    gl_Position.z += 0.001000000047497451305389404296875f;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_position = stage_input.in_position;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    return stage_output;
}
