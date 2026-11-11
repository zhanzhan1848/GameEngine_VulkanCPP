static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

struct SurfaceCacheCard
{
    float4 center;
    float4 extent;
    uint axis_direction;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint mesh_instance_id;
    uint static_mesh_slot;
    uint flags;
    uint page_card;
};

cbuffer CaptureParams : register(b6, space0)
{
    row_major float4x4 cp_invViewProjection : packoffset(c0);
    uint cp_card_count : packoffset(c4);
    uint cp_render_width : packoffset(c4.y);
    uint cp_render_height : packoffset(c4.z);
    uint cp_atlas_size : packoffset(c4.w);
};

RWByteAddressBuffer cdm : register(u10, space0);
cbuffer CaptureEmissive : register(b12, space0)
{
    float4 ce_emissive_rgba : packoffset(c0);
};

ByteAddressBuffer ie : register(t13, space0);
ByteAddressBuffer cb : register(t7, space0);
ByteAddressBuffer scgo : register(t8, space0);
ByteAddressBuffer scgc : register(t9, space0);
Texture2D<float4> gbuffer_depth : register(t0, space0);
Texture2D<float4> gbuffer_albedo : register(t1, space0);
Texture2D<float4> gbuffer_normal : register(t2, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u3, space0);
RWTexture2D<float2> normal_atlas : register(u4, space0);
RWTexture2D<float> depth_atlas : register(u5, space0);
RWTexture2D<float4> emissive_atlas : register(u11, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    int2 texel = int2(gl_GlobalInvocationID.xy);
    bool _34 = texel.x >= int(cp_atlas_size);
    bool _45;
    if (!_34)
    {
        _45 = texel.y >= int(cp_atlas_size);
    }
    else
    {
        _45 = _34;
    }
    if (_45)
    {
        return;
    }
    cdm.Store(((uint(texel.y) * cp_atlas_size) + uint(texel.x)) * 4 + 0, 4294967295u);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
