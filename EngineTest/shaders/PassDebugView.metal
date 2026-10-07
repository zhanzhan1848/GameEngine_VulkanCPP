/**
 * @file PassDebugView.metal
 * @brief 视口实时 pass 可视化着色器（PassDebugViewModule 自加载）
 * @details 单一 fragment 入口按 UBO.mode 分派：GBuffer albedo/normal/ORM/
 *          velocity、线性化深度、可见性缓冲 ID 类伪彩色（triangle/meshlet/
 *          cluster/mesh）、SSAO 灰度、HDR 系（deferred/GI indirect/SPGI）
 *          tonemap、SC atlas 直读。
 *
 * Binding 面（对齐 PassDebugViewModule::Initialize 的布局）：
 *   texture(0) float 源纹理        binding0 SampledImage
 *   （constexpr sampler，binding1 Sampler 为 Vulkan 组合采样保留，Metal 不用）
 *   buffer(2)  PassDebugParams     binding2 UniformBuffer
 *   texture(3) float 深度          binding3 SampledImage
 *   buffer(4)  cluster map         binding4 StorageBuffer（条目 4×u32：
 *                                  {globalMeshletIndex, instanceIndex, materialID, pad}）
 *   texture(5) uint 源（可见性缓冲）binding5 SampledImage(R32_UINT)
 *
 * ID 哈希与 Dawn GPUDrivenDraw.wgsl hash_id_to_color 逐位一致（三端同色）。
 */

#include <metal_stdlib>
using namespace metal;

struct PassDebugParams {
    float4x4 inv_view_proj;   // 深度反投影（保留）
    float4x4 proj;            // 线性化用（保留）
    float4 misc;              // x=near, y=far, z=view(mode), w=cluster_count
    float4 misc2;             // x=width, y=height
};

struct PassDebugVSOut {
    float4 position [[position]];
    float2 uv;
};

