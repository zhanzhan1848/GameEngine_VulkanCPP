#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::volume {

// ============================================================================
// FroxelParams — GPU constant buffer struct (must match Metal shader)
// ============================================================================
// Layout: 384 bytes. Allocated as 512 bytes in constant buffer.

struct FroxelParams {
    // Camera (80 bytes)
    math::v4 CameraPos;           // 0:   xyz = camera position
    math::v4 InvViewProj[4];      // 16:  inverse view-projection matrix (64 bytes)

    // Frustum (16 bytes)
    math::v4 FrustumParams;       // 80:  x=near, y=far, z=log2(far/near), w=1.0/depth_slices

    // Froxel grid (32 bytes)
    math::v4 FroxelDims;          // 96:  x=width, y=height, z=depth, w=1.0/width
    math::v4 FroxelDims2;         // 112: x=1.0/height, y=1.0/depth, z=pad, w=pad

    // Volume params (48 bytes)
    math::v4 VolumeParams1;       // 128: x=extinction_scale, y=scattering_albedo, z=density_threshold, w=density_fade_range
    math::v4 VolumeParams2;       // 144: x=height_fog_base, y=height_fog_scale, z=ambient_intensity, w=time
    math::v4 VolumeParams3;       // 160: x=phase_g, y=noise_scale, z=noise_speed,
                                  //      w=shadow_mode (P2-B: 0=无阴影输入恒 lit,
                                  //      1=VSM moments; P1 reaudit §4.4: 2=PCSS
                                  //      light-space 深度比较, Vulkan only)

    // Screen (16 bytes)
    math::v4 ScreenParams;        // 176: x=screen_width, y=screen_height, z=1.0/width, w=1.0/height

    // GlobalSDF cascade data (same packing as VolumeParams)
    math::v4 SdfOrigins[3];       // 192
    math::v4 SdfVoxelSizes[3];    // 240
    math::v4 SdfExtents[3];       // 288
    math::v4 SdfResolutions;      // 336: xyz=resolutions, w=cascade_count

    // Light (32 bytes)
    math::v4 LightDirection;      // 352: xyz = direction
    math::v4 LightColor;          // 368: xyz = color

    // Shadow VP matrices (256 bytes) — P2-B: 4 cascades (was 2)。
    // 坐标契约修正：froxel 逐世界坐标投影到 light clip space 后对
    // light-space VSM moments 做深度比较；不再采样屏幕空间表面可见性图。
    // Metal MSL 端已镜像 [4][4]（仅消费 [0..1]），布局逐字节一致。
    math::v4 ShadowVP[4][4];      // 384: [cascade][column], lightProj * lightView

    // 2026-09-16（volume-gi-repair-audit V3/V5）: 相机深度与雾范围分离。
    // FrustumParams 是雾体自身的 near/far（只定义 froxel 切片分布），scene
    // depth 解码必须用真实相机 near/far——此前 ndcToViewZ 用 0.5/200 去解
    // 0.1/1000 投影的深度（5m 场景点被还原成 22m）。z=density_uniform_debug：
    // >0 时密度注入绕过 noise/SDF/height/fade 直接写常量介质（V-Test-A）。
    math::v4 FrustumParams2;      // 640: x=camNear, y=camFar, z=density_uniform_debug, w=pad

    // 2026-09-30（froxel-upgrade 计划 WP1/WP2/WP3，布局三方镜像 GLSL/MSL）:
    math::v4 VolumeParams4;       // 656: x=march_steps(WP1, 0=legacy 单查询),
                                  //      y=march_distance(世界单位),
                                  //      z=ambient_source(WP2, 0=硬编码天空色
                                  //      fallback, 1=SPGI 辐照纹理),
                                  //      w=temporal_blend(WP3, 0=关)
    math::v4 ViewProj[4];         // 672: 当前帧 view-projection（WP2: froxel
                                  //      世界坐标 → 屏幕 UV 采 SPGI 辐照）
    math::v4 PrevInvViewProj[4];  // 736: 上一帧 inverse view-projection（WP3:
                                  //      体积时间重投影；首帧/切镜无效，
                                  //      temporal_blend=0 时 passthrough）

    // 2026-09-30（云原语 WP5）: 局部云体密度源——球形包络 × FBM × Worley
    // 细节,云底压扁；绕过 SDF/高度门(云悬空于开放空域)。
    math::v4 CloudA;             // 800: x=enable(0/1), y=radius(世界),
                                 //      z=density(峰值), w=noise_freq(噪声频率)
    math::v4 CloudB;             // 816: xyz=center(world), w=wind(漂移速度)
};

static_assert(sizeof(FroxelParams) == 832, "FroxelParams GPU layout mismatch");

// ============================================================================
// Froxel Grid Configuration
// ============================================================================

struct FroxelGridConfig {
    u32   grid_width       = 160;
    u32   grid_height      = 90;
    u32   grid_depth       = 64;
    float near_plane       = 0.5f;
    float far_plane        = 200.0f;
    float extinction_scale  = 0.1f;
    float scattering_albedo = 0.8f;
    float density_threshold = 0.01f;
    float density_fade_range = 10.0f;
    float phase_g          = 0.0f;       // Henyey-Greenstein g parameter (0=isotropic)
    float noise_scale      = 1.0f;
    float noise_speed      = 0.1f;
    float height_fog_base  = 0.0f;
    float height_fog_scale = 0.0f;
    float ambient_intensity = 0.3f;
    // 2026-09-16（audit V5/V-Test-A）: >0 时密度注入为常量介质（绕过
    // noise/SDF/height/fade，仅保留几何清零）——坐标/积分标定的基准路径。
    float density_uniform_debug = -1.0f;
    // 2026-09-30（froxel-upgrade 计划 WP1/WP2/WP3）:
    // march_steps>0 时积分 pass 沿光方向 N 步 march（雾对雾自阴影），0=现行
    // 单次阴影查询（回滚位）。march_distance 是 march 总长（世界单位）。
    u32   march_steps = 0;
    float march_distance = 16.0f;
    // WP2: >0 时环境项采样 SPGI 辐照（无效句柄/未初始化自动回退硬编码色）。
    u32   ambient_source_spgi = 0;
    // WP3: temporal_blend>0 时启用体积时间重投影（当前:历史 = blend）。
    float temporal_blend = 0.0f;
    // WP5 云原语（2026-09-30）: 局部云体密度源,默认关。
    float cloud_enable = 0.0f;
    float cloud_radius = 6.0f;
    float cloud_density = 1.2f;
    float cloud_noise_freq = 0.16f;
    float cloud_center[3] = {8.0f, 4.0f, 1.0f};
    float cloud_wind = 0.0f;
};

// Reuse VolumeCameraData from VolumeTypes.h for per-frame camera/light input.

} // namespace primal::graphics::volume
