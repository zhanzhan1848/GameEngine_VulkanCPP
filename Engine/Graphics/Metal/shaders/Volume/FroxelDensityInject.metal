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
    texture3d<float, access::sample>  cloud_volume   [[texture(7)]],
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

    // WP5 云原语（3D 纹理路线，与 GLSL 同式）：云 AABB 内采样预烘焙体积
    if (params.CloudA.x > 0.5) {
        float3 cuvw = (world_pos - params.CloudB.xyz) / (2.0 * params.CloudA.y) + 0.5;
        if (cuvw.x > 0.0 && cuvw.x < 1.0 &&
            cuvw.y > 0.0 && cuvw.y < 1.0 &&
            cuvw.z > 0.0 && cuvw.z < 1.0) {
            constexpr sampler cloud_s(coord::normalized, filter::linear,
                                      address::clamp_to_edge, mip_filter::none);
            float cd = cloud_volume.sample(cloud_s, cuvw).r;
            density = max(density, params.CloudA.z * cd);
        }
    }

    // Threshold
    if (density < params.VolumeParams1.z) density = 0.0;

    froxel_density.write(density, gid);
}
