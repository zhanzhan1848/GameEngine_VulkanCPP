cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    float4 viewData_cameraPos : packoffset(c4);
};

cbuffer M1PushConsts
{
    row_major float4x4 pc_model : packoffset(c0);
    float4 pc_baseColor : packoffset(c4);
};


static float4 gl_Position;
static float3 in_position;
static float3 outWorldPos;
static float3 outWorldNormal;
static uint2 in_normal;
static float2 outUV;
static float2 in_uv;
static float4 outBaseColor;
static uint in_colorTSign;
static uint2 in_tangent;

struct SPIRV_Cross_Input
{
    float3 in_position : TEXCOORD0;
    uint in_colorTSign : TEXCOORD1;
    uint2 in_normal : TEXCOORD2;
    uint2 in_tangent : TEXCOORD3;
    float2 in_uv : TEXCOORD4;
};

struct SPIRV_Cross_Output
{
    float3 outWorldPos : TEXCOORD0;
    float3 outWorldNormal : TEXCOORD1;
    float2 outUV : TEXCOORD2;
    float4 outBaseColor : TEXCOORD3;
    float4 gl_Position : SV_Position;
};

float3 UnpackNormal(uint2 p)
{
    float2 f = float2(p);
    f = (f * 3.05180437862873077392578125e-05f) - 1.0f.xx;
    float d = dot(f, f);
    if (d > 1.0f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z);
}

void vert_main()
{
    float4 worldPos = mul(float4(in_position, 1.0f), pc_model);
    outWorldPos = worldPos.xyz;
    float3x3 normalMatrix = float3x3(float3(pc_model[0].xyz), float3(pc_model[1].xyz), float3(pc_model[2].xyz));
    uint2 param = in_normal;
    outWorldNormal = normalize(mul(UnpackNormal(param), normalMatrix));
    outUV = float2(in_uv.x, 1.0f - in_uv.y);
    outBaseColor = pc_baseColor;
    gl_Position = mul(worldPos, viewData_viewProjection);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_position = stage_input.in_position;
    in_normal = stage_input.in_normal;
    in_uv = stage_input.in_uv;
    in_colorTSign = stage_input.in_colorTSign;
    in_tangent = stage_input.in_tangent;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outWorldPos = outWorldPos;
    stage_output.outWorldNormal = outWorldNormal;
    stage_output.outUV = outUV;
    stage_output.outBaseColor = outBaseColor;
    return stage_output;
}
