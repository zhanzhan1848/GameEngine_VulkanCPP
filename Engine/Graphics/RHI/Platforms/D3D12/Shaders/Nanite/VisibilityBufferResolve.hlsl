// Vulkan reference: Nanite/VisibilityBufferResolve.comp (ComputeMain).
#include "../Common/TextureDimensions.hlsli"

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

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;

    int2 tex_size = int2(imageDimensions(output_color));
    if (((gid.x >= uint(tex_size.x)) || (gid.y >= uint(tex_size.y))))
    {
        return;
    }
    uint visibility_data = visibility_buffer.Load(int3(int2(gid), 0)).x;
    uint triangle_id = visibility_data & 16777215u;
    uint material_id = (visibility_data >> 24u) & 255u;
    float depth = depth_buffer.Load(int3(int2(gid), 0)).x;
    float4 final_color = float4(0.0f, 0.2f, 0.4f, 1.0f);
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
