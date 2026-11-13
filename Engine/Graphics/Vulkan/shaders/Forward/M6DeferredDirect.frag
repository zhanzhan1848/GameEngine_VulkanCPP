#version 450 core

// M6 Deferred Direct (D3D12 M6 GI 重构) — GI 模式下的延迟直射光 pass（P2）。
// 顶点阶段复用 M3Post.vert 字节（同 M4DeferredLight；恒等 UV 映射）。
// 单一 GLSL 源双链消费（同 M1..M4；入口: main）。
//
// 语义契约（M6 GI 重构核心）：输出 = 纯直射光 linear HDR——
//   rgb = lightColor * (diffuse + specular) * shadow（与 M4DeferredLight
//   的直射项逐表达式同源，见下"逐字复制"说明）；
//   完全阴影面（shadow=0）值恰好为 0：无 ambient 常数、无旧 GI、无 IBL、
//   无天空/自发光（共同链材质模型无 emissive 通道，天空=黑清屏）。
// 该缓冲是 GI 注入（M6ProbeInject）的唯一辐射源输入——direct-only 保证
// GI 间接项独立于历史输出（无光反馈回路：GI 输入只由 GBuffer + 阴影 +
// 光参数构成，从不包含 indirect 或前一帧最终图像）。
//
// MRT 双输出（诊断通道契约）：
//   location 0 SV_Target0 outDirect         rgba=直射全项，a=ndl（未乘阴影
//                                            的几何项——VERIFY 用它分离
//                                            "背光面 ndl=0"与"阴影遮挡"）
//   location 1 SV_Target1 outDirectDiffuse   rgb=光色×漫反射×阴影（GI 输入，
//                                            Lambert 发射体辐射值 L_j），
//                                            a=shadow（可见性诊断——VERIFY
//                                            遮挡/半影/受光掩码的种子）
// 背景像素（depth >= 1）：双输出均 vec4(0)——背景既非 GI 发射体也非接收体。
//
// 单位（M6 §3）：harness linear HDR、预曝光预 ACES；Lambert 不带 1/π——
// 反照率 1 的表面被 color·intensity=1 的光正对照射时值为 1.0；漫反射
// 辐射 L_j = 直接漫反射显示值，GI 侧以同一单位回积（E_h = E_phys/π）。
//
// 光照/阴影主体 = M4DeferredLight.frag main() 逐字复制（同一 texelFetch
// 取数、深度重建（inUV*2-1，无 Y 翻转，M4 §F4-4 约定）、法线重建
// （RG16F xy + material.a 符号）、L/V/H、ndl/ndh、slope/offsetPos/bias、
// clip/sc/shadowUV/ref、PCF 3×3、kShininess=48/kSpecularStrength=0.22）
// 仅删除 kAmbient 与 GI 采样项——直射表达式逐位同构，保证 GI-on 与
// GI-off（M4DeferredLight 路径）的直射项可逐位对账（VERIFY 身份核验）。
//
// 绑定布局（set0 单 set；b0–b7 与 M4DeferredLight.frag 同名同型同尺寸，
// 宿主直接复用 M4 light set layout / pipeline layout / descriptor set
// （11 绑定超集，本 shader 只声明消费 b0–b7）：
//   set 0 binding 0 = DeferredViewData UBO（80B：invViewProjection + cameraPos）
//   set 0 binding 1 = LightShadowData 合并 UBO（112B = LightData 32B +
//                     ShadowData 80B 拼接，逐帧上传）
//   set 0 binding 2 = depthTexture（D32 → R32F；texelFetch 精确纹素）
//   set 0 binding 3 = gbufferAlbedo（RT0 RGBA8）
//   set 0 binding 4 = gbufferNormal（RT1 RG16F）
//   set 0 binding 5 = gbufferMaterial（RT2 RGBA8；a=N.z 符号）
//   set 0 binding 6 = shadowMap（D32，比较采样）
//   set 0 binding 7 = shadowSampler（比较采样器，宿主 LessEqual/Linear/Clamp）

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

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outDirect;
layout(location = 1) out vec4 outDirectDiffuse;

void main() {
    // 1:1 纹素直取（gl_FragCoord.xy 像素中心 → 整数纹素）。
    ivec2 texel = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(depthTexture, texel, 0).r;
    vec3 albedo = texelFetch(gbufferAlbedo, texel, 0).rgb;
    vec2 nxy = texelFetch(gbufferNormal, texel, 0).xy;
    vec4 mat = texelFetch(gbufferMaterial, texel, 0);

    // 背景（无几何）：既非 GI 发射体也非接收体——双输出清零（含 alpha）。
    if (depth >= 1.0) {
        outDirect = vec4(0.0);
        outDirectDiffuse = vec4(0.0);
        return;
    }

    // 深度重建世界位（生产同源公式；Y 约定差异见 M4DeferredLight 文件头）。
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

    // 固定参数 Blinn-Phong（M1Forward.frag:52-61 逐字同源；无 kAmbient）。
    const float kShininess = 48.0;
    const float kSpecularStrength = 0.22;

    vec3 diffuse  = albedo * ndl;
    vec3 specular = vec3(kSpecularStrength * pow(ndh, kShininess)) * ndl;

    // 直射项乘阴影（= M4 直射项同一表达式，ambient 剥离）；alpha 携带
    // 诊断通道（文件头 MRT 契约）：outDirect.a=ndl，outDirectDiffuse.a=shadow。
    // shadow=0 ⇒ 双输出 rgb 恰为 0（无任何加性项）——完全阴影面零值契约。
    outDirect        = vec4(lightColor * (diffuse + specular) * shadow, ndl);
    outDirectDiffuse = vec4(lightColor * diffuse * shadow, shadow);
}
