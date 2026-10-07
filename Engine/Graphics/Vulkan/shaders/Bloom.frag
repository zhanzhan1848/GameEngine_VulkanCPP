#version 450 core

// Vulkan HDR bright-pass extraction.  P3（sponza-gap-audit §4.9）: threshold
// / knee / energy 从编译期常数改为 UBO（binding 2），由 BloomPass.cpp 每帧
// 上传——默认值 = 旧标定常数（4.0 / 1.0 / 0.20），行为零变化，但配置真正
// 可调（PRIMAL_BLOOM_THRESHOLD / PRIMAL_BLOOM_KNEE / PRIMAL_BLOOM_ENERGY；
// 合成强度在 ToneMapping 端 PRIMAL_BLOOM_INTENSITY）。
// 量级注记（取代过时标定）：当前 Sponza 校准的解析天空基色在 HDR 中约
// 4.2–6.0（旧注释按 2.75 标定）。默认阈值 4.0 下天空可能部分进入 bright
// pass——调参者应实看 bright buffer（TONE_OFF + PRIMAL_BLOOM=1）后按需
// 抬高阈值，而不是沿用旧注释的量级假设。

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform texture2D inputTexture;
layout(set = 0, binding = 1) uniform sampler texSampler;

layout(std140, binding = 2) uniform BloomParamsBlock {
    float threshold; // HDR 亮度阈值（平滑过渡带中心）
    float knee;      // 过渡半宽：带为 [threshold-knee, threshold+knee]
    float energy;    // 提亮能量乘数
    float _pad;
} bloomParams;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void bright_pass_fs() {
    vec4 color = texture(sampler2D(inputTexture, texSampler), inUv);
    float brightness = max(dot(max(color.rgb, vec3(0.0)),
                               vec3(0.2126, 0.7152, 0.0722)), 0.0);

    // The transition covers [threshold-knee, threshold+knee].  smoothstep is
    // continuous at both ends, which keeps downsampled cloth edges from
    // producing a visible threshold contour before the blur.
    float kneeStart = max(0.0, bloomParams.threshold - bloomParams.knee);
    float kneeEnd = bloomParams.threshold + bloomParams.knee;
    float contribution = smoothstep(kneeStart, kneeEnd, brightness);

    outColor = vec4(max(color.rgb, vec3(0.0)) * contribution * bloomParams.energy,
                    color.a);
}
