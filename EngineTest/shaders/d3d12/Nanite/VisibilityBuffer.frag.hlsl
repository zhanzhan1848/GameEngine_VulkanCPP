struct VSOut
{
    float4 position;
    float depth;
    uint meshlet_id;
    uint primitive_id;
};

static VSOut IN;
static uint out_visibility;

struct SPIRV_Cross_Input
{
    float4 VSOut_position : TEXCOORD0;
    float VSOut_depth : TEXCOORD1;
    nointerpolation uint VSOut_meshlet_id : TEXCOORD2;
    nointerpolation uint VSOut_primitive_id : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    uint out_visibility : SV_Target0;
};

void frag_main()
{
    uint meshlet_part = (IN.meshlet_id & 255u) << 24u;
    uint primitive_part = IN.primitive_id & 16777215u;
    out_visibility = meshlet_part | primitive_part;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    IN.position = stage_input.VSOut_position;
    IN.depth = stage_input.VSOut_depth;
    IN.meshlet_id = stage_input.VSOut_meshlet_id;
    IN.primitive_id = stage_input.VSOut_primitive_id;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.out_visibility = out_visibility;
    return stage_output;
}
