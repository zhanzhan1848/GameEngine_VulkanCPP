#version 450 core

// M4 Deferred Light (D3D12 M4 F4-3/F4-4) — 全屏延迟光照像素阶段。
// 顶点阶段复用 M3Post.vert（gl_VertexIndex 全屏三角形 + 恒等 UV 映射，
// M3 §6 初版翻转坑已修正的那一版——延迟 pass 与 GBuffer pass 同 viewport
// 约定，采样恒等保向）。单一 GLSL 源双链消费（同 M1/M2/M3；入口: main）。
//
// 光照数学 = M1Forward.frag/M2ForwardLit.frag 逐表达式同源（固定参数
// Blinn-Phong + M2 单级联 PCF3×3 阴影）——同轨迹同帧 DEFERRED=0 vs =1 的
// 输出差异只剩 GBuffer 量化（albedo RGBA8 / normal RG16F f16 / 深度 f32 重建
// 的浮点误差），这是 M4 阶段的核心等价证据设计（F4-4 像素对比）。
//
// 深度重建（F4-4）：与生产 DeferredLighting.frag:274-297 同源公式——
//   ndc = (uv.x*2-1, uv.y*2-1, depth)；worldPos = (invViewProjection*ndc4).xyz
//   / max(w, 1e-6)
// 生产 Y 项为 (1 - uv.y*2)（补偿生产 WGSL GBuffer 光栅 naga 自动 Y-negate，
// 2026-10-23 追踪 §3.2）；共同链 GBuffer VS 为 GLSL 直写 gl_Position 无
// Y-negate（M1 §7.2-1），且全屏三角形 UV 为恒等映射 → 本处取 (uv*2-1)，
// 与生产公式的差异=两链 NDC 约定差异，登记于 M4 阶段文档盘点表。
//
// 天空/背景：depth >= 1.0 早退写清屏色 (0,0,0,1)——forward 路径背景=RT 清屏
// 值（无几何像素不执行 PS），延迟全屏三角形必须显式复现该语义（生产同构
// 早退位 DeferredLighting.frag:272，生产写程序化天空，共同链无天空=黑）。
//
// 绑定布局（set0 单 set；反射契约冻结）：
//   set 0 binding 0 = DeferredViewData UBO（80B：invViewProjection + cameraPos）
//   set 0 binding 1 = LightShadowData 合并 UBO（112B = LightData 32B +
//                     ShadowData 80B 逐字段拼接；宿主逐帧上传——光照关键帧
//                     段由轨迹 config 驱动，非时变）
//   set 0 binding 2 = depthTexture（D32 → R32F 采样；texelFetch 精确纹素）
//   set 0 binding 3 = gbufferAlbedo（RT0 RGBA8）
//   set 0 binding 4 = gbufferNormal（RT1 RG16F）
//   set 0 binding 5 = gbufferMaterial（RT2 RGBA8；a=N.z 符号）
//   set 0 binding 6 = shadowMap（D32，比较采样）
//   set 0 binding 7 = shadowSampler（比较采样器，宿主 LessEqual/Linear/Clamp）
// M6 F6-2 GI 消费扩展（RHI_TEST_GI 门控面；默认路径逐位不变——见下）：
//   set 0 binding 8  = giProbeTexture（RGBA16F 3D 纹理 sampler3D——GI 切片
//                      为探针辐照网格 ping-pong 当前帧；GI=0 时宿主绑 1x1x1
//                      黑 3D 占位纹理）
//   set 0 binding 9  = giProbeSampler（Linear/Clamp tri-linear 采样）
//   set 0 binding 10 = GIParams UBO（32B：gridMinAndScale xyz=网格世界盒 min
//                      w=giScale（GI=0 时 0.0）；gridInvExtent xyz=1/盒尺寸）
// texelFetch（samplerless 语义）读 GBuffer/深度：1:1 纹素直取无过滤——
// 消除过滤差异，量化误差是唯一剩余变量。
//
// M6 逐位不变契约：ambient 项扩展为 albedo*kAmbient + albedo*gi.rgb*
// giScale。giScale=0（RHI_TEST_GI=0）时 gi 项=+0.0——IEEE 加零不舍入，
// 后续表达式与 M4/M5 版本逐位一致（黑占位纹理无 NaN/Inf，0*有限=+0）；
// 编译器指令调度差异不改变逐浮点操作的舍入语义。

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform DeferredViewData {
    mat4 invViewProjection;   // 主相机 VP 的逆（宿主逐帧上传）
    vec4 cameraPos;           // xyz = 相机世界坐标
} viewData;

layout(set = 0, binding = 1) uniform LightShadowData {
    // ---- LightData（32B，字段语义与 M1Forward.frag LightData 同源）----
    vec4 dirAndIntensity;     // xyz = 表面指向光源（未归一），w = 强度
    vec4 color;
    // ---- ShadowData（80B，与 M2ForwardLit.frag ShadowData 同源）----
    mat4 lightViewProjection;
    vec4 shadowParams;        // x=texelWorld y=depthSpan z=texelUV w=_
} lightShadowData;

