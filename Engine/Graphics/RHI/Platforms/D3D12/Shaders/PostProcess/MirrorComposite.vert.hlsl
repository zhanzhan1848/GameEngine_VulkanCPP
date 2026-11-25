// Vulkan reference: PostProcess/MirrorComposite.vert (main).
static const float2 _19[6] = { (-1.0f).xx, float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(-1.0f, 1.0f), float2(1.0f, -1.0f), 1.0f.xx };

cbuffer CompositeParams : register(b0, space0)
{
    row_major float4x4 view_projection : packoffset(c0);
    row_major float4x4 reflected_view_projection : packoffset(c4);
    float4 plane_position : packoffset(c8);
    float4 plane_normal : packoffset(c9);
    float4 plane_axes : packoffset(c10);
    float4 camera_position : packoffset(c11);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float3 vWorldPos : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float2 corner = _19[int(input.vertexID)];
    float3 N = normalize(plane_normal.xyz);
    bool3 _55 = (abs(N.y) < 0.99f).xxx;
    float3 U = normalize(cross(float3(_55.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _55.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _55.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z), N));
    float3 V = cross(N, U);
    float3 worldPos = (plane_position.xyz + (U * (corner.x * plane_axes.x))) + (V * (corner.y * plane_axes.y));
    output.vWorldPos = worldPos;
    output.positionCS = mul(float4(worldPos, 1.0f), view_projection);
    return output;
}
