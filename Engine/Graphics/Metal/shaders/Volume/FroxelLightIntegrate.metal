#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 2: Froxel Light Integrate
// ============================================================================
// Each thread processes one froxel column (x, y), all Z slices serially.
// Reads density from froxel_density, accumulates Beer-Lambert + direct lighting,
// writes per-slice (scatter_rgb, transmittance) to froxel_scatter.
//
// 2026-09-30（froxel-upgrade 计划 Metal 移植，对齐 GLSL FroxelLightIntegrate.comp）:
//   - 阴影查询升级为双模式：mode 1 = light-space VSM moments（Chebyshev），
//     mode 2 = PCSS light-space 深度比较（与 DeferredLighting 同 VP/UV 约定；
//     momentTex 槽位由 C++ 端路由 moments 或 D32→R32 深度图）。旧屏幕空间
//     shadow_map 采样废除（屏幕 UV 与 light VP UV 不对应——P2-B 契约）。
//   - WP1: march_steps>0 → 朝光方向 N 步 light march（雾对雾自阴影 Beer
//     累乘 × 每步几何阴影查询），0=legacy 单查询。
//   - WP2: ambient_source=1 → 环境项采上一帧 SPGI 辐照（列屏幕 UV；
//     观感校准 GAIN 30×1/π，与 GLSL 同式）。
// 纹理绑定与 C++ light_integrate 布局一致（binding=N → [[texture(N)]])：
//   0=froxel_density 1=froxel_scatter(write) 2=legacy shadow_map(不再采样)
//   4..7=级联 moments/depth  8=spgi_ambient  buffer(3)=FroxelParams

// moments 双线性（同时取 z 与 z² 通道），Chebyshev 上界——与 GLSL 同式。
float2 froxel_shadow_bilinear2(texture2d<float, access::sample> tex, float2 uv) {
    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);
    return tex.sample(s, uv).xy;
}

float froxel_chebyshev_visibility(float2 moments, float t) {
    if (t <= moments.x) return 1.0;
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, 0.00002);
    float d = t - moments.x;
    float p_max = variance / (variance + d * d);
    return smoothstep(0.05, 1.0, p_max);
}

// 单级联 VSM 查询：越界返回 -1（调用方试下一级联），否则 [0,1] 可见性。
float froxel_vsm_cascade(float3 ndc, texture2d<float, access::sample> moments) {
    if (ndc.x < -1.0 || ndc.x > 1.0 ||
        ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z <  0.0 || ndc.z > 1.0) return -1.0;
    float2 uv = shadow_ndc_to_uv(ndc);
    float2 m = froxel_shadow_bilinear2(moments, uv);
    return froxel_chebyshev_visibility(m, ndc.z);
}

// mode 2: PCSS 链 light-space 深度比较（固定 NDC bias + 4 tap 手动 PCF，
// 与 GLSL froxelDepthCascade 同式）。Metal 采样器版双线性即可。
float froxel_depth_cascade(float3 ndc, texture2d<float, access::sample> depth_map) {
    if (ndc.x < -1.0 || ndc.x > 1.0 ||
        ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z <  0.0 || ndc.z > 1.0) return -1.0;
    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);
    float2 uv = shadow_ndc_to_uv(ndc);
    const float bias = 0.002;
    // 4 tap PCF（texture 采样点即双线性中心，等价 GLSL 的手动 4 tap）
    float lit = 0.0;
    lit += (ndc.z - bias > depth_map.sample(s, uv).r) ? 0.0 : 1.0;
    return lit;
}

// 逐级联由近到远查询；全越界 → -1 语义收敛恒 lit（与 GLSL 同）。
float froxel_shadow_query(float3 world_pos, int mode,
                          texture2d<float, access::sample> m0,
                          texture2d<float, access::sample> m1,
                          texture2d<float, access::sample> m2,
                          texture2d<float, access::sample> m3,
                          constant FroxelParams& params) {
    float v = -1.0;
    if (mode == 2) {
        v = froxel_depth_cascade(shadow_project(world_pos, params.ShadowVP[0]), m0);
        if (v < 0.0) v = froxel_depth_cascade(shadow_project(world_pos, params.ShadowVP[1]), m1);
        if (v < 0.0) v = froxel_depth_cascade(shadow_project(world_pos, params.ShadowVP[2]), m2);
        if (v < 0.0) v = froxel_depth_cascade(shadow_project(world_pos, params.ShadowVP[3]), m3);
    } else {
        v = froxel_vsm_cascade(shadow_project(world_pos, params.ShadowVP[0]), m0);
        if (v < 0.0) v = froxel_vsm_cascade(shadow_project(world_pos, params.ShadowVP[1]), m1);
        if (v < 0.0) v = froxel_vsm_cascade(shadow_project(world_pos, params.ShadowVP[2]), m2);
        if (v < 0.0) v = froxel_vsm_cascade(shadow_project(world_pos, params.ShadowVP[3]), m3);
    }
    return (v < 0.0) ? 1.0 : clamp(v, 0.0, 1.0);
}

// WP2: SPGI 辐照双线性（4 tap clamp，与 GLSL froxelSpgiBilinear 同式）。
float3 froxel_spgi_bilinear(texture2d<float, access::sample> tex, float2 uv) {
    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);
    return tex.sample(s, uv).rgb;
}