vertex PassDebugVSOut pass_debug_vs(uint vertexID [[vertex_id]]) {
    PassDebugVSOut out;
    float4 positions[3] = {
        float4(-1.0, -1.0, 0.0, 1.0),
        float4( 3.0, -1.0, 0.0, 1.0),
        float4(-1.0,  3.0, 0.0, 1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(2.0, 1.0),
        float2(0.0, -1.0)
    };
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// 与 Dawn GPUDrivenDraw.wgsl hash_id_to_color 逐位一致。
static inline float3 hash_id_to_color(uint id) {
    uint h = id * 2654435761u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return float3((h & 0xFFu) / 255.0, ((h >> 8u) & 0xFFu) / 255.0, ((h >> 16u) & 0xFFu) / 255.0);
}

static inline float3 aces_tonemap(float3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// [0,1] 深度 → 线性视空间深度（与 CommonFunction.metal NDCDepthToLinear 一致）。
static inline float ndc_depth_to_linear(float ndcDepth, float nearPlane, float farPlane) {
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcDepth * (farPlane - nearPlane));
}

// 从 cluster map 反查 meshlet 所在条目；返回 {entryIndex, instanceIndex}，
// 未命中 entryIndex = 0xFFFFFFFF。
static inline float2 cluster_map_lookup(device const uint* cluster_map, uint cluster_count,
                                        uint meshlet_id) {
    for (uint i = 0; i < cluster_count; ++i) {
        if (cluster_map[i * 4u + 0u] == meshlet_id) {
            return float2(float(i), float(cluster_map[i * 4u + 1u]));
        }
    }
    return float2(4294967295.0, 4294967295.0);
}

fragment float4 pass_debug_fs(PassDebugVSOut in [[stage_in]],
                              texture2d<float> src_tex [[texture(0)]],
                              texture2d<float> depth_tex [[texture(3)]],
                              texture2d<uint> id_tex [[texture(5)]],
                              constant PassDebugParams& params [[buffer(2)]],
                              device const uint* cluster_map [[buffer(4)]],
                              device const float* shadow_vis [[buffer(6)]]) {
    constexpr sampler s(mag_filter::nearest, min_filter::nearest, mip_filter::none,
                        address::clamp_to_edge);
    const float mode = params.misc.z;
    const float2 uv = in.uv;
    const float near_p = params.misc.x;
    const float far_p = params.misc.y;

    // 背景（天空/无几何）统一深灰，深度 >= 1 判定。
    const float d = depth_tex.sample(s, uv).r;
    const bool background = (d >= 0.9999);

    if (mode < 0.5) {
        return float4(0.05, 0.05, 0.05, 1.0);
    } else if (mode < 1.5) {
        // Albedo：直读 + gamma（GBuffer 存线性色，debug 直观看要 sRGB）。
        float3 c = src_tex.sample(s, uv).rgb;
        return float4(pow(clamp(c, 0.0, 1.0), float3(1.0 / 2.2)), 1.0);
    } else if (mode < 2.5) {
        // Normal：GBuffer 已是 [0,1] 编码，直读。
        return float4(src_tex.sample(s, uv).rgb, 1.0);
    } else if (mode < 3.5) {
        // ORM：R=AO, G=Roughness, B=Metallic 直读。
        return float4(src_tex.sample(s, uv).rgb, 1.0);
    } else if (mode < 4.5) {
        // Depth：线性深度 / far → 灰度。
        if (background) return float4(0.05, 0.05, 0.05, 1.0);
        float lin = ndc_depth_to_linear(d, near_p, far_p);
        return float4(float3(clamp(lin / far_p, 0.0, 1.0)), 1.0);
    } else if (mode < 5.5) {
        // Velocity：RG ∈ [-0.5,0.5] → +0.5 显示（R=水平, G=垂直）。
        float3 v = src_tex.sample(s, uv).rgb + 0.5;
        return float4(clamp(v, 0.0, 1.0), 1.0);
    } else if (mode < 9.5) {
        // 6 Visibility(triangle) / 7 MeshletID / 8 ClusterID / 9 MeshID
        if (background) return float4(0.05, 0.05, 0.05, 1.0);
        uint2 pix = uint2(uv * params.misc2.xy);
        uint packed = id_tex.read(pix).r;
        uint tri = packed & 0x00FFFFFFu;
        uint meshlet = (packed >> 24u) & 0xFFu;
        // [PassDebugView 排查] 背景判定改为 packed==0：旧 tri==0 会把每个
        // meshlet 的第 0 号三角形当背景丢弃（黑洞随相机角度移动）。
        if (mode < 6.5) {
            if (packed == 0u) return float4(0.05, 0.05, 0.05, 1.0);
            return float4(hash_id_to_color(tri), 1.0);
        } else if (mode < 7.5) {
            if (packed == 0u) return float4(0.05, 0.05, 0.05, 1.0);
            return float4(hash_id_to_color(meshlet), 1.0);
        } else {
            if (packed == 0u) return float4(0.05, 0.05, 0.05, 1.0);
            float2 found = cluster_map_lookup(cluster_map, (uint)params.misc.w, meshlet);
            if (found.x >= 4294967295.0) {
                // 反查未命中：退化为 meshlet 哈希色并偏红提示。
                return float4(hash_id_to_color(meshlet) * 0.5 + float3(0.25, 0.0, 0.0), 1.0);
            }
            // mode 8 → cluster 序号，mode 9 → instance 序号。
            return float4(hash_id_to_color(mode < 8.5 ? (uint)found.x : (uint)found.y), 1.0);
        }
    } else if (mode < 10.5) {
        // SSAO：R 通道灰度。
        float a = src_tex.sample(s, uv).r;
        return float4(float3(a), 1.0);
    } else if (mode < 12.5) {
        // 11 Deferred / 12 GI Indirect：HDR → ACES tonemap（工作范围内直读）。
        float3 c = src_tex.sample(s, uv).rgb;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else if (mode < 13.5) {
        // 13 SPGI（filtered）：HDR irradiance——乘显示曝光 misc2.z 再 tonemap
        //（sky 2.0 档下 ~9-12，ACES 直读全白）。
        float3 c = src_tex.sample(s, uv).rgb * params.misc2.z;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else if (mode < 14.5) {
        // 14 SC Lighting Atlas：RGBA8 直读。
        return float4(clamp(src_tex.sample(s, uv).rgb, 0.0, 1.0), 1.0);
    } else if (mode < 15.5) {
        // [F3-V1] 15 SC Albedo atlas：rgba8 线性 → gamma 显示（同 GBuffer albedo）。
        float3 c = src_tex.sample(s, uv).rgb;
        return float4(pow(clamp(c, 0.0, 1.0), float3(1.0 / 2.2)), 1.0);
    } else if (mode < 16.5) {
        // [F3-V1] 16 SC Normal atlas：rg 八面体编码 → 解码 rgb（n*0.5+0.5）。
        // 与 Lumen/ShaderLib/MathLib.glsl octDecode 逐位一致。
        float2 f = src_tex.sample(s, uv).rg * 2.0 - 1.0;
        float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
        float t = clamp(-n.z, 0.0, 1.0);
        n.xy += mix(float2(t), float2(-t), step(float2(0.0), n.xy));
        return float4(normalize(n) * 0.5 + 0.5, 1.0);
    } else if (mode < 17.5) {
        // [F3-V1 修正 2026-09-20] 17 SC capture 深度（语义同 GLSL 孪生注释）：
        // 到卡平面轴向距离（主流 0-0.3m）；v<0 暗紫 = 未初始化（-1）。
        float dep = src_tex.sample(s, uv).r;
        if (dep < 0.0) return float4(0.16, 0.04, 0.24, 1.0);
        float g = pow(clamp(dep / 2.0, 0.0, 1.0), 0.4);
        return float4(float3(g), 1.0);
    } else if (mode < 18.5) {
        // [F3-V2] 18 SPGI raw：降噪前原始 gather 输出——乘显示曝光
        //（同 13）再 tonemap。黑洞 = gather 无覆盖区（denoise 归因用）。
        float3 c = src_tex.sample(s, uv).rgb * params.misc2.z;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else if (mode < 19.5) {
        // [F3 扩展] 19 SC Emissive atlas：HDR 自发光 → 显示曝光 × tonemap。
        float3 c = src_tex.sample(s, uv).rgb * params.misc2.z;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else if (mode < 20.5) {
        // [F3 扩展] 20 SC 阴影可见性（buffer，LightEval 同索引
        // x + y×atlas_size）：白 = lit，黑 = shadowed，灰 = 部分可见。
        int2 t = int2(uv * params.misc2.w);
        float v = shadow_vis[t.x + t.y * (int)params.misc2.w];
        return float4(clamp(float3(v), 0.0, 1.0), 1.0);
    } else if (mode < 21.5) {
        // [F3 扩展] 21 SC radiosity 间接光（albedo × tile 入射）：
        // HDR → 显示曝光 × tonemap（×16 归一：量级低探针 irradiance
        // ~两个量级，同 GLSL 孪生）。
        float3 c = src_tex.sample(s, uv).rgb * params.misc2.z * 16.0;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else if (mode < 22.5) {
        // [F3 扩展] 22 SC tile 入射辐射（tile 域低频图，A=validity）：
        // HDR → 显示曝光 × tonemap（×16 同 21）。
        float3 c = src_tex.sample(s, uv).rgb * params.misc2.z * 16.0;
        return float4(aces_tonemap(max(c, 0.0)), 1.0);
    } else {
        // [F3 扩展] 23 SC tile 兼容证据：R/G = oct 编码，B = 中心深度
        //（0..16m 标尺），A = 归属卡 id（不显示）。
        float4 a = src_tex.sample(s, uv);
        return float4(clamp(float3(a.r, a.g, a.b / 16.0), 0.0, 1.0), 1.0);
    }
}

// 二分实验入口：极简绑定面（仅 texture(0) 直出）——隔离多绑定/UBO 因素。
fragment float4 pass_debug_simple_fs(PassDebugVSOut in [[stage_in]],
                                     texture2d<float> src_tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::nearest, min_filter::nearest, mip_filter::none);
    return float4(src_tex.sample(s, in.uv).rgb, 1.0);
}
