// Vulkan reference: Nanite/VisibilityBuffer.frag (visibility_fragment_shader).
struct VSOut
{
    float4 position;
    float depth;
    uint meshlet_id;
    uint primitive_id;
};

static VSOut IN;

struct StageInput
{
    float4 VSOut_position : TEXCOORD0;
    float VSOut_depth : TEXCOORD1;
    nointerpolation uint VSOut_meshlet_id : TEXCOORD2;
    nointerpolation uint VSOut_primitive_id : TEXCOORD3;
};

struct StageOutput
{
    uint out_visibility : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    IN.position = input.VSOut_position;
    IN.depth = input.VSOut_depth;
    IN.meshlet_id = input.VSOut_meshlet_id;
    IN.primitive_id = input.VSOut_primitive_id;
    uint meshlet_part = (IN.meshlet_id & 255u) << 24u;
    uint primitive_part = IN.primitive_id & 16777215u;
    output.out_visibility = meshlet_part | primitive_part;
    return output;
}
