// FroxelCommon.glsl — P2.1 (2026-09-15): FroxelFogPass 的 GLSL 移植
// （移植自 Metal/shaders/Volume/FroxelCommon.metal，路线图 Phase 2：
//  FroxelFog Vulkan 启用）。Vulkan 扁平绑定命名空间下 UBO 使用独立
//  binding（6/3/3，与 C++ FroxelFogPass 布局重编号一致），不再与
//  StorageImage 共占 0（Metal 双命名空间写法在 Vulkan 触发 00279）。
// 采样语义：RHI 的 SampledImage 是无采样器绑定，硬件线性采样不可用——
//  手写三线性（8 tap，clamp/REPEAT 各按用途）与 09-06 SDF 三线性修复
//  （TraceLib.glsl）同一做法。
#extension GL_EXT_samplerless_texture_functions : require

// ---- FroxelParams（与 C++ FroxelTypes.h 逐字节一致，512B，std140）----
// binding 由各 comp 在 include 前定义（density=6，light/resolve=3）。
#ifndef FROXEL_PARAMS_BINDING
#define FROXEL_PARAMS_BINDING 6
#endif
layout(std140, binding = FROXEL_PARAMS_BINDING) uniform FroxelParamsBlock {
    vec4 CameraPos;          // 0
    vec4 InvViewProj[4];     // 16
    vec4 FrustumParams;      // 80:  x=near, y=far, z=log2(far/near), w=1/depth_slices
    vec4 FroxelDims;         // 96:  x=width, y=height, z=depth, w=1/width
    vec4 FroxelDims2;        // 112: x=1/height, y=1/depth, z=pad, w=pad
    vec4 VolumeParams1;      // 128: x=extinction_scale, y=scattering_albedo, z=density_threshold, w=density_fade_range
    vec4 VolumeParams2;      // 144: x=height_fog_base, y=height_fog_scale, z=ambient_intensity, w=time
    vec4 VolumeParams3;      // 160: x=phase_g, y=noise_scale, z=noise_speed,
                             //      w=shadow_mode (P2-B: 0=无阴影恒lit, 1=VSM moments)
    vec4 ScreenParams;       // 176: x=width, y=height, z=1/width, w=1/height
    vec4 SdfOrigins[3];      // 192
    vec4 SdfVoxelSizes[3];   // 240
    vec4 SdfExtents[3];      // 288
    vec4 SdfResolutions;     // 336: xyz= resolutions, w= cascade_count
    vec4 LightDirection;     // 352
    vec4 LightColor;         // 368
    vec4 ShadowVP[4][4];     // 384: [cascade][column] — P2-B: 4 级联 light VP
    vec4 FrustumParams2;     // 640: x=camNear, y=camFar, z=density_uniform_debug, w=pad
    // 2026-09-30（froxel-upgrade 计划 WP1/WP2/WP3，与 C++ FroxelTypes.h
    // 逐字节一致，800B）:
    vec4 VolumeParams4;      // 656: x=march_steps(0=legacy 单查询), y=march_distance,
                             //      z=ambient_source(0=硬编码 fallback,1=SPGI), w=temporal_blend
    vec4 ViewProj[4];        // 672: 当前帧 view-projection（WP2 世界→屏幕采 GI）
    vec4 PrevInvViewProj[4]; // 736: 上一帧 inverse view-projection（WP3 重投影）
    vec4 CloudA;             // 800: x=enable, y=radius, z=density, w=noise_freq（云原语）
    vec4 CloudB;             // 816: xyz=center(world), w=wind(漂移速度)
} params;

// ---- 噪声/杂项 ----
float froxelHash(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yxz + 19.19);
    return fract((p.x + p.y) * p.z);
}

float froxelWorley(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    float minDist = 1.0;
    for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
    for (int z = -1; z <= 1; ++z) {
        vec3 neighbor = vec3(float(x), float(y), float(z));
        vec3 point = vec3(
            froxelHash(i + neighbor + vec3(0.0, 0.0, 0.0)),
            froxelHash(i + neighbor + vec3(37.0, 0.0, 0.0)),
            froxelHash(i + neighbor + vec3(0.0, 0.0, 74.0)));
        minDist = min(minDist, length(neighbor + point - f));
    }
    return minDist;
}

