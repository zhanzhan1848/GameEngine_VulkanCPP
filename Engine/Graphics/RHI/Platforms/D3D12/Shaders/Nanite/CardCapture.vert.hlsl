// Vulkan reference: Nanite/CardCapture.vert (main).
struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float bounds_center_x;
    float bounds_center_y;
    float bounds_center_z;
    float _pad1;
    float bounds_radius;
    uint _pad2;
    uint _pad3;
    uint _pad4;
};

struct Meshlet
{
    uint4 header;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct VSOut
{
    float2 uv;
    float3 normal;
    uint material_id;
    float3 world_pos;
};

ByteAddressBuffer cib : register(t5, space0);
ByteAddressBuffer cmb : register(t6, space0);
ByteAddressBuffer idb : register(t7, space0);
ByteAddressBuffer mb : register(t1, space0);
ByteAddressBuffer mtb : register(t3, space0);
ByteAddressBuffer mvb : register(t2, space0);
ByteAddressBuffer pb : register(t4, space0);
ByteAddressBuffer eb : register(t8, space0);
cbuffer CardParams : register(b0, space0)
{
    row_major float4x4 cb_view : packoffset(c0);
    row_major float4x4 cb_proj : packoffset(c4);
    float4 cb_center : packoffset(c8);
    float4 cb_fwd : packoffset(c9);
    float4 cb_misc : packoffset(c10);
};

static VSOut OUT;

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float2 VSOut_uv : TEXCOORD0;
    float3 VSOut_normal : TEXCOORD1;
    nointerpolation uint VSOut_material_id : TEXCOORD2;
    float3 VSOut_world_pos : TEXCOORD3;
    float4 positionCS : SV_Position;
};

float3 unpack_normal(uint packed, uint color_t_sign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    float inv = 2.0 / 65535.0;
    float2 f = float2(hi * inv - 1.0, lo * inv - 1.0);
    float d = dot(f, f);
    if (d > 1.0) return float3(0.0, 0.0, 1.0);
    float z = sqrt(max(0.0, 1.0 - d));
    uint signs = (color_t_sign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0;
    return float3(f.x, f.y, z * nSign);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint vid = uint(int(input.vertexID));
    uint instance_id = uint(int(input.instanceID));
    uint global_cluster_id = cib.Load(instance_id * 4);
    uint4 cmap = cmb.Load4(global_cluster_id * 16);
    uint global_meshlet_index = cmap.x;
    uint instance_index = cmap.y;
    uint material_id = cmap.z;
    InstanceData _136;
    _136.world_matrix = asfloat(uint4x4(idb.Load4(instance_index * 192), idb.Load4(instance_index * 192 + 16), idb.Load4(instance_index * 192 + 32), idb.Load4(instance_index * 192 + 48)));
    _136.inverse_world_matrix = asfloat(uint4x4(idb.Load4(instance_index * 192 + 64), idb.Load4(instance_index * 192 + 80), idb.Load4(instance_index * 192 + 96), idb.Load4(instance_index * 192 + 112)));
    _136.meta0 = idb.Load4(instance_index * 192 + 128);
    _136.meta1 = idb.Load4(instance_index * 192 + 144);
    _136.bounds_center_x = asfloat(idb.Load(instance_index * 192 + 160));
    _136.bounds_center_y = asfloat(idb.Load(instance_index * 192 + 164));
    _136.bounds_center_z = asfloat(idb.Load(instance_index * 192 + 168));
    _136._pad1 = asfloat(idb.Load(instance_index * 192 + 172));
    _136.bounds_radius = asfloat(idb.Load(instance_index * 192 + 176));
    _136._pad2 = idb.Load(instance_index * 192 + 180);
    _136._pad3 = idb.Load(instance_index * 192 + 184);
    _136._pad4 = idb.Load(instance_index * 192 + 188);
    InstanceData inst;
    inst.world_matrix = _136.world_matrix;
    inst.inverse_world_matrix = _136.inverse_world_matrix;
    inst.meta0 = _136.meta0;
    inst.meta1 = _136.meta1;
    inst.bounds_center_x = _136.bounds_center_x;
    inst.bounds_center_y = _136.bounds_center_y;
    inst.bounds_center_z = _136.bounds_center_z;
    inst._pad1 = _136._pad1;
    inst.bounds_radius = _136.bounds_radius;
    inst._pad2 = _136._pad2;
    inst._pad3 = _136._pad3;
    inst._pad4 = _136._pad4;
    Meshlet _184;
    _184.header = mb.Load4(global_meshlet_index * 64);
    _184.cone0 = asfloat(mb.Load4(global_meshlet_index * 64 + 16));
    _184.cone1 = asfloat(mb.Load4(global_meshlet_index * 64 + 32));
    _184.cone2 = asfloat(mb.Load4(global_meshlet_index * 64 + 48));
    Meshlet meshlet;
    meshlet.header = _184.header;
    meshlet.cone0 = _184.cone0;
    meshlet.cone1 = _184.cone1;
    meshlet.cone2 = _184.cone2;
    uint triangle_count = meshlet.header.w;
    uint actual_vertex_count = triangle_count * 3u;
    if (vid >= actual_vertex_count)
    {
        OUT.uv = 0.0f.xx;
        OUT.normal = float3(0.0f, 1.0f, 0.0f);
        OUT.material_id = material_id;
        OUT.world_pos = 0.0f.xxx;
        output.positionCS = float4(0.0f, 0.0f, 0.0f, 1.0f);
        { output.VSOut_uv = OUT.uv; output.VSOut_normal = OUT.normal; output.VSOut_material_id = OUT.material_id; output.VSOut_world_pos = OUT.world_pos; return output; }
    }
    uint tri_offset = meshlet.header.y;
    uint byte_index = tri_offset + vid;
    uint local_vertex_idx = (mtb.Load((byte_index >> 2u) * 4) >> ((byte_index & 3u) * 8u)) & 255u;
    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvb.Load((vert_offset + local_vertex_idx) * 4);
    float3 p = float3(asfloat(pb.Load(((vert_idx * 3u) + 0u) * 4)), asfloat(pb.Load(((vert_idx * 3u) + 1u) * 4)), asfloat(pb.Load(((vert_idx * 3u) + 2u) * 4)));
    uint elem_base = vert_idx * 6u;
    uint color_t_sign = eb.Load((elem_base + 0u) * 4);
    uint packed_normal = eb.Load((elem_base + 1u) * 4);
    float2 uv = float2(asfloat(eb.Load((elem_base + 4u) * 4)), asfloat(eb.Load((elem_base + 5u) * 4)));
    uint param = packed_normal;
    uint param_1 = color_t_sign;
    float3 normal = unpack_normal(param, param_1);
    float4 world_pos = mul(float4(p, 1.0f), inst.world_matrix);
    float3x3 normal_matrix = float3x3(float3(inst.world_matrix[0].xyz), float3(inst.world_matrix[1].xyz), float3(inst.world_matrix[2].xyz));
    float3 world_normal = normalize(mul(normal, normal_matrix));
    uv.y = 1.0f - uv.y;
    OUT.uv = uv;
    OUT.normal = world_normal;
    OUT.material_id = material_id;
    OUT.world_pos = world_pos.xyz;
    output.positionCS = mul(mul(world_pos, cb_view), cb_proj);
    output.positionCS.y = -output.positionCS.y;
    output.VSOut_uv = OUT.uv;
    output.VSOut_normal = OUT.normal;
    output.VSOut_material_id = OUT.material_id;
    output.VSOut_world_pos = OUT.world_pos;
    return output;
}
