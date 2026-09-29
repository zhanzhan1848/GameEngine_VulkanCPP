#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 3 (WP3): Froxel Temporal Reproject — 2026-09-30 froxel-upgrade 计划
// ============================================================================
// 与 GLSL FroxelReproject.comp 同式：每 froxel 一线程，当前相机网格点的世界
// 坐标经上一帧 PrevInvViewProj 投影 → 采上一帧混合结果 → 3×3×3 邻域
// min/max 钳制 → out = mix(current, history_clamped, blend)。
// VolumeParams4.w=0 时旁路（passthrough 当前值）。
// 绑定：0=froxel_scatter_curr(sample) 1=froxel_scatter_prev(sample)
//       2=froxel_scatter_out(write)  buffer(3)=FroxelParams

kernel void froxel_reproject(
    texture3d<float, access::sample>  froxel_scatter_curr [[texture(0)]],
    texture3d<float, access::sample>  froxel_scatter_prev [[texture(1)]],
    texture3d<float, access::write>   froxel_scatter_out  [[texture(2)]],
    constant FroxelParams& params                          [[buffer(3)]],
    uint3 gid [[thread_position_in_grid]])
{
    uint3 dims = {uint(params.FroxelDims.x), uint(params.FroxelDims.y),
                  uint(params.FroxelDims.z)};
    if (gid.x >= dims.x || gid.y >= dims.y || gid.z >= dims.z) return;

    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);

    float4 cur = froxel_scatter_curr.read(gid);
    float blend = params.VolumeParams4.w;
    if (blend <= 0.0) {
        froxel_scatter_out.write(cur, gid);
        return;
    }

    // 当前相机网格点的世界坐标（整数 = 单元中心，与 inject/integrate 同契约）
    float3 world_pos = froxel_to_world(gid, params);

    // 投影到上一帧 froxel 网格
    float4 pclip = params.PrevInvViewProj[0] * world_pos.x
                 + params.PrevInvViewProj[1] * world_pos.y
                 + params.PrevInvViewProj[2] * world_pos.z
                 + params.PrevInvViewProj[3];
    float3 pndc = pclip.xyz / pclip.w;
    if (pndc.x < -1.0 || pndc.x > 1.0 ||
        pndc.y < -1.0 || pndc.y > 1.0 ||
        pndc.z <  0.0 || pndc.z > 1.0) {
        froxel_scatter_out.write(cur, gid);   // 上一帧视野外：passthrough
        return;
    }
    float3 puvw = float3(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5, pndc.z);

    // 历史三线性采样
    float4 hist = froxel_scatter_prev.sample(s, puvw);

    // 3×3×3 邻域 min/max 钳制
    float3 fdims = float3(params.FroxelDims.xyz);
    float3 pcoord = puvw * fdims - 0.5;
    int3 c0 = int3(floor(pcoord));
    float4 mn = float4(1e30);
    float4 mx = float4(-1e30);
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        int3 c = clamp(c0 + int3(dx, dy, dz), int3(0), int3(fdims) - int3(1));
        float4 v = froxel_scatter_prev.read(uint3(c));
        mn = min(mn, v);
        mx = max(mx, v);
    }
    float4 hist_clamped = clamp(hist, mn, mx);

    froxel_scatter_out.write(mix(cur, hist_clamped, blend), gid);
}
