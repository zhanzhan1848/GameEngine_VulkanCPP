#include <metal_stdlib>
using namespace metal;

// Reuse noise functions and low-level SDF cascade sampling from VolumeCommon
#include "VolumeCommon.metal"

// ============================================================================
// FroxelParams — must match C++ FroxelTypes.h exactly (384 bytes)
// ============================================================================

struct FroxelParams {
    // Camera (80 bytes)
    float4 CameraPos;           // 0
    float4 InvViewProj[4];      // 16

    // Frustum (16 bytes)
    float4 FrustumParams;       // 80: x=near, y=far, z=log2(far/near), w=1.0/depth_slices

    // Froxel grid (32 bytes)
    float4 FroxelDims;          // 96:  x=width, y=height, z=depth, w=1.0/width
    float4 FroxelDims2;         // 112: x=1.0/height, y=1.0/depth, z=pad, w=pad

    // Volume params (48 bytes)
    float4 VolumeParams1;       // 128: x=extinction_scale, y=scattering_albedo, z=density_threshold, w=density_fade_range
    float4 VolumeParams2;       // 144: x=height_fog_base, y=height_fog_scale, z=ambient_intensity, w=time
    float4 VolumeParams3;       // 160: x=phase_g, y=noise_scale, z=noise_speed, w=pad

    // Screen (16 bytes)
    float4 ScreenParams;        // 176: x=screen_width, y=screen_height, z=1.0/width, w=1.0/height

    // SDF cascades (144 bytes)
    float4 SdfOrigins[3];       // 192
    float4 SdfVoxelSizes[3];    // 240
    float4 SdfExtents[3];       // 288
    float4 SdfResolutions;      // 336: xyz=resolutions (float), w=cascade_count

    // Light (32 bytes)
    float4 LightDirection;      // 352
    float4 LightColor;          // 368

    // Shadow VP matrices (256 bytes) — P2-B: 4 cascades（与 C++ 逐字节镜像，
    // 当前路径仅消费 [0..1]）
    float4 ShadowVP[4][4];      // 384: [cascade][column], lightProj * lightView

    // 2026-09-16（audit V3/V5，与 GLSL 同步）: 真实相机 near/far + 常量介质
    // 调试密度。
    float4 FrustumParams2;      // 640: x=camNear, y=camFar, z=density_uniform_debug, w=pad

    // 2026-09-30（froxel-upgrade 计划 WP1/WP2/WP3，与 C++ FroxelTypes.h /
    // GLSL FroxelCommon.glsl 逐字节一致，800B）。Metal 端行为接线在本文件
    // 的 integrate/后续 pass 移植时消费；布局先行同步是三方镜像铁律
    //（计划 §3.4：布局不一致 = MSL 端未定义行为）。
    float4 VolumeParams4;       // 656: x=march_steps(0=legacy 单查询),
                                //      y=march_distance(世界单位),
                                //      z=ambient_source(0=硬编码,1=SPGI),
                                //      w=temporal_blend(0=关)
    float4 ViewProj[4];         // 672: 当前帧 view-projection（WP2）
    float4 PrevInvViewProj[4];  // 736: 上一帧 inverse view-projection（WP3）
    float4 CloudA;              // 800: x=enable, y=radius, z=density,
                                //      w=noise_freq（WP5 云原语）
    float4 CloudB;              // 816: xyz=center(world), w=wind(漂移速度)
};

// ============================================================================
// Froxel coordinate → world position
// ============================================================================