// ---- 手写三线性（clamp-to-edge；用于 SDF/密度/scatter 采样）----
// vec4 版：density/scatter 的 RGBA 通道都需要时共用（.r 场景取返回值 .r）。
vec4 froxelTrilinearClamp4(texture3D tex, vec3 uvw, vec3 dims) {
    vec3 p = clamp(uvw, vec3(0.0), vec3(1.0)) * dims - 0.5;
    ivec3 c0 = ivec3(floor(p));
    vec3 t = fract(p);
    ivec3 c1 = min(c0 + ivec3(1), ivec3(dims) - ivec3(1));
    c0 = max(c0, ivec3(0));
    vec4 v000 = texelFetch(tex, ivec3(c0.x, c0.y, c0.z), 0);
    vec4 v100 = texelFetch(tex, ivec3(c1.x, c0.y, c0.z), 0);
    vec4 v010 = texelFetch(tex, ivec3(c0.x, c1.y, c0.z), 0);
    vec4 v110 = texelFetch(tex, ivec3(c1.x, c1.y, c0.z), 0);
    vec4 v001 = texelFetch(tex, ivec3(c0.x, c0.y, c1.z), 0);
    vec4 v101 = texelFetch(tex, ivec3(c1.x, c0.y, c1.z), 0);
    vec4 v011 = texelFetch(tex, ivec3(c0.x, c1.y, c1.z), 0);
    vec4 v111 = texelFetch(tex, ivec3(c1.x, c1.y, c1.z), 0);
    return mix(mix(mix(v000, v100, t.x), mix(v010, v110, t.x), t.y),
               mix(mix(v001, v101, t.x), mix(v011, v111, t.x), t.y), t.z);
}

float froxelTrilinearClamp(texture3D tex, vec3 uvw, vec3 dims) {
    return froxelTrilinearClamp4(tex, uvw, dims).r;
}

// ---- 手写三线性（REPEAT；用于噪声纹理，世界空间无界坐标）----
float froxelTrilinearRepeat(texture3D tex, vec3 uvw, vec3 dims) {
    vec3 p = fract(uvw) * dims - 0.5;
    ivec3 c0 = ivec3(floor(p));
    vec3 t = fract(p);
    ivec3 d = ivec3(dims);
    ivec3 c1 = (c0 + ivec3(1)) % d;
    c0 = ((c0 % d) + d) % d;
    float v000 = texelFetch(tex, ivec3(c0.x, c0.y, c0.z), 0).r;
    float v100 = texelFetch(tex, ivec3(c1.x, c0.y, c0.z), 0).r;
    float v010 = texelFetch(tex, ivec3(c0.x, c1.y, c0.z), 0).r;
    float v110 = texelFetch(tex, ivec3(c1.x, c1.y, c0.z), 0).r;
    float v001 = texelFetch(tex, ivec3(c0.x, c0.y, c1.z), 0).r;
    float v101 = texelFetch(tex, ivec3(c1.x, c0.y, c1.z), 0).r;
    float v011 = texelFetch(tex, ivec3(c0.x, c1.y, c1.z), 0).r;
    float v111 = texelFetch(tex, ivec3(c1.x, c1.y, c1.z), 0).r;
    return mix(mix(mix(v000, v100, t.x), mix(v010, v110, t.x), t.y),
               mix(mix(v001, v101, t.x), mix(v011, v111, t.x), t.y), t.z);
}

