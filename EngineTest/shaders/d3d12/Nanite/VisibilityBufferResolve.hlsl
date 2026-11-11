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

struct ClusterData
{
    uint cluster_id;
    float3 center;
    float radius;
    uint primitive_count;
    uint vertex_offset;
    uint index_offset;
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer ConstantsBlock : register(b3, space0)
{
    DrawConstants dc_constants : packoffset(c0);
};

ByteAddressBuffer clusterData : register(t4, space0);
RWTexture2D<unorm float4> output_color : register(u2, space0);
Texture2D<uint4> visibility_buffer : register(t0, space0);
SamplerState _visibility_buffer_sampler : register(s0, space0);
Texture2D<float4> depth_buffer : register(t1, space0);
SamplerState _depth_buffer_sampler : register(s1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvImageSize(RWTexture2D<unorm float4> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

void comp_main()
{
    uint2 gid = gl_GlobalInvocationID.xy;
    uint _24_dummy_parameter;
    int2 tex_size = int2(spvImageSize(output_color, _24_dummy_parameter));
    bool _34 = gid.x >= uint(tex_size.x);
    bool _45;
    if (!_34)
    {
        _45 = gid.y >= uint(tex_size.y);
    }
    else
    {
        _45 = _34;
    }
    if (_45)
    {
        return;
    }
    uint visibility_data = visibility_buffer.Load(int3(int2(gid), 0)).x;
    uint triangle_id = visibility_data & 16777215u;
    uint material_id = (visibility_data >> 24u) & 255u;
    float depth = depth_buffer.Load(int3(int2(gid), 0)).x;
    float4 final_color = float4(0.0f, 0.20000000298023223876953125f, 0.4000000059604644775390625f, 1.0f);
    if ((triangle_id != 0u) && (depth < 1.0f))
    {
        float material_color = float(material_id) / 255.0f;
        final_color = float4(material_color, 0.5f, 1.0f - material_color, 1.0f);
        float depth_shading = 1.0f - depth;
        float4 _114 = final_color;
        float3 _116 = _114.xyz * depth_shading;
        final_color.x = _116.x;
        final_color.y = _116.y;
        final_color.z = _116.z;
    }
    output_color[int2(gid)] = final_color;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
