cbuffer PC
{
    row_major float4x4 pc_view_proj : packoffset(c0);
    float4 pc_sun_dir : packoffset(c4);
    float4 pc_cam_pos : packoffset(c5);
    float4 pc_misc : packoffset(c6);
};


static float4 gl_Position;
static uint vProbe;
static float aProbe;
static float vState;
static float aState;
static float vMode;
static float vTotal;
static float3 vWorld;
static float3 aPos;
static float3 vNormal;
static float3 aNormal;
static float3 aCenter;
static float4 vClip;

struct SPIRV_Cross_Input
{
    float3 aPos : TEXCOORD0;
    float3 aNormal : TEXCOORD1;
    float aProbe : TEXCOORD2;
    float aState : TEXCOORD3;
    float3 aCenter : TEXCOORD4;
};

struct SPIRV_Cross_Output
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
    nointerpolation float vState : TEXCOORD4;
    nointerpolation float vMode : TEXCOORD5;
    nointerpolation float vTotal : TEXCOORD6;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    vProbe = uint(aProbe);
    vState = aState;
    vMode = pc_misc.x;
    vTotal = pc_misc.y;
    vWorld = aPos;
    vNormal = aNormal;
    if (distance(aCenter, pc_cam_pos.xyz) < pc_cam_pos.w)
    {
        gl_Position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        vClip = gl_Position;
        return;
    }
    vClip = mul(float4(aPos, 1.0f), pc_view_proj);
    gl_Position = vClip;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    aProbe = stage_input.aProbe;
    aState = stage_input.aState;
    aPos = stage_input.aPos;
    aNormal = stage_input.aNormal;
    aCenter = stage_input.aCenter;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.vProbe = vProbe;
    stage_output.vState = vState;
    stage_output.vMode = vMode;
    stage_output.vTotal = vTotal;
    stage_output.vWorld = vWorld;
    stage_output.vNormal = vNormal;
    stage_output.vClip = vClip;
    return stage_output;
}
