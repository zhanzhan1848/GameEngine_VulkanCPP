#version 450 core

// M1 Forward (D3D12 M1 F1-7) — 最小 forward 像素阶段：单方向光 Blinn-Phong。
// 与 M1Forward.vert 同源同链（GLSL 单一事实源）。
// 入口: main。
//
// 绑定布局：
//   set 0 (global): binding 0 = ViewData UBO（vert+frag 可见）
//                   binding 1 = LightData UBO（仅 frag，32B）
//   set 1 (material): binding 0 = albedo texture2D（分离采样镜像）
//                     binding 1 = sampler（同 GBuffer.frag 的分离 image/sampler 模式）
//
// LightData 约定：dirAndIntensity.xyz = 从表面指向光源的方向（未归一，shader
// 内 normalize），w = 强度倍率；color.rgb = 光色。M1 离屏 RT 为 BGRA8 UNorm
// 直读直写（M1 §7.2-3：无 sRGB 硬转换、shader 内不做 pow 补偿）。

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    vec4 cameraPos;   // xyz = 相机世界坐标
} viewData;

layout(set = 0, binding = 1) uniform LightData {
    vec4 dirAndIntensity;
    vec4 color;
} lightData;

layout(set = 1, binding = 0) uniform texture2D albedoTexture;
layout(set = 1, binding = 1) uniform sampler    albedoSampler;

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

    // 固定参数 Blinn-Phong（无贴图粗糙度；确定性常量便于跨后端逐位比较）。
    const float kShininess = 48.0;
    const float kSpecularStrength = 0.22;
    const float kAmbient = 0.15;

    vec3 diffuse  = albedo * ndl;
    vec3 specular = vec3(kSpecularStrength * pow(ndh, kShininess)) * ndl;
    vec3 ambient  = albedo * kAmbient;

    outColor = vec4(ambient + lightColor * (diffuse + specular), 1.0);
}
