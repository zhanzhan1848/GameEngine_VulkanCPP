#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 1: Froxel Density Inject
// ============================================================================
// Each thread processes one froxel (x, y, z).
// Reconstructs world position, samples density (noise + SDF), writes R channel.

kernel void froxel_density_inject(
    texture3d<float, access::write>   froxel_density [[texture(0)]],
    texture3d<float, access::sample>  noise_tex      [[texture(1)]],
    texture3d<float, access::sample>  sdf_cascade_0  [[texture(2)]],
    texture3d<float, access::sample>  sdf_cascade_1  [[texture(3)]],
    texture3d<float, access::sample>  sdf_cascade_2  [[texture(4)]],
    texture2d<float, access::read>    depth_buffer   [[texture(5)]],
    constant FroxelParams& params                    [[buffer(6)]],
    uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= froxel_density.get_width()  ||
        gid.y >= froxel_density.get_height() ||
        gid.z >= froxel_density.get_depth()) return;

    // Reconstruct world position from froxel coordinates（整数 = 单元中心）
    float3 world_pos = froxel_to_world(gid, params);

    // 场景几何之后的 froxel 置零：屏幕足迹 4 象限取最小深度（最近表面），
    // 与 GLSL 同步（audit V2/V3）；切片后边界越面即清零。
    bool cleared = false;
    {
        float2 pix_per_froxel = float2(params.ScreenParams.xy) / float2(params.FroxelDims.xy);
        float2 center_pix = (float2(gid.xy) + 0.5) * pix_per_froxel;
        float2 half_pix = pix_per_froxel * 0.5;
        for (int q = 0; q < 4; ++q) {
            float2 off = float2((q & 1) != 0 ? half_pix.x : -half_pix.x,
                                (q & 2) != 0 ? half_pix.y : -half_pix.y);
            uint2 depth_pixel = clamp(uint2(max(center_pix + off, float2(0.0))),
                                      uint2(0), uint2(params.ScreenParams.xy) - 1u);
            float scene_depth = depth_buffer.read(depth_pixel).x;
            if (scene_depth > 0.0 && scene_depth < 1.0) {
                float scene_z = ndc_to_view_z(scene_depth, params);
                float froxel_z_far = froxel_z_to_depth(float(gid.z) + 0.5, params);
                if (froxel_z_far > scene_z) { cleared = true; break; }
            }
        }
    }
    if (cleared) {
        froxel_density.write(0.0, gid);
        return;
    }

    // 常量介质模式：绕过噪声/SDF（几何清零已处理）
    if (params.FrustumParams2.z > 0.0f) {
        froxel_density.write(params.FrustumParams2.z, gid);
        return;
    }

    // 全场景基础霾保留；WP5 云体在下方 max() 叠加（与 GLSL 同步）
    float density = froxel_sample_density(world_pos, noise_tex,
                                          sdf_cascade_0, sdf_cascade_1, sdf_cascade_2,
                                          params);

    // WP5 云原语（与 GLSL FroxelDensityInject.comp 同式）：球形包络略压扁 ×
    // FBM 基底 × Worley 细节侵蚀,云底淡出；绕过 SDF/高度门。
    if (params.CloudA.x > 0.5) {
        float radius = params.CloudA.y;
        float3 q = world_pos - params.CloudB.xyz;
        q.y *= 1.18;
        float dist = length(q);
        if (dist < radius) {
            constexpr sampler noise_s(coord::normalized, filter::linear,
                                      address::repeat, mip_filter::none);
            float drift = params.CloudB.w * params.VolumeParams2.w;
            float body = smoothstep(radius, radius * 0.42, dist);
            body *= 0.55 + 0.45 * smoothstep(-radius * 0.5, -radius * 0.12,
                                             q.y / max(radius, 1e-3));
            float base = noise_tex.sample(noise_s,
                world_pos * params.CloudA.w + float3(drift, drift * 0.23, 0.0)).r;
            float detail = worley_noise_3d(world_pos * (params.CloudA.w * 3.1)
                                           + float3(drift * 1.7, 0.0, 0.0));
            float shape = clamp((base * 0.72 + detail * 0.38) * body * 1.85 - 0.32,
                                0.0, 1.0);
            density = max(density, params.CloudA.z * shape);
        }
    }

    // Threshold
    if (density < params.VolumeParams1.z) density = 0.0;

    froxel_density.write(density, gid);
}