// ---- 坐标换算（与 Metal FroxelCommon 逐式一致）----
// 契约（2026-09-16 audit V2）: 参数是连续 froxel 坐标，**整数值 = 单元中心**；
// cell 中心由调用方传整数值（不再自行 +0.5），helper 内对 xy/z 统一加 0.5。
// NDC 采用引擎表面约定 ndc=(2u-1, 1-2v)（旧式 `1-(2u-1)` 得 2-2v，等于把
// 中心射线抬向画面上边界，整片雾场错位）。
vec3 froxelToWorld(vec3 froxelXyz) {
    float zFrac = (froxelXyz.z + 0.5) * params.FrustumParams.w;
    // zView 是雾体自身范围（FrustumParams），只定义切片分布
    float zView = params.FrustumParams.x *
                  pow(params.FrustumParams.y / params.FrustumParams.x, zFrac);
    vec2 screenUv = (froxelXyz.xy + 0.5) / params.FroxelDims.xy;
    vec2 ndc = vec2(screenUv.x * 2.0 - 1.0, 1.0 - 2.0 * screenUv.y);
    vec4 near4 = params.InvViewProj[0] * ndc.x
               + params.InvViewProj[1] * ndc.y
               + params.InvViewProj[2] * 0.0
               + params.InvViewProj[3];
    near4 /= near4.w;
    vec3 camToNear = near4.xyz - params.CameraPos.xyz;
    // 近点在相机 near 平面（NDC z=0 ↔ camNear），缩放基准必须是 camNear；
    // 旧式除以雾 near（0.5 vs 相机 0.1）放大 5 倍（audit V3）。
    float scale = zView / params.FrustumParams2.x;
    return params.CameraPos.xyz + camToNear * scale;
}

float froxelZToDepth(float slice) {
    float zFrac = (slice + 0.5) * params.FrustumParams.w;
    return params.FrustumParams.x *
           pow(params.FrustumParams.y / params.FrustumParams.x, zFrac);
}

// scene depth 解码用真实相机 near/far（audit V3）：雾 near/far 不再混入。
float ndcToViewZ(float ndcZ) {
    float n = params.FrustumParams2.x;
    float f = params.FrustumParams2.y;
    return (n * f) / (f - ndcZ * (f - n));
}

// 相机 forward 轴（屏幕中心远平面点方向）——ray 沿轴深度 ↔ 真实长度换算用。
vec3 froxelCameraForward() {
    vec2 centerUv = vec2(0.5, 0.5);
    vec2 ndc = vec2(centerUv.x * 2.0 - 1.0, 1.0 - 2.0 * centerUv.y);
    vec4 far4 = params.InvViewProj[0] * ndc.x
              + params.InvViewProj[1] * ndc.y
              + params.InvViewProj[2] * 1.0
              + params.InvViewProj[3];
    far4 /= far4.w;
    return normalize(far4.xyz - params.CameraPos.xyz);
}

// WP1: 世界坐标 → froxel 归一化 UVW（march 沿路采 froxel_density 用）。
// froxelToWorld 的逆：深度用视轴投影（与 froxelZToDepth 同一指数分布），
// xy 用 ViewProj 投影（与 FroxelResolve 的 uv←ndc 约定一致）。
// 越界（相机身后/雾 far 之外）不裁剪——由三线性的 clamp-to-edge 兜底。
vec3 froxelWorldToUvw(vec3 worldPos) {
    vec4 clip = params.ViewProj[0] * worldPos.x
              + params.ViewProj[1] * worldPos.y
              + params.ViewProj[2] * worldPos.z
              + params.ViewProj[3];
    vec3 sc = clip.xyz / clip.w;
    vec2 uv = vec2(sc.x * 0.5 + 0.5, 0.5 - sc.y * 0.5);
    float zView = max(dot(worldPos - params.CameraPos.xyz, froxelCameraForward()),
                      params.FrustumParams.x);
    float w = clamp(log2(zView / params.FrustumParams.x) / params.FrustumParams.z,
                    0.0, 1.0);
    return vec3(uv, w);
}

// ---- SDF 级联采样（手写三线性）----
float froxelSampleSdf(texture3D sdfTex, vec3 pos, vec4 origin, vec4 voxelSize, vec4 extent, vec3 dims) {
    vec3 uvw = (pos - origin.xyz) / extent.xyz;
    if (uvw.x < 0.0 || uvw.x > 1.0 ||
        uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0) return 1e10;
    return froxelTrilinearClamp(sdfTex, uvw, dims);
}

