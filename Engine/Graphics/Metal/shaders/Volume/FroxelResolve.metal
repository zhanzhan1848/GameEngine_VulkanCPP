#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 3: Froxel Resolve
// ============================================================================
// Each thread processes one screen pixel (half-res).
// Reads depth, computes froxel UVW, trilinear samples froxel_scatter,
// outputs RGBA16_Float (RGB=scatter, A=transmittance).

kernel void froxel_resolve(
    texture3d<float, access::sample>  froxel_scatter [[texture(0)]],
    texture2d<float, access::read>    depth_buffer   [[texture(1)]],
    texture2d<float, access::write>   output_color   [[texture(2)]],
    constant FroxelParams& params                    [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint w = output_color.get_width();
    uint h = output_color.get_height();
    if (gid.x >= w || gid.y >= h) return;

    // Sample depth buffer at full-res center of this half-res pixel
    uint2 full_res_pixel = gid * 2u + 1u;
    full_res_pixel = min(full_res_pixel,
                         uint2(params.ScreenParams.xy) - 1u);
    float depth = depth_buffer.read(full_res_pixel).x;

    // audit V8（与 GLSL 同步）: 天空/远开口像素同样积分到雾 far，不再返回
    // 恒等值（开口与遮挡边缘的雾断层）。
    bool is_sky = (depth <= 0.0 || depth >= 1.0);
    float z_view = is_sky ? params.FrustumParams.y
                          : ndc_to_view_z(depth, params);

    // 边界分数（0=fogNear，1=fogFar）；前缀语义：slice z = 积分到后边界
    // (z+1)/N，目标边界 f 对应连续 slice 坐标 f*N-1。
    float f = clamp(log2(z_view / params.FrustumParams.x) / params.FrustumParams.z,
                    0.0, 1.0);
    float slice_coord = f * float(params.FroxelDims.z) - 1.0;

    // Screen UV for froxel lookup
    float2 screen_uv = (float2(gid) + 0.5) / float2(w, h);
    float3 uvw = float3(screen_uv, slice_coord / float(params.FroxelDims.z));

    // Trilinear sample
    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);
    float4 result = froxel_scatter.sample(s, clamp(uvw, float3(0.0), float3(1.0)));

    output_color.write(result, gid);
}
