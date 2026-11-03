#version 450 core

// M2 Forward Lit (D3D12 M2 F2-5) — M1Forward.frag + 单级联方向光阴影。
// 单一 GLSL 源双链消费（同 M1Forward.*）；入口: main。
// 顶点阶段复用 M1Forward.vert（worldPos/worldNormal/uv/baseColor 输出不变）。
//
// 绑定布局（set0 扩一个 ShadowData UBO；set1 扩 shadow map 采样对）：
//   set 0 binding 0 = ViewData UBO（80B，同 M1）
//   set 0 binding 1 = LightData UBO（32B，同 M1）
//   set 0 binding 2 = ShadowData UBO（80B：light VP + shadowParams）
//   set 1 binding 0 = albedo texture2D + binding 1 sampler（同 M1）
//   set 1 binding 2 = shadow map texture2D（分离镜像）
//   set 1 binding 3 = samplerShadow（比较采样器，宿主 LessEqual/Linear/Clamp）
//
// ShadowData.shadowParams:
//   x = texelWorld   一张 shadow map 纹素的世界尺寸（= 2*orthoExtent/size）
//   y = depthSpan    正交盒 NDC z 跨度对应的世界深度（= far - near）
//   z = texelUV      1/shadowMapSize（PCF 3×3 步进）
//   w = 未用
//
// T1 盘点生产值对齐（ShadowFilter.comp B17.2，单级联化）：
//   slope      = clamp(sqrt(1-ndl²)/max(|ndl|,0.25), 0, 4)   ← 生产同式
//   normal偏移 = N * texelWorld * min(1+slope, 4)            ← 生产正面档
//   接收端bias = texelWorld * (0.5 + 2*min(slope,3)) / span  ← 生产正面档
//   （生产另有背面部档 min(slope,4)/(1+2·min(slope,4))——前向 diffuse 已乘
//    ndl≤0 归零，背面阴影因子不可见，M2 不携带；报告已记录该裁剪。）
//   比较语义 = 生产 `sc.z - bias <= d ? lit : shadow` ⇔ ref=sc.z-bias 与
//   LessEqual 比较采样器（ref ≤ stored → 1.0）。
//
// PCF 3×3 = 9 个确定性 SampleCmp taps（无生产 PCSS/PCF 的 IGN 旋转 Poisson
// 盘——M2 共同最小路径选型，见任务书；确定性利于跨帧/跨后端逐 SHA 比对）。
//
// shadow UV：sc.xy*0.5+0.5 无 Y 翻转——M2ShadowDepth.vert 无 Y-negate，
// lit pass 与 depth pass 同一 NDC 约定；D3D12 负 viewport 高度对两 pass
// 对称生效，shader 内不按后端分支（M1 §7.2-1 决策沿袭，报告记录与生产
// ShadowFilter 0.5-y*0.5 的差异成因）。

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    vec4 cameraPos;   // xyz = 相机世界坐标
} viewData;

layout(set = 0, binding = 1) uniform LightData {
    vec4 dirAndIntensity;   // xyz = 表面指向光源（未归一），w = 强度
    vec4 color;
} lightData;

layout(set = 0, binding = 2) uniform ShadowData {
    mat4 lightViewProjection;
    vec4 shadowParams;      // x=texelWorld y=depthSpan z=texelUV w=_
} shadowData;

layout(set = 1, binding = 0) uniform texture2D albedoTexture;
layout(set = 1, binding = 1) uniform sampler    albedoSampler;
layout(set = 1, binding = 2) uniform texture2D  shadowMap;
layout(set = 1, binding = 3) uniform samplerShadow shadowSampler;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inBaseColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo = texture(sampler2D(albedoTexture, albedoSampler), inUV).rgb
                * inBaseColor.rgb;

    vec3 N = normalize(inWorldNormal);
    vec3 L = normalize(lightData.dirAndIntensity.xyz);
    vec3 V = normalize(viewData.cameraPos.xyz - inWorldPos);
    vec3 H = normalize(L + V);

    float intensity = lightData.dirAndIntensity.w;
    vec3  lightColor = lightData.color.rgb * intensity;

    float ndl = max(dot(N, L), 0.0);
    float ndh = max(dot(N, H), 0.0);

    // ---- 阴影可见性（生产 B17.2 参数，单级联）----
    // slope 用未截断的 dot（背面档不可见，但保持公式与生产同源）。
    float ndlRaw = dot(N, L);
    float slope = clamp(sqrt(max(0.0, 1.0 - ndlRaw * ndlRaw)) /
                        max(abs(ndlRaw), 0.25), 0.0, 4.0);
    vec3 offsetPos = inWorldPos +
        N * shadowData.shadowParams.x * min(1.0 + slope, 4.0);
    float bias = shadowData.shadowParams.x *
                 (0.5 + 2.0 * min(slope, 3.0)) / shadowData.shadowParams.y;

    vec4 clip = shadowData.lightViewProjection * vec4(offsetPos, 1.0);
    vec3 sc = clip.xyz / max(clip.w, 1e-6);
    vec2 shadowUV = sc.xy * 0.5 + 0.5;          // 无 Y 翻转（见文件头）
    float ref = sc.z - bias;                     // 生产比较语义 → LessEqual

    float shadow = 1.0;                          // 界外 = lit（生产同）
    if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 &&
        shadowUV.y >= 0.0 && shadowUV.y <= 1.0 &&
        sc.z >= 0.0 && sc.z <= 1.0) {
        float sum = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec2 tapUV = shadowUV +
                    vec2(float(dx), float(dy)) * shadowData.shadowParams.z;
                sum += texture(sampler2DShadow(shadowMap, shadowSampler),
                               vec3(tapUV, ref));
            }
        }
        shadow = sum / 9.0;
    }

    // 固定参数 Blinn-Phong（同 M1Forward.frag；常量便于跨后端逐位比较）。
    const float kShininess = 48.0;
    const float kSpecularStrength = 0.22;
    const float kAmbient = 0.15;

    vec3 diffuse  = albedo * ndl;
    vec3 specular = vec3(kSpecularStrength * pow(ndh, kShininess)) * ndl;
    vec3 ambient  = albedo * kAmbient;

    // 直射项乘阴影；ambient 不乘（生产 deferred 直射/环境分离同构）。
    outColor = vec4(ambient + lightColor * (diffuse + specular) * shadow, 1.0);
}
