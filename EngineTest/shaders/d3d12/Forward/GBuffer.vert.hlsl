struct InstanceData
{
    row_major float4x4 transform;
    float4 baseColor;
    float roughness;
    float metallic;
    float alphaCutoff;
    float transmission;
};

ByteAddressBuffer instanceData : register(t2, space0);
cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    row_major float4x4 viewData_invViewProjection : packoffset(c4);
    row_major float4x4 viewData_previousViewProjection : packoffset(c8);
};

cbuffer SceneData : register(b1, space0)
{
    row_major float4x4 sceneData_model : packoffset(c0);
    float4 sceneData_lightPos : packoffset(c4);
    float4 sceneData_lightColor : packoffset(c5);
    float4 sceneData_reflectionPlane : packoffset(c6);
    float4 sceneData_reflectionPlane2 : packoffset(c7);
    float4 sceneData_reflectionPlane3 : packoffset(c8);
    row_major float4x4 sceneData_previousModel : packoffset(c9);
    float2 sceneData_jitter : packoffset(c13);
    float2 sceneData_previousJitter : packoffset(c13.z);
    float2 sceneData_padding : packoffset(c14);
    float4 sceneData_viewPos : packoffset(c15);
    row_major float4x4 sceneData_shadowMatrix0 : packoffset(c16);
    row_major float4x4 sceneData_shadowMatrix1 : packoffset(c20);
};

cbuffer PushConsts
{
    row_major float4x4 pc_transform : packoffset(c0);
    float4 pc_use_pad : packoffset(c4);
};


static float4 gl_Position;
static int gl_InstanceIndex;
static float4 outInstanceBaseColor;
static float outInstanceRoughness;
static float outInstanceMetallic;
static float outInstanceTransmission;
static float3 in_position;
static float3 outWorldPos;
static uint2 in_normal;
static float3 outWorldNormal;
static uint2 in_tangent;
static float3 outWorldTangent;
static float2 outUV;
static float2 in_uv;
static float4 outCurrentPos;
static float4 outPreviousPos;
static uint in_colorTSign;

struct SPIRV_Cross_Input
{
    float3 in_position : TEXCOORD0;
    uint in_colorTSign : TEXCOORD1;
    uint2 in_normal : TEXCOORD2;
    uint2 in_tangent : TEXCOORD3;
    float2 in_uv : TEXCOORD4;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float3 outWorldPos : TEXCOORD0;
    float3 outWorldNormal : TEXCOORD1;
    float3 outWorldTangent : TEXCOORD2;
    float2 outUV : TEXCOORD3;
    float4 outCurrentPos : TEXCOORD4;
    float4 outPreviousPos : TEXCOORD5;
    float4 outInstanceBaseColor : TEXCOORD6;
    float outInstanceRoughness : TEXCOORD7;
    float outInstanceMetallic : TEXCOORD8;
    float outInstanceTransmission : TEXCOORD9;
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
    float4x4 model;
    if (pc_use_pad.x != 0.0f)
    {
        InstanceData _81;
        _81.transform = asfloat(uint4x4(instanceData.Load4(gl_InstanceIndex * 96 + 0), instanceData.Load4(gl_InstanceIndex * 96 + 16), instanceData.Load4(gl_InstanceIndex * 96 + 32), instanceData.Load4(gl_InstanceIndex * 96 + 48)));
        _81.baseColor = asfloat(instanceData.Load4(gl_InstanceIndex * 96 + 64));
        _81.roughness = asfloat(instanceData.Load(gl_InstanceIndex * 96 + 80));
        _81.metallic = asfloat(instanceData.Load(gl_InstanceIndex * 96 + 84));
        _81.alphaCutoff = asfloat(instanceData.Load(gl_InstanceIndex * 96 + 88));
        _81.transmission = asfloat(instanceData.Load(gl_InstanceIndex * 96 + 92));
        InstanceData inst;
        inst.transform = _81.transform;
        inst.baseColor = _81.baseColor;
        inst.roughness = _81.roughness;
        inst.metallic = _81.metallic;
        inst.alphaCutoff = _81.alphaCutoff;
        inst.transmission = _81.transmission;
        model = inst.transform;
        outInstanceBaseColor = inst.baseColor;
        outInstanceRoughness = inst.roughness;
        outInstanceMetallic = inst.metallic;
        outInstanceTransmission = inst.transmission;
    }
    else
    {
        model = pc_transform;
        outInstanceBaseColor = 1.0f.xxxx;
        outInstanceRoughness = 0.5f;
        outInstanceMetallic = 0.0f;
        outInstanceTransmission = 0.0f;
    }
    float4 worldPos = mul(float4(in_position, 1.0f), model);
    outWorldPos = worldPos.xyz;
    float3x3 normalMatrix = float3x3(float3(model[0].xyz), float3(model[1].xyz), float3(model[2].xyz));
    uint2 param = in_normal;
    float3 rawNormal = UnpackNormal(param);
    outWorldNormal = normalize(mul(rawNormal, normalMatrix));
    uint2 param_1 = in_tangent;
    float3 rawTangent = UnpackNormal(param_1);
    outWorldTangent = normalize(mul(rawTangent, normalMatrix));
    outUV = float2(in_uv.x, 1.0f - in_uv.y);
    gl_Position = mul(worldPos, viewData_viewProjection);
    outCurrentPos = gl_Position;
    outPreviousPos = mul(mul(float4(in_position, 1.0f), sceneData_previousModel), viewData_previousViewProjection);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    in_position = stage_input.in_position;
    in_normal = stage_input.in_normal;
    in_tangent = stage_input.in_tangent;
    in_uv = stage_input.in_uv;
    in_colorTSign = stage_input.in_colorTSign;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outInstanceBaseColor = outInstanceBaseColor;
    stage_output.outInstanceRoughness = outInstanceRoughness;
    stage_output.outInstanceMetallic = outInstanceMetallic;
    stage_output.outInstanceTransmission = outInstanceTransmission;
    stage_output.outWorldPos = outWorldPos;
    stage_output.outWorldNormal = outWorldNormal;
    stage_output.outWorldTangent = outWorldTangent;
    stage_output.outUV = outUV;
    stage_output.outCurrentPos = outCurrentPos;
    stage_output.outPreviousPos = outPreviousPos;
    return stage_output;
}
