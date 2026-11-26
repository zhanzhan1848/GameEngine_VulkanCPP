// Vulkan reference: PFX/ParticleFXMesh.vert (main).
ByteAddressBuffer instances : register(t2, space0);
ByteAddressBuffer positions : register(t0, space0);
ByteAddressBuffer normals : register(t1, space0);
cbuffer VP : register(b3, space0)
{
    row_major float4x4 vp_view_projection : packoffset(c0);
    float4 vp_color : packoffset(c4);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float3 out_normal : TEXCOORD0;
    float4 out_color : TEXCOORD1;
    float4 positionCS : SV_Position;
};

float3 quat_rotate(float4 q, float3 v) {
    float3 qv = q.xyz;
    float3 t = 2.0 * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint base = uint(int(input.instanceID)) * 3u;
    float4 pos_and_pad = asfloat(instances.Load4((base + 0u) * 16));
    float4 rot = asfloat(instances.Load4((base + 1u) * 16));
    float4 scale_and_pad = asfloat(instances.Load4((base + 2u) * 16));
    uint vi = uint(int(input.vertexID)) * 3u;
    float3 local = float3(asfloat(positions.Load(vi * 4)), asfloat(positions.Load((vi + 1u) * 4)), asfloat(positions.Load((vi + 2u) * 4))) * max(scale_and_pad.x, 0.0001f);
    float4 param = rot;
    float3 param_1 = local;
    float3 world = pos_and_pad.xyz + quat_rotate(param, param_1);
    float4 param_2 = rot;
    float3 param_3 = float3(asfloat(normals.Load(vi * 4)), asfloat(normals.Load((vi + 1u) * 4)), asfloat(normals.Load((vi + 2u) * 4)));
    output.out_normal = quat_rotate(param_2, param_3);
    output.positionCS = mul(float4(world, 1.0f), vp_view_projection);
    float3 n = normalize(output.out_normal);
    float3 l1 = float3(0.32444283f, 0.8111071f, 0.48666427f);
    float3 tint = float3(pos_and_pad.w, scale_and_pad.z, scale_and_pad.w);
    output.out_color = (vp_color * (0.35f + (0.65f * max(dot(n, l1), 0.0f)))) * float4(tint, 1.0f);
    return output;
}
