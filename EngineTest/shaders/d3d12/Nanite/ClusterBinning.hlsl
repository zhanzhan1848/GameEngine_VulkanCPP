struct ClusterData
{
    uint cluster_id;
    float3 center;
    float radius;
    uint primitive_count;
    uint vertex_offset;
    uint index_offset;
};

struct BinningConfig
{
    uint bin_size;
    uint max_bins;
    uint max_clusters_per_bin;
    uint enable_spatial_sorting;
};

struct BinData
{
    uint cluster_count;
    uint clusters[256];
};

ByteAddressBuffer clusterData : register(t0, space0);
cbuffer ConfigBlock : register(b1, space0)
{
    BinningConfig cfg_config : packoffset(c0);
};

RWByteAddressBuffer binData : register(u2, space0);
RWByteAddressBuffer counter : register(u3, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= 10000u)
    {
        return;
    }
    ClusterData _38;
    _38.cluster_id = clusterData.Load(gid * 48 + 0);
    _38.center = asfloat(clusterData.Load3(gid * 48 + 16));
    _38.radius = asfloat(clusterData.Load(gid * 48 + 28));
    _38.primitive_count = clusterData.Load(gid * 48 + 32);
    _38.vertex_offset = clusterData.Load(gid * 48 + 36);
    _38.index_offset = clusterData.Load(gid * 48 + 40);
    ClusterData c;
    c.cluster_id = _38.cluster_id;
    c.center = _38.center;
    c.radius = _38.radius;
    c.primitive_count = _38.primitive_count;
    c.vertex_offset = _38.vertex_offset;
    c.index_offset = _38.index_offset;
    float4 clip_pos = float4(c.center, 1.0f);
    uint sx = uint(clip_pos.x);
    uint sy = uint(clip_pos.y);
    uint bin_index = ((sy / cfg_config.bin_size) * (1920u / cfg_config.bin_size)) + (sx / cfg_config.bin_size);
    if (bin_index >= cfg_config.max_bins)
    {
        return;
    }
    uint _114;
    binData.InterlockedAdd(bin_index * 1028 + 0, 1u, _114);
    uint slot = _114;
    if (slot < cfg_config.max_clusters_per_bin)
    {
        binData.Store(bin_index * 1028 + slot * 4 + 4, c.cluster_id);
    }
}

[numthreads(1, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