float3 froxel_to_world(uint3 froxel_xyz, constant FroxelParams& params) {
    // Exponential Z: z_view = near * pow(far/near, (slice + 0.5) / depth_count)
    // 契约（audit V2，与 GLSL 一致）: 整数 = 单元中心，helper 内统一 +0.5。
    float z_frac = (float(froxel_xyz.z) + 0.5) * params.FrustumParams.w;
    float z_view = params.FrustumParams.x *
                   pow(params.FrustumParams.y / params.FrustumParams.x, z_frac);

    // Screen UV → NDC：引擎表面约定 ndc=(2u-1, 1-2v)（旧式 1-(2u-1) 得
    // 2-2v，Y 偏了半个视场）
    float2 screen_uv = (float2(froxel_xyz.xy) + 0.5) / float2(params.FroxelDims.xy);
    float2 ndc = float2(screen_uv.x * 2.0 - 1.0, 1.0 - 2.0 * screen_uv.y);

    // Near plane world position for this pixel (NDC z = 0)
    float4 near4 = params.InvViewProj[0] * ndc.x
                 + params.InvViewProj[1] * ndc.y
                 + params.InvViewProj[2] * 0.0
                 + params.InvViewProj[3];
    near4 /= near4.w;

    // Scale from near plane: 近点在相机 near 平面，基准必须是 camNear
    //（audit V3: 旧式除以雾 near 放大 5 倍）
    float3 cam_to_near = near4.xyz - params.CameraPos.xyz;
    float scale = z_view / params.FrustumParams2.x;
    return params.CameraPos.xyz + cam_to_near * scale;
}

// ============================================================================
// View-space depth from froxel Z slice
// ============================================================================

float froxel_z_to_depth(float slice, constant FroxelParams& params) {
    float z_frac = (slice + 0.5) * params.FrustumParams.w;
    return params.FrustumParams.x *
           pow(params.FrustumParams.y / params.FrustumParams.x, z_frac);
}

// ============================================================================
// NDC depth → view-space Z
// ============================================================================

// scene depth 解码用真实相机 near/far（audit V3，与 GLSL 同步）
float ndc_to_view_z(float ndc_z, constant FroxelParams& params) {
    float near = params.FrustumParams2.x;
    float far = params.FrustumParams2.y;
    return (near * far) / (far - ndc_z * (far - near));
}

// 相机 forward 轴（屏幕中心远平面点方向，与 GLSL froxelCameraForward 同式）
float3 froxel_camera_forward(constant FroxelParams& params) {
    float4 far4 = params.InvViewProj[0] * 0.0
                + params.InvViewProj[1] * 0.0
                + params.InvViewProj[2] * 1.0
                + params.InvViewProj[3];
    far4 /= far4.w;
    return normalize(far4.xyz - params.CameraPos.xyz);
}

