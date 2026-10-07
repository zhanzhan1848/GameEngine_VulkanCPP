#version 450 core
// PassDebugView.frag — 视口实时 pass 可视化（对齐 EngineTest/shaders/
// PassDebugView.metal pass_debug_fs 的解码逻辑与 binding 面）。
// Entry point: main
//
// Descriptor layout (matches PassDebugViewModule::Initialize):
//   set 0 binding 0 = SampledImage      float 源
//   set 0 binding 1 = Sampler           组合采样
//   set 0 binding 2 = UniformBuffer     PassDebugParams
//   set 0 binding 3 = SampledImage      深度纹理
//   set 0 binding 4 = StorageBuffer     cluster map（4×u32/条目）
//   set 0 binding 5 = SampledImage(R32U) 可见性缓冲

// 组合采样构造 sampler2D(tex, samp) 与 texelFetch(usampler2D) 均为 450 core
// 语义（不用 GL_EXT_samplerless_texture_functions——该扩展会覆盖 texelFetch
// 重载，与 usampler2D 冲突）。

layout(set = 0, binding = 0) uniform texture2D srcTex;
layout(set = 0, binding = 1) uniform sampler inputSamp;
layout(set = 0, binding = 3) uniform texture2D depthTex;
// R32_UINT 可见性缓冲用组合 usampler2D（对齐 Nanite/VisibilityBufferResolve.comp
// 的先例：R32U 无 LINEAR_FILTER 位，Metal 端 texture2d<uint> 直读）。
layout(set = 0, binding = 5) uniform usampler2D idTex;

layout(std140, set = 0, binding = 2) uniform PassDebugParams {
    mat4 inv_view_proj;
    mat4 proj;
    vec4 misc;   // x=near, y=far, z=view(mode), w=cluster_count
    vec4 misc2;  // x=width, y=height, z=GI 显示曝光（PRIMAL_PASS_DEBUG_GI_SCALE）,
                 // w=SC atlas 边长（sc_shadow buffer 索引）
} params;

layout(std430, set = 0, binding = 4) readonly buffer ClusterMapBuf {
    uint entries[];  // 条目 stride 4：{globalMeshletIndex, instanceIndex, materialID, pad}
} cluster_map;

// [F3 扩展 2026-09-20] binding 6 = SC 逐 texel 阴影可见性（sc_shadow 视图；
// LightEval 同索引 x + y×atlas_size；非该视图时绑定 64B 哑缓冲兜底）。
layout(std430, set = 0, binding = 6) readonly buffer ShadowVisBuf {
    float data[];
} shadow_vis;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

// 与 Dawn GPUDrivenDraw.wgsl hash_id_to_color 逐位一致。
vec3 hash_id_to_color(uint id) {
    uint h = id * 2654435761u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return vec3(float(h & 0xFFu) / 255.0, float((h >> 8u) & 0xFFu) / 255.0,
                float((h >> 16u) & 0xFFu) / 255.0);
}

vec3 aces_tonemap(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// [0,1] 深度 → 线性视空间深度（与 CommonFunction.metal NDCDepthToLinear 一致）。
float ndc_depth_to_linear(float ndcDepth, float nearPlane, float farPlane) {
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcDepth * (farPlane - nearPlane));
}

// 从 cluster map 反查 meshlet 所在条目；未命中返回 0xFFFFFFFF。
uint cluster_map_lookup(uint cluster_count, uint meshlet_id, out uint instance_index) {
    for (uint i = 0u; i < cluster_count; ++i) {
        if (cluster_map.entries[i * 4u + 0u] == meshlet_id) {
            instance_index = cluster_map.entries[i * 4u + 1u];
            return i;
        }
    }
    instance_index = 0xFFFFFFFFu;
    return 0xFFFFFFFFu;
}