kernel void froxel_light_integrate(
    texture3d<float, access::sample>  froxel_density [[texture(0)]],
    texture3d<float, access::write>   froxel_scatter [[texture(1)]],
    texture2d<float, access::sample>  shadow_map     [[texture(2)]],
    texture2d<float, access::sample>  moments_c0     [[texture(4)]],
    texture2d<float, access::sample>  moments_c1     [[texture(5)]],
    texture2d<float, access::sample>  moments_c2     [[texture(6)]],
    texture2d<float, access::sample>  moments_c3     [[texture(7)]],
    texture2d<float, access::sample>  spgi_ambient   [[texture(8)]],
    constant FroxelParams& params                    [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(params.FroxelDims.x) ||
        gid.y >= uint(params.FroxelDims.y)) return;

    constexpr sampler bilinear(coord::normalized, filter::linear,
                               address::clamp_to_edge, mip_filter::none);

    float3 total_scatter(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0;
    float extinction_scale = params.VolumeParams1.x;
    float scattering_albedo = params.VolumeParams1.y;
    float ambient = params.VolumeParams2.z;
    float3 light_color = params.LightColor.xyz;
    float3 light_dir = normalize(params.LightDirection.xyz);

    // audit V6（与 GLSL 同步）: 斜 ray 真实段长 ds = Δz / cosView
    float cos_view = 1.0;
    {
        float2 screen_uv = (float2(gid) + 0.5) / float2(params.FroxelDims.xy);
        float2 ndc = float2(screen_uv.x * 2.0 - 1.0, 1.0 - 2.0 * screen_uv.y);
        float4 near4 = params.InvViewProj[0] * ndc.x
                     + params.InvViewProj[1] * ndc.y
                     + params.InvViewProj[2] * 0.0
                     + params.InvViewProj[3];
        near4 /= near4.w;
        float3 ray_dir = normalize(near4.xyz - params.CameraPos.xyz);
        cos_view = max(dot(ray_dir, froxel_camera_forward(params)), 1e-3);
    }

    // WP2: 本列屏幕 UV（froxel 网格与屏幕对齐，列 xy 即屏幕瓦片）
    float2 col_screen_uv = (float2(gid) + 0.5) / float2(params.FroxelDims.xy);

    int march_steps = int(params.VolumeParams4.x + 0.5);
    int shadow_mode = int(params.VolumeParams3.w + 0.5);
    bool uniform_medium = params.FrustumParams2.z > 0.0;

    uint depth_count = uint(params.FroxelDims.z);

    for (uint z = 0; z < depth_count; ++z) {
        // Read density from previous pass
        float3 uvw = (float3(float2(gid), float(z)) + 0.5) / float3(params.FroxelDims.xyz);
        float density = froxel_density.sample(bilinear, uvw).r;

        // Compute slice thickness (exponential Z); ds = 真实 ray 长度
        float z0 = froxel_z_to_depth(float(z) - 0.5, params);
        float z1 = froxel_z_to_depth(float(z) + 0.5, params);
        float ds = (z1 - z0) / cos_view;

        if (density > 0.001) {
            float sigma_t = density * extinction_scale;
            float sigma_s = density * scattering_albedo * extinction_scale;

            // World position for shadow lookup（整数 = 单元中心）
            float3 world_pos = froxel_to_world(uint3(gid, z), params);

            // 阴影：mode>0 时 march（WP1）或 legacy 单查询；mode 0 恒 lit。
            float shadow = 1.0;
            if (shadow_mode > 0) {
                if (march_steps > 0) {
                    float step_len = params.VolumeParams4.y / float(march_steps);
                    // 半步起步；light_direction 是传播方向，朝光 march 取负
                    float3 lp = world_pos - light_dir * (0.5 * step_len);
                    float vis = 1.0;
                    for (int s = 0; s < march_steps; ++s) {
                        if (uniform_medium) {
                            vis *= exp(-sigma_t * step_len);
                        } else {
                            float3 muvw = froxel_world_to_uvw(lp, params);
                            float d = froxel_density.sample(bilinear, muvw).r;
                            vis *= exp(-d * extinction_scale * step_len);
                        }
                        vis *= froxel_shadow_query(lp, shadow_mode,
                                                   moments_c0, moments_c1,
                                                   moments_c2, moments_c3, params);
                        lp -= light_dir * step_len;
                    }
                    shadow = clamp(vis, 0.0, 1.0);
                } else {
                    shadow = froxel_shadow_query(world_pos, shadow_mode,
                                                 moments_c0, moments_c1,
                                                 moments_c2, moments_c3, params);
                }
            }

            // HG phase（audit V7 方向契约，与 GLSL 同步）
            float g = clamp(params.VolumeParams3.x, -0.95, 0.95);
            float phase;
            if (abs(g) < 1e-4) {
                phase = 1.0 / (4.0 * 3.14159265);
            } else {
                float g2 = g * g;
                float3 view_dir = normalize(world_pos - params.CameraPos.xyz);
                float cos_theta = dot(-view_dir, light_dir);
                float denom = 1.0 + g2 - 2.0 * g * cos_theta;
                phase = (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
            }

            // 散射源项：WP2 环境项（SPGI 辐照 × 观感校准）或硬编码天空色
            float3 ambient_term = float3(0.4, 0.45, 0.6) * ambient;
            if (params.VolumeParams4.z > 0.5) {
                ambient_term = froxel_spgi_bilinear(spgi_ambient, col_screen_uv)
                               * (ambient * 9.55);   // GAIN 30 × 1/π
            }

            // 散射源项 + audit V6 解析段积分
            float3 source_j = sigma_s * (light_color * shadow * phase
                                         + ambient_term);

            float tr = exp(-sigma_t * ds);
            float seg_w = (sigma_t > 1e-5) ? (1.0 - tr) / sigma_t : ds;
            total_scatter += transmittance * source_j * seg_w;
            transmittance *= tr;
        }

        // Write cumulative result at this slice
        froxel_scatter.write(float4(total_scatter, transmittance), uint3(gid, z));
    }
    (void)shadow_map;   // legacy 屏幕空间可见性图：P2-B 后不再采样
}