layout(set = 0, binding = 2) uniform texture2D depthTexture;
layout(set = 0, binding = 3) uniform texture2D gbufferAlbedo;
layout(set = 0, binding = 4) uniform texture2D gbufferNormal;
layout(set = 0, binding = 5) uniform texture2D gbufferMaterial;
layout(set = 0, binding = 6) uniform texture2D  shadowMap;
layout(set = 0, binding = 7) uniform samplerShadow shadowSampler;
// M6 F6-2 GI 消费扩展（b8/b9/b10；GI=0 时 giScale=0 → 输出逐位不变）。
layout(set = 0, binding = 8) uniform texture3D giProbeTexture;
layout(set = 0, binding = 9) uniform sampler giProbeSampler;
layout(set = 0, binding = 10) uniform GIParams {
    vec4 gridMinAndScale;   // xyz = 探针网格世界盒 min，w = giScale（0/1）
    vec4 gridInvExtent;     // xyz = 1/(max-min)
} giParams;

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    // 1:1 纹素直取（gl_FragCoord.xy 像素中心 → 整数纹素）。
    ivec2 texel = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(depthTexture, texel, 0).r;
    vec3 albedo = texelFetch(gbufferAlbedo, texel, 0).rgb;
    vec2 nxy = texelFetch(gbufferNormal, texel, 0).xy;
    vec4 mat = texelFetch(gbufferMaterial, texel, 0);

    // 背景（无几何）：复现 forward 清屏语义（生产同构早退位见文件头）。
    if (depth >= 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // 深度重建世界位（生产同源公式；Y 约定差异见文件头）。
    vec2 ndcXY = vec2(inUV.x * 2.0 - 1.0, inUV.y * 2.0 - 1.0);
    vec4 ndc = vec4(ndcXY, depth, 1.0);
    vec4 worldPosH = viewData.invViewProjection * ndc;
    vec3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // 法线重建（RG16F xy 直存 + rt2.a 符号；写入侧 normalize 后量化，
    // 重建侧再 normalize——与 forward 的逐像素 normalize 对齐到量化误差）。
    float zSign = mat.a * 2.0 - 1.0;
    vec3 N = normalize(vec3(nxy, zSign * sqrt(max(0.0, 1.0 - dot(nxy, nxy)))));

    // ---- 以下光照/阴影表达式 = M2ForwardLit.frag 逐字复制（等价证据设计）----
    vec3 L = normalize(lightShadowData.dirAndIntensity.xyz);
    vec3 V = normalize(viewData.cameraPos.xyz - worldPos);
    vec3 H = normalize(L + V);

    float intensity = lightShadowData.dirAndIntensity.w;
    vec3  lightColor = lightShadowData.color.rgb * intensity;

    float ndl = max(dot(N, L), 0.0);
    float ndh = max(dot(N, H), 0.0);

    // ---- 阴影可见性（生产 B17.2 参数，单级联；M2ForwardLit.frag:80-109 同源）----
    float ndlRaw = dot(N, L);
    float slope = clamp(sqrt(max(0.0, 1.0 - ndlRaw * ndlRaw)) /
                        max(abs(ndlRaw), 0.25), 0.0, 4.0);
    vec3 offsetPos = worldPos +
        N * lightShadowData.shadowParams.x * min(1.0 + slope, 4.0);
    float bias = lightShadowData.shadowParams.x *
                 (0.5 + 2.0 * min(slope, 3.0)) / lightShadowData.shadowParams.y;

    vec4 clip = lightShadowData.lightViewProjection * vec4(offsetPos, 1.0);
    vec3 sc = clip.xyz / max(clip.w, 1e-6);
    vec2 shadowUV = sc.xy * 0.5 + 0.5;          // 无 Y 翻转（M2 §3 契约）
    float ref = sc.z - bias;                     // 生产比较语义 → LessEqual

    float shadow = 1.0;                          // 界外 = lit（生产同）
    if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 &&
        shadowUV.y >= 0.0 && shadowUV.y <= 1.0 &&
        sc.z >= 0.0 && sc.z <= 1.0) {
        float sum = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec2 tapUV = shadowUV +
                    vec2(float(dx), float(dy)) * lightShadowData.shadowParams.z;
                sum += texture(sampler2DShadow(shadowMap, shadowSampler),
                               vec3(tapUV, ref));
            }
        }
        shadow = sum / 9.0;
    }

    // 固定参数 Blinn-Phong（M1Forward.frag:52-61 逐字同源）。
    const float kShininess = 48.0;
    const float kSpecularStrength = 0.22;
    const float kAmbient = 0.15;

    // M6 F6-2：GI ambient 项——探针网格 tri-linear 采样调制（世界位→网格
    // uvw，盒外钳制）。giScale=0 时 giAmbient=+0.0，ambient 与 M4/M5 逐位
    // 一致（见文件头逐位不变契约）。
    vec3 giUVW = clamp((worldPos - giParams.gridMinAndScale.xyz)
                       * giParams.gridInvExtent.xyz, vec3(0.0), vec3(1.0));
    vec3 giAmbient = albedo
        * texture(sampler3D(giProbeTexture, giProbeSampler), giUVW).rgb
        * giParams.gridMinAndScale.w;

    vec3 diffuse  = albedo * ndl;
    vec3 specular = vec3(kSpecularStrength * pow(ndh, kShininess)) * ndl;
    vec3 ambient  = albedo * kAmbient + giAmbient;

    // 直射项乘阴影；ambient 不乘（M2ForwardLit.frag:121 同式）。
    outColor = vec4(ambient + lightColor * (diffuse + specular) * shadow, 1.0);
}