void main() {
    const float mode = params.misc.z;
    const vec2 uv = inUv;
    const float near_p = params.misc.x;
    const float far_p = params.misc.y;

    const float d = texture(sampler2D(depthTex, inputSamp), uv).r;
    const bool background = (d >= 0.9999);

    if (mode < 0.5) {
        outColor = vec4(0.05, 0.05, 0.05, 1.0);
    } else if (mode < 1.5) {
        // Albedo：直读 + gamma。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb;
        outColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
    } else if (mode < 2.5) {
        // Normal：GBuffer 已是 [0,1] 编码，直读。
        outColor = vec4(texture(sampler2D(srcTex, inputSamp), uv).rgb, 1.0);
    } else if (mode < 3.5) {
        // ORM：R=AO, G=Roughness, B=Metallic。
        outColor = vec4(texture(sampler2D(srcTex, inputSamp), uv).rgb, 1.0);
    } else if (mode < 4.5) {
        // Depth：线性深度 / far。
        if (background) { outColor = vec4(0.05, 0.05, 0.05, 1.0); return; }
        float lin = ndc_depth_to_linear(d, near_p, far_p);
        outColor = vec4(vec3(clamp(lin / far_p, 0.0, 1.0)), 1.0);
    } else if (mode < 5.5) {
        // Velocity：RG ∈ [-0.5,0.5] → +0.5。
        vec3 v = texture(sampler2D(srcTex, inputSamp), uv).rgb + 0.5;
        outColor = vec4(clamp(v, 0.0, 1.0), 1.0);
    } else if (mode < 9.5) {
        // 6 Visibility(triangle) / 7 MeshletID / 8 ClusterID / 9 MeshID
        if (background) { outColor = vec4(0.05, 0.05, 0.05, 1.0); return; }
        ivec2 pix = ivec2(uv * params.misc2.xy);
        uint packed = texelFetch(idTex, pix, 0).r;
        uint tri = packed & 0x00FFFFFFu;
        uint meshlet = (packed >> 24u) & 0xFFu;
        if (mode < 6.5) {
            if (tri == 0u) { outColor = vec4(0.05, 0.05, 0.05, 1.0); return; }
            outColor = vec4(hash_id_to_color(tri), 1.0);
        } else if (mode < 7.5) {
            if (meshlet == 0u && tri == 0u) { outColor = vec4(0.05, 0.05, 0.05, 1.0); return; }
            outColor = vec4(hash_id_to_color(meshlet), 1.0);
        } else {
            if (tri == 0u) { outColor = vec4(0.05, 0.05, 0.05, 1.0); return; }
            uint instance_index;
            uint entry = cluster_map_lookup(uint(params.misc.w), meshlet, instance_index);
            if (entry == 0xFFFFFFFFu) {
                // 反查未命中：退化为 meshlet 哈希色并偏红提示。
                outColor = vec4(hash_id_to_color(meshlet) * 0.5 + vec3(0.25, 0.0, 0.0), 1.0);
                return;
            }
            // mode 8 → cluster 序号，mode 9 → instance 序号。
            outColor = vec4(hash_id_to_color(mode < 8.5 ? entry : instance_index), 1.0);
        }
    } else if (mode < 10.5) {
        // SSAO：R 通道灰度。
        float a = texture(sampler2D(srcTex, inputSamp), uv).r;
        outColor = vec4(vec3(a), 1.0);
    } else if (mode < 12.5) {
        // 11 Deferred / 12 GI Indirect：HDR → ACES tonemap（两者工作范围
        // 在 tonemap 线性区内，直读即可）。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else if (mode < 13.5) {
        // 13 SPGI（filtered）：探针 gather 输出是 HDR irradiance（sky 2.0 档
        // 下天窗带 ~9-12）——ACES x>4 即饱和，直读整屏全白。乘显示曝光
        // misc2.z（PRIMAL_PASS_DEBUG_GI_SCALE，默认 0.05）再 tonemap。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb * params.misc2.z;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else if (mode < 14.5) {
        // 14 SC Lighting Atlas：RGBA8 直读。
        outColor = vec4(clamp(texture(sampler2D(srcTex, inputSamp), uv).rgb, 0.0, 1.0), 1.0);
    } else if (mode < 15.5) {
        // [F3-V1] 15 SC Albedo atlas：rgba8 线性 → gamma 显示（同 GBuffer albedo）。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb;
        outColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
    } else if (mode < 16.5) {
        // [F3-V1] 16 SC Normal atlas：rg 八面体编码 → 解码 rgb（n*0.5+0.5）。
        // 与 Lumen/ShaderLib/MathLib.glsl octDecode 逐位一致（内联以规避
        // awk inliner 的 "../" 解析限制）。
        vec2 f = texture(sampler2D(srcTex, inputSamp), uv).rg * 2.0 - 1.0;
        vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
        float t = clamp(-n.z, 0.0, 1.0);
        n.xy += mix(vec2(t), vec2(-t), step(vec2(0.0), n.xy));
        outColor = vec4(normalize(n) * 0.5 + 0.5, 1.0);
    } else if (mode < 17.5) {
        // [F3-V1 修正 2026-09-20] 17 SC capture 深度。存储语义（capture 取
        // 最近表面）：到卡平面的轴向距离，GBuffer 抓的表面贴在卡平面上
        // → 主流值 0-0.3m，CardFill 深层 texel 可达数米；未初始化 = -1
        //（AtlasInit 种子，查询契约 depth<0 直接拒绝）。旧 0..16m 线性
        // 标尺把主流值压成 <2% 灰（视图近黑是标尺错，非数据错）。
        // 新映射：v<0 → 暗紫 = 未初始化；v≥0 → pow(min(v,2)/2, 0.4)，
        // 贴卡面层次可见，≥2m 饱和白。
        float dep = texture(sampler2D(srcTex, inputSamp), uv).r;
        if (dep < 0.0) {
            outColor = vec4(0.16, 0.04, 0.24, 1.0);
        } else {
            float g = pow(clamp(dep / 2.0, 0.0, 1.0), 0.4);
            outColor = vec4(vec3(g), 1.0);
        }
    } else if (mode < 18.5) {
        // [F3-V2] 18 SPGI raw：降噪前原始 gather 输出，HDR → 乘显示曝光
        //（同 13，见上注）再 ACES tonemap。raw 视图另含 gather 无覆盖
        // 区（weightSum=0 → 黑洞）——正是 denoise 归因要看的东西。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb * params.misc2.z;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else if (mode < 19.5) {
        // [F3 扩展] 19 SC Emissive atlas：HDR 自发光 → 显示曝光 × tonemap。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb * params.misc2.z;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else if (mode < 20.5) {
        // [F3 扩展] 20 SC 阴影可见性（buffer，LightEval 同索引 x + y×atlas）：
        // 白 = lit，黑 = shadowed，灰 = 部分可见（PCSS 半影）。
        ivec2 t = ivec2(uv * params.misc2.w);
        float v = shadow_vis.data[t.x + t.y * int(params.misc2.w)];
        outColor = vec4(vec3(clamp(v, 0.0, 1.0)), 1.0);
    } else if (mode < 21.5) {
        // [F3 扩展] 21 SC radiosity 间接光（albedo × tile 入射）：
        // HDR → 显示曝光 × tonemap。工作量级 ~0.01-0.5，比探针 irradiance
        // 低 ~两个量级——额外 ×16 归一（0.05×16=0.8 有效曝光）。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb * params.misc2.z * 16.0;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else if (mode < 22.5) {
        // [F3 扩展] 22 SC tile 入射辐射（tile 域低频图，A=validity）：
        // HDR → 显示曝光 × tonemap（×16 同 21）。
        vec3 c = texture(sampler2D(srcTex, inputSamp), uv).rgb * params.misc2.z * 16.0;
        outColor = vec4(aces_tonemap(max(c, vec3(0.0))), 1.0);
    } else {
        // [F3 扩展] 23 SC tile 兼容证据：R/G = oct 编码，B = 中心深度
        //（0..16m 标尺），A = 归属卡 id（不显示）。
        vec4 a = texture(sampler2D(srcTex, inputSamp), uv);
        outColor = vec4(clamp(vec3(a.r, a.g, a.b / 16.0), vec3(0.0), vec3(1.0)), 1.0);
    }
}