// WP1: 世界坐标 → froxel 归一化 UVW（与 GLSL froxelWorldToUvw 逐式一致；
// Metal march 移植时消费）
float3 froxel_world_to_uvw(float3 world_pos, constant FroxelParams& params) {
    float4 clip = params.ViewProj[0] * world_pos.x
                + params.ViewProj[1] * world_pos.y
                + params.ViewProj[2] * world_pos.z
                + params.ViewProj[3];
    float3 sc = clip.xyz / clip.w;
    float2 uv = float2(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
    float z_view = max(dot(world_pos - params.CameraPos.xyz,
                           froxel_camera_forward(params)),
                       params.FrustumParams.x);
    float w = clamp(log2(z_view / params.FrustumParams.x) / params.FrustumParams.z,
                    0.0, 1.0);
    return float3(uv, w);
}

// ============================================================================
// View-space Z → froxel W coordinate [0, 1]
// ============================================================================

float view_z_to_froxel_w(float z_view, constant FroxelParams& params) {
    // w = log2(z/near) / log2(far/near)
    return log2(z_view / params.FrustumParams.x) / params.FrustumParams.z;
}

// ============================================================================
// Froxel-specific SDF cascade sampling
// ============================================================================

float froxel_sample_best_sdf(float3 pos,
                              texture3d<float, access::sample> sdf0,
                              texture3d<float, access::sample> sdf1,
                              texture3d<float, access::sample> sdf2,
                              constant FroxelParams& params) {
    uint count = uint(params.SdfResolutions.w);
    if (count >= 1) {
        float d = sampleSDFCascade(pos, sdf0,
            params.SdfOrigins[0], params.SdfVoxelSizes[0], params.SdfExtents[0],
            uint(params.SdfResolutions.x));
        if (d < 1e9f) return d;
    }
    if (count >= 2) {
        float d = sampleSDFCascade(pos, sdf1,
            params.SdfOrigins[1], params.SdfVoxelSizes[1], params.SdfExtents[1],
            uint(params.SdfResolutions.y));
        if (d < 1e9f) return d;
    }
    if (count >= 3) {
        float d = sampleSDFCascade(pos, sdf2,
            params.SdfOrigins[2], params.SdfVoxelSizes[2], params.SdfExtents[2],
            uint(params.SdfResolutions.z));
        if (d < 1e9f) return d;
    }
    return 1e10f;
}

// ============================================================================
// Density sampling (world-space, no AABB bounds — frustum-aligned)
// ============================================================================

float froxel_sample_density(float3 world_pos,
                             texture3d<float, access::sample> noise_tex,
                             texture3d<float, access::sample> sdf0,
                             texture3d<float, access::sample> sdf1,
                             texture3d<float, access::sample> sdf2,
                             constant FroxelParams& params) {
    // V-Test-A: 常量介质调试模式（与 GLSL 同步）
    if (params.FrustumParams2.z > 0.0) return params.FrustumParams2.z;

    // Height gradient (world-space Y)
    float height_grad = 1.0;
    if (params.VolumeParams2.y > 0.0) {
        float h = smoothstep(params.VolumeParams2.x - params.VolumeParams2.y,
                             params.VolumeParams2.x + params.VolumeParams2.y,
                             world_pos.y);
        height_grad = 1.0 - h;
    }

    // Noise texture sample (world-space)
    float3 noise_uvw = world_pos * params.VolumeParams3.y;
    constexpr sampler s(coord::normalized, filter::linear,
                        address::repeat, mip_filter::none);
    float base_shape = noise_tex.sample(s, noise_uvw).x;

    // Detail + erosion (procedural Worley)
    float detail = worley_noise_3d(world_pos * 2.0);
    float density = (base_shape * 0.6 + detail * 0.4) * height_grad;

    float erosion = worley_noise_3d(world_pos * 2.5);
    density *= smoothstep(0.0, 0.35, erosion);

    // SDF fade
    uint cascade_count = uint(params.SdfResolutions.w);
    if (cascade_count > 0) {
        float sdf = froxel_sample_best_sdf(world_pos, sdf0, sdf1, sdf2, params);
        // audit V5（与 GLSL 同步）: 固体内部剔除 + 远场 0.25 霾底；覆盖外
        // 保留霾底。所有 smoothstep 保证 edge0<edge1（旧式反向写法未定义）。
        float sdf_gate;
        if (sdf > 1e9f) {
            sdf_gate = 0.25f;
        } else {
            float air = 1.0 - smoothstep(0.0, 10.0, max(sdf, 0.0));
            float not_solid = smoothstep(-1.5, -0.1, sdf);
            sdf_gate = mix(0.25, 1.0, air) * not_solid;
        }
        density *= sdf_gate;
    }

    // Density fade with distance
    float dist = length(world_pos - params.CameraPos.xyz);
    if (params.VolumeParams1.w > 0.0) {
        float fade = 1.0 - smoothstep(0.0, params.VolumeParams1.w,
                                      dist - params.VolumeParams1.w);
        density *= fade;
    }

    return max(0.0, density);
}

// ============================================================================
// Shadow VP matrix multiply (column-major float4[4] * float4 → NDC)
// ============================================================================

// audit V4（与 GLSL 同步）: 旧式逐列 dot = Mᵀ·p（转置）。列主序契约下的
// 标准乘法是 col0*p.x + col1*p.y + col2*p.z + col3*p.w。
float3 shadow_project(float3 world_pos, constant float4 vp[4]) {
    float4 clip = vp[0] * world_pos.x
                + vp[1] * world_pos.y
                + vp[2] * world_pos.z
                + vp[3];
    return clip.xyz / clip.w;
}

// Shadow UV from NDC clip coords (Metal UV: Y flipped)
float2 shadow_ndc_to_uv(float3 ndc) {
    return float2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}
