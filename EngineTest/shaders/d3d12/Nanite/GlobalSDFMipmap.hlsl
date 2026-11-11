static const uint3 gl_WorkGroupSize = uint3(4u, 4u, 4u);

struct MeshletData
{
    uint4 header;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct ClusterMap
{
    uint4 data;
};

struct InstanceData
{
    float4 world_col0;
    float4 world_col1;
    float4 world_col2;
    float4 world_col3;
    float4 inv_col0;
    float4 inv_col1;
    float4 inv_col2;
    float4 inv_col3;
    float4 meta0;
    float4 meta1;
    float4 bounds_center;
    float4 bounds_meta;
};

cbuffer Cascade : register(b1, space0)
{
    float4 _110_origin : packoffset(c0);
    uint4 _110_params : packoffset(c1);
};

ByteAddressBuffer _114 : register(t2, space0);
ByteAddressBuffer _119 : register(t3, space0);
ByteAddressBuffer _123 : register(t4, space0);
ByteAddressBuffer _127 : register(t5, space0);
ByteAddressBuffer _132 : register(t6, space0);
ByteAddressBuffer _137 : register(t7, space0);
RWTexture3D<float> mip_dst : register(u1, space0);
RWTexture3D<float> mip_src : register(u0, space0);
RWTexture3D<float> sdf_output : register(u0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint3 spvImageSize(RWTexture3D<float> Tex, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(ret.x, ret.y, ret.z);
    Param = 0u;
    return ret;
}

void comp_main()
{
    uint3 tid = gl_GlobalInvocationID;
    uint _21_dummy_parameter;
    uint3 dstDims = uint3(int3(spvImageSize(mip_dst, _21_dummy_parameter)));
    if (any(bool3(tid.x >= dstDims.x, tid.y >= dstDims.y, tid.z >= dstDims.z)))
    {
        return;
    }
    uint _35_dummy_parameter;
    uint3 srcDims = uint3(int3(spvImageSize(mip_src, _35_dummy_parameter)));
    uint3 c = tid * uint3(2u, 2u, 2u);
    float best = 3.3999999521443642490773241379936e+38f;
    for (uint z = 0u; z < 2u; z++)
    {
        for (uint y = 0u; y < 2u; y++)
        {
            for (uint x = 0u; x < 2u; x++)
            {
                uint3 sc = min((c + uint3(x, y, z)), (srcDims - uint3(1u, 1u, 1u)));
                best = min(best, mip_src[int3(sc)].xxxx.x);
            }
        }
    }
    mip_dst[int3(tid)] = float4(best, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(4, 4, 4)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
