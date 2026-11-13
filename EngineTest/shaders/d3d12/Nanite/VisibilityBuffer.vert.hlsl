struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float bounds_center_x;
    float bounds_center_y;
    float bounds_center_z;
    float _pad_after_center;
    float bounds_radius;
    uint3 bounds_padding;
    float bounds_padding2;
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
    float4 position;
    float depth;
    uint meshlet_id;
    uint primitive_id;
};

struct DrawConstants
{
    row_major float4x4 view_matrix;
    row_major float4x4 proj_matrix;
    row_major float4x4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint _pad;
};

ByteAddressBuffer ccib : register(t5, space0);
ByteAddressBuffer cmb : register(t6, space0);
ByteAddressBuffer idb : register(t7, space0);
ByteAddressBuffer mb : register(t1, space0);
ByteAddressBuffer mtb : register(t3, space0);
ByteAddressBuffer mvb : register(t2, space0);
ByteAddressBuffer pb : register(t4, space0);
cbuffer ConstantsBlock : register(b0, space0)
{
    DrawConstants dc_constants : packoffset(c0);
};


static float4 gl_Position;
static int gl_VertexIndex;
static int gl_InstanceIndex;
static VSOut OUT;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float4 VSOut_position : TEXCOORD0;
    float VSOut_depth : TEXCOORD1;
    nointerpolation uint VSOut_meshlet_id : TEXCOORD2;
    nointerpolation uint VSOut_primitive_id : TEXCOORD3;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    uint vid = uint(gl_VertexIndex);
    uint instance_id = uint(gl_InstanceIndex);
    uint global_cluster_id = ccib.Load(instance_id * 4 + 0);
    uint4 cmap = cmb.Load4(global_cluster_id * 16 + 0);
    uint global_meshlet_index = cmap.x;
    uint instance_index = cmap.y;
    InstanceData _62;
    _62.world_matrix = asfloat(uint4x4(idb.Load4(instance_index * 208 + 0), idb.Load4(instance_index * 208 + 16), idb.Load4(instance_index * 208 + 32), idb.Load4(instance_index * 208 + 48)));
    _62.inverse_world_matrix = asfloat(uint4x4(idb.Load4(instance_index * 208 + 64), idb.Load4(instance_index * 208 + 80), idb.Load4(instance_index * 208 + 96), idb.Load4(instance_index * 208 + 112)));
    _62.meta0 = idb.Load4(instance_index * 208 + 128);
    _62.meta1 = idb.Load4(instance_index * 208 + 144);
    _62.bounds_center_x = asfloat(idb.Load(instance_index * 208 + 160));
    _62.bounds_center_y = asfloat(idb.Load(instance_index * 208 + 164));
    _62.bounds_center_z = asfloat(idb.Load(instance_index * 208 + 168));
    _62._pad_after_center = asfloat(idb.Load(instance_index * 208 + 172));
    _62.bounds_radius = asfloat(idb.Load(instance_index * 208 + 176));
    _62.bounds_padding = idb.Load3(instance_index * 208 + 192);
    _62.bounds_padding2 = asfloat(idb.Load(instance_index * 208 + 204));
    InstanceData inst;
    inst.world_matrix = _62.world_matrix;
    inst.inverse_world_matrix = _62.inverse_world_matrix;
    inst.meta0 = _62.meta0;
    inst.meta1 = _62.meta1;
    inst.bounds_center_x = _62.bounds_center_x;
    inst.bounds_center_y = _62.bounds_center_y;
    inst.bounds_center_z = _62.bounds_center_z;
    inst._pad_after_center = _62._pad_after_center;
    inst.bounds_radius = _62.bounds_radius;
    inst.bounds_padding = _62.bounds_padding;
    inst.bounds_padding2 = _62.bounds_padding2;
    Meshlet _109;
    _109.header = mb.Load4(global_meshlet_index * 64 + 0);
    _109.cone0 = asfloat(mb.Load4(global_meshlet_index * 64 + 16));
    _109.cone1 = asfloat(mb.Load4(global_meshlet_index * 64 + 32));
    _109.cone2 = asfloat(mb.Load4(global_meshlet_index * 64 + 48));
    Meshlet meshlet;
    meshlet.header = _109.header;
    meshlet.cone0 = _109.cone0;
    meshlet.cone1 = _109.cone1;
    meshlet.cone2 = _109.cone2;
    uint triangle_count = meshlet.header.w;
    uint actual_vertex_count = triangle_count * 3u;
    if (vid >= actual_vertex_count)
    {
        OUT.position = 0.0f.xxxx;
        OUT.depth = 0.0f;
        OUT.meshlet_id = 0u;
        OUT.primitive_id = 0u;
        gl_Position = 0.0f.xxxx;
        return;
    }
    uint tri_offset = meshlet.header.y;
    uint base_byte = tri_offset + vid;
    uint word_idx = base_byte / 4u;
    uint byte_shift = (base_byte % 4u) * 8u;
    uint local_vertex_idx = (mtb.Load(word_idx * 4 + 0) >> byte_shift) & 255u;
    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvb.Load((vert_offset + local_vertex_idx) * 4 + 0);
    float3 pos = float3(asfloat(pb.Load(((vert_idx * 3u) + 0u) * 4 + 0)), asfloat(pb.Load(((vert_idx * 3u) + 1u) * 4 + 0)), asfloat(pb.Load(((vert_idx * 3u) + 2u) * 4 + 0)));
    float4 world_pos = mul(float4(pos, 1.0f), inst.world_matrix);
    float4 view_pos = mul(world_pos, dc_constants.view_matrix);
    float4 clip_pos = mul(view_pos, dc_constants.proj_matrix);
    OUT.position = clip_pos;
    OUT.depth = clip_pos.z / clip_pos.w;
    OUT.meshlet_id = global_meshlet_index;
    OUT.primitive_id = vid / 3u;
    gl_Position = clip_pos;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.VSOut_position = OUT.position;
    stage_output.VSOut_depth = OUT.depth;
    stage_output.VSOut_meshlet_id = OUT.meshlet_id;
    stage_output.VSOut_primitive_id = OUT.primitive_id;
    return stage_output;
}