float froxelSampleBestSdf(vec3 pos, texture3D sdf0, texture3D sdf1, texture3D sdf2) {
    uint count = uint(params.SdfResolutions.w);
    if (count >= 1u) {
        float d = froxelSampleSdf(sdf0, pos, params.SdfOrigins[0], params.SdfVoxelSizes[0],
                                  params.SdfExtents[0], vec3(params.SdfResolutions.x));
        if (d < 1e9) return d;
    }
    if (count >= 2u) {
        float d = froxelSampleSdf(sdf1, pos, params.SdfOrigins[1], params.SdfVoxelSizes[1],
                                  params.SdfExtents[1], vec3(params.SdfResolutions.y));
        if (d < 1e9) return d;
    }
    if (count >= 3u) {
        float d = froxelSampleSdf(sdf2, pos, params.SdfOrigins[2], params.SdfVoxelSizes[2],
                                  params.SdfExtents[2], vec3(params.SdfResolutions.z));
        if (d < 1e9) return d;
    }
    return 1e10;
}

// ---- 密度采样（与 Metal froxel_sample_density 逐式一致）----
// audit V5: 密度描述空气。SDF 只做"固体内部剔除 + 表面邻近富集"，级联
// 覆盖外保留基础霾底（不再因 SDF 缺失把整片空气清零）；所有 smoothstep
// 保证 edge0<edge1（反向写成 1-smoothstep，旧式 edge0>edge1 在 GLSL 未定义）。
float froxelSampleDensity(vec3 worldPos, texture3D noiseTex, vec3 noiseDims,
                          texture3D sdf0, texture3D sdf1, texture3D sdf2) {
    // V-Test-A: 常量介质调试模式——完全绕过 noise/SDF/height/fade。
    if (params.FrustumParams2.z > 0.0) return params.FrustumParams2.z;

    float heightGrad = 1.0;
    if (params.VolumeParams2.y > 0.0) {
        float h = smoothstep(params.VolumeParams2.x - params.VolumeParams2.y,
                             params.VolumeParams2.x + params.VolumeParams2.y,
                             worldPos.y);
        heightGrad = 1.0 - h;
    }

    vec3 noiseUvw = worldPos * params.VolumeParams3.y;
    float baseShape = froxelTrilinearRepeat(noiseTex, noiseUvw, noiseDims);
    float detail = froxelWorley(worldPos * 2.0);
    float density = (baseShape * 0.6 + detail * 0.4) * heightGrad;

    float erosion = froxelWorley(worldPos * 2.5);
    density *= smoothstep(0.0, 0.35, erosion);

    uint cascadeCount = uint(params.SdfResolutions.w);
    if (cascadeCount > 0u) {
        float sdf = froxelSampleBestSdf(worldPos, sdf0, sdf1, sdf2);
        // 表面邻近满密度 → 远场 0.25 霾底；固体内部（sdf<0）剔除；
        // 覆盖外（1e10 哨兵）= 无几何信息的高空/远场，保留霾底不清零。
        float sdfGate;
        if (sdf > 1e9) {
            sdfGate = 0.25;
        } else {
            float air = 1.0 - smoothstep(0.0, 10.0, max(sdf, 0.0));
            float notSolid = smoothstep(-1.5, -0.1, sdf);
            sdfGate = mix(0.25, 1.0, air) * notSolid;
        }
        density *= sdfGate;
    }

    float dist = length(worldPos - params.CameraPos.xyz);
    if (params.VolumeParams1.w > 0.0) {
        // 距相机 fade_range 内全密度，其后在 fade_range 跨度内收敛 0
        float fade = 1.0 - smoothstep(0.0, params.VolumeParams1.w,
                                      dist - params.VolumeParams1.w);
        density *= fade;
    }

    return max(0.0, density);
}

// ---- 阴影投影（列主序 float4[4]）----
// audit V4: 旧式逐列 dot(vec4(p,1), col) 等效 Mᵀ·p（转置矩阵）。列主序契约
// 下的标准乘法是 col0*p.x + col1*p.y + col2*p.z + col3*p.w，与表面
// ShadowFilter.comp 的 `vp * vec4(worldPos,1)` 同约定。
vec3 froxelShadowProject(vec3 worldPos, vec4 col0, vec4 col1, vec4 col2, vec4 col3) {
    vec4 clip = col0 * worldPos.x
              + col1 * worldPos.y
              + col2 * worldPos.z
              + col3;
    return clip.xyz / clip.w;
}

// Shadow UV（NDC → UV，Y 翻转与 Metal 端一致）
vec2 froxelShadowNdcToUv(vec3 ndc) {
    return vec2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}
