#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct CaptureVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};

struct CaptureVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 world_normal;
    float3 world_pos;
    uint   material_id;
};

struct CapturePassData {
    SurfaceCacheCard card;
    uint instance_offset;
    uint _pad[3];
};

// Build ortho view matrix from card axis/direction
vertex CaptureVertexOut surfaceCacheCaptureVS(
    CaptureVertexIn in [[stage_in]],
    constant CapturePassData& pass [[buffer(1)]],
    constant float4x4* instances [[buffer(2)]])
{
    CaptureVertexOut out;
    float4x4 world = instances[pass.instance_offset];
    float4 wp = world * float4(in.position, 1.0);
    float3 wn = normalize((world * float4(in.normal, 0.0)).xyz);

    uint axis = pass.card.axis_direction & 0xFF;
    uint dir = (pass.card.axis_direction >> 8) & 0xFF;

    float3 fwd, right, up;
    if (axis == 0) { right=float3(0,1,0); up=float3(0,0,1); fwd=float3(dir?1:-1,0,0); }
    else if (axis == 1) { right=float3(1,0,0); up=float3(0,0,1); fwd=float3(0,dir?1:-1,0); }
    else { right=float3(1,0,0); up=float3(0,1,0); fwd=float3(0,0,dir?1:-1); }

    float3 origin = pass.card.center.xyz;
    float4x4 view;
    view[0]=float4(right.x,up.x,fwd.x,0);
    view[1]=float4(right.y,up.y,fwd.y,0);
    view[2]=float4(right.z,up.z,fwd.z,0);
    view[3]=float4(-dot(right,origin),-dot(up,origin),-dot(fwd,origin),1);

    float rx = (axis==0) ? pass.card.extent.x : pass.card.extent.y;
    float ry = (axis==0) ? pass.card.extent.z : ((axis==1) ? pass.card.extent.z : pass.card.extent.y);
    float rz = pass.card.extent.x;
    float4x4 proj;
    proj[0]=float4(1.0/max(rx,0.001),0,0,0);
    proj[1]=float4(0,1.0/max(ry,0.001),0,0);
    proj[2]=float4(0,0,1.0/max(rz,0.001),0);
    proj[3]=float4(0,0,0,1);

    out.position = proj * view * wp;
    out.world_pos = wp.xyz;
    out.world_normal = wn;
    out.uv = in.uv;
    out.material_id = 0;
    return out;
}

// MUST be separate functions — Apple Silicon inline texture sampling bug
static float4 sampleAlbedo(uint mid, float2 uv, texture2d_array<float> tex) {
    constexpr sampler s(coord::normalized, filter::linear, address::repeat);
    return tex.sample(s, uv, mid);
}

// M4.4: VT 两段式采样 — 页表 read + 物理 atlas 采样。
// material_id → VT 参数表索引;未启用 VT 的材质(vtParams[mid].flags 无
// VT 位)回退 texture array 路径。
struct VTMaterialGPUParams {
    uint32_t flags;          // bit0 = VT;bits[4:8) = vtId
    uint32_t pages_top;
    uint32_t pt_origin_x;
    uint32_t pt_origin_y;
    float    avg_color;
    float    pad[3];
};

static float4 sampleAlbedoVT(
    uint mid, float2 uv,
    texture2d_array<float> fallback_array,
    texture2d<uint, access::sample> page_table,
    texture2d<float, access::sample> atlas,
    constant VTMaterialGPUParams* vtParams,
    float atlas_inv,
    float tile_size)
{
    constant VTMaterialGPUParams& mp = vtParams[mid];
    if ((mp.flags & 0x1u) == 0u) {
        // 非 VT 材质 — 原 texture array 路径
        constexpr sampler s(coord::normalized, filter::linear, address::repeat);
        return fallback_array.sample(s, uv, mid);
    }
    // 两段式:页表条目(R 通道 = 层 0/albedo)
    const uint s_grid = max(mp.pages_top, 1u);
    const float2 f = saturate(uv) * float(s_grid);
    const uint2 vPage = uint2(min(f, float2(s_grid - 1u)));
    const uint entry = page_table.read(ushort2(mp.pt_origin_x + vPage.x,
                                               mp.pt_origin_y + vPage.y), 0).r;
    if ((entry & 1u) == 0u) {
        return float4(mp.avg_color);   // 平均色兜底
    }
    const uint ox = (entry >> 8) & 0x7FFu;
    const uint oy = (entry >> 19) & 0x7FFu;
    const float2 frac = f - float2(vPage);
    const float2 pUV = (float2(float(ox), float(oy)) + 4.0f + frac * tile_size)
                     * atlas_inv;
    constexpr sampler vs(coord::normalized, filter::linear, address::clamp_to_edge);
    return atlas.sample(vs, pUV);
}

struct CaptureOutput {
    float4 albedo  [[color(0)]];
    float4 normal  [[color(1)]];
    float  depth   [[color(2)]];
    float4 emissive [[color(3)]];
};

fragment CaptureOutput surfaceCacheCaptureFS(
    CaptureVertexOut in [[stage_in]],
    texture2d_array<float> albedo_array [[texture(0)]],
    texture2d_array<float> normal_array [[texture(1)]],
    texture2d_array<float> orm_array    [[texture(2)]],
    constant CapturePassData& pass      [[buffer(1)]],
    // M4.4: VT 绑定(可选 — VT 系统启用时由管线写入;未启用时为零值,
    // flags 无 VT 位 → 走 array 回退)
    texture2d<uint, access::sample> vt_page_table [[texture(3)]],
    texture2d<float, access::sample> vt_atlas     [[texture(4)]],
    constant VTMaterialGPUParams* vt_params       [[buffer(5)]])
{
    CaptureOutput out;
    // M4.4:VT 两段式采样(VT 材质走页表+atlas,非 VT 走 array)
    out.albedo = sampleAlbedoVT(in.material_id, in.uv, albedo_array,
                                vt_page_table, vt_atlas, vt_params,
                                1.0f / 2048.0f, 128.0f);

    float3 wn = normalize(in.world_normal);
    out.normal = float4(octEncode(wn), 0.0, 1.0);

    float3 local = in.world_pos - pass.card.center.xyz;
    uint axis = pass.card.axis_direction & 0xFF;
    float depth;
    if (axis == 0) depth = abs(local.x);
    else if (axis == 1) depth = abs(local.y);
    else depth = abs(local.z);
    out.depth = depth;

    // TODO: sample emissive from material data when available
    out.emissive = float4(0.0, 0.0, 0.0, 1.0);
    return out;
}
