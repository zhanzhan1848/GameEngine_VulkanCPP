// Vulkan reference: Debug/ProbeSphere.vert (main).
cbuffer PC
{
    row_major float4x4 pc_view_proj : packoffset(c0);
    float4 pc_sun_dir : packoffset(c4);
    float4 pc_cam_pos : packoffset(c5);
    float4 pc_misc : packoffset(c6);
};

struct StageInput
{
    float3 aPos : TEXCOORD0;
    float3 aNormal : TEXCOORD1;
    float aProbe : TEXCOORD2;
    float aState : TEXCOORD3;
    float3 aCenter : TEXCOORD4;
};

struct StageOutput
{
    float3 vWorld : TEXCOORD0;
    float3 vNormal : TEXCOORD1;
    float4 vClip : TEXCOORD2;
    nointerpolation uint vProbe : TEXCOORD3;
    nointerpolation float vState : TEXCOORD4;
    nointerpolation float vMode : TEXCOORD5;
    nointerpolation float vTotal : TEXCOORD6;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.vProbe = uint(input.aProbe);
    output.vState = input.aState;
    output.vMode = pc_misc.x;
    output.vTotal = pc_misc.y;
    output.vWorld = input.aPos;
    output.vNormal = input.aNormal;
    if (distance(input.aCenter, pc_cam_pos.xyz) < pc_cam_pos.w)
    {
        output.positionCS = float4(2.0f, 2.0f, 2.0f, 1.0f);
        output.vClip = output.positionCS;
        return output;
    }
    output.vClip = mul(float4(input.aPos, 1.0f), pc_view_proj);
    output.positionCS = output.vClip;
    return output;
}
